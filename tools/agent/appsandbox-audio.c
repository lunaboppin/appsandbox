/*
 * appsandbox-audio.exe — Guest-side audio for AppSandbox.
 *
 * Runs as SYSTEM in the console session, spawned by the agent service.
 * Listens on AF_HYPERV socket (GUID :0004) for the host IDD display to
 * connect, then:
 *
 *   VM sound -> host: loopback-captures the guest playback device and
 *   streams raw PCM to the host. Device preference: the AppSandbox virtual
 *   audio driver (test-signed kernel driver: only loads without Secure Boot),
 *   then VB-Audio "Hi-Fi Cable", then whatever playback device is default.
 *
 *   Host microphone -> VM: while a guest app is using the microphone, asks
 *   the host to stream its microphone and plays it into VB-Audio "CABLE
 *   Input"; guest apps record it from "CABLE Output". The VB-Audio drivers
 *   are Microsoft-signed, so this works with Secure Boot on.
 *
 * VB-Audio drivers may not be bundled, so when they are missing the helper
 * offers (once per boot, unless dismissed for good) to open the download
 * page in the user's browser.
 *
 * Wire protocol (one TCP-like stream each way):
 *   guest -> host: AudioHeader { 'ASA1', rate, ch, bits, fmt, align }
 *                  (rate 0 = no playback device, control frames only)
 *                  then repeating AudioFrameHeader { bytes } + PCM, or a
 *                  control frame { AUDIO_CTRL_FLAG | flags } with no payload.
 *   host -> guest: MicHeader { 'ASM1', ... } per capture start, then
 *                  AudioFrameHeader { bytes } + PCM.
 *
 * Low-latency path: socket writes are basically memcpy on the virtual bus,
 * so we poll the capture client at 5 ms and drain.
 *
 * Logs to C:\Windows\AppSandbox\audio.log.
 */

#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include "../transport/asb_transport.h"   /* AF_HYPERV on PC, ivshmem on Mac */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

/* ---- GUID storage (materialize COM + KS GUIDs locally) ---- */

DEFINE_GUID(APPSANDBOX_CLSID_MMDeviceEnumerator,
    0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(APPSANDBOX_IID_IMMDeviceEnumerator,
    0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(APPSANDBOX_IID_IAudioClient,
    0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(APPSANDBOX_IID_IAudioCaptureClient,
    0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(APPSANDBOX_IID_IAudioRenderClient,
    0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(APPSANDBOX_KSDATAFORMAT_SUBTYPE_PCM,
    0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(APPSANDBOX_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
    0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
/* Undocumented but long-stable IPolicyConfig (used to set default devices). */
DEFINE_GUID(APPSANDBOX_CLSID_PolicyConfigClient,
    0x870AF99C, 0x171D, 0x4F9E, 0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9);
DEFINE_GUID(APPSANDBOX_IID_IPolicyConfig,
    0xF8679F50, 0x850A, 0x41CF, 0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8);

typedef struct PolicyConfigVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    void *GetMixFormat, *GetDeviceFormat, *ResetDeviceFormat, *SetDeviceFormat,
         *GetProcessingPeriod, *SetProcessingPeriod, *GetShareMode, *SetShareMode,
         *GetPropertyValue, *SetPropertyValue;
    HRESULT (STDMETHODCALLTYPE *SetDefaultEndpoint)(void *, LPCWSTR, ERole);
    void *SetEndpointVisibility;
} PolicyConfigVtbl;
typedef struct { const PolicyConfigVtbl *lpVtbl; } PolicyConfig;

/* ---- Wire protocol ---- */

#define AUDIO_HEADER_MAGIC  0x31415341u  /* 'ASA1' little-endian */
#define MIC_HEADER_MAGIC    0x314D5341u  /* 'ASM1' little-endian */

#define AUDIO_CTRL_FLAG        0x80000000u
#define AUDIO_CTRL_MIC_WANTED  0x00000001u  /* a guest app is recording */
#define AUDIO_CTRL_NO_MIC_SINK 0x00000002u  /* VB-CABLE missing */
#define AUDIO_CTRL_NO_SPEAKER  0x00000004u  /* no playback device to capture */

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;       /* 1 = WAVE_FORMAT_PCM, 3 = WAVE_FORMAT_IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

/* Endpoint friendly-name fragments. */
static const wchar_t VAD_DEVICE_NAME[]    = L"App Sandbox Virtual Audio";
static const wchar_t HIFI_CABLE_NAME[]    = L"VB-Audio Hi-Fi Cable";
static const wchar_t VB_CABLE_NAME[]      = L"VB-Audio Virtual Cable";
static const wchar_t VB_DOWNLOAD_URL[]    = L"https://vb-audio.com/Cable/";

/* Capture poll interval — 5 ms gives good latency without burning CPU. */
#define AUDIO_POLL_MS       5
/* How often to re-check whether a guest app is using the microphone. */
#define MIC_DEMAND_POLL_MS  250

/* WASAPI requested buffer size (100-ns units). 40 ms = 400000. */
#define AUDIO_BUFFER_HNS    400000
/* Mic render: 100 ms buffer, but drop input beyond ~60 ms queued so
   latency cannot build up when the host runs slightly faster. */
#define MIC_BUFFER_HNS      1000000
#define MIC_MAX_QUEUED_MS   60

/* ---- Logging ---- */

static void audio_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\audio.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- Reliable send / recv ---- */

static BOOL send_all(AsbConn *c, const void *buf, int len)
{
    return asb_send(c, buf, len) == len;
}

static BOOL recv_all(AsbConn *c, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = asb_recv(c, p, len);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* ---- Endpoint lookup ---- */

static IMMDeviceEnumerator *create_enumerator(void)
{
    IMMDeviceEnumerator *pEnum = NULL;
    HRESULT hr = CoCreateInstance(&APPSANDBOX_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &APPSANDBOX_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) audio_log("CoCreateInstance(MMDeviceEnumerator) failed (0x%08lx).", hr);
    return pEnum;
}

/* First active endpoint of the given flow whose friendly name contains
   `fragment` (friendly names look like "Speakers (App Sandbox Virtual
   Audio)"; substring match tolerates the class prefix and any dup index). */
static IMMDevice *find_endpoint(EDataFlow flow, const wchar_t *fragment)
{
    IMMDeviceEnumerator *pEnum = create_enumerator();
    IMMDeviceCollection *pColl = NULL;
    IMMDevice *pFound = NULL;
    UINT count = 0, i;

    if (!pEnum) return NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(pEnum, flow, DEVICE_STATE_ACTIVE, &pColl))) {
        IMMDeviceEnumerator_Release(pEnum);
        return NULL;
    }
    IMMDeviceCollection_GetCount(pColl, &count);
    for (i = 0; i < count && !pFound; i++) {
        IMMDevice *pDev = NULL;
        IPropertyStore *pProps = NULL;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (FAILED(IMMDeviceCollection_Item(pColl, i, &pDev))) continue;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(pDev, STGM_READ, &pProps)) &&
            SUCCEEDED(IPropertyStore_GetValue(pProps, &PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal && wcsstr(name.pwszVal, fragment)) {
            pFound = pDev;
            pDev = NULL;
        }
        PropVariantClear(&name);
        if (pProps) IPropertyStore_Release(pProps);
        if (pDev) IMMDevice_Release(pDev);
    }
    IMMDeviceCollection_Release(pColl);
    IMMDeviceEnumerator_Release(pEnum);
    return pFound;
}

static IMMDevice *default_endpoint(EDataFlow flow)
{
    IMMDeviceEnumerator *pEnum = create_enumerator();
    IMMDevice *pDev = NULL;
    if (!pEnum) return NULL;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, flow, eConsole, &pDev)))
        pDev = NULL;
    IMMDeviceEnumerator_Release(pEnum);
    return pDev;
}

static void log_endpoint(const char *what, IMMDevice *pDev)
{
    IPropertyStore *pProps = NULL;
    PROPVARIANT name;
    PropVariantInit(&name);
    if (pDev && SUCCEEDED(IMMDevice_OpenPropertyStore(pDev, STGM_READ, &pProps)) &&
        SUCCEEDED(IPropertyStore_GetValue(pProps, &PKEY_Device_FriendlyName, &name)) &&
        name.vt == VT_LPWSTR)
        audio_log("%s: %ls", what, name.pwszVal);
    else
        audio_log("%s: none", what);
    PropVariantClear(&name);
    if (pProps) IPropertyStore_Release(pProps);
}

static BOOL same_endpoint(IMMDevice *a, IMMDevice *b)
{
    LPWSTR ida = NULL, idb = NULL;
    BOOL same = FALSE;
    if (a && b && SUCCEEDED(IMMDevice_GetId(a, &ida)) && SUCCEEDED(IMMDevice_GetId(b, &idb)))
        same = wcscmp(ida, idb) == 0;
    if (ida) CoTaskMemFree(ida);
    if (idb) CoTaskMemFree(idb);
    return same;
}

/* Guest playback device to loopback-capture for the host. */
static IMMDevice *choose_speaker(void)
{
    IMMDevice *pDev = find_endpoint(eRender, VAD_DEVICE_NAME);
    if (!pDev) pDev = find_endpoint(eRender, HIFI_CABLE_NAME);
    if (!pDev) pDev = default_endpoint(eRender);
    return pDev;
}

/* Make `pDev` the default device for every role (so apps play to / record
   from it without the user changing settings). Skips when already default. */
static void make_default(IMMDevice *pDev, EDataFlow flow, const char *what)
{
    IMMDevice *pCurrent = default_endpoint(flow);
    LPWSTR id = NULL, current_id = NULL;
    PolicyConfig *pPolicy = NULL;
    BOOL same;
    if (!pDev || FAILED(IMMDevice_GetId(pDev, &id))) goto out;
    if (pCurrent) IMMDevice_GetId(pCurrent, &current_id);
    same = current_id && wcscmp(id, current_id) == 0;
    if (!same &&
        SUCCEEDED(CoCreateInstance(&APPSANDBOX_CLSID_PolicyConfigClient, NULL, CLSCTX_ALL,
                                   &APPSANDBOX_IID_IPolicyConfig, (void **)&pPolicy))) {
        pPolicy->lpVtbl->SetDefaultEndpoint(pPolicy, id, eConsole);
        pPolicy->lpVtbl->SetDefaultEndpoint(pPolicy, id, eMultimedia);
        pPolicy->lpVtbl->SetDefaultEndpoint(pPolicy, id, eCommunications);
        pPolicy->lpVtbl->Release(pPolicy);
        log_endpoint(what, pDev);
    }
out:
    if (id) CoTaskMemFree(id);
    if (current_id) CoTaskMemFree(current_id);
    if (pCurrent) IMMDevice_Release(pCurrent);
}

/* ---- Microphone demand ----
   Windows records microphone use per app under CapabilityAccessManager (the
   same data behind the taskbar microphone indicator): an entry whose
   LastUsedTimeStop is 0 while LastUsedTimeStart is set is recording now.
   Reading it works from this SYSTEM process for every signed-in user. */

static BOOL consent_key_in_use(HKEY key)
{
    ULONGLONG start = 0, stop = 0;
    DWORD size = sizeof(start);
    if (RegQueryValueExW(key, L"LastUsedTimeStart", NULL, NULL, (BYTE *)&start, &size) != ERROR_SUCCESS)
        return FALSE;
    size = sizeof(stop);
    if (RegQueryValueExW(key, L"LastUsedTimeStop", NULL, NULL, (BYTE *)&stop, &size) != ERROR_SUCCESS)
        return FALSE;
    return start != 0 && stop == 0;
}

/* Walk microphone\<app> and microphone\NonPackaged\<app>. */
static BOOL consent_tree_in_use(HKEY root, const wchar_t *path, int depth)
{
    HKEY key;
    DWORD i;
    wchar_t child[512];
    BOOL in_use = FALSE;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
    for (i = 0; !in_use; i++) {
        DWORD len = _countof(child);
        HKEY sub;
        if (RegEnumKeyExW(key, i, child, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(key, child, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        in_use = consent_key_in_use(sub);
        if (!in_use && depth > 0 && _wcsicmp(child, L"NonPackaged") == 0) {
            wchar_t nested[1024];
            swprintf_s(nested, _countof(nested), L"%s\\%s", path, child);
            in_use = consent_tree_in_use(root, nested, depth - 1);
        }
        RegCloseKey(sub);
    }
    RegCloseKey(key);
    return in_use;
}

static BOOL microphone_in_use(void)
{
    static const wchar_t kConsent[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    wchar_t sid[256], path[1024];
    DWORD i;
    if (consent_tree_in_use(HKEY_LOCAL_MACHINE, kConsent, 1)) return TRUE;
    for (i = 0; ; i++) {
        DWORD len = _countof(sid);
        if (RegEnumKeyExW(HKEY_USERS, i, sid, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (wcsncmp(sid, L"S-1-5-21-", 9) != 0 || wcsstr(sid, L"_Classes")) continue;
        swprintf_s(path, _countof(path), L"%s\\%s", sid, kConsent);
        if (consent_tree_in_use(HKEY_USERS, path, 1)) return TRUE;
    }
    return FALSE;
}

/* ---- "Install VB-Audio drivers" prompt ---- */

static const wchar_t kPromptKey[] = L"SOFTWARE\\AppSandbox\\Guest";

static void open_url_as_user(const wchar_t *url)
{
    HANDLE token = NULL;
    void *env = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[512];
    if (!WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token)) {
        audio_log("Prompt: no signed-in user to open %ls (%lu).", url, GetLastError());
        return;
    }
    CreateEnvironmentBlock(&env, token, FALSE);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"winsta0\\default";
    swprintf_s(cmd, _countof(cmd), L"explorer.exe \"%s\"", url);
    if (CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT,
                             env, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        audio_log("Prompt: could not open browser (%lu).", GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);
}

/* ---- Automatic VB-Audio install (after the user says Yes) ----
   Downloads the newest package linked from VB-Audio's own page, verifies the
   setup program's Authenticode signature (valid, and issued to VB-Audio's
   publisher) and runs it silently as SYSTEM. Any failure falls back to
   opening the download page. */

static const wchar_t kVbHost[] = L"vb-audio.com";
static const wchar_t kVbDownloadHost[] = L"download.vb-audio.com";
static const wchar_t kVbWorkDir[] = L"C:\\Windows\\AppSandbox\\vbaudio";

/* HTTPS GET into memory (page) or a file (download). Returns bytes read. */
static DWORD https_get(const wchar_t *host, const wchar_t *path, char *mem, DWORD mem_cap,
                       const wchar_t *file)
{
    HINTERNET session = NULL, connect = NULL, request = NULL;
    HANDLE out = INVALID_HANDLE_VALUE;
    DWORD total = 0, status = 0, size = sizeof(status), got = 0;
    char buf[16384];

    session = WinHttpOpen(L"AppSandbox-Guest/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto out;
    connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) goto out;
    request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        audio_log("VB-Audio: GET https://%ls%ls failed (status %lu, err %lu).", host, path, status, GetLastError());
        goto out;
    }
    if (file) {
        out = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) goto out;
    }
    while (WinHttpReadData(request, buf, sizeof(buf), &got) && got) {
        if (file) {
            DWORD written = 0;
            if (!WriteFile(out, buf, got, &written, NULL) || written != got) { total = 0; goto out; }
        } else {
            DWORD room = mem_cap - 1 - total;
            if (got > room) got = room;
            memcpy(mem + total, buf, got);
        }
        total += got;
        if (total > 64u * 1024 * 1024 || (!file && total >= mem_cap - 1)) break;
    }
    if (!file) mem[total] = '\0';
out:
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return total;
}

/* Newest "https://download.vb-audio.com/Download_CABLE/<prefix><N>.zip" on
   the page (highest N). Writes the URL path ("/Download_CABLE/...zip"). */
static BOOL newest_package(const char *page, const char *prefix, wchar_t *path, size_t path_chars)
{
    static const char base[] = "https://download.vb-audio.com";
    const char *p = page;
    long best = -1;
    char best_path[256] = {0};
    while ((p = strstr(p, base)) != NULL) {
        const char *q = p + sizeof(base) - 1;
        const char *end = strstr(q, ".zip");
        size_t prefix_len = strlen(prefix);
        p = q;
        if (!end || (size_t)(end - q) >= sizeof(best_path) - 4) continue;
        if (strncmp(q, prefix, prefix_len) == 0) {
            long n = strtol(q + prefix_len, NULL, 10);
            const char *c;
            BOOL clean = TRUE;
            for (c = q; c < end; c++)
                if (!isalnum((unsigned char)*c) && *c != '/' && *c != '_' && *c != '-') clean = FALSE;
            if (clean && n > best) {
                best = n;
                memcpy(best_path, q, (size_t)(end - q));
                best_path[end - q] = '\0';
                strcat_s(best_path, sizeof(best_path), ".zip");
            }
        }
    }
    if (best < 0) return FALSE;
    swprintf_s(path, path_chars, L"%S", best_path);
    return TRUE;
}

static DWORD run_and_wait(wchar_t *cmd, DWORD timeout_ms)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return code;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, (UINT)-1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

/* Valid Authenticode signature whose signer is VB-Audio's publisher. */
static BOOL signed_by_vb_audio(const wchar_t *file)
{
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    CRYPT_PROVIDER_DATA *prov;
    CRYPT_PROVIDER_SGNR *signer;
    CRYPT_PROVIDER_CERT *cert;
    wchar_t name[256] = {0};
    LONG trust;
    BOOL ok = FALSE;

    ZeroMemory(&fi, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = file;
    ZeroMemory(&wd, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    trust = WinVerifyTrust(NULL, &action, &wd);
    if (trust == ERROR_SUCCESS &&
        (prov = WTHelperProvDataFromStateData(wd.hWVTStateData)) != NULL &&
        (signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0)) != NULL &&
        (cert = WTHelperGetProvCertFromChain(signer, 0)) != NULL &&
        CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, _countof(name))) {
        /* VB-Audio signs as "BUREL VINCENT Entrepreneur individuel". */
        wchar_t lower[256];
        wcscpy_s(lower, _countof(lower), name);
        _wcslwr_s(lower, _countof(lower));
        ok = (wcsstr(lower, L"burel") && wcsstr(lower, L"vincent")) || wcsstr(lower, L"vb-audio");
    }
    audio_log("VB-Audio: signature of %ls: trust=0x%08lx signer=\"%ls\" -> %s",
              file, (unsigned long)trust, name, ok ? "accepted" : "REJECTED");
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &wd);
    return ok;
}

/* First match of `pattern` under dir (one level of subfolders). */
static BOOL find_setup(const wchar_t *dir, const wchar_t *pattern, wchar_t *out, size_t out_chars)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t spec[MAX_PATH];
    swprintf_s(spec, _countof(spec), L"%s\\%s", dir, pattern);
    h = FindFirstFileW(spec, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        swprintf_s(out, out_chars, L"%s\\%s", dir, fd.cFileName);
        FindClose(h);
        return TRUE;
    }
    swprintf_s(spec, _countof(spec), L"%s\\*", dir);
    h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
            wchar_t sub[MAX_PATH], sub_spec[MAX_PATH];
            WIN32_FIND_DATAW sd;
            HANDLE sh;
            swprintf_s(sub, _countof(sub), L"%s\\%s", dir, fd.cFileName);
            swprintf_s(sub_spec, _countof(sub_spec), L"%s\\%s", sub, pattern);
            sh = FindFirstFileW(sub_spec, &sd);
            if (sh != INVALID_HANDLE_VALUE) {
                swprintf_s(out, out_chars, L"%s\\%s", sub, sd.cFileName);
                FindClose(sh);
                FindClose(h);
                return TRUE;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return FALSE;
}

/* Download, verify and silently install one package. */
static BOOL install_vb_package(const char *page, const char *prefix, const wchar_t *setup_pattern,
                               const wchar_t *setup_fallback, const wchar_t *label)
{
    wchar_t url_path[300], zip[MAX_PATH], dir[MAX_PATH], setup[MAX_PATH], cmd[1024];
    DWORD code;
    if (!newest_package(page, prefix, url_path, _countof(url_path))) {
        audio_log("VB-Audio: no %ls download link found on the page.", label);
        return FALSE;
    }
    swprintf_s(zip, _countof(zip), L"%s\\%ls.zip", kVbWorkDir, label);
    swprintf_s(dir, _countof(dir), L"%s\\%ls", kVbWorkDir, label);
    CreateDirectoryW(dir, NULL);
    audio_log("VB-Audio: downloading https://%ls%ls", kVbDownloadHost, url_path);
    if (!https_get(kVbDownloadHost, url_path, NULL, 0, zip)) return FALSE;
    swprintf_s(cmd, _countof(cmd), L"C:\\Windows\\System32\\tar.exe -xf \"%s\" -C \"%s\"", zip, dir);
    if (run_and_wait(cmd, 120000) != 0) { audio_log("VB-Audio: extracting %ls failed.", zip); return FALSE; }
    if (!find_setup(dir, setup_pattern, setup, _countof(setup)) &&
        !find_setup(dir, setup_fallback, setup, _countof(setup))) {
        audio_log("VB-Audio: no setup program in %ls.", dir);
        return FALSE;
    }
    if (!signed_by_vb_audio(setup)) return FALSE;
    swprintf_s(cmd, _countof(cmd), L"\"%s\" -i -h", setup);
    code = run_and_wait(cmd, 180000);
    audio_log("VB-Audio: %ls setup exited with %lu.", label, code);
    return code == 0;
}

/* Returns TRUE when everything that was needed installed. */
static BOOL install_vb_audio(BOOL need_mic, BOOL need_speaker)
{
    static char page[512 * 1024];
    BOOL ok = TRUE;
    CreateDirectoryW(kVbWorkDir, NULL);
    if (!https_get(kVbHost, L"/Cable/", page, sizeof(page), NULL)) return FALSE;
    if (need_mic)
        ok = install_vb_package(page, "/Download_CABLE/VBCABLE_Driver_Pack",
                                L"VBCABLE_Setup_x64.exe", L"*Setup*.exe", L"VB-CABLE") && ok;
    if (need_speaker)
        ok = install_vb_package(page, "/Download_CABLE/HiFiCableAsioBridgeSetup_v",
                                L"*Setup*x64*.exe", L"*Setup*.exe", L"Hi-Fi Cable") && ok;
    return ok;
}

static void offer_restart(void)
{
    int choice = MessageBoxW(NULL,
        L"The VB-Audio drivers are installed.\n\nRestart the VM now to finish setting up "
        L"sound and microphone passthrough?",
        L"AppSandbox - audio drivers",
        MB_YESNO | MB_ICONINFORMATION | MB_SERVICE_NOTIFICATION | MB_TOPMOST | MB_SETFOREGROUND);
    if (choice == IDYES) {
        HANDLE token;
        TOKEN_PRIVILEGES tp;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
                AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
            CloseHandle(token);
        }
        if (InitiateShutdownW(NULL, L"AppSandbox: restarting to finish installing audio drivers.", 0,
                              SHUTDOWN_RESTART | SHUTDOWN_FORCE_OTHERS | SHUTDOWN_GRACE_OVERRIDE,
                              SHTDN_REASON_MAJOR_SOFTWARE | SHTDN_REASON_MINOR_INSTALLATION |
                              SHTDN_REASON_FLAG_PLANNED) != ERROR_SUCCESS)
            audio_log("VB-Audio: restart request failed (%lu).", GetLastError());
    }
}

/* Like VMware Tools: when the signed VB-Audio drivers are missing, offer
   (once per boot) to download and install them; their licence does not
   allow bundling, so the user's Yes is what fetches them. "Cancel" stops
   asking for good. */
static DWORD WINAPI prompt_thread(LPVOID param)
{
    BOOL need_mic = ((DWORD)(ULONG_PTR)param & AUDIO_CTRL_NO_MIC_SINK) != 0;
    BOOL need_speaker = ((DWORD)(ULONG_PTR)param & AUDIO_CTRL_NO_SPEAKER) != 0;
    DWORD never = 0, size = sizeof(never);
    HANDLE token;
    wchar_t text[1024];
    int choice, i;

    RegGetValueW(HKEY_LOCAL_MACHINE, kPromptKey, L"AudioDriverPromptDismissed",
                 RRF_RT_REG_DWORD, NULL, &never, &size);
    if (never) return 0;
    /* Wait (up to ~10 minutes) for someone to sign in. */
    for (i = 0; i < 120; i++) {
        if (WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token)) { CloseHandle(token); break; }
        Sleep(5000);
    }
    if (i == 120) return 0;
    Sleep(15000);   /* let the desktop settle first */

    swprintf_s(text, _countof(text),
        L"AppSandbox can pass %s%s%s through to this VM using the free VB-Audio "
        L"virtual cable drivers (Microsoft-signed, so they work with Secure Boot).\n\n"
        L"Download and install %s from vb-audio.com now? A restart is needed afterwards.\n\n"
        L"Yes = install now    No = ask again next boot    Cancel = don't ask again",
        need_speaker ? L"sound" : L"",
        need_speaker && need_mic ? L" and " : L"",
        need_mic ? L"your microphone" : L"",
        need_mic && need_speaker ? L"VB-CABLE (microphone) and Hi-Fi Cable (sound)"
            : need_mic ? L"VB-CABLE" : L"Hi-Fi Cable");
    choice = MessageBoxW(NULL, text, L"AppSandbox - audio drivers",
                         MB_YESNOCANCEL | MB_ICONINFORMATION | MB_SERVICE_NOTIFICATION |
                         MB_TOPMOST | MB_SETFOREGROUND);
    if (choice == IDYES) {
        if (install_vb_audio(need_mic, need_speaker)) {
            offer_restart();
        } else if (MessageBoxW(NULL,
                       L"AppSandbox couldn't install the VB-Audio drivers automatically "
                       L"(see C:\\Windows\\AppSandbox\\audio.log).\n\nOpen the download page instead?",
                       L"AppSandbox - audio drivers",
                       MB_YESNO | MB_ICONWARNING | MB_SERVICE_NOTIFICATION | MB_TOPMOST |
                       MB_SETFOREGROUND) == IDYES) {
            open_url_as_user(VB_DOWNLOAD_URL);
        }
    } else if (choice == IDCANCEL) {
        HKEY key;
        DWORD one = 1;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPromptKey, 0, NULL, 0, KEY_SET_VALUE, NULL,
                            &key, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(key, L"AudioDriverPromptDismissed", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
            RegCloseKey(key);
        }
    }
    audio_log("Prompt: driver prompt answered (%d).", choice);
    return 0;
}

/* ---- Resolve the wire format tag (PCM=1, float=3) from a WAVEFORMATEX ---- */

static UINT16 wfx_format_tag(const WAVEFORMATEX *wfx)
{
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        if (IsEqualGUID(&ext->SubFormat, &APPSANDBOX_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return WAVE_FORMAT_IEEE_FLOAT;
        if (IsEqualGUID(&ext->SubFormat, &APPSANDBOX_KSDATAFORMAT_SUBTYPE_PCM))
            return WAVE_FORMAT_PCM;
    }
    return wfx->wFormatTag;
}

/* ---- Initialize loopback capture on the given device ---- */

static BOOL init_capture(IMMDevice *pDevice,
                         IAudioClient **outClient,
                         IAudioCaptureClient **outCapture,
                         WAVEFORMATEX **outFmt)
{
    IAudioClient *pClient = NULL;
    IAudioCaptureClient *pCapture = NULL;
    WAVEFORMATEX *pwfx = NULL;
    HRESULT hr;

    hr = IMMDevice_Activate(pDevice, &APPSANDBOX_IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&pClient);
    if (FAILED(hr)) {
        audio_log("IMMDevice::Activate(IAudioClient) failed (0x%08lx).", hr);
        return FALSE;
    }

    hr = IAudioClient_GetMixFormat(pClient, &pwfx);
    if (FAILED(hr) || !pwfx) {
        audio_log("GetMixFormat failed (0x%08lx).", hr);
        IAudioClient_Release(pClient);
        return FALSE;
    }

    audio_log("Mix format: %u Hz, %u ch, %u bit, tag=0x%04x (wire tag=%u).",
              pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample,
              pwfx->wFormatTag, wfx_format_tag(pwfx));

    hr = IAudioClient_Initialize(pClient, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 AUDIO_BUFFER_HNS, 0, pwfx, NULL);
    if (FAILED(hr)) {
        audio_log("IAudioClient::Initialize failed (0x%08lx).", hr);
        CoTaskMemFree(pwfx);
        IAudioClient_Release(pClient);
        return FALSE;
    }

    hr = IAudioClient_GetService(pClient, &APPSANDBOX_IID_IAudioCaptureClient,
                                 (void **)&pCapture);
    if (FAILED(hr)) {
        audio_log("GetService(IAudioCaptureClient) failed (0x%08lx).", hr);
        CoTaskMemFree(pwfx);
        IAudioClient_Release(pClient);
        return FALSE;
    }

    hr = IAudioClient_Start(pClient);
    if (FAILED(hr)) {
        audio_log("IAudioClient::Start failed (0x%08lx).", hr);
        IAudioCaptureClient_Release(pCapture);
        CoTaskMemFree(pwfx);
        IAudioClient_Release(pClient);
        return FALSE;
    }

    *outClient  = pClient;
    *outCapture = pCapture;
    *outFmt     = pwfx;
    return TRUE;
}

static void teardown_capture(IAudioClient *pClient,
                             IAudioCaptureClient *pCapture,
                             WAVEFORMATEX *pwfx)
{
    if (pClient)  IAudioClient_Stop(pClient);
    if (pCapture) IAudioCaptureClient_Release(pCapture);
    if (pClient)  IAudioClient_Release(pClient);
    if (pwfx)     CoTaskMemFree(pwfx);
}

/* ---- Host microphone -> VB-CABLE Input ---- */

typedef struct {
    AsbConn *conn;
    volatile LONG ended;    /* set when the host side of the stream closes */
} MicReceiver;

static DWORD WINAPI mic_receive_thread(LPVOID param)
{
    MicReceiver *mr = (MicReceiver *)param;
    IMMDevice *pSink = NULL;
    IAudioClient *pAC = NULL;
    IAudioRenderClient *pRC = NULL;
    WAVEFORMATEX wfx;
    UINT32 buffer_frames = 0, max_queued = 0;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    BOOL started = FALSE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL com_ok = SUCCEEDED(hr);

    ZeroMemory(&wfx, sizeof(wfx));
    for (;;) {
        UINT32 word;
        if (!recv_all(mr->conn, &word, sizeof(word))) break;

        if (word == MIC_HEADER_MAGIC) {
            AudioHeader hdr;
            hdr.magic = word;
            if (!recv_all(mr->conn, (BYTE *)&hdr + sizeof(UINT32), sizeof(hdr) - sizeof(UINT32))) break;
            /* (Re)open the render stream in the host's capture format;
               WASAPI converts to the cable's mix format. */
            if (started) { IAudioClient_Stop(pAC); started = FALSE; }
            if (pRC) { IAudioRenderClient_Release(pRC); pRC = NULL; }
            if (pAC) { IAudioClient_Release(pAC); pAC = NULL; }
            if (!pSink) pSink = find_endpoint(eRender, VB_CABLE_NAME);
            if (!pSink) { audio_log("Mic: VB-CABLE Input not present; discarding host audio."); continue; }
            ZeroMemory(&wfx, sizeof(wfx));
            wfx.wFormatTag      = hdr.format_tag;
            wfx.nChannels       = hdr.channels;
            wfx.nSamplesPerSec  = hdr.sample_rate;
            wfx.wBitsPerSample  = hdr.bits_per_sample;
            wfx.nBlockAlign     = hdr.block_align ? hdr.block_align
                                  : (WORD)(hdr.channels * hdr.bits_per_sample / 8);
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            if (FAILED(hr = IMMDevice_Activate(pSink, &APPSANDBOX_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC)) ||
                FAILED(hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                    MIC_BUFFER_HNS, 0, &wfx, NULL)) ||
                FAILED(hr = IAudioClient_GetBufferSize(pAC, &buffer_frames)) ||
                FAILED(hr = IAudioClient_GetService(pAC, &APPSANDBOX_IID_IAudioRenderClient, (void **)&pRC)) ||
                FAILED(hr = IAudioClient_Start(pAC))) {
                audio_log("Mic: render setup on VB-CABLE failed (0x%08lx).", hr);
                if (pRC) { IAudioRenderClient_Release(pRC); pRC = NULL; }
                if (pAC) { IAudioClient_Release(pAC); pAC = NULL; }
                continue;
            }
            started = TRUE;
            max_queued = wfx.nSamplesPerSec * MIC_MAX_QUEUED_MS / 1000;
            audio_log("Mic: host input -> VB-CABLE (%u Hz, %u ch, %u bit).",
                      hdr.sample_rate, hdr.channels, hdr.bits_per_sample);
            continue;
        }

        /* PCM frame: `word` is its byte length. */
        if (word == 0 || word > 4 * 1024 * 1024) { audio_log("Mic: bad frame length %u.", word); break; }
        if (word > scratch_cap) {
            BYTE *nb = (BYTE *)realloc(scratch, word);
            if (!nb) break;
            scratch = nb;
            scratch_cap = word;
        }
        if (!recv_all(mr->conn, scratch, (int)word)) break;
        if (!started || !wfx.nBlockAlign) continue;
        {
            UINT32 padding = 0, frames = word / wfx.nBlockAlign, room;
            BYTE *dst;
            if (FAILED(IAudioClient_GetCurrentPadding(pAC, &padding))) continue;
            if (padding >= max_queued) continue;            /* keep latency bounded */
            room = buffer_frames - padding;
            if (frames > room) frames = room;
            if (frames && SUCCEEDED(IAudioRenderClient_GetBuffer(pRC, frames, &dst))) {
                memcpy(dst, scratch, (size_t)frames * wfx.nBlockAlign);
                IAudioRenderClient_ReleaseBuffer(pRC, frames, 0);
            }
        }
    }

    if (started) IAudioClient_Stop(pAC);
    if (pRC) IAudioRenderClient_Release(pRC);
    if (pAC) IAudioClient_Release(pAC);
    if (pSink) IMMDevice_Release(pSink);
    free(scratch);
    InterlockedExchange(&mr->ended, 1);
    if (com_ok) CoUninitialize();
    return 0;
}

/* ---- Control frames ---- */

static BOOL send_control(AsbConn *s, UINT32 flags)
{
    AudioFrameHeader fh;
    fh.bytes = AUDIO_CTRL_FLAG | flags;
    return send_all(s, &fh, sizeof(fh));
}

/* Re-check mic demand at most every MIC_DEMAND_POLL_MS; tell the host on change. */
static BOOL update_mic_demand(AsbConn *s, UINT32 static_flags, BOOL have_sink,
                              BOOL *wanted, ULONGLONG *next_check)
{
    ULONGLONG now = GetTickCount64();
    BOOL in_use;
    if (now < *next_check) return TRUE;
    *next_check = now + MIC_DEMAND_POLL_MS;
    in_use = have_sink && microphone_in_use();
    if (in_use == *wanted) return TRUE;
    *wanted = in_use;
    audio_log("Mic: guest app %s the microphone.", in_use ? "started using" : "stopped using");
    return send_control(s, static_flags | (in_use ? AUDIO_CTRL_MIC_WANTED : 0));
}

/* ---- Stream loop: drain packets, send over socket ---- */

static BOOL stream_loop(AsbConn *s,
                        IAudioCaptureClient *pCapture,
                        const WAVEFORMATEX *pwfx,
                        MicReceiver *mic, UINT32 static_flags, BOOL have_sink)
{
    /* Silence buffer for AUDCLNT_BUFFERFLAGS_SILENT packets — zero once. */
    static BYTE silence[64 * 1024];
    BOOL mic_wanted = FALSE;
    ULONGLONG next_check = 0;

    for (;;) {
        UINT32 packetLen = 0;
        HRESULT hr;

        Sleep(AUDIO_POLL_MS);

        if (InterlockedCompareExchange(&mic->ended, 0, 0)) return FALSE;
        if (!update_mic_demand(s, static_flags, have_sink, &mic_wanted, &next_check))
            return FALSE;

        /* ivshmem only: detect a host disconnect while idle. During silence there is no PCM to send,
           so the send_all calls below never fire; asb_poll reports when the host has dropped the slot
           so we fall back to the accept loop. On a Windows (AF_HYPERV) host a disconnect is reported
           by send_all failing (or the mic receiver ending), so this idle poll is only needed on ivshmem. */
        if (asb_transport_is_ivshmem() && asb_poll(s, 0) < 0)
            return FALSE;

        if (!pCapture) continue;   /* no playback device: control frames only */

        /* Drain everything that's queued right now. */
        for (;;) {
            BYTE *pData = NULL;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            UINT32 bytes;
            AudioFrameHeader fh;

            hr = IAudioCaptureClient_GetNextPacketSize(pCapture, &packetLen);
            if (FAILED(hr)) {
                audio_log("GetNextPacketSize failed (0x%08lx).", hr);
                return FALSE;
            }
            if (packetLen == 0) break;

            hr = IAudioCaptureClient_GetBuffer(pCapture, &pData, &numFrames, &flags, NULL, NULL);
            if (FAILED(hr)) {
                audio_log("GetBuffer failed (0x%08lx).", hr);
                return FALSE;
            }

            bytes = numFrames * pwfx->nBlockAlign;
            fh.bytes = bytes;

            if (!send_all(s, &fh, sizeof(fh))) {
                IAudioCaptureClient_ReleaseBuffer(pCapture, numFrames);
                return FALSE;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                UINT32 remaining = bytes;
                while (remaining > 0) {
                    UINT32 chunk = remaining > sizeof(silence) ? (UINT32)sizeof(silence) : remaining;
                    if (!send_all(s, silence, (int)chunk)) {
                        IAudioCaptureClient_ReleaseBuffer(pCapture, numFrames);
                        return FALSE;
                    }
                    remaining -= chunk;
                }
            } else if (!send_all(s, pData, (int)bytes)) {
                IAudioCaptureClient_ReleaseBuffer(pCapture, numFrames);
                return FALSE;
            }

            IAudioCaptureClient_ReleaseBuffer(pCapture, numFrames);
        }
    }
}

/* ---- Handle one connected host ---- */

static void handle_client(AsbConn *s)
{
    IMMDevice *pSpeaker = choose_speaker();
    IMMDevice *pSink = find_endpoint(eRender, VB_CABLE_NAME);        /* CABLE Input */
    IMMDevice *pCableMic = find_endpoint(eCapture, VB_CABLE_NAME);   /* CABLE Output */
    IAudioClient *pClient = NULL;
    IAudioCaptureClient *pCapture = NULL;
    WAVEFORMATEX *pwfx = NULL;
    AudioHeader hdr;
    UINT32 static_flags = 0;
    MicReceiver mic;
    HANDLE mic_thread;

    log_endpoint("Playback device (to host)", pSpeaker);
    log_endpoint("Microphone sink (VB-CABLE Input)", pSink);

    /* Apps should play to the captured device and record from the cable,
       without the user digging through Sound settings. If the only playback
       device is the cable's own input (no Hi-Fi Cable / VAD), VM sound and
       the microphone share it -- say so, since the mic will hear VM audio. */
    if (pSpeaker && same_endpoint(pSpeaker, pSink))
        audio_log("Only VB-CABLE is installed: VM sound also reaches its microphone. "
                  "Install Hi-Fi Cable to separate them.");
    else if (pSpeaker)
        make_default(pSpeaker, eRender, "Default playback set to");
    if (pCableMic)
        make_default(pCableMic, eCapture, "Default microphone set to");

    if (!pSink) static_flags |= AUDIO_CTRL_NO_MIC_SINK;
    if (!pSpeaker) static_flags |= AUDIO_CTRL_NO_SPEAKER;

    ZeroMemory(&hdr, sizeof(hdr));
    hdr.magic = AUDIO_HEADER_MAGIC;
    if (pSpeaker && init_capture(pSpeaker, &pClient, &pCapture, &pwfx)) {
        hdr.sample_rate     = pwfx->nSamplesPerSec;
        hdr.channels        = pwfx->nChannels;
        hdr.bits_per_sample = pwfx->wBitsPerSample;
        hdr.format_tag      = wfx_format_tag(pwfx);
        hdr.block_align     = pwfx->nBlockAlign;
    } else {
        static_flags |= AUDIO_CTRL_NO_SPEAKER;   /* header rate 0: control only */
    }
    if (pSpeaker) IMMDevice_Release(pSpeaker);
    if (pCableMic) IMMDevice_Release(pCableMic);

    if (!send_all(s, &hdr, sizeof(hdr)) || !send_control(s, static_flags)) {
        audio_log("Failed to send header (%d).", WSAGetLastError());
        teardown_capture(pClient, pCapture, pwfx);
        if (pSink) IMMDevice_Release(pSink);
        asb_close(s);
        return;
    }
    if (hdr.sample_rate)
        audio_log("Capture started: %u Hz, %u ch, %u bit, tag=%u.",
                  hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);
    else
        audio_log("No playback device to capture; control channel only.");

    if (static_flags & (AUDIO_CTRL_NO_MIC_SINK | AUDIO_CTRL_NO_SPEAKER)) {
        static LONG prompted;
        if (InterlockedExchange(&prompted, 1) == 0) {
            HANDLE t = CreateThread(NULL, 0, prompt_thread, (LPVOID)(ULONG_PTR)static_flags, 0, NULL);
            if (t) CloseHandle(t);
        }
    }

    mic.conn = s;
    mic.ended = 0;
    mic_thread = CreateThread(NULL, 0, mic_receive_thread, &mic, 0, NULL);

    stream_loop(s, pCapture, pwfx, &mic, static_flags, pSink != NULL);

    audio_log("Stream loop ended (host disconnected or error).");
    teardown_capture(pClient, pCapture, pwfx);
    if (pSink) IMMDevice_Release(pSink);
    /* Closing the connection also ends the mic receiver's blocked receive. */
    asb_close(s);
    if (mic_thread) {
        WaitForSingleObject(mic_thread, 5000);
        CloseHandle(mic_thread);
    }
}

/* ---- Main ---- */

int main(void)
{
    HRESULT hr;
    AsbListener *l;

    audio_log("Starting (PID=%lu, session=%lu).",
              GetCurrentProcessId(),
              WTSGetActiveConsoleSessionId());

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        audio_log("CoInitializeEx failed (0x%08lx).", hr);
        return 1;
    }

    if (asb_transport_init() != 0) {
        audio_log("asb_transport_init failed.");
        CoUninitialize();
        return 1;
    }

    l = asb_listen(ASB_CH_AUDIO);
    if (!l) {
        audio_log("asb_listen(ASB_CH_AUDIO) failed.");
        CoUninitialize();
        return 1;
    }
    audio_log("Listening on audio channel (transport=%s).",
              asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        audio_log("Host connected.");
        handle_client(c);   /* closes c */
        audio_log("Host disconnected.");
    }
}

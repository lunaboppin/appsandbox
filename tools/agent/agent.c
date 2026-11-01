/*
 * appsandbox-agent.exe — Guest-side Windows service for AppSandbox.
 *
 * Listens on a Hyper-V socket (AF_HYPERV) for commands from the host.
 * Maintains a persistent connection: sends "hello" on connect, then
 * periodic "heartbeat" messages. Processes commands inline.
 *
 * Also handles GPU driver file copy via embedded 9P client (p9copy)
 * when the host sends gpu_query_response with share metadata.
 *
 * Supports: ping, shutdown, restart, gpu_copy, gpu_query_response,
 *           gpu_none, idd_connect, set_ip.
 *
 * Usage:
 *   appsandbox-agent.exe --install   Install and start the service
 *   appsandbox-agent.exe --remove    Stop and remove the service
 *   (no args)                        Run as Windows service (SCM only)
 */

#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <userenv.h>
#include <ntsecapi.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "p9copy.h"
#include "lab_recorder.h"
#include "file_drop.h"
#include "../transport/asb_transport.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "crypt32.lib")

/* GUID_DEVCLASS_DISPLAY = {4D36E968-E325-11CE-BFC1-08002BE10318} */
static const GUID GUID_DISPLAY_CLASS =
    { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

/* DEVPKEY_Device_DriverInfPath = {A8B865DD-2E3D-4094-AD97-E593A70C75D6}, 5 */
static const DEVPROPKEY DPKEY_DriverInfPath =
    { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 5 };

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* HV_GUID_WILDCARD = {00000000-0000-0000-0000-000000000000} */
static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Must match VM_AGENT_SERVICE_GUID_STR in vm_agent.h
   {A5B0CAFE-0001-4000-8000-000000000001} */
static const GUID AGENT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0001, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Service constants ---- */

#define SERVICE_NAME    "AppSandboxAgent"
#define DISPLAY_NAME    "AppSandbox Guest Agent"

static SERVICE_STATUS        g_status;
static SERVICE_STATUS_HANDLE g_status_handle;
static HANDLE                g_stop_event;
static AsbConn *             g_client_sock = NULL; /* Active persistent connection */
static volatile BOOL         g_os_shutting_down = FALSE;
static CRITICAL_SECTION      g_send_cs;     /* Protects send_line from concurrent callers */
static wchar_t               g_shared_management_ip[64];
static char                  g_shared_nic_mac[64];

/* ---- Logging ---- */

static int send_line(AsbConn *s, const char *msg);

static void agent_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\agent.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* Send a log message to the host via the command channel (non-recursive, won't call agent_log) */
static void agent_log_to_host(const char *fmt, ...)
{
    char msg[1024];
    char line[1040];
    va_list ap;

    if (g_client_sock == NULL) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    /* One line: script output ends in a newline, and the host reads an empty line
       as the connection closing. */
    {
        size_t end = strcspn(msg, "\r\n");
        msg[end] = '\0';
    }

    snprintf(line, sizeof(line), "log:%s", msg);
    line[sizeof(line) - 1] = '\0';
    send_line(g_client_sock, line);
}

/* ---- p9copy log adapter ---- */

/* Wraps agent_log for p9copy's P9LogFn signature (identical, but explicit). */
static void p9copy_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\agent.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- GPU copy state ---- */

#define MAX_GPU_SHARES 64

typedef struct {
    char share_name[128];
    char dest_path[512];    /* wchar_t as UTF-8 — converted when used */
    char filter[4096];
} GpuShareInfo;

typedef struct {
    GpuShareInfo shares[MAX_GPU_SHARES];
    int          count;
    AsbConn *    notify_sock;    /* Socket to send progress/done to host */
    volatile BOOL copying;
} GpuCopyState;

static GpuCopyState g_gpu_copy = {0};

/* Forward declarations — defined later but needed by GPU copy */
static int recv_line(AsbConn *s, char *buf, int buf_size);
static BOOL enable_privilege(LPCWSTR priv_name);

/* ---- GPU copy background thread ---- */

/* Check if any Display class device using vrd.inf has error code 43.
   Returns TRUE if at least one vrd.inf device has problem 43. */
static BOOL check_gpu_error43(void)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    BOOL found_error43 = FALSE;

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        agent_log("GPU check: SetupDiGetClassDevs failed (%lu).", GetLastError());
        return FALSE;
    }

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        ULONG status = 0, problem = 0;
        CONFIGRET cr;
        wchar_t dev_id[512] = {0};

        CM_Get_Device_IDW(dev_info.DevInst, dev_id, 512, 0);

        /* Get driver inf path */
        if (!SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath,
                                        &prop_type, (PBYTE)inf_path,
                                        sizeof(inf_path), NULL, 0)) {
            continue;
        }

        /* Only care about vrd.inf (GPU-PV virtual render driver) */
        if (_wcsicmp(inf_path, L"vrd.inf") != 0)
            continue;

        agent_log("GPU check: found vrd.inf device: %ls", dev_id);

        cr = CM_Get_DevNode_Status(&status, &problem, dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU check: CM_Get_DevNode_Status failed (%lu) for %ls.", cr, dev_id);
            continue;
        }

        if (problem == 43) {
            agent_log("GPU check: device %ls has error 43.", dev_id);
            found_error43 = TRUE;
        } else {
            agent_log("GPU check: device %ls OK (status=0x%lx, problem=%lu).",
                      dev_id, status, problem);
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return found_error43;
}

static void disable_hyperv_video(AsbConn *notify_sock);


/* Disable and re-enable vrd.inf display devices.
   If only_error43 is TRUE, only cycles devices with problem code 43.
   If FALSE, cycles all vrd.inf devices (used at preshutdown). */
static void cycle_gpu_devices(AsbConn *notify_sock, BOOL only_error43)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    char msg[512];

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return;

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        ULONG status = 0, problem = 0;
        CONFIGRET cr;
        wchar_t dev_id[512] = {0};

        CM_Get_Device_IDW(dev_info.DevInst, dev_id, 512, 0);

        if (!SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath,
                                        &prop_type, (PBYTE)inf_path,
                                        sizeof(inf_path), NULL, 0))
            continue;

        if (_wcsicmp(inf_path, L"vrd.inf") != 0)
            continue;

        if (only_error43) {
            cr = CM_Get_DevNode_Status(&status, &problem, dev_info.DevInst, 0);
            if (cr != CR_SUCCESS || problem != 43)
                continue;
        }

        agent_log("GPU cycle: disabling %ls...", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:disabling %ls", dev_id);
        if (notify_sock != NULL) send_line(notify_sock, msg);

        cr = CM_Disable_DevNode(dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU cycle: CM_Disable_DevNode failed (%lu).", cr);
            sprintf_s(msg, sizeof(msg), "gpu_device_status:disable failed (%lu)", cr);
            if (notify_sock != NULL) send_line(notify_sock, msg);
            continue;
        }

        Sleep(1000);

        agent_log("GPU cycle: re-enabling %ls...", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:re-enabling %ls", dev_id);
        if (notify_sock != NULL) send_line(notify_sock, msg);

        cr = CM_Enable_DevNode(dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU cycle: CM_Enable_DevNode failed (%lu).", cr);
            sprintf_s(msg, sizeof(msg), "gpu_device_status:enable failed (%lu)", cr);
            if (notify_sock != NULL) send_line(notify_sock, msg);
            continue;
        }

        agent_log("GPU cycle: device %ls re-enabled.", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:re-enabled %ls", dev_id);
        if (notify_sock != NULL) send_line(notify_sock, msg);
    }

    SetupDiDestroyDeviceInfoList(devs);
}

/* WTS constants — defined manually to avoid wtsapi32.h dependency (dynamic load) */
#define MY_WTS_USERNAME 5  /* WTS_INFO_CLASS value for WTSUserName */

/* Wait for a real user to log in (not defaultuser0 or SYSTEM).
   Polls WTS sessions every 2 seconds up to timeout_ms.
   Returns the session ID, or 0xFFFFFFFF on timeout. */
static DWORD wait_for_user_login(AsbConn *notify_sock, int timeout_ms,
                                  const char *notify_msg)
{
    typedef BOOL (WINAPI *PFN_WTSQuerySessionInformationW)(
        HANDLE, DWORD, DWORD, LPWSTR *, DWORD *);
    typedef void (WINAPI *PFN_WTSFreeMemory)(PVOID);

    HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
    PFN_WTSQuerySessionInformationW pfnQuery;
    PFN_WTSFreeMemory pfnFree;
    DWORD start, now;

    if (!wts) {
        agent_log("wait_for_user_login: cannot load wtsapi32.dll.");
        return 0xFFFFFFFF;
    }

    pfnQuery = (PFN_WTSQuerySessionInformationW)
        GetProcAddress(wts, "WTSQuerySessionInformationW");
    pfnFree = (PFN_WTSFreeMemory)GetProcAddress(wts, "WTSFreeMemory");

    if (!pfnQuery || !pfnFree) {
        agent_log("wait_for_user_login: WTS functions not found.");
        FreeLibrary(wts);
        return 0xFFFFFFFF;
    }

    agent_log("Waiting for user login...");
    if (notify_sock != NULL && notify_msg)
        send_line(notify_sock, notify_msg);

    start = GetTickCount();

    for (;;) {
        DWORD session_id = WTSGetActiveConsoleSessionId();
        if (session_id != 0xFFFFFFFF) {
            LPWSTR username = NULL;
            DWORD size = 0;

            if (pfnQuery(NULL, session_id, MY_WTS_USERNAME, &username, &size)) {
                if (username && wcslen(username) > 0 &&
                    _wcsicmp(username, L"defaultuser0") != 0 &&
                    _wcsicmp(username, L"SYSTEM") != 0) {
                    agent_log("User '%ls' logged in (session %lu).", username, session_id);
                    pfnFree(username);
                    FreeLibrary(wts);
                    return session_id;
                }
                if (username) pfnFree(username);
            }
        }

        now = GetTickCount();
        if ((now - start) >= (DWORD)timeout_ms) {
            agent_log("wait_for_user_login: timed out after %d ms.", timeout_ms);
            FreeLibrary(wts);
            return 0xFFFFFFFF;
        }

        Sleep(2000);
    }
}

/* Log off a specific user session via WTSLogoffSession. */
static BOOL logoff_session(DWORD session_id)
{
    typedef BOOL (WINAPI *PFN_WTSLogoffSession)(HANDLE, DWORD, BOOL);

    HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
    PFN_WTSLogoffSession pfnLogoff;
    BOOL result;

    if (!wts) return FALSE;

    pfnLogoff = (PFN_WTSLogoffSession)GetProcAddress(wts, "WTSLogoffSession");
    if (!pfnLogoff) {
        FreeLibrary(wts);
        return FALSE;
    }

    agent_log("Logging off session %lu...", session_id);
    result = pfnLogoff(NULL, session_id, TRUE /* bWait */);
    if (!result)
        agent_log("WTSLogoffSession failed: %lu", GetLastError());
    else
        agent_log("Session %lu logged off.", session_id);

    FreeLibrary(wts);
    return result;
}

/* Run a command hidden and wait for it (used for takeown/icacls). The cmd buffer
   must be writable (CreateProcessW may modify it). */
static void run_quiet(wchar_t *cmd)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* Deploy the AppSandbox public key into administrators_authorized_keys -- the file
   Windows OpenSSH uses for members of the Administrators group (the guest account
   is an admin). sshd's StrictModes requires that file be writable only by the
   Administrators group + SYSTEM with inheritance removed, so we fix the ACL too. */
static BOOL deploy_ssh_key(const char *pubkey)
{
    wchar_t base[MAX_PATH], ssh_dir[MAX_PATH], authk[MAX_PATH], cmd[MAX_PATH + 256];
    FILE *f = NULL;

    if (!pubkey || !*pubkey) return FALSE;
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(ssh_dir, MAX_PATH, L"%s\\ssh", base);
    swprintf_s(authk, MAX_PATH, L"%s\\administrators_authorized_keys", ssh_dir);
    CreateDirectoryW(ssh_dir, NULL);

    if (_wfopen_s(&f, authk, L"w") != 0 || !f) {
        agent_log("SSH key: cannot open authorized_keys (%lu)", GetLastError());
        return FALSE;
    }
    fprintf(f, "%s\n", pubkey);
    fclose(f);

    swprintf_s(cmd, MAX_PATH + 256,
               L"icacls \"%s\" /inheritance:r /grant *S-1-5-32-544:F /grant *S-1-5-18:F",
               authk);
    run_quiet(cmd);
    agent_log("SSH key: deployed to administrators_authorized_keys");
    return TRUE;
}

/* File size in bytes, or (ULONGLONG)-1 if the file is absent. */
static ULONGLONG file_size_w(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return (ULONGLONG)-1;
    return ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

/* Provision the D3D mapping layers after the agent has copied them into `dir`
   (C:\Windows\AppSandbox\d3dlayers) over Plan9. Runs as SYSTEM from the GPU copy
   thread, AFTER the GPU driver copy. Deploys Mesa's standalone opengl32 trio +
   dxil.dll into System32 and registers the OpenCL + Vulkan ICDs via their Khronos
   registry keys. Idempotent + self-healing across boots. */
static void gl_provision(const wchar_t *dir)
{
    wchar_t path[MAX_PATH], sys[MAX_PATH], dst[MAX_PATH];
    HKEY key;
    DWORD zero = 0;

    agent_log("GL: provisioning mapping layers from %ls", dir);

    /* dxil.dll -> System32 (OpenGLOn12 / vulkan_dzn load it by leaf name). */
    if (GetSystemDirectoryW(sys, MAX_PATH)) {
        swprintf_s(path, MAX_PATH, L"%s\\dxil.dll", dir);
        swprintf_s(dst, MAX_PATH, L"%s\\dxil.dll", sys);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(path, dst, FALSE))
                agent_log("GL: staged dxil.dll to System32.");
            else
                agent_log("GL: dxil.dll -> System32 failed (%lu).", GetLastError());
        }
    }

    /* OpenGL: deploy Mesa's standalone opengl32 trio into System32. */
    if (GetSystemDirectoryW(sys, MAX_PATH)) {
        wchar_t srcdll[MAX_PATH], dstdll[MAX_PATH], bak[MAX_PATH], oldp[MAX_PATH];
        wchar_t cmd[MAX_PATH * 2];

        /* gallium_wgl.dll + z-1.dll: copied into System32 (opengl32 imports them). */
        swprintf_s(srcdll, MAX_PATH, L"%s\\gallium_wgl.dll", dir);
        swprintf_s(dstdll, MAX_PATH, L"%s\\gallium_wgl.dll", sys);
        CopyFileW(srcdll, dstdll, FALSE);
        swprintf_s(srcdll, MAX_PATH, L"%s\\z-1.dll", dir);
        swprintf_s(dstdll, MAX_PATH, L"%s\\z-1.dll", sys);
        CopyFileW(srcdll, dstdll, FALSE);

        /* opengl32.dll is TrustedInstaller-owned and memory-mapped. Replace it
           only when it isn't already our build (size differs) — self-heals if
           Windows servicing/SFC restores Microsoft's. Back up the MS copy once,
           take ownership + grant SYSTEM, rename the in-use file aside, copy ours. */
        swprintf_s(srcdll, MAX_PATH, L"%s\\opengl32.dll", dir);
        swprintf_s(dstdll, MAX_PATH, L"%s\\opengl32.dll", sys);
        if (file_size_w(srcdll) != (ULONGLONG)-1 &&
            file_size_w(srcdll) != file_size_w(dstdll)) {
            swprintf_s(bak,  MAX_PATH, L"%s\\opengl32.dll.msbak", sys);
            swprintf_s(oldp, MAX_PATH, L"%s\\opengl32.dll.old", sys);
            if (file_size_w(bak) == (ULONGLONG)-1)
                CopyFileW(dstdll, bak, TRUE);  /* back up pristine MS copy once */
            swprintf_s(cmd, MAX_PATH * 2, L"%s\\takeown.exe /f \"%s\"", sys, dstdll);
            run_quiet(cmd);
            swprintf_s(cmd, MAX_PATH * 2, L"%s\\icacls.exe \"%s\" /grant *S-1-5-18:F", sys, dstdll);
            run_quiet(cmd);
            MoveFileExW(dstdll, oldp, MOVEFILE_REPLACE_EXISTING);  /* rename in-use aside */
            if (CopyFileW(srcdll, dstdll, FALSE))
                agent_log("GL: deployed Mesa opengl32 trio to System32.");
            else
                agent_log("GL: opengl32 -> System32 failed (%lu).", GetLastError());
        } else {
            agent_log("GL: Mesa opengl32 already current in System32.");
        }
    } else {
        agent_log("GL: GetSystemDirectory failed; OpenGL not deployed.");
    }

    /* OpenCL: Khronos vendor key — value name = ICD path, data 0 (= load it). */
    swprintf_s(path, MAX_PATH, L"%s\\OpenCLOn12.dll", dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenCL\\Vendors",
                0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(key, path, 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
            RegCloseKey(key);
            agent_log("GL: registered OpenCL ICD %ls", path);
        }
    }

    /* Vulkan: Khronos driver key — value name = ICD manifest path, data 0.
       The manifest's library_path is the absolute guest DLL path (set host-side
       in d3dlayers.c) so the loader can LoadLibraryEx it under
       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, which requires a fully-qualified path. */
    swprintf_s(path, MAX_PATH, L"%s\\dzn_icd.json", dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers",
                0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(key, path, 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
            RegCloseKey(key);
            agent_log("GL: registered Vulkan (Dozen) ICD %ls", path);
        }
    }

    agent_log("GL: provisioning complete.");
}

static DWORD WINAPI gpu_copy_thread(LPVOID param)
{
    GpuCopyState *state = (GpuCopyState *)param;
    int i;
    int total_files = 0;
    int failed_shares = 0;
    char msg[256];
    wchar_t gl_dir[MAX_PATH];

    gl_dir[0] = 0;
    agent_log("GPU copy starting (%d shares)...", state->count);

    for (i = 0; i < state->count; i++) {
        GpuShareInfo *si = &state->shares[i];
        wchar_t dest_wide[MAX_PATH];
        int files = 0;
        int rc;

        dest_wide[0] = 0;
        if (MultiByteToWideChar(CP_UTF8, 0, si->dest_path, -1, dest_wide, MAX_PATH) == 0) {
            agent_log("GPU copy share '%s': dest path conversion failed (%lu), skipping.",
                      si->share_name, GetLastError());
            failed_shares++;
            continue;
        }

        /* The GL mapping-layer share is provisioned after the copy loop. */
        if (strcmp(si->share_name, "AppSandbox.GlLayers") == 0)
            wcscpy_s(gl_dir, MAX_PATH, dest_wide);

        agent_log("GPU copy share %d/%d: %s -> %s%s%s",
                  i + 1, state->count, si->share_name, si->dest_path,
                  si->filter[0] ? " [filter: " : "",
                  si->filter[0] ? si->filter : "");

        rc = p9_copy_share(50001, si->share_name, dest_wide,
                           si->filter[0] ? si->filter : NULL, &files);

        if (rc != P9_OK) {
            agent_log("GPU copy share '%s' failed (rc=%d).", si->share_name, rc);
            failed_shares++;
        } else {
            agent_log("GPU copy share '%s' done (%d files).", si->share_name, files);
        }
        total_files += files;

        /* Send progress to host */
        sprintf_s(msg, sizeof(msg), "gpu_copy_progress:%d/%d", i + 1, state->count);
        if (state->notify_sock != NULL)
            send_line(state->notify_sock, msg);
    }

    /* Provision the GL/CL/Vulkan mapping layers now — AFTER the GPU driver copy
       but BEFORE the device disable/enable cycle, so the GL configuration (Mesa
       opengl32 trio + dxil.dll in System32, Khronos OpenCL/Vulkan keys) is in
       place when the GPU device is restarted below. */
    if (gl_dir[0])
        gl_provision(gl_dir);

    /* If files were copied and vrd.inf has error 43, restart GPU + IDD devices
       and re-disable Hyper-V Video adapter. */
    if (total_files > 0 && check_gpu_error43()) {
        agent_log("GPU drivers copied and error 43 detected - attempting device restart.");
        cycle_gpu_devices(state->notify_sock, TRUE);

        /* TODO: re-enable once IDD display is stable
        Sleep(2000);
        disable_hyperv_video(state->notify_sock);
        */
    } else if (total_files > 0) {
        agent_log("GPU drivers copied, no error 43 - no device restart needed.");
    } else {
        agent_log("All GPU driver files already present (pre-staged) - no copy or restart needed.");
    }

    /* Send final result to host */
    if (failed_shares == 0) {
        sprintf_s(msg, sizeof(msg), "gpu_copy_done:%d", total_files);
        agent_log("GPU copy complete: %d files, %d shares.", total_files, state->count);
    } else {
        sprintf_s(msg, sizeof(msg), "gpu_copy_error:%d/%d shares failed",
                  failed_shares, state->count);
        agent_log("GPU copy finished with errors: %d/%d shares failed.",
                  failed_shares, state->count);
    }

    if (state->notify_sock != NULL)
        send_line(state->notify_sock, msg);

    state->copying = FALSE;
    return 0;
}

/* Parse gpu_query_response and start background copy.
   Format: "gpu_query_response:N" followed by N lines of "share|dest|filter"
   (filter may be empty). */
static void handle_gpu_query_response(AsbConn *client, int share_count)
{
    int i;
    HANDLE thread;

    if (share_count <= 0 || share_count > MAX_GPU_SHARES) {
        agent_log("GPU query response: invalid share count %d.", share_count);
        return;
    }

    if (g_gpu_copy.copying) {
        agent_log("GPU copy already in progress, ignoring.");
        return;
    }

    g_gpu_copy.count = 0;

    for (i = 0; i < share_count; i++) {
        char line[8192];
        int n = recv_line(client, line, sizeof(line));
        if (n <= 0) {
            agent_log("GPU query: failed to read share line %d.", i);
            return;
        }

        /* Parse "share_name|dest_path|filter" */
        {
            char *p1, *p2;
            GpuShareInfo *si = &g_gpu_copy.shares[g_gpu_copy.count];

            p1 = strchr(line, '|');
            if (!p1) {
                agent_log("GPU query: malformed share line: %s", line);
                continue;
            }
            *p1 = '\0';
            p1++;

            p2 = strchr(p1, '|');
            if (p2) {
                *p2 = '\0';
                p2++;
                strncpy_s(si->filter, sizeof(si->filter), p2, _TRUNCATE);
            } else {
                si->filter[0] = '\0';
            }

            strncpy_s(si->share_name, sizeof(si->share_name), line, _TRUNCATE);
            strncpy_s(si->dest_path, sizeof(si->dest_path), p1, _TRUNCATE);
            g_gpu_copy.count++;

            agent_log("GPU share [%d]: %s -> %s%s%s",
                      g_gpu_copy.count, si->share_name, si->dest_path,
                      si->filter[0] ? " filter=" : "",
                      si->filter[0] ? si->filter : "");
        }
    }

    if (g_gpu_copy.count == 0) {
        agent_log("GPU query: no valid shares to copy.");
        send_line(client, "gpu_copy_done:0");
        return;
    }

    /* Start background copy */
    g_gpu_copy.notify_sock = client;
    g_gpu_copy.copying = TRUE;
    thread = CreateThread(NULL, 0, gpu_copy_thread, &g_gpu_copy, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else {
        agent_log("Failed to create GPU copy thread (error %lu).", GetLastError());
        g_gpu_copy.copying = FALSE;
        send_line(client, "gpu_copy_error:thread creation failed");
    }
}

/* ---- Line I/O ---- */

static int recv_line(AsbConn *s, char *buf, int buf_size)
{
    int pos = 0;
    while (pos < buf_size - 1) {
        char c;
        int n = asb_recv(s, &c, 1);
        if (n <= 0) return n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int send_line(AsbConn *s, const char *msg)
{
    int len = (int)strlen(msg);
    int n;
    EnterCriticalSection(&g_send_cs);
    n = asb_send(s, msg, len);
    if (n > 0) n = asb_send(s, "\n", 1);
    LeaveCriticalSection(&g_send_cs);
    return n;
}

/* ---- Privilege helper ---- */

static BOOL enable_privilege(LPCWSTR priv_name)
{
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(NULL, priv_name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }
    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(token);
    return GetLastError() == ERROR_SUCCESS;
}

/* Tell Explorer in an interactive user session that a global SMB drive was
   added. The mapping itself is deliberately still created by the service as a
   global mapping; this helper only refreshes the shell that was already open
   when the mapping arrived. */
static BOOL spawn_shell_refresh_in_session(DWORD session_id, char drive_letter)
{
    HANDLE user_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t command_line[MAX_PATH + 64];
    wchar_t *slash;
    LPVOID env = NULL;
    BOOL ok;

    if (!WTSQueryUserToken(session_id, &user_token)) {
        agent_log("Shell refresh: WTSQueryUserToken(session=%lu) failed (%lu).",
                  session_id, GetLastError());
        return FALSE;
    }

    if (!GetModuleFileNameW(NULL, exe_path, _countof(exe_path))) {
        agent_log("Shell refresh: GetModuleFileNameW failed (%lu).", GetLastError());
        CloseHandle(user_token);
        return FALSE;
    }
    slash = wcsrchr(exe_path, L'\\');
    if (!slash) {
        CloseHandle(user_token);
        return FALSE;
    }
    swprintf_s(command_line, _countof(command_line), L"\"%ls\" --refresh-drive %c",
               exe_path, towupper((wchar_t)drive_letter));

    if (!CreateEnvironmentBlock(&env, user_token, FALSE))
        env = NULL; /* the helper does not require an environment block */

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessAsUserW(user_token, NULL, command_line, NULL, NULL, FALSE,
                              CREATE_NO_WINDOW |
                                  (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                              env, NULL, &si, &pi);
    if (!ok) {
        agent_log("Shell refresh: CreateProcessAsUserW failed (%lu).", GetLastError());
    } else {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(user_token);
    return ok;
}

static void notify_shell_drive_added(const char *letter_a)
{
    DWORD session_id;

    if (!letter_a || strlen(letter_a) != 1 ||
        !isalpha((unsigned char)letter_a[0]))
        return;
    session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF)
        return;
    if (!spawn_shell_refresh_in_session(
            session_id, (char)towupper((wchar_t)(unsigned char)letter_a[0])))
        agent_log("Shell refresh: could not notify session %lu for drive %c:.",
                  session_id, towupper((wchar_t)(unsigned char)letter_a[0]));
}

static int refresh_drive_shell(const char *letter_a)
{
    wchar_t root[8];

    if (!letter_a || strlen(letter_a) != 1 ||
        !isalpha((unsigned char)letter_a[0]))
        return ERROR_INVALID_PARAMETER;
    swprintf_s(root, _countof(root), L"%c:\\",
               towupper((wchar_t)(unsigned char)letter_a[0]));
    SHChangeNotify(SHCNE_DRIVEADD, SHCNF_PATHW, root, NULL);
    SHChangeNotify(SHCNE_DRIVEADDGUI, SHCNF_PATHW, root, NULL);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, root, NULL);
    return ERROR_SUCCESS;
}

/* ---- Input helper process (spawned into console session as SYSTEM) ---- */

static HANDLE g_input_process = NULL;
static DWORD  g_input_session = 0xFFFFFFFF;  /* session the helper was spawned into */
static volatile BOOL g_input_monitor_running = FALSE;
static HANDLE g_input_monitor_thread = NULL;

static void kill_input_helper(void)
{
    if (g_input_process) {
        TerminateProcess(g_input_process, 0);
        WaitForSingleObject(g_input_process, 3000);
        CloseHandle(g_input_process);
        g_input_process = NULL;
        g_input_session = 0xFFFFFFFF;
    }
}

/* ---- Backstage session (appsandbox-backstage.exe) ----
   A SYSTEM shell in session 0, like ScreenConnect's Backstage: session 0
   has a screen of its own that the VM never shows, so the signed-in user
   and the sign-in screen never see it. The shell streams that screen to the
   host's Backstage window itself (ASB_CH_BACKSTAGE). "backstage" starts it
   if it isn't running; "backstage_stop" ends it through the exit event. */
#define BACKSTAGE_EXIT_EVENT    L"Global\\AppSandboxBackstageExit"
#define BACKSTAGE_RUNNING_MUTEX L"Global\\AppSandboxBackstageRunning"

static HANDLE g_backstage_exit;

static BOOL backstage_running(void)
{
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, BACKSTAGE_RUNNING_MUTEX);
    if (!running) return FALSE;
    CloseHandle(running);
    return TRUE;
}

static void backstage_stop(char *reply, size_t cap)
{
    if (g_backstage_exit && backstage_running()) {
        SetEvent(g_backstage_exit);
        agent_log("Backstage: asked to close.");
    }
    strcpy_s(reply, cap, "ok:stopped");
}

static void backstage_start(char *reply, size_t cap)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH], cmd[MAX_PATH + 8], *slash;
    int waited;

    if (!g_backstage_exit)
        g_backstage_exit = CreateEventW(NULL, TRUE, FALSE, BACKSTAGE_EXIT_EVENT);
    if (backstage_running()) {
        strcpy_s(reply, cap, "ok:running");
        return;
    }
    if (!g_backstage_exit) {
        sprintf_s(reply, cap, "error:could not create the exit event (%lu)", GetLastError());
        return;
    }
    ResetEvent(g_backstage_exit);

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-backstage.exe");
    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        strcpy_s(reply, cap, "error:appsandbox-backstage.exe is not in the VM yet; restart the VM to add it");
        return;
    }
    swprintf_s(cmd, _countof(cmd), L"\"%s\"", exe_path);

    /* Our own SYSTEM token, staying in session 0, on its interactive
       window station rather than the service one we run on. */
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe_path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        sprintf_s(reply, cap, "error:could not start the Backstage shell (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);

    /* The shell takes the running mutex as it starts. */
    for (waited = 0; waited < 15000; waited += 100) {
        DWORD code = 0;
        if (backstage_running()) {
            CloseHandle(pi.hProcess);
            agent_log("Backstage: started in session 0.");
            strcpy_s(reply, cap, "ok:started");
            return;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            sprintf_s(reply, cap, "error:the Backstage shell ended at start (code %lu); "
                      "see C:\\Windows\\AppSandbox\\backstage.log in the VM", code);
            return;
        }
    }
    CloseHandle(pi.hProcess);
    strcpy_s(reply, cap, "error:the Backstage shell did not start in time "
                         "(see C:\\Windows\\AppSandbox\\backstage.log in the VM)");
}

/* ---- Seamless mode (appsandbox-seamless.exe) ----
   Started on the host's request in the signed-in user's session, and ends by
   itself when the host's connection does. It runs as the user, not as SYSTEM,
   because Windows.Graphics.Capture doesn't work under SYSTEM; with the user's
   full token where UAC split it, so that input reaches elevated windows too. */
static HANDLE g_seamless_process;
static DWORD  g_seamless_session = 0xFFFFFFFF;

static void seamless_start(char *reply, size_t cap)
{
    DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE user_token = NULL, run_token;
    TOKEN_LINKED_TOKEN linked;
    DWORD got = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH], *slash;
    LPVOID env = NULL;

    if (g_seamless_process && WaitForSingleObject(g_seamless_process, 0) != WAIT_OBJECT_0) {
        if (session == g_seamless_session) {
            strcpy_s(reply, cap, "ok:running");
            return;
        }
        TerminateProcess(g_seamless_process, 0);
        WaitForSingleObject(g_seamless_process, 3000);
    }
    if (g_seamless_process) {
        CloseHandle(g_seamless_process);
        g_seamless_process = NULL;
    }

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-seamless.exe");
    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        strcpy_s(reply, cap, "error:appsandbox-seamless.exe is not in the VM yet; restart the VM to add it");
        return;
    }
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &user_token)) {
        strcpy_s(reply, cap, "error:no_user");   /* the sign-in screen; the host shows the display */
        return;
    }
    run_token = user_token;
    linked.LinkedToken = NULL;
    if (GetTokenInformation(user_token, TokenLinkedToken, &linked, sizeof(linked), &got) && linked.LinkedToken) {
        DWORD elevated = 0;
        TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
        if (GetTokenInformation(user_token, TokenElevationType, &type, sizeof(type), &elevated) &&
            type == TokenElevationTypeLimited)
            run_token = linked.LinkedToken;
    }
    if (!CreateEnvironmentBlock(&env, user_token, FALSE)) env = NULL;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessAsUserW(run_token, exe_path, NULL, NULL, NULL, FALSE,
                              CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                              env, NULL, &si, &pi)) {
        sprintf_s(reply, cap, "error:could not start the seamless helper (%lu)", GetLastError());
    } else {
        agent_log("Seamless helper: spawned PID %lu in session %lu%s.", pi.dwProcessId, session,
                  run_token == user_token ? "" : " (full token)");
        g_seamless_process = pi.hProcess;
        g_seamless_session = session;
        CloseHandle(pi.hThread);
        strcpy_s(reply, cap, "ok:started");
    }
    if (env) DestroyEnvironmentBlock(env);
    if (linked.LinkedToken) CloseHandle(linked.LinkedToken);
    CloseHandle(user_token);
}

/* Spawn appsandbox-input.exe as SYSTEM in the given session.
   Duplicates our own token, sets the session ID, then CreateProcessAsUser. */
static BOOL spawn_input_in_session(DWORD session_id)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;

    /* Build path to appsandbox-input.exe (same directory as agent) */
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-input.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Input helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("Input helper: OpenProcessToken failed (%lu).", GetLastError());
        return FALSE;
    }

    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("Input helper: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token);
        return FALSE;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("Input helper: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("Input helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    agent_log("Input helper: spawned PID %lu in session %lu.", pi.dwProcessId, session_id);
    g_input_process = pi.hProcess;
    g_input_session = session_id;
    CloseHandle(pi.hThread);
    CloseHandle(dup_token);
    return TRUE;
}

/* Monitor thread: every 3 seconds, check if the console session changed
   or if the helper process died.  Respawn as needed. */
static DWORD WINAPI input_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Input monitor: started.");

    while (g_input_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            BOOL helper_alive = g_input_process &&
                WaitForSingleObject(g_input_process, 0) != WAIT_OBJECT_0;

            if (cur_session != g_input_session) {
                /* Console session changed — kill old helper and respawn */
                if (helper_alive) {
                    agent_log("Input monitor: console session changed %lu -> %lu, respawning.",
                               g_input_session, cur_session);
                    kill_input_helper();
                }
                spawn_input_in_session(cur_session);
            } else if (!helper_alive) {
                /* Same session but helper died — respawn */
                if (g_input_process) {
                    CloseHandle(g_input_process);
                    g_input_process = NULL;
                }
                agent_log("Input monitor: helper died, respawning in session %lu.", cur_session);
                spawn_input_in_session(cur_session);
            }
        }

        /* Sleep 3 seconds, but check stop flag every 500ms */
        {
            int i;
            for (i = 0; i < 6 && g_input_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_input_helper();
    agent_log("Input monitor: stopped.");
    return 0;
}

static void start_input_monitor(void)
{
    g_input_monitor_running = TRUE;
    g_input_monitor_thread = CreateThread(NULL, 0, input_monitor_thread, NULL, 0, NULL);
}

static void stop_input_monitor(void)
{
    g_input_monitor_running = FALSE;
    if (g_input_monitor_thread) {
        WaitForSingleObject(g_input_monitor_thread, 5000);
        CloseHandle(g_input_monitor_thread);
        g_input_monitor_thread = NULL;
    }
}

/* ---- Clipboard helper process (same pattern as input helper) ---- */

/* Protects the clipboard helper/reader process globals below.  Both the SCM
   control handler thread (SESSIONCHANGE LOGON/UNLOCK) and the clipboard
   monitor threads mutate these globals (kill/spawn plus the test-then-close),
   so without serialization the same HANDLE can be closed twice. */
static CRITICAL_SECTION g_clip_proc_cs;

static HANDLE g_clipboard_process = NULL;
static DWORD  g_clipboard_session = 0xFFFFFFFF;
static volatile BOOL g_clipboard_monitor_running = FALSE;
static HANDLE g_clipboard_monitor_thread = NULL;

static void kill_clipboard_helper(void)
{
    HANDLE proc;

    EnterCriticalSection(&g_clip_proc_cs);
    proc = g_clipboard_process;
    g_clipboard_process = NULL;
    g_clipboard_session = 0xFFFFFFFF;
    LeaveCriticalSection(&g_clip_proc_cs);

    if (proc) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

static BOOL spawn_clipboard_in_session(DWORD session_id)
{
    HANDLE user_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    LPVOID env = NULL;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-clipboard.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Clipboard helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!WTSQueryUserToken(session_id, &user_token)) {
        agent_log("Clipboard helper: WTSQueryUserToken(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        return FALSE;
    }

    if (!CreateEnvironmentBlock(&env, user_token, FALSE)) {
        agent_log("Clipboard helper: CreateEnvironmentBlock failed (%lu).", GetLastError());
        env = NULL;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(user_token, exe_path, NULL, NULL, NULL,
                               FALSE,
                               CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                               env, NULL, &si, &pi)) {
        agent_log("Clipboard helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(user_token);
        return FALSE;
    }

    agent_log("Clipboard helper: spawned PID %lu in session %lu (as user).",
               pi.dwProcessId, session_id);
    EnterCriticalSection(&g_clip_proc_cs);
    g_clipboard_process = pi.hProcess;
    g_clipboard_session = session_id;
    LeaveCriticalSection(&g_clip_proc_cs);
    CloseHandle(pi.hThread);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(user_token);
    return TRUE;
}

static DWORD WINAPI clipboard_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Clipboard monitor: started.");

    while (g_clipboard_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            HANDLE proc_snap;
            DWORD  sess_snap;
            BOOL helper_alive;

            EnterCriticalSection(&g_clip_proc_cs);
            proc_snap = g_clipboard_process;
            sess_snap = g_clipboard_session;
            LeaveCriticalSection(&g_clip_proc_cs);

            helper_alive = proc_snap &&
                WaitForSingleObject(proc_snap, 0) != WAIT_OBJECT_0;

            if (cur_session != sess_snap) {
                if (helper_alive) {
                    agent_log("Clipboard monitor: console session changed %lu -> %lu, respawning.",
                               sess_snap, cur_session);
                    kill_clipboard_helper();
                }
                spawn_clipboard_in_session(cur_session);
            } else if (!helper_alive) {
                HANDLE dead;
                EnterCriticalSection(&g_clip_proc_cs);
                dead = g_clipboard_process;
                g_clipboard_process = NULL;
                LeaveCriticalSection(&g_clip_proc_cs);
                if (dead) CloseHandle(dead);
                agent_log("Clipboard monitor: helper died, respawning in session %lu.", cur_session);
                spawn_clipboard_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_clipboard_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_clipboard_helper();
    agent_log("Clipboard monitor: stopped.");
    return 0;
}

static void start_clipboard_monitor(void)
{
    g_clipboard_monitor_running = TRUE;
    g_clipboard_monitor_thread = CreateThread(NULL, 0, clipboard_monitor_thread, NULL, 0, NULL);
}

static void stop_clipboard_monitor(void)
{
    g_clipboard_monitor_running = FALSE;
    if (g_clipboard_monitor_thread) {
        WaitForSingleObject(g_clipboard_monitor_thread, 5000);
        CloseHandle(g_clipboard_monitor_thread);
        g_clipboard_monitor_thread = NULL;
    }
}

/* ---- Clipboard reader process (runs as USER, :0006) ---- */

static HANDLE g_clipboard_reader_process = NULL;
static DWORD  g_clipboard_reader_session = 0xFFFFFFFF;
static volatile BOOL g_clipboard_reader_monitor_running = FALSE;
static HANDLE g_clipboard_reader_monitor_thread = NULL;

static void kill_clipboard_reader(void)
{
    HANDLE proc;

    EnterCriticalSection(&g_clip_proc_cs);
    proc = g_clipboard_reader_process;
    g_clipboard_reader_process = NULL;
    g_clipboard_reader_session = 0xFFFFFFFF;
    LeaveCriticalSection(&g_clip_proc_cs);

    if (proc) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

static BOOL spawn_clipboard_reader_in_session(DWORD session_id)
{
    HANDLE user_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    LPVOID env = NULL;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-clipboard-reader.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Clipboard reader: %ls not found.", exe_path);
        return FALSE;
    }

    /* Get the logged-in user's token — this runs the process as the user,
       which allows it to read the user's clipboard. */
    if (!WTSQueryUserToken(session_id, &user_token)) {
        agent_log("Clipboard reader: WTSQueryUserToken(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        return FALSE;
    }

    /* Create environment block for the user */
    if (!CreateEnvironmentBlock(&env, user_token, FALSE)) {
        agent_log("Clipboard reader: CreateEnvironmentBlock failed (%lu).", GetLastError());
        env = NULL;  /* proceed without it */
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(user_token, exe_path, NULL, NULL, NULL,
                               FALSE,
                               CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                               env, NULL, &si, &pi)) {
        agent_log("Clipboard reader: CreateProcessAsUserW failed (%lu).", GetLastError());
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(user_token);
        return FALSE;
    }

    agent_log("Clipboard reader: spawned PID %lu in session %lu (as user).",
               pi.dwProcessId, session_id);
    EnterCriticalSection(&g_clip_proc_cs);
    g_clipboard_reader_process = pi.hProcess;
    g_clipboard_reader_session = session_id;
    LeaveCriticalSection(&g_clip_proc_cs);
    CloseHandle(pi.hThread);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(user_token);
    return TRUE;
}

static DWORD WINAPI clipboard_reader_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Clipboard reader monitor: started.");

    while (g_clipboard_reader_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            HANDLE proc_snap;
            DWORD  sess_snap;
            BOOL helper_alive;

            EnterCriticalSection(&g_clip_proc_cs);
            proc_snap = g_clipboard_reader_process;
            sess_snap = g_clipboard_reader_session;
            LeaveCriticalSection(&g_clip_proc_cs);

            helper_alive = proc_snap &&
                WaitForSingleObject(proc_snap, 0) != WAIT_OBJECT_0;

            if (cur_session != sess_snap) {
                if (helper_alive) {
                    agent_log("Clipboard reader monitor: console session changed %lu -> %lu, respawning.",
                               sess_snap, cur_session);
                    kill_clipboard_reader();
                }
                spawn_clipboard_reader_in_session(cur_session);
            } else if (!helper_alive) {
                HANDLE dead;
                EnterCriticalSection(&g_clip_proc_cs);
                dead = g_clipboard_reader_process;
                g_clipboard_reader_process = NULL;
                LeaveCriticalSection(&g_clip_proc_cs);
                if (dead) CloseHandle(dead);
                agent_log("Clipboard reader monitor: helper died, respawning in session %lu.", cur_session);
                spawn_clipboard_reader_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_clipboard_reader_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_clipboard_reader();
    agent_log("Clipboard reader monitor: stopped.");
    return 0;
}

static void start_clipboard_reader_monitor(void)
{
    g_clipboard_reader_monitor_running = TRUE;
    g_clipboard_reader_monitor_thread = CreateThread(NULL, 0, clipboard_reader_monitor_thread, NULL, 0, NULL);
}

static void stop_clipboard_reader_monitor(void)
{
    g_clipboard_reader_monitor_running = FALSE;
    if (g_clipboard_reader_monitor_thread) {
        WaitForSingleObject(g_clipboard_reader_monitor_thread, 5000);
        CloseHandle(g_clipboard_reader_monitor_thread);
        g_clipboard_reader_monitor_thread = NULL;
    }
}

/* ---- Audio capture helper process (SYSTEM in console session, :0004) ---- */

static HANDLE g_audio_process = NULL;
static DWORD  g_audio_session = 0xFFFFFFFF;
static volatile BOOL g_audio_monitor_running = FALSE;
static HANDLE g_audio_monitor_thread = NULL;

static void kill_audio_helper(void)
{
    if (g_audio_process) {
        TerminateProcess(g_audio_process, 0);
        WaitForSingleObject(g_audio_process, 3000);
        CloseHandle(g_audio_process);
        g_audio_process = NULL;
        g_audio_session = 0xFFFFFFFF;
    }
}

static BOOL spawn_audio_in_session(DWORD session_id)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-audio.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Audio helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("Audio helper: OpenProcessToken failed (%lu).", GetLastError());
        return FALSE;
    }

    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("Audio helper: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token);
        return FALSE;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("Audio helper: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("Audio helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    agent_log("Audio helper: spawned PID %lu in session %lu.", pi.dwProcessId, session_id);
    g_audio_process = pi.hProcess;
    g_audio_session = session_id;
    CloseHandle(pi.hThread);
    CloseHandle(dup_token);
    return TRUE;
}

static DWORD WINAPI audio_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Audio monitor: started.");

    while (g_audio_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            BOOL helper_alive = g_audio_process &&
                WaitForSingleObject(g_audio_process, 0) != WAIT_OBJECT_0;

            if (cur_session != g_audio_session) {
                if (helper_alive) {
                    agent_log("Audio monitor: console session changed %lu -> %lu, respawning.",
                               g_audio_session, cur_session);
                    kill_audio_helper();
                }
                spawn_audio_in_session(cur_session);
            } else if (!helper_alive) {
                if (g_audio_process) {
                    CloseHandle(g_audio_process);
                    g_audio_process = NULL;
                }
                agent_log("Audio monitor: helper died, respawning in session %lu.", cur_session);
                spawn_audio_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_audio_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_audio_helper();
    agent_log("Audio monitor: stopped.");
    return 0;
}

static void start_audio_monitor(void)
{
    g_audio_monitor_running = TRUE;
    g_audio_monitor_thread = CreateThread(NULL, 0, audio_monitor_thread, NULL, 0, NULL);
}

static void stop_audio_monitor(void)
{
    g_audio_monitor_running = FALSE;
    if (g_audio_monitor_thread) {
        WaitForSingleObject(g_audio_monitor_thread, 5000);
        CloseHandle(g_audio_monitor_thread);
        g_audio_monitor_thread = NULL;
    }
}

/* ---- IDD driver status check ---- */

/* Check the AppSandboxVDD driver state and report "idd_status:<status>" to the host
   (ok / error / disabled / not_found / unknown), derived from devcon's PnP devnode
   state. Called once at connect (force=1: always send, so a reconnect re-syncs the
   host's latched idd_ready) AND periodically from the device-check loop (force=0:
   send only on change) -- so the host's display-readiness tracks the LIVE VDD state,
   which can self-heal via ensure_vdd_running(). The host re-latches every line, so
   the change-gate avoids spamming the daemon log/events. */
static void report_idd_status(AsbConn *client, int force)
{
    static char last_status[16];   /* last value sent (process-global; force=1 overrides on (re)connect) */
    char output[4096];
    wchar_t cmd[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    DWORD bytes_read;
    int pos = 0;
    const char *st = "not_found";

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = L'\0';
    swprintf_s(cmd, MAX_PATH, L"\"%sdrivers\\devcon.exe\" status Root\\AppSandboxVDD", exe_dir);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        st = "not_found"; goto emit;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("IDD status: devcon failed to launch (%lu).", GetLastError());
        CloseHandle(hRead);
        CloseHandle(hWrite);
        st = "not_found"; goto emit;
    }
    CloseHandle(hWrite);

    while (ReadFile(hRead, output + pos, (DWORD)(sizeof(output) - pos - 1), &bytes_read, NULL) && bytes_read > 0)
        pos += (int)bytes_read;
    output[pos] = '\0';
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (strstr(output, "running")) {
        agent_log("IDD status: AppSandboxVDD running.");
        st = "ok";
    } else if (strstr(output, "problem")) {
        agent_log("IDD status: AppSandboxVDD has a problem. Output: %s", output);
        st = "error";
    } else if (strstr(output, "disabled")) {
        agent_log("IDD status: AppSandboxVDD disabled.");
        st = "disabled";
    } else if (strstr(output, "No matching")) {
        agent_log("IDD status: AppSandboxVDD not found.");
        st = "not_found";
    } else {
        agent_log("IDD status: unknown. Output: %s", output);
        st = "unknown";
    }

emit:
    /* Send only when the value changes (force overrides on connect). */
    if (force || strcmp(st, last_status) != 0) {
        char line[32];
        sprintf_s(line, sizeof(line), "idd_status:%s", st);
        send_line(client, line);
        strcpy_s(last_status, sizeof(last_status), st);
    }
}

/* ---- Helper: run devcon command and capture stdout ---- */

static BOOL run_devcon(const wchar_t *args, char *output, int output_size, DWORD *out_exit_code)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    wchar_t cmd[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    DWORD bytes_read;
    int pos = 0;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = L'\0';

    swprintf_s(cmd, MAX_PATH, L"\"%sdrivers\\devcon.exe\" %s", exe_dir, args);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return FALSE;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return FALSE;
    }
    CloseHandle(hWrite);

    /* Wait for devcon to finish (10s max). If it hangs, kill it. */
    if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) {
        agent_log("run_devcon: process timed out, killing.");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
    }

    /* Now read whatever output is available (process is dead, pipe will EOF) */
    while (ReadFile(hRead, output + pos, (DWORD)(output_size - pos - 1), &bytes_read, NULL) && bytes_read > 0)
        pos += (int)bytes_read;
    output[pos] = '\0';
    CloseHandle(hRead);

    if (out_exit_code) GetExitCodeProcess(pi.hProcess, out_exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

/* ---- VDD device check: restart if not running ---- */

static void ensure_vdd_running(void)
{
    char output[4096];
    DWORD exit_code = 0;

    if (!run_devcon(L"status Root\\AppSandboxVDD", output, sizeof(output), &exit_code)) {
        agent_log("ensure_vdd_running: devcon failed to launch (%lu).", GetLastError());
        return;
    }

    if (strstr(output, "running"))
        return;

    if (strstr(output, "No matching"))
        return; /* not installed yet */

    /* A device in CM_PROB_DISABLED (explicitly disabled) has its CONFIGFLAG_DISABLED bit set;
       `devcon restart` (DICS_PROPCHANGE: stop->start) reports success but CANNOT clear that bit,
       so the device snaps straight back to disabled. Only `devcon enable` (DICS_ENABLE) clears it.
       So: ENABLE a disabled device; RESTART a stuck-but-enabled ("problem") one. */
    if (strstr(output, "disabled")) {
        agent_log("VDD: disabled - enabling.");
        if (!run_devcon(L"enable Root\\AppSandboxVDD", output, sizeof(output), &exit_code)) {
            agent_log("VDD enable: devcon failed to launch (%lu).", GetLastError());
            return;
        }
        agent_log("VDD enable (exit=%lu): %.400s", exit_code, output);
        return;
    }

    if (strstr(output, "problem"))
        agent_log("VDD: problem - restarting.");
    else
        agent_log("VDD: unknown state - restarting.");

    if (!run_devcon(L"restart Root\\AppSandboxVDD", output, sizeof(output), &exit_code)) {
        agent_log("VDD restart: devcon failed to launch (%lu).", GetLastError());
        return;
    }
    agent_log("VDD restart (exit=%lu): %.400s", exit_code, output);
}

/* ---- Driver updates staged by the host ----
   The host refreshes driver files beside the agent while the VM is off and
   leaves drivers\<name>-update.pending. Installing them at service start gets
   driver fixes to VMs created before them. devcon install/update can take
   far longer than run_devcon's 2 s, so these wait up to two minutes. */

static void agent_dir(wchar_t *dir, size_t chars)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, dir, (DWORD)chars);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0';
}

/* devcon <args> with up to two minutes to finish; returns its exit code, or
   (DWORD)-1 when it could not be run. */
static DWORD run_devcon_wait(const wchar_t *args)
{
    wchar_t dir[MAX_PATH], cmd[MAX_PATH * 3];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = (DWORD)-1;

    agent_dir(dir, MAX_PATH);
    swprintf_s(cmd, _countof(cmd), L"\"%s\\drivers\\devcon.exe\" %s", dir, args);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("devcon %ls: failed to launch (%lu).", args, GetLastError());
        return exit_code;
    }
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
        agent_log("devcon %ls: timed out, killing.", args);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exit_code;
}

/* devcon <verb> "<dir>\drivers\<inf>" <hwid> */
static DWORD run_devcon_inf(const wchar_t *verb, const wchar_t *inf, const wchar_t *hwid)
{
    wchar_t dir[MAX_PATH], args[MAX_PATH * 2];
    agent_dir(dir, MAX_PATH);
    swprintf_s(args, _countof(args), L"%s \"%s\\drivers\\%s\" %s", verb, dir, inf, hwid);
    return run_devcon_wait(args);
}

/* "<size>-<last write>" of drivers\<binary>; CopyFile keeps the write time,
   so it changes exactly when the host stages a different build. */
static BOOL staged_driver_stamp(const wchar_t *binary, char *out, size_t chars)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    agent_dir(dir, MAX_PATH);
    swprintf_s(path, MAX_PATH, L"%s\\drivers\\%s", dir, binary);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return FALSE;
    sprintf_s(out, chars, "%lu-%lu-%lu", fa.nFileSizeLow,
              fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
    return TRUE;
}

/* Install drivers\<inf> when the host left drivers\<marker_name>, or when
   the staged binary isn't the build this agent last installed (recorded in
   drivers\<record_name>). The record covers a marker consumed by an attempt
   that didn't take, and VMs from before the record existed. */
static void apply_staged_driver_update(const wchar_t *marker_name, const wchar_t *inf,
                                       const wchar_t *hwid, const wchar_t *binary,
                                       const wchar_t *record_name)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH], record[MAX_PATH];
    char stamp[64] = "", installed[64] = "";
    BOOL pending;
    DWORD exit_code;
    FILE *f;
    agent_dir(dir, MAX_PATH);
    swprintf_s(marker, MAX_PATH, L"%s\\drivers\\%s", dir, marker_name);
    swprintf_s(record, MAX_PATH, L"%s\\drivers\\%s", dir, record_name);
    pending = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
    if (!staged_driver_stamp(binary, stamp, sizeof(stamp))) return;
    if (_wfopen_s(&f, record, L"r") == 0 && f) {
        if (!fgets(installed, sizeof(installed), f)) installed[0] = '\0';
        fclose(f);
    }
    if (!pending && strcmp(stamp, installed) == 0) return;
    /* One attempt per staged build, so a package that won't install can't loop. */
    DeleteFileW(marker);
    if (_wfopen_s(&f, record, L"w") == 0 && f) {
        fputs(stamp, f);
        fclose(f);
    }
    /* The device is already running by service start, so its binary is in
       use and an update only swaps it at the next reboot. Disabling it first
       unloads the binary; the update then applies now, on this boot. */
    swprintf_s(marker, MAX_PATH, L"disable %s", hwid);
    agent_log("Driver update %ls: devcon disable exit=%lu.", inf, run_devcon_wait(marker));
    exit_code = run_devcon_inf(L"update", inf, hwid);
    agent_log("Driver update %ls: devcon update exit=%lu (0 = installed, 1 = after reboot).",
              inf, exit_code);
    swprintf_s(marker, MAX_PATH, L"enable %s", hwid);
    agent_log("Driver update %ls: devcon enable exit=%lu.", inf, run_devcon_wait(marker));
}

static void apply_staged_vdd_update(void)
{
    apply_staged_driver_update(L"vdd-update.pending", L"AppSandboxVDD.inf", L"Root\\AppSandboxVDD",
                               L"AppSandboxVDD.dll", L"vdd-installed.stamp");
    apply_staged_driver_update(L"vad-update.pending", L"AppSandboxVAD.inf", L"Root\\AppSandboxVAD",
                               L"AppSandboxVAD.sys", L"vad-installed.stamp");
}

/* ---- Virtual audio device check ----
   With no AppSandboxVAD the audio helper finds no "App Sandbox Virtual Audio"
   endpoint and the VM has no sound (seen on VMs whose first install of the
   driver failed). Install it from the package beside the agent when it is
   missing, enable it when disabled, restart it when it has a problem. */
static void ensure_vad_installed(void)
{
    wchar_t dir[MAX_PATH], inf[MAX_PATH];
    char output[4096];
    DWORD exit_code = 0;

    if (!run_devcon(L"status Root\\AppSandboxVAD", output, sizeof(output), &exit_code)) {
        agent_log("ensure_vad_installed: devcon failed to launch (%lu).", GetLastError());
        return;
    }
    if (strstr(output, "running")) return;
    if (strstr(output, "No matching")) {
        agent_dir(dir, MAX_PATH);
        swprintf_s(inf, MAX_PATH, L"%s\\drivers\\AppSandboxVAD.inf", dir);
        if (GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES) {
            agent_log("VAD: not installed and no package beside the agent.");
            return;
        }
        agent_log("VAD: not installed - installing.");
        exit_code = run_devcon_inf(L"install", L"AppSandboxVAD.inf", L"Root\\AppSandboxVAD");
        agent_log("VAD install: devcon exit=%lu (0 = installed, 1 = after reboot).", exit_code);
        return;
    }
    if (strstr(output, "disabled")) {
        agent_log("VAD: disabled - enabling.");
        run_devcon(L"enable Root\\AppSandboxVAD", output, sizeof(output), &exit_code);
        agent_log("VAD enable (exit=%lu): %.400s", exit_code, output);
        return;
    }
    agent_log("VAD: %s - restarting.", strstr(output, "problem") ? "problem" : "unknown state");
    run_devcon(L"restart Root\\AppSandboxVAD", output, sizeof(output), &exit_code);
    agent_log("VAD restart (exit=%lu): %.400s", exit_code, output);
}

/* ---- Hyper-V Video device check: disable if running ---- */

#define HYPERV_VIDEO_HWID L"*DA0A7802*"

static void ensure_hyperv_video_disabled(AsbConn *notify)
{
    char output[4096];
    DWORD exit_code = 0;

    if (!run_devcon(L"status " HYPERV_VIDEO_HWID, output, sizeof(output), &exit_code)) {
        agent_log("ensure_hyperv_video_disabled: devcon failed to launch (%lu).", GetLastError());
        return;
    }

    if (strstr(output, "disabled") || strstr(output, "No matching"))
        return;

    if (!strstr(output, "running"))
        return;

    agent_log("Hyper-V Video: running — disabling.");

    if (!run_devcon(L"disable " HYPERV_VIDEO_HWID, output, sizeof(output), &exit_code)) {
        agent_log("Hyper-V Video disable: devcon failed to launch (%lu).", GetLastError());
        return;
    }

    if (notify != NULL)
        send_line(notify, "hyperv_video:disabled");
    agent_log("Hyper-V Video disable (exit=%lu): %.400s", exit_code, output);
}

/* ---- Keep the AppSandbox monitor the only one ----
   With GPU-PV the host GPU's driver can expose a display output of its own;
   Windows then makes that the main monitor and the AppSandbox window shows
   the secondary one. appsandbox-displays.exe --only-appsandbox (run in the
   console session, where display configuration applies) switches the other
   monitors off and makes ours the main one. Cheap when nothing changes, so
   it runs on the device-check cadence and catches a monitor that appears
   later (e.g. after the GPU driver loads). */
static void keep_only_appsandbox_display(void)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH], cmd[MAX_PATH + 32];
    wchar_t *slash;
    DWORD session_id, bytes_read;
    char output[256];
    int total = 0;

    session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF || session_id == 0) return;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-displays.exe");
    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) return;
    swprintf_s(cmd, _countof(cmd), L"\"%s\" --only-appsandbox", exe_path);

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* SYSTEM token placed in the console session, as for the input helper */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token) ||
        !DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                          SecurityImpersonation, TokenPrimary, &dup_token) ||
        !SetTokenInformation(dup_token, TokenSessionId, &session_id, sizeof(session_id))) {
        agent_log("Displays: could not prepare the session token (%lu).", GetLastError());
        if (cur_token) CloseHandle(cur_token);
        if (dup_token) CloseHandle(dup_token);
        CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        return;
    }
    CloseHandle(cur_token);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessAsUserW(dup_token, NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                              NULL, NULL, &si, &pi)) {
        agent_log("Displays: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        return;
    }
    CloseHandle(dup_token);
    CloseHandle(hWritePipe);
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    while (total < (int)sizeof(output) - 1 &&
           ReadFile(hReadPipe, output + total, (DWORD)(sizeof(output) - 1 - total),
                    &bytes_read, NULL) && bytes_read > 0)
        total += (int)bytes_read;
    output[total] = '\0';
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    if (strncmp(output, "applied", 7) == 0 || strncmp(output, "error", 5) == 0) {
        agent_log("Displays: %s", output);
        if (strncmp(output, "applied", 7) == 0)
            agent_log_to_host("Switched off an extra monitor in the VM so the AppSandbox "
                              "display is the only one (%s).", output);
    }
}

/* ---- IDD connect: respawn helpers in console session ---- */

/* Send a response, prepending the sequence tag if present */
static void send_reply(AsbConn *s, const char *tag, const char *msg)
{
    if (tag[0]) {
        /* Sized to the reply: domain_list returns kilobytes of JSON. */
        size_t size = strlen(tag) + strlen(msg) + 1;
        char *rb = (char *)malloc(size);
        if (!rb) return;
        sprintf_s(rb, size, "%s%s", tag, msg);
        send_line(s, rb);
        free(rb);
    } else {
        send_line(s, msg);
    }
}

static void handle_idd_connect(AsbConn *client, const char *tag)
{
    DWORD new_console = WTSGetActiveConsoleSessionId();

    /* Kill and respawn input helper in the console session */
    kill_input_helper();
    agent_log("idd_connect: respawning input helper in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_input_in_session(new_console);

    /* Kill and respawn clipboard helper (SYSTEM, :0005) in the console session */
    kill_clipboard_helper();
    agent_log("idd_connect: respawning clipboard helper in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_clipboard_in_session(new_console);

    /* Kill and respawn clipboard reader (user, :0006) in the console session */
    kill_clipboard_reader();
    agent_log("idd_connect: respawning clipboard reader in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_clipboard_reader_in_session(new_console);

    send_reply(client, tag, "ok");
}

static int write_agent_script(const wchar_t *path, const char *contents)
{
    HANDLE file; DWORD written; size_t bytes = strlen(contents);
    CreateDirectoryW(L"C:\\ProgramData\\AppSandbox", NULL);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_HIDDEN, NULL);
    if (file == INVALID_HANDLE_VALUE) return (int)GetLastError();
    if (!WriteFile(file, contents, (DWORD)bytes, &written, NULL) || written != bytes ||
        !FlushFileBuffers(file)) {
        DWORD err = GetLastError(); CloseHandle(file); return (int)err;
    }
    CloseHandle(file); return ERROR_SUCCESS;
}

static int run_powershell_capture(const wchar_t *script, DWORD timeout_ms,
                                  char *output, int output_size)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; wchar_t cmd[8192]; DWORD ec, bytes;
    SECURITY_ATTRIBUTES sa;
    HANDLE read_end = NULL, write_end = NULL;
    int pos = 0;

    if (output && output_size > 0) output[0] = '\0';
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return (int)GetLastError();
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); ZeroMemory(&pi, sizeof(pi));
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = NULL;
    swprintf_s(cmd, _countof(cmd),
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"%s\"",
        script);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        ec = GetLastError();
        CloseHandle(read_end); CloseHandle(write_end);
        return (int)ec;
    }
    CloseHandle(write_end);
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(pi.hProcess, 5000);
        ec = ERROR_TIMEOUT;
    } else if (!GetExitCodeProcess(pi.hProcess, &ec)) {
        ec = GetLastError();
    }
    if (output && output_size > 1) {
        while (pos < output_size - 1 &&
               ReadFile(read_end, output + pos, (DWORD)(output_size - pos - 1),
                        &bytes, NULL) && bytes > 0)
            pos += (int)bytes;
        output[pos] = '\0';
    }
    CloseHandle(read_end);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)ec;
}

static void get_agent_powershell_directory(wchar_t *directory, size_t directory_count)
{
    DWORD length;
    if (!directory || directory_count < 2) return;
    length = GetSystemDirectoryW(directory, (UINT)directory_count);
    if (!length || length >= directory_count)
        wcscpy_s(directory, directory_count, L"C:\\Windows\\System32");
}

static int run_agent_powershell_out(const wchar_t *script_path, DWORD timeout_ms,
                                    char *output, int output_size)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; wchar_t cmd[1024];
    wchar_t system_directory[MAX_PATH]; DWORD ec, bytes;
    SECURITY_ATTRIBUTES sa;
    HANDLE read_end = NULL, write_end = NULL;
    int pos = 0;

    if (output && output_size > 0) output[0] = '\0';
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return (int)GetLastError();
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); ZeroMemory(&pi, sizeof(pi));
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = NULL;
    get_agent_powershell_directory(system_directory, _countof(system_directory));
    swprintf_s(cmd, _countof(cmd),
        L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\"",
        script_path);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, system_directory, &si, &pi)) {
        ec = GetLastError();
        CloseHandle(read_end); CloseHandle(write_end);
        return (int)ec;
    }
    CloseHandle(write_end);
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(pi.hProcess, 5000);
        ec = ERROR_TIMEOUT;
    } else if (!GetExitCodeProcess(pi.hProcess, &ec)) {
        ec = GetLastError();
    }
    if (output && output_size > 1) {
        while (pos < output_size - 1 &&
               ReadFile(read_end, output + pos, (DWORD)(output_size - pos - 1),
                        &bytes, NULL) && bytes > 0)
            pos += (int)bytes;
        output[pos] = '\0';
    }
    CloseHandle(read_end);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)ec;
}

static int run_agent_powershell(const wchar_t *script_path, DWORD timeout_ms)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; wchar_t cmd[1024];
    wchar_t system_directory[MAX_PATH]; DWORD ec;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); ZeroMemory(&pi, sizeof(pi));
    get_agent_powershell_directory(system_directory, _countof(system_directory));
    swprintf_s(cmd, _countof(cmd),
        L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\"",
        script_path);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, system_directory, &si, &pi))
        return (int)GetLastError();
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT); ec = ERROR_TIMEOUT;
    } else if (!GetExitCodeProcess(pi.hProcess, &ec)) ec = GetLastError();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return (int)ec;
}

/* ---- AppSandbox domain ----
   On the Server Core appliance: promote it to the domain controller and
   manage its users and groups. On a managed client VM: join the domain.
   Arguments arrive ':'-separated (passwords and group names base64) and
   reach the scripts only through ASB_DOM_* environment variables. Each
   script prints "result:<text>" (or "json:<text>" for the list). */
#define DOMAIN_OUT_MAX 32768

static BOOL domain_field_ok(const char *v, const char *extra, size_t max_len)
{
    size_t n = strlen(v), i;
    if (n > max_len) return FALSE;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)v[i]) && !strchr(extra, v[i])) return FALSE;
    return TRUE;
}

/* Share names (asb_<hex>$): letters, digits, '_' and '$'. */
static BOOL domain_field_ok_n(const char *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)v[i]) && v[i] != '_' && v[i] != '$') return FALSE;
    return n > 0;
}

/* Split "a:b::c" in place, keeping empty fields. */
static int domain_split(char *s, char **fields, int max)
{
    int n = 0;
    fields[n++] = s;
    for (; *s && n < max; s++)
        if (*s == ':') { *s = '\0'; fields[n++] = s + 1; }
    return n;
}

static const char DOMAIN_PROMOTE_PS[] =
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " if((Get-CimInstance Win32_ComputerSystem).DomainRole -ge 4){ Write-Output 'result:already'; exit 0 }\r\n"
        " $pw=AsbText $env:ASB_DOM_PW\r\n"
        " Install-WindowsFeature AD-Domain-Services -IncludeManagementTools | Out-Null\r\n"
        " # The local policy may require complex passwords, which the appliance password need not\r\n"
        " # meet; the domain drops these rules anyway. Relax them first so the Administrator and\r\n"
        " # recovery (DSRM) passwords below are accepted.\r\n"
        " $inf=Join-Path $env:TEMP 'asb-secpol.inf'; $sdb=Join-Path $env:TEMP 'asb-secpol.sdb'\r\n"
        " secedit /export /cfg $inf /areas SECURITYPOLICY | Out-Null\r\n"
        " (Get-Content $inf) -replace '^PasswordComplexity\\s*=.*','PasswordComplexity = 0' -replace '^MinimumPasswordLength\\s*=.*','MinimumPasswordLength = 0' -replace '^PasswordHistorySize\\s*=.*','PasswordHistorySize = 0' | Set-Content $inf -Encoding Unicode\r\n"
        " secedit /configure /db $sdb /cfg $inf /areas SECURITYPOLICY /quiet | Out-Null\r\n"
        " Remove-Item $inf,$sdb -Force -ErrorAction SilentlyContinue\r\n"
        " # The built-in Administrator becomes the domain's; it needs a password and to be enabled.\r\n"
        " $adm=Get-CimInstance Win32_UserAccount -Filter 'LocalAccount=True' | Where-Object { $_.SID -like '*-500' } | Select-Object -First 1\r\n"
        " $a=[ADSI]('WinNT://'+$env:COMPUTERNAME+'/'+$adm.Name+',user'); $a.SetPassword($pw); $a.UserFlags.Value=([int]$a.UserFlags.Value -band (-bnot 0x2)); $a.SetInfo()\r\n"
        " Import-Module ADDSDeployment\r\n"
        " $s=ConvertTo-SecureString $pw -AsPlainText -Force\r\n"
        " Install-ADDSForest -DomainName $env:ASB_DOM_FQDN -DomainNetbiosName $env:ASB_DOM_NB -SafeModeAdministratorPassword $s -InstallDns -NoRebootOnCompletion -Force -WarningAction SilentlyContinue | Out-Null\r\n"
        " Write-Output 'result:promoted'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_FINISH_PS[] =
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " $pw=AsbText $env:ASB_DOM_PW\r\n"
        " $d=$null\r\n"
        " for($i=0;$i -lt 90 -and -not $d;$i++){ try{ $d=Get-ADDomain }catch{ Start-Sleep -Seconds 10 } }\r\n"
        " if(-not $d){ throw 'Active Directory did not start.' }\r\n"
        " # Any password is acceptable: no complexity, length, history, expiry or lockout.\r\n"
        " Set-ADDefaultDomainPasswordPolicy -Identity $d.DistinguishedName -ComplexityEnabled $false -MinPasswordLength 0 -PasswordHistoryCount 0 -MinPasswordAge 0 -MaxPasswordAge 0 -LockoutThreshold 0\r\n"
        " $n=$env:ASB_DOM_USER\r\n"
        " $sec=ConvertTo-SecureString $pw -AsPlainText -Force\r\n"
        " $u=Get-ADUser -Filter 'SamAccountName -eq $n'\r\n"
        " if(-not $u){ New-ADUser -Name $n -SamAccountName $n -UserPrincipalName ($n+'@'+$d.DNSRoot) -AccountPassword $sec -Enabled $true -PasswordNeverExpires $true }\r\n"
        " else{ Set-ADAccountPassword -Identity $u -Reset -NewPassword $sec; Enable-ADAccount -Identity $u }\r\n"
        " Add-ADGroupMember -Identity 'Domain Admins' -Members $n\r\n"
        " if(-not (Get-ADGroup -Filter \"Name -eq 'AppSandbox VM Admins'\" -ErrorAction SilentlyContinue)){ New-ADGroup -Name 'AppSandbox VM Admins' -SamAccountName 'AppSandbox VM Admins' -GroupScope Global -GroupCategory Security -Description 'Administrators on every managed AppSandbox VM' }\r\n"
        " # Clients reach DNS, Kerberos, LDAP and the rest only over the private share network.\r\n"
        " $net=($env:ASB_MANAGEMENT_HOST -replace '\\.\\d+$','.0')+'/24'\r\n"
        " Get-NetFirewallRule -DisplayName 'AppSandbox Domain' -ErrorAction SilentlyContinue | Remove-NetFirewallRule\r\n"
        " New-NetFirewallRule -DisplayName 'AppSandbox Domain' -Direction Inbound -Action Allow -RemoteAddress $net -Profile Any | Out-Null\r\n"
        " Write-Output 'result:ready'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_LIST_PS[] =
        "function AsbRid($g){ [int]($g.SID.Value.Split('-')[-1]) }\r\n"
        "function AsbManageable($g){ (AsbRid $g) -ge 1000 -or $g.Name -eq 'Domain Admins' }\r\n"
        "function AsbSystemGroup($g){ (AsbRid $g) -lt 1000 -or @('DnsAdmins','DnsUpdateProxy','AppSandbox VM Admins') -contains $g.Name }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " for($i=0;$i -lt 18;$i++){ try{ $null=Get-ADDomain; break }catch{ if($i -eq 0){ Start-Service ADWS -ErrorAction SilentlyContinue }; if($i -eq 17){ throw 'Active Directory is still starting on the appliance; try again in a minute.' }; Start-Sleep -Seconds 5 } }\r\n"
        " if(-not (Get-ADGroup -Filter \"Name -eq 'AppSandbox VM Admins'\" -ErrorAction SilentlyContinue)){ New-ADGroup -Name 'AppSandbox VM Admins' -SamAccountName 'AppSandbox VM Admins' -GroupScope Global -GroupCategory Security -Description 'Administrators on every managed AppSandbox VM' }\r\n"
        " $hide=@('krbtgt','Guest','DefaultAccount','AppSandboxShare')\r\n"
        " $users=@(Get-ADUser -Filter * -Properties MemberOf | Where-Object { $hide -notcontains $_.SamAccountName } | Sort-Object SamAccountName | ForEach-Object { [ordered]@{ name=$_.SamAccountName; enabled=[bool]$_.Enabled; groups=@($_.MemberOf | ForEach-Object { (($_ -split ',')[0]).Substring(3) }) } })\r\n"
        " $groups=@(Get-ADGroup -Filter * | Where-Object { AsbManageable $_ } | Sort-Object Name | ForEach-Object { [ordered]@{ name=$_.Name; system=[bool](AsbSystemGroup $_) } })\r\n"
        " Write-Output ('json:' + (ConvertTo-Json -InputObject ([ordered]@{ users=$users; groups=$groups }) -Depth 4 -Compress))\r\n"
        " exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_USER_SET_PS[] =
        "function AsbHome($sam){\r\n"
        " $d=Get-ADDomain\r\n"
        " $root='C:\\AppSandboxData\\Users'\r\n"
        " New-Item -ItemType Directory -Force -Path $root | Out-Null\r\n"
        " $h=Join-Path $root $sam\r\n"
        " foreach($f in 'Desktop','Documents','Downloads','Pictures'){ New-Item -ItemType Directory -Force -Path (Join-Path $h $f) | Out-Null }\r\n"
        " $acl=New-Object System.Security.AccessControl.DirectorySecurity\r\n"
        " $acl.SetAccessRuleProtection($true,$false)\r\n"
        " foreach($who in @('NT AUTHORITY\\SYSTEM','BUILTIN\\Administrators',($d.NetBIOSName+'\\'+$sam))){\r\n"
        "  $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($who,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))\r\n"
        " }\r\n"
        " Set-Acl -LiteralPath $h -AclObject $acl\r\n"
        " $unc='\\\\'+[System.Net.Dns]::GetHostName()+'.'+$d.DNSRoot+'\\Users$\\'+$sam\r\n"
        " Set-ADUser -Identity $sam -HomeDirectory $unc -HomeDrive 'H:'\r\n"
        "}\r\n"
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "function AsbRid($g){ [int]($g.SID.Value.Split('-')[-1]) }\r\n"
        "function AsbManageable($g){ (AsbRid $g) -ge 1000 -or $g.Name -eq 'Domain Admins' }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " for($i=0;$i -lt 18;$i++){ try{ $null=Get-ADDomain; break }catch{ if($i -eq 0){ Start-Service ADWS -ErrorAction SilentlyContinue }; if($i -eq 17){ throw 'Active Directory is still starting on the appliance; try again in a minute.' }; Start-Sleep -Seconds 5 } }\r\n"
        " $n=$env:ASB_DOM_USER\r\n"
        " $d=Get-ADDomain\r\n"
        " $sec=$null\r\n"
        " if($env:ASB_DOM_PWSET -eq '1'){ $pw=AsbText $env:ASB_DOM_PW; if($pw.Length){ $sec=ConvertTo-SecureString $pw -AsPlainText -Force } else { $sec=New-Object Security.SecureString } }\r\n"
        " $u=Get-ADUser -Filter 'SamAccountName -eq $n'\r\n"
        " if(-not $u){\r\n"
        "  if($null -eq $sec){ throw 'A new user needs a password (it can be empty).' }\r\n"
        "  New-ADUser -Name $n -SamAccountName $n -UserPrincipalName ($n+'@'+$d.DNSRoot) -AccountPassword $sec -Enabled $true -PasswordNeverExpires $true\r\n"
        "  $u=Get-ADUser -Filter 'SamAccountName -eq $n'\r\n"
        " } elseif($null -ne $sec){ Set-ADAccountPassword -Identity $u -Reset -NewPassword $sec }\r\n"
        " if($env:ASB_DOM_ENABLED -eq '1'){ Enable-ADAccount -Identity $u } else { Disable-ADAccount -Identity $u }\r\n"
        " $want=@((AsbText $env:ASB_DOM_GROUPS) -split ',' | Where-Object { $_ })\r\n"
        " $have=@((Get-ADUser -Identity $u -Properties MemberOf).MemberOf | ForEach-Object { (($_ -split ',')[0]).Substring(3) })\r\n"
        " foreach($g in @(Get-ADGroup -Filter * | Where-Object { AsbManageable $_ })){\r\n"
        "  $in=$have -contains $g.Name; $should=$want -contains $g.Name\r\n"
        "  if($should -and -not $in){ Add-ADGroupMember -Identity $g -Members $u }\r\n"
        "  elseif($in -and -not $should){ Remove-ADGroupMember -Identity $g -Members $u -Confirm:$false }\r\n"
        " }\r\n"
        " # Their home folder (H:) with the roaming Desktop, Documents, Downloads and Pictures.\r\n"
        " try{ AsbHome $n }catch{ Write-Output ('result:Saved, but the home folder failed: ' + $_.Exception.Message); exit 0 }\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_USER_DELETE_PS[] =
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " for($i=0;$i -lt 18;$i++){ try{ $null=Get-ADDomain; break }catch{ if($i -eq 0){ Start-Service ADWS -ErrorAction SilentlyContinue }; if($i -eq 17){ throw 'Active Directory is still starting on the appliance; try again in a minute.' }; Start-Sleep -Seconds 5 } }\r\n"
        " Remove-ADUser -Identity $env:ASB_DOM_USER -Confirm:$false\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_GROUP_ADD_PS[] =
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " for($i=0;$i -lt 18;$i++){ try{ $null=Get-ADDomain; break }catch{ if($i -eq 0){ Start-Service ADWS -ErrorAction SilentlyContinue }; if($i -eq 17){ throw 'Active Directory is still starting on the appliance; try again in a minute.' }; Start-Sleep -Seconds 5 } }\r\n"
        " $g=AsbText $env:ASB_DOM_GROUP\r\n"
        " New-ADGroup -Name $g -SamAccountName $g -GroupScope Global -GroupCategory Security\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_GROUP_DELETE_PS[] =
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "function AsbRid($g){ [int]($g.SID.Value.Split('-')[-1]) }\r\n"
        "function AsbSystemGroup($g){ (AsbRid $g) -lt 1000 -or @('DnsAdmins','DnsUpdateProxy','AppSandbox VM Admins') -contains $g.Name }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " for($i=0;$i -lt 18;$i++){ try{ $null=Get-ADDomain; break }catch{ if($i -eq 0){ Start-Service ADWS -ErrorAction SilentlyContinue }; if($i -eq 17){ throw 'Active Directory is still starting on the appliance; try again in a minute.' }; Start-Sleep -Seconds 5 } }\r\n"
        " $g=Get-ADGroup -Identity (AsbText $env:ASB_DOM_GROUP)\r\n"
        " if(AsbSystemGroup $g){ throw 'Built-in groups cannot be deleted.' }\r\n"
        " Remove-ADGroup -Identity $g -Confirm:$false\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";
static const char DOMAIN_JOIN_PS[] =
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " $cs=Get-CimInstance Win32_ComputerSystem\r\n"
        " $dc=$env:ASB_DOM_DC\r\n"
        " $prefix=($dc -replace '\\.\\d+$','.')\r\n"
        " $a=Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress.StartsWith($prefix) } | Select-Object -First 1\r\n"
        " if(-not $a){ throw 'This VM has no network link to the domain controller.' }\r\n"
        " # The share adapter's DNS is reset on every boot; point it, and the domain's names, at the DC.\r\n"
        " Set-DnsClientServerAddress -InterfaceIndex $a.InterfaceIndex -ServerAddresses $dc\r\n"
        " Get-DnsClientNrptRule | Where-Object { $_.Namespace -contains $env:ASB_DOM_FQDN } | ForEach-Object { Remove-DnsClientNrptRule -Name $_.Name -Force }\r\n"
        " Add-DnsClientNrptRule -Namespace ('.'+$env:ASB_DOM_FQDN),$env:ASB_DOM_FQDN -NameServers $dc | Out-Null\r\n"
        " Clear-DnsClientCache\r\n"
        " # Members of the domain's VM Admins group administer every managed VM. Re-applied on\r\n"
        " # every boot, so VMs joined before the group existed pick it up too.\r\n"
        " if($cs.PartOfDomain -and $cs.Domain -eq $env:ASB_DOM_FQDN){\r\n"
        "  try{ $g=[ADSI]('WinNT://'+$env:COMPUTERNAME+'/Administrators,group'); $g.Add('WinNT://'+$env:ASB_DOM_NB+'/AppSandbox VM Admins') }catch{}\r\n"
        "  Write-Output 'result:already'; exit 0\r\n"
        " }\r\n"
        " $sku=(Get-CimInstance Win32_OperatingSystem).OperatingSystemSKU\r\n"
        " if(@(98,99,100,101) -contains $sku){ throw ('This VM runs ' + (Get-CimInstance Win32_OperatingSystem).Caption + ', which cannot join a domain. Upgrade it to Pro (Settings > System > Activation > Change product key), or create a new VM: new VMs install Pro.') }\r\n"
        " $cred=New-Object System.Management.Automation.PSCredential($env:ASB_DOM_USER,(ConvertTo-SecureString (AsbText $env:ASB_DOM_PW) -AsPlainText -Force))\r\n"
        " $last='not attempted'\r\n"
        " for($i=0;$i -lt 12;$i++){ try{ Add-Computer -DomainName $env:ASB_DOM_FQDN -Credential $cred -Force -ErrorAction Stop; $last=$null; break }catch{ $last=$_.Exception.Message; Start-Sleep -Seconds 10 } }\r\n"
        " if($last){ throw $last }\r\n"
        " # Members of VM Admins administer this VM (applied again after the restart, at the next boot).\r\n"
        " try{ $g=[ADSI]('WinNT://'+$env:COMPUTERNAME+'/Administrators,group'); $g.Add('WinNT://'+$env:ASB_DOM_NB+'/AppSandbox VM Admins') }catch{}\r\n"
        " Write-Output 'result:joined'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";

static const char DOMAIN_ENSURE_PS[] =
        "function AsbHome($sam){\r\n"
        " $d=Get-ADDomain\r\n"
        " $root='C:\\AppSandboxData\\Users'\r\n"
        " New-Item -ItemType Directory -Force -Path $root | Out-Null\r\n"
        " $h=Join-Path $root $sam\r\n"
        " foreach($f in 'Desktop','Documents','Downloads','Pictures'){ New-Item -ItemType Directory -Force -Path (Join-Path $h $f) | Out-Null }\r\n"
        " $acl=New-Object System.Security.AccessControl.DirectorySecurity\r\n"
        " $acl.SetAccessRuleProtection($true,$false)\r\n"
        " foreach($who in @('NT AUTHORITY\\SYSTEM','BUILTIN\\Administrators',($d.NetBIOSName+'\\'+$sam))){\r\n"
        "  $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($who,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))\r\n"
        " }\r\n"
        " Set-Acl -LiteralPath $h -AclObject $acl\r\n"
        " $unc='\\\\'+[System.Net.Dns]::GetHostName()+'.'+$d.DNSRoot+'\\Users$\\'+$sam\r\n"
        " Set-ADUser -Identity $sam -HomeDirectory $unc -HomeDrive 'H:'\r\n"
        "}\r\n"
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " for($i=0;$i -lt 18;$i++){ try{ $null=Get-ADDomain; break }catch{ if($i -eq 0){ Start-Service ADWS -ErrorAction SilentlyContinue }; if($i -eq 17){ throw 'Active Directory is still starting on the appliance; try again in a minute.' }; Start-Sleep -Seconds 5 } }\r\n"
        " $d=Get-ADDomain\r\n"
        " if(-not (Get-ADGroup -Filter \"Name -eq 'AppSandbox VM Admins'\" -ErrorAction SilentlyContinue)){ New-ADGroup -Name 'AppSandbox VM Admins' -SamAccountName 'AppSandbox VM Admins' -GroupScope Global -GroupCategory Security -Description 'Administrators on every managed AppSandbox VM' }\r\n"
        " # Users$: one home folder per user. Users may only list the top level; each home is theirs.\r\n"
        " $root='C:\\AppSandboxData\\Users'\r\n"
        " New-Item -ItemType Directory -Force -Path $root | Out-Null\r\n"
        " $acl=New-Object System.Security.AccessControl.DirectorySecurity\r\n"
        " $acl.SetAccessRuleProtection($true,$false)\r\n"
        " $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule('NT AUTHORITY\\SYSTEM','FullControl','ContainerInherit,ObjectInherit','None','Allow')))\r\n"
        " $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule('BUILTIN\\Administrators','FullControl','ContainerInherit,ObjectInherit','None','Allow')))\r\n"
        " $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(($d.NetBIOSName+'\\Domain Users'),'ReadAndExecute','None','None','Allow')))\r\n"
        " Set-Acl -LiteralPath $root -AclObject $acl\r\n"
        " if(-not (Get-SmbShare -Name 'Users$' -ErrorAction SilentlyContinue)){ New-SmbShare -Name 'Users$' -Path $root -FullAccess ($d.NetBIOSName+'\\Domain Users'),'BUILTIN\\Administrators' -FolderEnumerationMode AccessBased | Out-Null }\r\n"
        " # Roaming files: Desktop, Documents, Downloads and Pictures live in the user's home (H:).\r\n"
        " if(-not (Get-Module -ListAvailable GroupPolicy)){ Install-WindowsFeature GPMC | Out-Null }\r\n"
        " Import-Module GroupPolicy\r\n"
        " $name='AppSandbox Roaming Files'\r\n"
        " if(-not (Get-GPO -Name $name -ErrorAction SilentlyContinue)){ New-GPO -Name $name -Comment 'Desktop, Documents, Downloads and Pictures on the AppSandbox appliance (H:)' | Out-Null }\r\n"
        " if(-not ((Get-GPInheritance -Target $d.DistinguishedName).GpoLinks | Where-Object { $_.DisplayName -eq $name })){ New-GPLink -Name $name -Target $d.DistinguishedName | Out-Null }\r\n"
        " $k='HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders'\r\n"
        " $map=[ordered]@{ 'Desktop'='Desktop'; '{754AC886-DF64-4CBA-86B5-F7FBF4FBCEF5}'='Desktop'; 'Personal'='Documents'; '{F42EE2D3-909F-4907-8871-4C22FC0BF756}'='Documents'; '{374DE290-123F-4565-9164-39C4925E467B}'='Downloads'; 'My Pictures'='Pictures'; '{0DDD015D-B06C-45D5-8C4C-F59713854639}'='Pictures' }\r\n"
        " foreach($v in $map.Keys){\r\n"
        "  $want='%HOMESHARE%\\'+$map[$v]\r\n"
        "  $cur=Get-GPRegistryValue -Name $name -Key $k -ValueName $v -ErrorAction SilentlyContinue\r\n"
        "  if(-not $cur -or $cur.Value -ne $want){ Set-GPRegistryValue -Name $name -Key $k -ValueName $v -Type ExpandString -Value $want | Out-Null }\r\n"
        " }\r\n"
        " # Apply user policy before the desktop appears, so the first sign-in is redirected too.\r\n"
        " Set-GPRegistryValue -Name $name -Key 'HKLM\\Software\\Policies\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon' -ValueName 'SyncForegroundPolicy' -Type DWord -Value 1 | Out-Null\r\n"
        " # Disposable VMs roll back to an old machine password; keep it so their trust survives.\r\n"
        " Set-GPRegistryValue -Name $name -Key 'HKLM\\SYSTEM\\CurrentControlSet\\Services\\Netlogon\\Parameters' -ValueName 'DisablePasswordChange' -Type DWord -Value 1 | Out-Null\r\n"
        " $hide=@('krbtgt','Guest','DefaultAccount','AppSandboxShare')\r\n"
        " foreach($u in @(Get-ADUser -Filter * | Where-Object { $hide -notcontains $_.SamAccountName })){ AsbHome $u.SamAccountName }\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + $_.Exception.Message); exit 1 }\r\n";

static const char *const DOMAIN_ENV[] = {
    "ASB_DOM_NB", "ASB_DOM_FQDN", "ASB_DOM_PW", "ASB_DOM_USER", "ASB_DOM_PWSET",
    "ASB_DOM_ENABLED", "ASB_DOM_GROUPS", "ASB_DOM_GROUP", "ASB_DOM_DC",
    "ASB_MANAGEMENT_HOST", NULL
};

/* Run a domain script; reply gets "ok", "ok:<data>" or "error:<message>". */
static void run_domain_script(const char *script, DWORD timeout_ms,
                              char *reply, size_t reply_size)
{
    wchar_t path[MAX_PATH];
    char *out = (char *)malloc(DOMAIN_OUT_MAX), *line, *next, *result = NULL, *json = NULL;
    int ec, i;
    if (!out) { strcpy_s(reply, reply_size, "error:out of memory"); return; }
    out[0] = '\0';
    swprintf_s(path, MAX_PATH, L"C:\\ProgramData\\AppSandbox\\domain-%lu.ps1", GetCurrentThreadId());
    ec = write_agent_script(path, script);
    if (!ec) ec = run_agent_powershell_out(path, timeout_ms, out, DOMAIN_OUT_MAX);
    DeleteFileW(path);
    for (i = 0; DOMAIN_ENV[i]; i++) SetEnvironmentVariableA(DOMAIN_ENV[i], NULL);
    for (line = out; line && *line; line = next) {
        next = strpbrk(line, "\r\n");
        if (next) { *next++ = '\0'; while (*next == '\r' || *next == '\n') next++; }
        if (strncmp(line, "result:", 7) == 0) result = line + 7;
        else if (strncmp(line, "json:", 5) == 0) json = line + 5;
    }
    if (ec == 0 && json) sprintf_s(reply, reply_size, "ok:%s", json);
    else if (ec == 0 && result && (strcmp(result, "ok") == 0 || strcmp(result, "ready") == 0))
        strcpy_s(reply, reply_size, "ok");
    else if (ec == 0 && result) sprintf_s(reply, reply_size, "ok:%s", result);
    else sprintf_s(reply, reply_size, "error:%.400s", result ? result : "the script failed to run");
    agent_log("Domain: %.200s", strncmp(reply, "ok:{", 4) == 0 ? "list" : reply);
    SecureZeroMemory(out, DOMAIN_OUT_MAX);
    free(out);
}

/* ---- Streaming (Sunshine) ----

   Sunshine is GPL-3, so App Sandbox doesn't ship it: when the host turns the option
   on, the official release is downloaded into the VM and checked against the hash
   pinned here before it runs. Everything runs as SYSTEM.

   stream_setup:<port>:<auto|software>:<allowed client>:<name> queues a background
   setup and says "ok"; stream_status reports progress and which encoders Sunshine
   found. The host sends setup at every agent start, so a port change or a new GPU
   driver takes effect and an install cut short by a reboot finishes. The web UI
   stays on localhost with a random password that only these scripts use. */
#define STREAM_SUNSHINE_VER  "2026.914.233613"
#define STREAM_SUNSHINE_URL  "https://github.com/LizardByte/Sunshine/releases/download/v2026.914.233613/Sunshine-Windows-AMD64-installer.msi"
#define STREAM_SUNSHINE_SHA  "1D7FED8BEECD5889DC7FF14CF9F42D6D38F37C3066C13C6C2A5F4E91847E0CCF"
#define STREAM_VIGEM_URL     "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe"
#define STREAM_VIGEM_SHA     "89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A"
#define STREAM_SETUP         L"C:\\ProgramData\\AppSandbox\\stream-setup.ps1"

static const char stream_common_ps[] =
    "$ErrorActionPreference='Stop'\r\n"
    "$ProgressPreference='SilentlyContinue'\r\n"
    "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12\r\n"
    "$dir='C:\\ProgramData\\AppSandbox\\stream'\r\n"
    "$root=Join-Path $env:ProgramFiles 'Sunshine'\r\n"
    "$exe=Join-Path $root 'sunshine.exe'\r\n"
    "$cfg=Join-Path $root 'config\\sunshine.conf'\r\n"
    "$creds=Join-Path $dir 'webui.txt'\r\n"
    "$statusFile=Join-Path $dir 'status.txt'\r\n"
    "function WebUi($path,$body){\r\n"
    /* Compiled: a script block has no runspace on the thread that does the handshake. */
    " if(-not ('AsbTrust' -as [type])){ Add-Type 'public static class AsbTrust{public static void On(){System.Net.ServicePointManager.ServerCertificateValidationCallback=delegate{return true;};}}' }\r\n"
    " [AsbTrust]::On()\r\n"
    " $p=(Get-Content $creds -Raw).Trim()\r\n"
    " $auth=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('asb:'+$p))\r\n"
    " $port=[int]((Select-String -Path $cfg -Pattern '^port = (\\d+)').Matches[0].Groups[1].Value)+1\r\n"
    " $a=@{Uri=\"https://localhost:$port$path\";Headers=@{Authorization=\"Basic $auth\"};UseBasicParsing=$true;TimeoutSec=20}\r\n"
    " if($null -ne $body){ $a.Method='Post'; $a.ContentType='application/json'; $a.Body=($body|ConvertTo-Json -Compress) }\r\n"
    " Invoke-RestMethod @a\r\n"
    "}\r\n";

static const char stream_setup_ps[] =
    "New-Item -ItemType Directory -Force $dir | Out-Null\r\n"
    "& icacls $dir /inheritance:r /grant:r 'SYSTEM:(OI)(CI)F' 'BUILTIN\\Administrators:(OI)(CI)F' | Out-Null\r\n"
    /* stream_status can be reading it at that moment. */
    "function Status($phase,$detail){\r\n"
    " for($i=0;;$i++){ try { Set-Content -Path $statusFile -Value ($phase+'|'+$detail) -Encoding ascii; return } catch { if($i -ge 40){ throw }; Start-Sleep -Milliseconds 250 } }\r\n"
    "}\r\n"
    "function Fetch($url,$sha,$file){\r\n"
    " $f=Join-Path $dir $file\r\n"
    " if(-not ((Test-Path $f) -and (Get-FileHash $f -Algorithm SHA256).Hash -eq $sha)){\r\n"
    /* The guest's network can come up after this starts, and a VPN VM's only
       once its tunnel does, so wait out network errors rather than failing. */
    "  for($try=1;;$try++){\r\n"
    "   try { Invoke-WebRequest -Uri $url -OutFile $f -UseBasicParsing; break }\r\n"
    "   catch [Net.WebException] {\r\n"
    "    if($_.Exception.Status -eq [Net.WebExceptionStatus]::ProtocolError -or $try -ge 40){ throw }\r\n"
    "    Status 'network' 'Waiting for the VM to reach the internet so it can download Sunshine...'\r\n"
    "    Start-Sleep -Seconds 15\r\n"
    "   }\r\n"
    "  }\r\n"
    "  $h=(Get-FileHash $f -Algorithm SHA256).Hash\r\n"
    "  if($h -ne $sha){ Remove-Item $f -Force; throw \"$file failed its hash check (got $h)\" }\r\n"
    " }\r\n"
    " return $f\r\n"
    "}\r\n"
    "try {\r\n"
    " $installed=(Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*' -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -eq 'Sunshine' }).DisplayVersion\r\n"
    " if($installed -ne $version -or -not (Test-Path $exe)){\r\n"
    "  Status 'downloading' \"Sunshine $version\"\r\n"
    "  $msi=Fetch $sunshineUrl $sunshineSha 'sunshine.msi'\r\n"
    "  Status 'installing' \"Sunshine $version\"\r\n"
    "  $p=Start-Process msiexec.exe -ArgumentList '/i',('\"'+$msi+'\"'),'/qn','/norestart' -Wait -PassThru\r\n"
    "  if($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010){ throw \"the Sunshine installer failed with code $($p.ExitCode)\" }\r\n"
    " }\r\n"
    " if(-not (Get-Service ViGEmBus -ErrorAction SilentlyContinue)){\r\n"
    "  Status 'installing' 'the gamepad driver'\r\n"
    "  try {\r\n"
    "   $v=Fetch $vigemUrl $vigemSha 'vigembus.exe'\r\n"
    "   Start-Process $v -ArgumentList '/qn','/norestart' -Wait | Out-Null\r\n"
    "  } catch { Add-Content (Join-Path $dir 'setup.log') (\"gamepad driver: \"+$_.Exception.Message) }\r\n"
    " }\r\n"
    " Status 'configuring' ''\r\n"
    " Stop-Service SunshineService -Force -ErrorAction SilentlyContinue\r\n"
    " if($encoder -ne 'software'){\r\n"
    "  # A GPU-PV guest has the host's driver files in HostDriverStore; the encoder\r\n"
    "  # library normally sits in System32, so put it where Sunshine looks.\r\n"
    "  $lib=Get-ChildItem 'C:\\Windows\\System32\\HostDriverStore\\FileRepository\\nv*\\nvEncodeAPI64.dll' -ErrorAction SilentlyContinue | Select-Object -First 1\r\n"
    "  if($lib){ Copy-Item $lib.FullName 'C:\\Windows\\System32\\nvEncodeAPI64.dll' -Force -ErrorAction SilentlyContinue }\r\n"
    " }\r\n"
    " if(-not (Test-Path $creds)){\r\n"
    "  $b=New-Object byte[] 24; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)\r\n"
    "  $pw=[Convert]::ToBase64String($b).Replace('+','a').Replace('/','b').Replace('=','')\r\n"
    "  Push-Location $root; & $exe --creds asb $pw | Out-Null; Pop-Location\r\n"
    "  Set-Content -Path $creds -Value $pw -Encoding ascii\r\n"
    " }\r\n"
    " $lines=@(\r\n"
    "  \"sunshine_name = $name\",\r\n"
    "  \"port = $port\",\r\n"
    "  'origin_web_ui_allowed = pc',\r\n"
    "  'upnp = disabled',\r\n"
    "  'capture = ddx',\r\n"
    "  'lan_encryption_mode = 1',\r\n"
    "  'wan_encryption_mode = 1',\r\n"
    "  'dd_configuration_option = disabled'\r\n"
    " )\r\n"
    " if($encoder -eq 'software'){ $lines+='encoder = software' }\r\n"
    " Set-Content -Path $cfg -Value $lines -Encoding ascii\r\n"
    " # Sunshine can't run a prep command with nobody signed in, and a failed one stops\r\n"
    " # the stream. So plain Desktop has none and works at the sign-in screen, scaled.\r\n"
    " $apps=Join-Path $root 'config\\apps.json'\r\n"
    " $fitName='Desktop (resize to this device)'\r\n"
    " $list=Get-Content $apps -Raw | ConvertFrom-Json\r\n"
    " $fit=@{name=$fitName;'image-path'='desktop.png';'prep-cmd'=@(@{do='C:\\Windows\\AppSandbox\\appsandbox-displays.exe --stream-prep';undo='';elevated=$true})}\r\n"
    " $list.apps=@($list.apps | Where-Object { $_.name -ne $fitName })+$fit\r\n"
    " Set-Content -Path $apps -Value ($list | ConvertTo-Json -Depth 6) -Encoding ascii\r\n"
    " # The installer lets Sunshine through for everyone. Only the host's relay, or\r\n"
    " # the local network on a bridged VM, should reach it.\r\n"
    " Get-NetFirewallRule -DisplayName 'Sunshine*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue\r\n"
    " Get-NetFirewallRule -Group 'AppSandboxStream' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue\r\n"
    " New-NetFirewallRule -DisplayName 'App Sandbox streaming' -Group 'AppSandboxStream' -Direction Inbound -Action Allow -Program $exe -RemoteAddress $allowed | Out-Null\r\n"
    " Set-Service SunshineService -StartupType Automatic\r\n"
    " Start-Service SunshineService\r\n"
    " Status 'ready' ''\r\n"
    " Write-Output 'result:ok'\r\n"
    "} catch {\r\n"
    " Status 'failed' ($_.Exception.Message -replace '[\\r\\n|]',' ')\r\n"
    " Write-Output ('result:'+$_.Exception.Message)\r\n"
    " exit 1\r\n"
    "}\r\n";

static const char stream_status_ps[] =
    "$phase='none'; $detail=''\r\n"
    "try { if(Test-Path $statusFile){ $parts=(Get-Content $statusFile -Raw).Trim().Split('|'); $phase=$parts[0]; if($parts.Count -gt 1){ $detail=$parts[1] } } } catch { $phase='configuring' }\r\n"
    "$svc=Get-Service SunshineService -ErrorAction SilentlyContinue\r\n"
    "$encoders=@{}\r\n"
    "$log=Join-Path $root 'config\\sunshine.log'\r\n"
    "if(Test-Path $log){\r\n"
    " Select-String -Path $log -Pattern 'Found (H\\.264|HEVC|AV1) encoder: (\\S+)' | ForEach-Object { $encoders[$_.Matches[0].Groups[1].Value]=$_.Matches[0].Groups[2].Value }\r\n"
    "}\r\n"
    "$address=(Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object { $_.IPv4DefaultGateway } | Select-Object -First 1).IPv4Address.IPAddress\r\n"
    "$o=@{phase=$phase;detail=$detail;running=[bool]($svc -and $svc.Status -eq 'Running');gamepad=[bool](Get-Service ViGEmBus -ErrorAction SilentlyContinue);encoders=$encoders;address=$address}\r\n"
    "Write-Output ('json:'+($o|ConvertTo-Json -Compress))\r\n";

static const char stream_pin_ps[] =
    "try {\r\n"
    " $waiting=@((WebUi '/api/pin' $null).pairings)\r\n"
    " if($waiting.Count -eq 0){ Write-Output 'result:No device is waiting to pair. Start pairing in Moonlight first, then enter the PIN it shows.'; exit 1 }\r\n"
    " if($waiting.Count -gt 1){ Write-Output 'result:More than one device is waiting to pair. Cancel the others, then try again.'; exit 1 }\r\n"
    " $r=WebUi '/api/pin' @{pairing_id=$waiting[0].id;pin=$pin;name=$client}\r\n"
    " if($r.status -eq $true -or $r.status -eq 'true'){ Write-Output 'result:ok' } else { Write-Output 'result:Pairing failed. Check the PIN and try again from Moonlight.'; exit 1 }\r\n"
    "} catch { Write-Output ('result:'+($_.Exception.Message -replace '[\\r\\n]',' ')); exit 1 }\r\n";

static const char stream_unpair_ps[] =
    "try { $r=WebUi '/api/clients/unpair-all' @{}; Write-Output 'result:ok' } catch { Write-Output ('result:'+($_.Exception.Message -replace '[\\r\\n]',' ')); exit 1 }\r\n";

static const char stream_off_ps[] =
    "Stop-Service SunshineService -Force -ErrorAction SilentlyContinue\r\n"
    "Set-Service SunshineService -StartupType Disabled -ErrorAction SilentlyContinue\r\n"
    "Get-NetFirewallRule -Group 'AppSandboxStream' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue\r\n"
    "if(Test-Path $statusFile){ Remove-Item $statusFile -Force }\r\n"
    "Write-Output 'result:ok'\r\n";

/* Letters, digits and the few other characters a field needs: nothing a
   PowerShell string or config line could read as syntax. */
static BOOL stream_token_ok(const char *s, const char *extra)
{
    if (!*s) return FALSE;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr(extra, *s)) return FALSE;
    return TRUE;
}

static char g_stream_vars[1024];
static volatile LONG g_stream_running;

static DWORD WINAPI stream_setup_thread(LPVOID param)
{
    size_t cap = sizeof(g_stream_vars) + sizeof(stream_common_ps) + sizeof(stream_setup_ps);
    char *script = (char *)malloc(cap);
    char out[1024];
    (void)param;
    if (script) {
        sprintf_s(script, cap, "%s%s%s", g_stream_vars, stream_common_ps, stream_setup_ps);
        if (!write_agent_script(STREAM_SETUP, script)) {
            int ec = run_agent_powershell_out(STREAM_SETUP, 45 * 60 * 1000, out, sizeof(out));
            agent_log("Streaming: setup finished (%d): %.300s", ec, out);
            if (ec != 0) agent_log_to_host("Streaming: Sunshine setup failed: %.300s", out);
        }
        free(script);
    }
    InterlockedExchange(&g_stream_running, 0);
    return 0;
}

static void stream_setup(const char *args, char *reply, size_t reply_size)
{
    char copy[300], *ctx = NULL, *port, *encoder, *allowed, *name;
    HANDLE t;
    if (strcpy_s(copy, sizeof(copy), args)) copy[0] = '\0';
    port = strtok_s(copy, ":", &ctx);
    encoder = strtok_s(NULL, ":", &ctx);
    allowed = strtok_s(NULL, ":", &ctx);
    name = strtok_s(NULL, ":", &ctx);
    if (!port || !encoder || !allowed || !name ||
        !stream_token_ok(port, "") || !stream_token_ok(encoder, "") ||
        !stream_token_ok(allowed, ".") || !stream_token_ok(name, "-")) {
        strcpy_s(reply, reply_size, "error:invalid arguments");
        return;
    }
    if (InterlockedExchange(&g_stream_running, 1)) {
        strcpy_s(reply, reply_size, "ok");
        return;
    }
    sprintf_s(g_stream_vars, sizeof(g_stream_vars),
              "$port=%s\r\n$encoder='%s'\r\n$allowed='%s'\r\n$name='%s'\r\n$version='%s'\r\n"
              "$sunshineUrl='%s'\r\n$sunshineSha='%s'\r\n$vigemUrl='%s'\r\n$vigemSha='%s'\r\n",
              port, encoder, allowed, name, STREAM_SUNSHINE_VER,
              STREAM_SUNSHINE_URL, STREAM_SUNSHINE_SHA, STREAM_VIGEM_URL, STREAM_VIGEM_SHA);
    t = CreateThread(NULL, 0, stream_setup_thread, NULL, 0, NULL);
    if (!t) {
        InterlockedExchange(&g_stream_running, 0);
        strcpy_s(reply, reply_size, "error:could not start setup");
        return;
    }
    CloseHandle(t);
    strcpy_s(reply, reply_size, "ok");
}

static void stream_run(const char *vars, const char *body, DWORD timeout_ms, char *reply, size_t reply_size)
{
    size_t cap = strlen(vars) + sizeof(stream_common_ps) + strlen(body) + 1;
    char *script = (char *)malloc(cap);
    if (!script) { strcpy_s(reply, reply_size, "error:out of memory"); return; }
    sprintf_s(script, cap, "%s%s%s", vars, stream_common_ps, body);
    run_domain_script(script, timeout_ms, reply, reply_size);
    free(script);
}

static void stream_command(const char *cmd, char *reply, size_t reply_size)
{
    if (strncmp(cmd, "stream_setup:", 13) == 0) {
        stream_setup(cmd + 13, reply, reply_size);
    } else if (strcmp(cmd, "stream_status") == 0) {
        stream_run("", stream_status_ps, 30000, reply, reply_size);
    } else if (strncmp(cmd, "stream_pin:", 11) == 0) {
        char copy[128] = "", vars[200], *ctx = NULL, *pin, *client;
        strncpy_s(copy, sizeof(copy), cmd + 11, _TRUNCATE);
        pin = strtok_s(copy, ":", &ctx);
        client = strtok_s(NULL, ":", &ctx);
        if (!pin || !client || strlen(pin) != 4 || !stream_token_ok(pin, "") ||
            !stream_token_ok(client, "- ")) {
            strcpy_s(reply, reply_size, "error:invalid arguments");
            return;
        }
        sprintf_s(vars, sizeof(vars), "$pin='%s'\r\n$client='%s'\r\n", pin, client);
        stream_run(vars, stream_pin_ps, 30000, reply, reply_size);
    } else if (strcmp(cmd, "stream_unpair") == 0) {
        stream_run("", stream_unpair_ps, 30000, reply, reply_size);
    } else if (strcmp(cmd, "stream_off") == 0) {
        stream_run("", stream_off_ps, 60000, reply, reply_size);
    } else {
        strcpy_s(reply, reply_size, "error:unknown");
    }
}

/* ---- Game Streaming ----

   The appliance checks a dashboard sign-in and forgets duplicates' computer accounts.
   A duplicate, booted from its generalized image, waits out Windows setup, then signs
   its user in once. An image build scans for games and generalizes the disk. */
static const char DOMAIN_VALIDATE_PS[] =
        "function AsbText($v){ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$v)) }\r\n"
        "try{\r\n"
        " Add-Type -AssemblyName System.DirectoryServices.AccountManagement\r\n"
        " $u=$env:ASB_DOM_USER; $p=AsbText $env:ASB_DOM_PW; $g=AsbText $env:ASB_DOM_GROUP\r\n"
        " $c=New-Object DirectoryServices.AccountManagement.PrincipalContext([DirectoryServices.AccountManagement.ContextType]::Domain)\r\n"
        " if(-not $c.ValidateCredentials($u,$p,[DirectoryServices.AccountManagement.ContextOptions]::Negotiate)){ Write-Output 'result:denied'; exit 0 }\r\n"
        " if($g){\r\n"
        "  $gp=[DirectoryServices.AccountManagement.GroupPrincipal]::FindByIdentity($c,$g)\r\n"
        "  $up=[DirectoryServices.AccountManagement.UserPrincipal]::FindByIdentity($c,$u)\r\n"
        "  if(-not $gp -or -not $up -or -not $up.IsMemberOf($gp)){ Write-Output 'result:not_allowed'; exit 0 }\r\n"
        " }\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + ($_.Exception.Message -replace '[\\r\\n]',' ')); exit 1 }\r\n";

static const char DOMAIN_COMPUTER_REMOVE_PS[] =
        "$ErrorActionPreference='Stop'\r\n"
        "try{\r\n"
        " foreach($c in @(Get-ADComputer -Filter (\"Name -like '\"+$env:ASB_DOM_USER+\"'\"))){ Remove-ADObject -Identity $c -Recursive -Confirm:$false }\r\n"
        " Write-Output 'result:ok'; exit 0\r\n"
        "}catch{ Write-Output ('result:' + ($_.Exception.Message -replace '[\\r\\n]',' ')); exit 1 }\r\n";

#define WINLOGON_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"

/* The sign-in's password is an LSA secret, where Winlogon looks first: nothing readable
   in the registry. NULL deletes it. */
static BOOL set_autologon_secret(const wchar_t *password)
{
    LSA_OBJECT_ATTRIBUTES attrs;
    LSA_HANDLE policy = NULL;
    LSA_UNICODE_STRING key, data;
    wchar_t name[] = L"DefaultPassword";
    NTSTATUS status;

    ZeroMemory(&attrs, sizeof(attrs));
    if (LsaOpenPolicy(NULL, &attrs, POLICY_CREATE_SECRET, &policy) != 0) return FALSE;
    key.Buffer = name;
    key.Length = (USHORT)(wcslen(name) * sizeof(wchar_t));
    key.MaximumLength = key.Length + sizeof(wchar_t);
    if (password) {
        data.Buffer = (PWSTR)password;
        data.Length = (USHORT)(wcslen(password) * sizeof(wchar_t));
        data.MaximumLength = data.Length + sizeof(wchar_t);
    }
    status = LsaStorePrivateData(policy, &key, password ? &data : NULL);
    LsaClose(policy);
    return status == 0;
}

static void winlogon_set(const wchar_t *name, const wchar_t *value)
{
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, name, REG_SZ, value,
                    (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

/* domain_autologon:<netbios>:<user>:<base64 password>. The next boot signs in once. */
static void session_autologon(const char *domain, const char *user, const char *b64,
                              char *reply, size_t reply_size)
{
    BYTE raw[512];
    wchar_t password[256], domain_w[32], user_w[64];
    DWORD raw_len = sizeof(raw) - 1, one = 1;
    int chars;

    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, raw, &raw_len, NULL, NULL)) {
        strcpy_s(reply, reply_size, "error:invalid arguments");
        return;
    }
    raw[raw_len] = 0;
    chars = MultiByteToWideChar(CP_UTF8, 0, (const char *)raw, -1, password, _countof(password));
    SecureZeroMemory(raw, sizeof(raw));
    MultiByteToWideChar(CP_UTF8, 0, domain, -1, domain_w, _countof(domain_w));
    MultiByteToWideChar(CP_UTF8, 0, user, -1, user_w, _countof(user_w));
    if (chars <= 0 || !set_autologon_secret(password)) {
        SecureZeroMemory(password, sizeof(password));
        strcpy_s(reply, reply_size, "error:could not store the sign-in");
        return;
    }
    SecureZeroMemory(password, sizeof(password));
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, L"DefaultPassword");
    winlogon_set(L"DefaultDomainName", domain_w);
    winlogon_set(L"DefaultUserName", user_w);
    winlogon_set(L"AutoAdminLogon", L"1");
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, L"AutoLogonCount", REG_DWORD, &one, sizeof(one));
    /* A domain sign-in needs the network, so Winlogon has to wait for it. */
    RegSetKeyValueW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                    L"SyncForegroundPolicy", REG_DWORD, &one, sizeof(one));
    strcpy_s(reply, reply_size, "ok");
}

static void session_autologon_clear(void)
{
    set_autologon_secret(NULL);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, L"DefaultPassword");
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, L"AutoLogonCount");
    winlogon_set(L"AutoAdminLogon", L"0");
}

/* Windows setup is still going after sysprep; a restart now would break it. */
static void session_setup_wait(char *reply, size_t reply_size)
{
    int i;
    for (i = 0; i < 240; i++) {
        DWORD setup = 1, oobe = 1, size = sizeof(DWORD);
        wchar_t state[64] = L"";
        /* The registry values clear before the out-of-box screens end; this is last. */
        GetPrivateProfileStringW(L"State", L"ImageState", L"", state, _countof(state),
                                 L"C:\\Windows\\Setup\\State\\State.ini");
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SystemSetupInProgress",
                     RRF_RT_REG_DWORD, NULL, &setup, &size);
        size = sizeof(DWORD);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"OOBEInProgress",
                         RRF_RT_REG_DWORD, NULL, &oobe, &size) != ERROR_SUCCESS)
            oobe = 0;
        if (!setup && !oobe && (!state[0] || wcscmp(state, L"IMAGE_STATE_COMPLETE") == 0)) {
            strcpy_s(reply, reply_size, "ok");
            return;
        }
        Sleep(5000);
    }
    strcpy_s(reply, reply_size, "error:Windows setup did not finish");
}

/* session_link:<ip>. The link to the domain controller is normally set up again at each
   boot, by the agent, which is too late for a sign-in at boot. A duplicate's next boot
   needs it from the start, and its DNS settings and name rule already persist. */
static const char SESSION_LINK_PS[] =
    "$ErrorActionPreference='Stop'\r\n"
    "try{\r\n"
    " $a=Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -eq $env:ASB_DOM_DC } | Select-Object -First 1\r\n"
    " if(-not $a){ throw 'The link to the domain controller is not up.' }\r\n"
    " # Without -PolicyStore it goes in both the active and the saved settings.\r\n"
    " Remove-NetIPAddress -InterfaceIndex $a.InterfaceIndex -IPAddress $env:ASB_DOM_DC -Confirm:$false\r\n"
    " New-NetIPAddress -InterfaceIndex $a.InterfaceIndex -AddressFamily IPv4 -IPAddress $env:ASB_DOM_DC -PrefixLength 24 | Out-Null\r\n"
    " Write-Output 'result:ok'; exit 0\r\n"
    "}catch{ Write-Output ('result:' + ($_.Exception.Message -replace '[\\r\\n]',' ')); exit 1 }\r\n";

/* Why a sign-in didn't happen, for the log. No password is involved. */
static const char SESSION_SIGNIN_WHY_PS[] =
    "$w=Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon'\r\n"
    "$e=Get-WinEvent -FilterHashtable @{LogName='Security';Id=4625} -MaxEvents 1 -ErrorAction SilentlyContinue\r\n"
    "$f=if($e){ 'lastfail='+$e.Properties[5].Value+' status='+('{0:X}' -f [uint32]$e.Properties[7].Value)+' sub='+('{0:X}' -f [uint32]$e.Properties[9].Value) }else{ 'no failed sign-in recorded' }\r\n"
    "$n=Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='NETLOGON'} -MaxEvents 1 -ErrorAction SilentlyContinue\r\n"
    "$l=if($n){ ' netlogon='+$n.Id+'@'+$n.TimeCreated.ToString('HH:mm:ss') }else{ '' }\r\n"
    "$u=@(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress+'/'+$_.Store }) -join ','\r\n"
    "$q=(& quser.exe 2>&1 | Out-String) -replace '\\s+',' '\r\n"
    "$o=@(Get-WinEvent -FilterHashtable @{LogName='Security';Id=4647,4634} -MaxEvents 3 -ErrorAction SilentlyContinue | ForEach-Object { ''+$_.Id+'@'+$_.TimeCreated.ToString('HH:mm:ss')+'='+$_.Properties[1].Value }) -join ','\r\n"
    "$p=@(Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Microsoft-Windows-User Profiles Service','Microsoft-Windows-User Profile Service';Level=1,2,3} -MaxEvents 3 -ErrorAction SilentlyContinue | ForEach-Object { ''+$_.Id+'@'+$_.TimeCreated.ToString('HH:mm:ss') }) -join ','\r\n"
    "$c=@(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000,1002} -MaxEvents 3 -ErrorAction SilentlyContinue | ForEach-Object { ''+$_.Id+'@'+$_.TimeCreated.ToString('HH:mm:ss')+'='+$_.Properties[0].Value }) -join ','\r\n"
    "Write-Output ('result:timeout quser=['+$q+'] logoffs=['+$o+'] profile=['+$p+'] crashes=['+$c+'] autologon='+$w.AutoAdminLogon+' count='+$w.AutoLogonCount+' user='+$w.DefaultDomainName+'\\'+$w.DefaultUserName+' boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('HH:mm:ss')+' '+$f+$l+' ips='+$u)\r\n";

/* The desktop has loaded in this session. */
static BOOL session_has_shell(DWORD session)
{
    WTS_PROCESS_INFOA *list = NULL;
    DWORD count = 0, i;
    BOOL found = FALSE;
    if (WTSEnumerateProcessesA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count)) {
        for (i = 0; i < count && !found; i++)
            found = list[i].SessionId == session && _stricmp(list[i].pProcessName, "explorer.exe") == 0;
        WTSFreeMemory(list);
    }
    return found;
}

/* Hold the host's network setup back while the boot's sign-in runs: that setup replaces
   the link the sign-in is using, and a name at the console only means it has started:
   the profile and redirected folders still need that link. "ok:signed:<user>" once the
   desktop is up, "ok:timeout ..." if it never comes. */
static void session_wait_signin(char *reply, size_t reply_size)
{
    int i;
    for (i = 0; i < 90; i++) {
        DWORD session = WTSGetActiveConsoleSessionId(), got;
        char *user = NULL;
        BOOL signed_in = session != 0xFFFFFFFF &&
            WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &got) &&
            user[0] && _strnicmp(user, "defaultuser", 11) != 0;
        if (signed_in && session_has_shell(session)) {
            sprintf_s(reply, reply_size, "ok:signed:%s", user);
            WTSFreeMemory(user);
            /* A first sign-in is still setting up the profile when the desktop process
               appears. */
            Sleep(30000);
            return;
        }
        if (user) WTSFreeMemory(user);
        Sleep(2000);
    }
    run_domain_script(SESSION_SIGNIN_WHY_PS, 60000, reply, reply_size);
}

/* "ok:" alone, or "ok:<domain>\<user>" for whoever is at the console. */
static void session_status(char *reply, size_t reply_size)
{
    DWORD session = WTSGetActiveConsoleSessionId(), got;
    char *user = NULL, *domain = NULL;
    strcpy_s(reply, reply_size, "ok:");
    if (session != 0xFFFFFFFF &&
        WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &got) &&
        WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, session, WTSDomainName, &domain, &got) &&
        user[0])
        sprintf_s(reply, reply_size, "ok:%s\\%s", domain, user);
    if (user) WTSFreeMemory(user);
    if (domain) WTSFreeMemory(domain);
}

/* Open a launcher link in the signed-in user's session. */
static BOOL session_open_target(const wchar_t *target, char *reply, size_t reply_size)
{
    DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    LPVOID env = NULL;
    wchar_t line[1024];
    BOOL ok;

    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) {
        strcpy_s(reply, reply_size, "error:no_user");
        return FALSE;
    }
    swprintf_s(line, _countof(line), L"explorer.exe \"%s\"", target);
    if (!CreateEnvironmentBlock(&env, token, FALSE)) env = NULL;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessAsUserW(token, NULL, line, NULL, NULL, FALSE,
                              env ? CREATE_UNICODE_ENVIRONMENT : 0, env, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        strcpy_s(reply, reply_size, "ok");
    } else {
        sprintf_s(reply, reply_size, "error:could not open it (%lu)", GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);
    return ok;
}

static BOOL session_digits(const char *s)
{
    if (!*s) return FALSE;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return FALSE;
    return TRUE;
}

/* session_launch:<catalog id>. Only ids the scan can produce, never a path or a link:
   steam:<digits>, epic:<namespace>:<item>:<app>, gog:<digits>. */
static void session_launch(const char *id, char *reply, size_t reply_size)
{
    wchar_t target[768];
    if (strncmp(id, "steam:", 6) == 0 && session_digits(id + 6)) {
        swprintf_s(target, _countof(target), L"steam://rungameid/%S", id + 6);
    } else if (strncmp(id, "epic:", 5) == 0 && domain_field_ok(id + 5, ":-_", 400)) {
        swprintf_s(target, _countof(target),
                   L"com.epicgames.launcher://apps/%S?action=launch&silent=true", id + 5);
    } else if (strncmp(id, "gog:", 4) == 0 && session_digits(id + 4)) {
        wchar_t key[128];
        DWORD size = sizeof(target);
        swprintf_s(key, _countof(key), L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\%S", id + 4);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"launchCommand", RRF_RT_REG_SZ, NULL, target, &size) != ERROR_SUCCESS) {
            strcpy_s(reply, reply_size, "error:that game is not installed");
            return;
        }
    } else {
        strcpy_s(reply, reply_size, "error:invalid arguments");
        return;
    }
    agent_log("Game Streaming: launching %s.", id);
    session_open_target(target, reply, reply_size);
}

#define CATALOG_DIR L"C:\\ProgramData\\AppSandbox\\catalog"
#define CATALOG_CHUNK 18000

static const char SESSION_CATALOG_PS[] =
    "$ErrorActionPreference='Continue'\r\n"
    "$dir='C:\\ProgramData\\AppSandbox\\catalog'\r\n"
    "Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue\r\n"
    "New-Item -ItemType Directory -Force $dir | Out-Null\r\n"
    "$games=New-Object System.Collections.ArrayList\r\n"
    "function Add($id,$name,$source,$art){ [void]$games.Add(@{id=$id;name=$name;source=$source;art=$art}) }\r\n"
    "$steam=(Get-ItemProperty 'HKLM:\\SOFTWARE\\WOW6432Node\\Valve\\Steam' -ErrorAction SilentlyContinue).InstallPath\r\n"
    "if($steam -and (Test-Path $steam)){\r\n"
    " $libs=@($steam)\r\n"
    " $vdf=Join-Path $steam 'steamapps\\libraryfolders.vdf'\r\n"
    " if(Test-Path $vdf){ foreach($m in [regex]::Matches((Get-Content $vdf -Raw),'\"path\"\\s+\"([^\"]+)\"')){ $libs+=$m.Groups[1].Value.Replace('\\\\','\\') } }\r\n"
    " foreach($lib in ($libs | Sort-Object -Unique)){\r\n"
    "  foreach($f in (Get-ChildItem (Join-Path $lib 'steamapps') -Filter 'appmanifest_*.acf' -ErrorAction SilentlyContinue)){\r\n"
    "   $t=Get-Content $f.FullName -Raw\r\n"
    "   $id=[regex]::Match($t,'\"appid\"\\s+\"(\\d+)\"').Groups[1].Value\r\n"
    "   $n=[regex]::Match($t,'\"name\"\\s+\"([^\"]+)\"').Groups[1].Value\r\n"
    "   if(-not $id -or -not $n -or $id -eq '228980' -or $n -like 'Steamworks*'){ continue }\r\n"
    "   $art=''\r\n"
    "   $cache=Join-Path $steam 'appcache\\librarycache'\r\n"
    "   $pick=@(Get-Item (Join-Path $cache ($id+'_header.jpg')) -ErrorAction SilentlyContinue)+@(Get-ChildItem (Join-Path $cache $id) -Recurse -Filter 'header.jpg' -ErrorAction SilentlyContinue) | Where-Object { $_ -and $_.Length -lt 1048576 } | Select-Object -First 1\r\n"
    "   if($pick){ $art='steam_'+$id+'.jpg'; Copy-Item $pick.FullName (Join-Path $dir $art) -Force }\r\n"
    "   Add ('steam:'+$id) $n 'Steam' $art\r\n"
    "  }\r\n"
    " }\r\n"
    "}\r\n"
    "foreach($f in (Get-ChildItem 'C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests' -Filter '*.item' -ErrorAction SilentlyContinue)){\r\n"
    " try{ $j=Get-Content $f.FullName -Raw | ConvertFrom-Json }catch{ continue }\r\n"
    " if($j.bIsIncompleteInstall){ continue }\r\n"
    " $id=$j.CatalogNamespace+':'+$j.CatalogItemId+':'+$j.AppName\r\n"
    " if($id -match '^[A-Za-z0-9:_-]+$' -and $j.DisplayName){ Add ('epic:'+$id) $j.DisplayName 'Epic Games' '' }\r\n"
    "}\r\n"
    "foreach($k in (Get-ChildItem 'HKLM:\\SOFTWARE\\WOW6432Node\\GOG.com\\Games' -ErrorAction SilentlyContinue)){\r\n"
    " $g=Get-ItemProperty $k.PSPath\r\n"
    " if($k.PSChildName -match '^\\d+$' -and $g.launchCommand){ Add ('gog:'+$k.PSChildName) $(if($g.gameName){$g.gameName}else{$g.GAMENAME}) 'GOG' '' }\r\n"
    "}\r\n"
    "$list=@($games | Sort-Object { $_.name })\r\n"
    "[IO.File]::WriteAllText((Join-Path $dir 'catalog.json'),(ConvertTo-Json -InputObject $list -Compress),(New-Object Text.UTF8Encoding($false)))\r\n"
    "Write-Output ('result:'+$list.Count)\r\n";

/* session_catalog_read:<file>:<offset>: "ok:<total bytes>:<base64 piece>". */
static void session_catalog_read(char *args, char *reply, size_t reply_size)
{
    char *f[3];
    wchar_t path[MAX_PATH];
    HANDLE file;
    BYTE *chunk;
    DWORD got = 0, cap, total;
    int pos;
    LONGLONG offset;
    LARGE_INTEGER at;

    if (domain_split(args, f, 3) != 2 || !f[0][0] || !domain_field_ok(f[0], "._", 64) ||
        strstr(f[0], "..") || !session_digits(f[1])) {
        strcpy_s(reply, reply_size, "error:invalid arguments");
        return;
    }
    offset = _atoi64(f[1]);
    swprintf_s(path, MAX_PATH, CATALOG_DIR L"\\%S", f[0]);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        strcpy_s(reply, reply_size, "error:not_found");
        return;
    }
    total = GetFileSize(file, NULL);
    at.QuadPart = offset;
    chunk = (BYTE *)malloc(CATALOG_CHUNK);
    pos = sprintf_s(reply, reply_size, "ok:%lu:", total);
    cap = (DWORD)(reply_size - pos);
    if (chunk && SetFilePointerEx(file, at, NULL, FILE_BEGIN) &&
        ReadFile(file, chunk, CATALOG_CHUNK, &got, NULL) && got) {
        if (!CryptBinaryToStringA(chunk, got, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, reply + pos, &cap))
            strcpy_s(reply, reply_size, "error:encode_failed");
    }
    free(chunk);
    CloseHandle(file);
}

/* The last step of an image build. The AD account stays: it belongs to the VM this disk
   was copied from, so the domain is left on this machine only (flags 0). Sysprep shuts
   the VM down; if it doesn't, the status file says why. */
static const char SESSION_IMAGE_PS[] =
    "$s='C:\\ProgramData\\AppSandbox\\image-status.txt'\r\n"
    "function St($t){ Set-Content -Path $s -Value $t -Encoding ascii }\r\n"
    "try{\r\n"
    " St 'cleaning'\r\n"
    " Stop-Service SunshineService -Force -ErrorAction SilentlyContinue\r\n"
    " $sun=Join-Path $env:ProgramFiles 'Sunshine\\config'\r\n"
    " foreach($p in 'sunshine_state.json','credentials','sunshine.log'){ Remove-Item (Join-Path $sun $p) -Recurse -Force -ErrorAction SilentlyContinue }\r\n"
    " foreach($p in 'webui.txt','status.txt'){ Remove-Item (Join-Path 'C:\\ProgramData\\AppSandbox\\stream' $p) -Force -ErrorAction SilentlyContinue }\r\n"
    " Remove-Item 'C:\\ProgramData\\AppSandbox\\catalog' -Recurse -Force -ErrorAction SilentlyContinue\r\n"
    " Remove-Item 'C:\\ProgramData\\AppSandbox\\user-drives.txt' -Force -ErrorAction SilentlyContinue\r\n"
    " # Setup would prefer these older answer files to the duplicate's own.\r\n"
    " foreach($p in 'C:\\Windows\\Panther\\unattend.xml','C:\\Windows\\Panther\\Unattend\\unattend.xml','C:\\Windows\\System32\\Sysprep\\unattend.xml'){ Remove-Item $p -Force -ErrorAction SilentlyContinue }\r\n"
    " if((Get-CimInstance Win32_ComputerSystem).PartOfDomain){\r\n"
    "  Add-Type 'using System.Runtime.InteropServices; public static class AsbJoin{[DllImport(\"netapi32.dll\",CharSet=CharSet.Unicode)] public static extern int NetUnjoinDomain(string s,string a,string p,int f);}'\r\n"
    "  $r=[AsbJoin]::NetUnjoinDomain($null,$null,$null,0)\r\n"
    "  if($r -ne 0){ throw \"Could not leave the domain (error $r).\" }\r\n"
    " }\r\n"
    " St 'sysprep'\r\n"
    " $p=Start-Process (Join-Path $env:WINDIR 'System32\\Sysprep\\sysprep.exe') -ArgumentList '/generalize','/oobe','/shutdown','/mode:vm' -PassThru -Wait\r\n"
    " Start-Sleep -Seconds 20\r\n"
    " $why=Get-Content (Join-Path $env:WINDIR 'System32\\Sysprep\\Panther\\setuperr.log') -Tail 3 -ErrorAction SilentlyContinue\r\n"
    " St ('failed|Windows Sysprep stopped (code '+$p.ExitCode+'). '+(($why -join ' ') -replace '[\\r\\n|]',' '))\r\n"
    "}catch{ St ('failed|'+($_.Exception.Message -replace '[\\r\\n|]',' ')) }\r\n";

static DWORD WINAPI session_image_thread(LPVOID param)
{
    static const wchar_t path[] = L"C:\\ProgramData\\AppSandbox\\image-prepare.ps1";
    char out[512];
    (void)param;
    if (!write_agent_script(path, SESSION_IMAGE_PS))
        run_agent_powershell_out(path, 60 * 60 * 1000, out, sizeof(out));
    return 0;
}

static void session_command(char *cmd, char *reply, size_t reply_size)
{
    if (strcmp(cmd, "session_status") == 0) {
        session_status(reply, reply_size);
    } else if (strcmp(cmd, "session_setup_wait") == 0) {
        session_setup_wait(reply, reply_size);
    } else if (strncmp(cmd, "session_link:", 13) == 0) {
        if (!domain_field_ok(cmd + 13, ".", 15) || !cmd[13]) {
            strcpy_s(reply, reply_size, "error:invalid arguments");
            return;
        }
        SetEnvironmentVariableA("ASB_DOM_DC", cmd + 13);
        run_domain_script(SESSION_LINK_PS, 60000, reply, reply_size);
    } else if (strcmp(cmd, "session_wait_signin") == 0) {
        session_wait_signin(reply, reply_size);
    } else if (strcmp(cmd, "session_signin_why") == 0) {
        run_domain_script(SESSION_SIGNIN_WHY_PS, 60000, reply, reply_size);
    } else if (strcmp(cmd, "session_signed_in") == 0) {
        session_autologon_clear();
        strcpy_s(reply, reply_size, "ok");
    } else if (strncmp(cmd, "session_launch:", 15) == 0) {
        session_launch(cmd + 15, reply, reply_size);
    } else if (strcmp(cmd, "session_catalog_scan") == 0) {
        run_domain_script(SESSION_CATALOG_PS, 5 * 60 * 1000, reply, reply_size);
    } else if (strncmp(cmd, "session_catalog_read:", 21) == 0) {
        session_catalog_read(cmd + 21, reply, reply_size);
    } else if (strcmp(cmd, "session_image_prepare") == 0) {
        HANDLE t = CreateThread(NULL, 0, session_image_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        strcpy_s(reply, reply_size, t ? "ok" : "error:could not start");
    } else if (strcmp(cmd, "session_image_status") == 0) {
        char text[400] = "";
        DWORD got = 0;
        HANDLE f = CreateFileW(L"C:\\ProgramData\\AppSandbox\\image-status.txt", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            ReadFile(f, text, sizeof(text) - 1, &got, NULL);
            CloseHandle(f);
        }
        text[got] = 0;
        text[strcspn(text, "\r\n")] = 0;
        sprintf_s(reply, reply_size, "ok:%s", text);
    } else {
        strcpy_s(reply, reply_size, "error:unknown");
    }
}

static void handle_domain_command(char *cmd, char *reply, size_t reply_size)
{
    char *f[8];
    int n;
    if (strcmp(cmd, "domain_list") == 0) {
        run_domain_script(DOMAIN_LIST_PS, 120000, reply, reply_size);
        return;
    }
    if (strncmp(cmd, "domain_promote:", 15) == 0) {
        n = domain_split(cmd + 15, f, 8);       /* NB, FQDN, pw */
        if (n != 3 || !domain_field_ok(f[0], "-", 15) || !domain_field_ok(f[1], ".-", 200) ||
            !domain_field_ok(f[2], "+/=", 600)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_NB", f[0]);
        SetEnvironmentVariableA("ASB_DOM_FQDN", f[1]);
        SetEnvironmentVariableA("ASB_DOM_PW", f[2]);
        run_domain_script(DOMAIN_PROMOTE_PS, 45 * 60 * 1000, reply, reply_size);
    } else if (strncmp(cmd, "domain_finish:", 14) == 0) {
        n = domain_split(cmd + 14, f, 8);       /* user, pw */
        if (n != 2 || !domain_field_ok(f[0], "._-", 20) || !domain_field_ok(f[1], "+/=", 600))
            goto invalid;
        SetEnvironmentVariableA("ASB_DOM_USER", f[0]);
        SetEnvironmentVariableA("ASB_DOM_PW", f[1]);
        SetEnvironmentVariableW(L"ASB_MANAGEMENT_HOST", g_shared_management_ip);
        run_domain_script(DOMAIN_FINISH_PS, 20 * 60 * 1000, reply, reply_size);
    } else if (strncmp(cmd, "domain_user_set:", 16) == 0) {
        n = domain_split(cmd + 16, f, 8);       /* user, pwset, pw, enabled, groups */
        if (n != 5 || !domain_field_ok(f[0], "._-", 20) || !domain_field_ok(f[1], "", 1) ||
            !domain_field_ok(f[2], "+/=", 600) || !domain_field_ok(f[3], "", 1) ||
            !domain_field_ok(f[4], "+/=", 1500)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_USER", f[0]);
        SetEnvironmentVariableA("ASB_DOM_PWSET", f[1]);
        SetEnvironmentVariableA("ASB_DOM_PW", f[2]);
        SetEnvironmentVariableA("ASB_DOM_ENABLED", f[3]);
        SetEnvironmentVariableA("ASB_DOM_GROUPS", f[4]);
        run_domain_script(DOMAIN_USER_SET_PS, 120000, reply, reply_size);
    } else if (strncmp(cmd, "domain_user_delete:", 19) == 0) {
        if (!*(cmd + 19) || !domain_field_ok(cmd + 19, "._-", 20)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_USER", cmd + 19);
        run_domain_script(DOMAIN_USER_DELETE_PS, 60000, reply, reply_size);
    } else if (strncmp(cmd, "domain_group_add:", 17) == 0 ||
               strncmp(cmd, "domain_group_delete:", 20) == 0) {
        BOOL add = cmd[13] == 'a';
        char *arg = cmd + (add ? 17 : 20);
        if (!*arg || !domain_field_ok(arg, "+/=", 200)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_GROUP", arg);
        run_domain_script(add ? DOMAIN_GROUP_ADD_PS : DOMAIN_GROUP_DELETE_PS, 60000,
                          reply, reply_size);
    } else if (strcmp(cmd, "domain_ensure") == 0) {
        run_domain_script(DOMAIN_ENSURE_PS, 10 * 60 * 1000, reply, reply_size);
    } else if (strncmp(cmd, "domain_validate:", 16) == 0) {
        n = domain_split(cmd + 16, f, 8);       /* user, pw, group or empty */
        if (n != 3 || !domain_field_ok(f[0], "._-", 20) || !domain_field_ok(f[1], "+/=", 600) ||
            !domain_field_ok(f[2], "+/=", 200)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_USER", f[0]);
        SetEnvironmentVariableA("ASB_DOM_PW", f[1]);
        SetEnvironmentVariableA("ASB_DOM_GROUP", f[2]);
        run_domain_script(DOMAIN_VALIDATE_PS, 60000, reply, reply_size);
    } else if (strncmp(cmd, "domain_computer_remove:", 23) == 0) {
        /* Only a duplicate's name, or GS-* for all of them: nothing else in the domain
           goes this way. */
        char *name = cmd + 23;
        if (strncmp(name, "GS-", 3) != 0 || !domain_field_ok(name, "-*", 15)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_USER", name);
        run_domain_script(DOMAIN_COMPUTER_REMOVE_PS, 60000, reply, reply_size);
    } else if (strncmp(cmd, "domain_autologon:", 17) == 0) {
        n = domain_split(cmd + 17, f, 8);       /* NB, user, pw */
        if (n != 3 || !f[0][0] || !f[1][0] || !domain_field_ok(f[0], "-", 15) ||
            !domain_field_ok(f[1], "._-", 20) || !domain_field_ok(f[2], "+/=", 600)) goto invalid;
        session_autologon(f[0], f[1], f[2], reply, reply_size);
    } else if (strncmp(cmd, "domain_join:", 12) == 0) {
        n = domain_split(cmd + 12, f, 8);       /* FQDN, NB, dc, NB\user, pw */
        if (n != 5 || !domain_field_ok(f[0], ".-", 200) || !domain_field_ok(f[1], "-", 15) ||
            !domain_field_ok(f[2], ".", 15) || !domain_field_ok(f[3], "\\._-", 40) ||
            !domain_field_ok(f[4], "+/=", 600)) goto invalid;
        SetEnvironmentVariableA("ASB_DOM_FQDN", f[0]);
        SetEnvironmentVariableA("ASB_DOM_NB", f[1]);
        SetEnvironmentVariableA("ASB_DOM_DC", f[2]);
        SetEnvironmentVariableA("ASB_DOM_USER", f[3]);
        SetEnvironmentVariableA("ASB_DOM_PW", f[4]);
        run_domain_script(DOMAIN_JOIN_PS, 5 * 60 * 1000, reply, reply_size);
        if (strcmp(reply, "ok:joined") == 0) {
            /* The join takes effect at the next boot. */
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            wchar_t restart[] = L"shutdown.exe /r /t 15 /c \"Joining the AppSandbox domain\"";
            if (CreateProcessW(NULL, restart, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                               NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    } else {
        strcpy_s(reply, reply_size, "error:unknown");
    }
    return;
invalid:
    strcpy_s(reply, reply_size, "error:invalid arguments");
}

/* ---- Group-restricted shared drives (managed VMs) ----
   The host sends "user_drives:R=asb_x$,S=asb_y$". They are mapped in the
   signed-in user's session with the user's own credentials, at sign-in and
   when the list arrives; a user outside the allowed groups simply gets no
   drive, because the appliance refuses the connection. */
#define USER_DRIVES_LIST   L"C:\\ProgramData\\AppSandbox\\user-drives.txt"
#define USER_DRIVES_SCRIPT L"C:\\ProgramData\\AppSandbox\\user-drives.ps1"

static const char USER_DRIVES_PS[] =
        "# Group-restricted shared drives, mapped as the signed-in user so the appliance decides.\r\n"
        "$list='C:\\ProgramData\\AppSandbox\\user-drives.txt'\r\n"
        "if(-not (Test-Path $list)){ exit 0 }\r\n"
        "$s=$env:LOGONSERVER\r\n"
        "if(-not $s){ try{ $s='\\\\'+([System.DirectoryServices.ActiveDirectory.Domain]::GetComputerDomain().FindDomainController().Name) }catch{ exit 0 } }\r\n"
        "foreach($line in Get-Content $list){\r\n"
        " $p=$line.Split('=')\r\n"
        " if($p.Count -ne 2){ continue }\r\n"
        " $l=$p[0]+':'\r\n"
        " if(Test-Path ($l+'\\')){ continue }\r\n"
        " & net.exe use $l ($s+'\\'+$p[1]) /persistent:no 2>&1 | Out-Null\r\n"
        "}\r\n";

static DWORD WINAPI user_drives_thread(LPVOID param)
{
    DWORD session = (DWORD)(ULONG_PTR)param;
    HANDLE token = NULL;
    LPVOID env = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[512];
    Sleep(3000);   /* let the new session settle */
    if (GetFileAttributesW(USER_DRIVES_LIST) == INVALID_FILE_ATTRIBUTES) return 0;
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) return 0;
    if (write_agent_script(USER_DRIVES_SCRIPT, USER_DRIVES_PS)) { CloseHandle(token); return 0; }
    if (!CreateEnvironmentBlock(&env, token, FALSE)) env = NULL;
    swprintf_s(cmd, _countof(cmd),
               L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
               L"-WindowStyle Hidden -File \"%s\"", USER_DRIVES_SCRIPT);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                             env, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        agent_log("User drives: CreateProcessAsUserW failed (%lu).", GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);
    return 0;
}

static void map_user_drives(DWORD session)
{
    HANDLE t = CreateThread(NULL, 0, user_drives_thread, (LPVOID)(ULONG_PTR)session, 0, NULL);
    if (t) CloseHandle(t);
}

/* "R=asb_x$,S=asb_y$" -> one "R=asb_x$" per line; empty clears the list. */
static BOOL set_user_drives(const char *list)
{
    const char *p = list;
    FILE *f;
    if (!*list) { DeleteFileW(USER_DRIVES_LIST); return TRUE; }
    CreateDirectoryW(L"C:\\ProgramData\\AppSandbox", NULL);
    if (_wfopen_s(&f, USER_DRIVES_LIST, L"w") != 0 || !f) return FALSE;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= 3 && len < 64 && isalpha((unsigned char)p[0]) && p[1] == '=' &&
            domain_field_ok_n(p + 2, len - 2))
            fprintf(f, "%c=%.*s\n", toupper((unsigned char)p[0]), (int)(len - 2), p + 2);
        if (!end) break;
        p = end + 1;
    }
    fclose(f);
    return TRUE;
}

/* ---- VB-CABLE fallback audio device ----
   Secure Boot blocks the test-signed AppSandboxVAD, so a VM booted with it on
   has no speaker for the audio helper to capture. VB-CABLE (VB-Audio,
   donationware, Microsoft-signed) loads under Secure Boot, so install it from
   vb-audio.com in that case. The script skips the install when Secure Boot is
   off or our driver runs, checks the setup's signature before running it, and
   exits 0 installed, 2 present, 3 not needed, 4/5 bad package, else failed. */
#define VBCABLE_TRIES 5

static DWORD WINAPI vbcable_thread(LPVOID param)
{
    static const char script[] =
        "$ErrorActionPreference='Stop'\r\n"
        "$sb=$false; try { $sb=Confirm-SecureBootUEFI } catch {}\r\n"
        "if(-not $sb){ Write-Output 'Secure Boot is off'; exit 3 }\r\n"
        "if(Get-PnpDevice -FriendlyName '*App Sandbox Virtual Audio*' -Status OK -ErrorAction SilentlyContinue){ Write-Output 'AppSandbox audio driver runs'; exit 3 }\r\n"
        "if(Get-PnpDevice -FriendlyName '*VB-Audio Virtual Cable*' -ErrorAction SilentlyContinue){ exit 2 }\r\n"
        "$dir='C:\\ProgramData\\AppSandbox\\vbcable'\r\n"
        "New-Item -ItemType Directory -Force -Path $dir | Out-Null\r\n"
        "$zip=Join-Path $dir 'VBCABLE_Driver_Pack45.zip'\r\n"
        "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12\r\n"
        "Invoke-WebRequest -UseBasicParsing -Uri 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip' -OutFile $zip\r\n"
        "$pack=Join-Path $dir 'pack'\r\n"
        "Expand-Archive -Force -LiteralPath $zip -DestinationPath $pack\r\n"
        "$setup=Get-ChildItem -LiteralPath $pack -Recurse -Filter 'VBCABLE_Setup_x64.exe' | Select-Object -First 1\r\n"
        "if(-not $setup){ Write-Output 'VBCABLE_Setup_x64.exe is not in the pack'; exit 4 }\r\n"
        "$sig=Get-AuthenticodeSignature -LiteralPath $setup.FullName\r\n"
        "if($sig.Status -ne 'Valid'){ Write-Output ('setup signature: ' + $sig.Status); exit 5 }\r\n"
        "$p=Start-Process -FilePath $setup.FullName -ArgumentList '-i','-h' -Wait -PassThru\r\n"
        "Write-Output ('setup exit ' + $p.ExitCode)\r\n"
        "if(Get-PnpDevice -FriendlyName '*VB-Audio Virtual Cable*' -ErrorAction SilentlyContinue){ exit 0 }\r\n"
        "exit 6\r\n";
    const wchar_t *path = L"C:\\ProgramData\\AppSandbox\\vbcable-install.ps1";
    char output[2048];
    int ec, attempt;
    (void)param;

    if (write_agent_script(path, script)) return 0;
    for (attempt = 1; attempt <= VBCABLE_TRIES; attempt++) {
        ec = run_agent_powershell_out(path, 300000, output, sizeof(output));
        if (ec == 0) {
            agent_log("VB-CABLE: installed. %.300s", output);
            agent_log_to_host("No AppSandbox audio driver under Secure Boot, so installed VB-CABLE "
                              "(VB-Audio donationware, vb-cable.com) for sound. "
                              "Restart the VM if there is no sound yet.");
            return 0;
        }
        if (ec == 2 || ec == 3) return 0;
        agent_log("VB-CABLE install attempt %d/%d failed (exit=%d): %.300s",
                  attempt, VBCABLE_TRIES, ec, output);
        if (ec == 4 || ec == 5) return 0;
        /* Most failures are the network not being up yet. */
        if (WaitForSingleObject(g_stop_event, 60000) == WAIT_OBJECT_0) return 0;
    }
    return 0;
}

static void ensure_fallback_audio(void)
{
    HANDLE t = CreateThread(NULL, 0, vbcable_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---- App preinstall ----
   The host sends "install_apps:<id>,<id>" (winget package IDs chosen when the
   VM was created). They queue in apps-pending.txt and install with winget in
   the signed-in user's session, elevated through the user's linked token, so
   per-user apps (Claude, Steam, VS Code) land in that profile and machine-wide
   ones need no UAC prompt. Failures stay queued and retry at the next few
   service starts; App Installer can take a while to register on a new
   account, so the script waits for winget first. */
#define APPS_DIR      L"C:\\ProgramData\\AppSandbox"
#define APPS_PENDING  APPS_DIR L"\\apps-pending.txt"
#define APPS_RESULT   APPS_DIR L"\\apps-result.txt"
#define APPS_ATTEMPTS APPS_DIR L"\\apps-attempts.txt"
#define APPS_SCRIPT   APPS_DIR L"\\apps-install.ps1"
#define APPS_MAX_RUNS 3

static volatile LONG g_apps_running;

/* winget IDs are Publisher.Name: letters, digits and . + _ - only, which also
   keeps them safe to pass through the script and command line. */
static BOOL valid_winget_id(const char *id, size_t len)
{
    size_t i;
    if (len == 0 || len > 100) return FALSE;
    for (i = 0; i < len; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '+' && c != '_' && c != '-')
            return FALSE;
    }
    return TRUE;
}

static BOOL apps_pending(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    return GetFileAttributesExW(APPS_PENDING, GetFileExInfoStandard, &fa) &&
           (fa.nFileSizeLow || fa.nFileSizeHigh);
}

/* Elevated token for the console user, waiting for a sign-in. */
static HANDLE wait_for_user_token(void)
{
    for (;;) {
        HANDLE user = NULL;
        DWORD session = WTSGetActiveConsoleSessionId();
        if (session != 0xFFFFFFFF && WTSQueryUserToken(session, &user)) {
            TOKEN_LINKED_TOKEN linked;
            DWORD len = 0;
            if (GetTokenInformation(user, TokenLinkedToken, &linked, sizeof(linked), &len)) {
                CloseHandle(user);
                return linked.LinkedToken;   /* primary: the agent holds SeTcbPrivilege */
            }
            return user;                     /* UAC off: already the full token */
        }
        if (WaitForSingleObject(g_stop_event, 15000) == WAIT_OBJECT_0) return NULL;
    }
}

static DWORD WINAPI apps_install_thread(LPVOID param)
{
    static const char script[] =
        "$ErrorActionPreference='Continue'\r\n"
        "$dir='C:\\ProgramData\\AppSandbox'\r\n"
        "$pending=Join-Path $dir 'apps-pending.txt'\r\n"
        "$log=Join-Path $dir 'apps-install.log'\r\n"
        "$result=Join-Path $dir 'apps-result.txt'\r\n"
        "function Log($m){ Add-Content -Path $log -Value ('[' + (Get-Date -Format s) + '] ' + $m) }\r\n"
        "$ids=@(Get-Content $pending -ErrorAction SilentlyContinue | ForEach-Object { $_.Trim() } | Where-Object { $_ })\r\n"
        "if(-not $ids){ exit 0 }\r\n"
        "$winget=$null\r\n"
        "for($i=0;$i -lt 60 -and -not $winget;$i++){\r\n"
        " $winget=(Get-Command winget.exe -ErrorAction SilentlyContinue).Source\r\n"
        " if(-not $winget){\r\n"
        "  if($i -eq 0){ try { Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe -ErrorAction Stop } catch { Log ('App Installer registration: ' + $_.Exception.Message) } }\r\n"
        "  Start-Sleep -Seconds 10\r\n"
        " }\r\n"
        "}\r\n"
        "if(-not $winget){ Log 'winget is not available'; Set-Content -Path $result -Value 'nowinget'; exit 2 }\r\n"
        "& $winget source update --disable-interactivity | Out-Null\r\n"
        "$ok=@(); $failed=@()\r\n"
        "foreach($id in $ids){\r\n"
        " Log ('Installing ' + $id)\r\n"
        " & $winget install --id $id -e --silent --accept-package-agreements --accept-source-agreements --disable-interactivity 2>&1 | Out-File -Append -FilePath $log -Encoding utf8\r\n"
        " $code=$LASTEXITCODE\r\n"
        " # 0x8A15002B: no newer version, 0x8A150061: already installed\r\n"
        " if($code -eq 0 -or $code -eq -1978335189 -or $code -eq -1978335135){ $ok+=$id } else { Log ($id + ' failed (' + $code + ')'); $failed+=$id }\r\n"
        "}\r\n"
        "Set-Content -Path $pending -Value $failed\r\n"
        "Set-Content -Path $result -Value @(('ok ' + ($ok -join ', ')), ('failed ' + ($failed -join ', ')))\r\n"
        "exit 0\r\n";
    HANDLE token;
    LPVOID env = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[512], sysdir[MAX_PATH];
    DWORD ec = (DWORD)-1;
    int runs = 0;
    FILE *f;
    (void)param;

    if (_wfopen_s(&f, APPS_ATTEMPTS, L"r") == 0 && f) {
        if (fscanf_s(f, "%d", &runs) != 1) runs = 0;
        fclose(f);
    }
    if (runs >= APPS_MAX_RUNS) {
        agent_log("Apps: giving up after %d runs; still pending are left in apps-pending.txt.", runs);
        agent_log_to_host("Some apps could not be installed after %d tries; see "
                          "C:\\ProgramData\\AppSandbox\\apps-install.log in the VM.", runs);
        DeleteFileW(APPS_PENDING);
        DeleteFileW(APPS_ATTEMPTS);
        InterlockedExchange(&g_apps_running, 0);
        return 0;
    }
    if (write_agent_script(APPS_SCRIPT, script)) {
        InterlockedExchange(&g_apps_running, 0);
        return 0;
    }
    agent_log("Apps: waiting for a signed-in user to install the queued apps.");
    token = wait_for_user_token();
    if (!token) { InterlockedExchange(&g_apps_running, 0); return 0; }
    if (_wfopen_s(&f, APPS_ATTEMPTS, L"w") == 0 && f) {
        fprintf(f, "%d", runs + 1);
        fclose(f);
    }
    DeleteFileW(APPS_RESULT);
    if (!CreateEnvironmentBlock(&env, token, FALSE)) env = NULL;
    get_agent_powershell_directory(sysdir, _countof(sysdir));
    swprintf_s(cmd, _countof(cmd),
               L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
               L"-WindowStyle Hidden -File \"%s\"", APPS_SCRIPT);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    agent_log_to_host("Installing the apps chosen for this VM with winget...");
    if (CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                             env, sysdir, &si, &pi)) {
        HANDLE waits[2] = { pi.hProcess, g_stop_event };
        if (WaitForMultipleObjects(2, waits, FALSE, 2 * 60 * 60 * 1000) == WAIT_OBJECT_0)
            GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        agent_log("Apps: CreateProcessAsUserW failed (%lu).", GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);

    if (_wfopen_s(&f, APPS_RESULT, L"r") == 0 && f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (strncmp(line, "ok ", 3) == 0 && line[3]) {
                agent_log("Apps installed: %s", line + 3);
                agent_log_to_host("Installed apps: %s", line + 3);
            } else if (strncmp(line, "failed ", 7) == 0 && line[7]) {
                agent_log("Apps failed: %s", line + 7);
                agent_log_to_host("Could not install: %s (retried at the next start)", line + 7);
            } else if (strcmp(line, "nowinget") == 0) {
                agent_log_to_host("winget is not available in the VM yet; apps install at the next start.");
            }
        }
        fclose(f);
    } else {
        agent_log("Apps: installer exit=%lu with no result.", ec);
    }
    if (!apps_pending()) DeleteFileW(APPS_ATTEMPTS);
    InterlockedExchange(&g_apps_running, 0);
    return 0;
}

static void start_app_install(void)
{
    HANDLE t;
    if (!apps_pending() || InterlockedExchange(&g_apps_running, 1)) return;
    t = CreateThread(NULL, 0, apps_install_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_apps_running, 0);
}

/* Queue the validated IDs from "id,id,..." behind any already pending. */
static BOOL queue_apps(const char *list)
{
    const char *p = list;
    FILE *f;
    int queued = 0;
    CreateDirectoryW(APPS_DIR, NULL);
    if (_wfopen_s(&f, APPS_PENDING, L"a") != 0 || !f) return FALSE;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (valid_winget_id(p, len)) {
            fprintf(f, "%.*s\n", (int)len, p);
            queued++;
        } else if (len) {
            agent_log("Apps: ignoring invalid package ID.");
        }
        if (!end) break;
        p = end + 1;
    }
    fclose(f);
    DeleteFileW(APPS_ATTEMPTS);   /* a new request gets fresh tries */
    agent_log("Apps: queued %d package(s).", queued);
    return TRUE;
}

static int configure_shared_nic(const char *mac_a, const char *ip_a)
{
    static const char script[] =
        "$ErrorActionPreference='Stop'\r\n"
        "Set-Location -LiteralPath ([Environment]::SystemDirectory)\r\n"
        "$m=$env:ASB_NET_MAC -replace '[:-]',''\r\n"
        "$a=$null\r\n"
        "for($i=0;$i -lt 60 -and -not $a;$i++){\r\n"
        " $a=Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object {(($_.MacAddress) -replace '[:-]','') -eq $m} | Select-Object -First 1\r\n"
        " if(-not $a){Start-Sleep -Milliseconds 500}\r\n"
        "}\r\n"
        "if(-not $a){\r\n"
        " Write-Output ('want=' + $m)\r\n"
        " Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | ForEach-Object {\r\n"
        "  Write-Output ('have name=' + $_.Name + ' mac=' + $_.MacAddress + ' status=' + $_.Status + ' if=' + $_.ifIndex)\r\n"
        " }\r\n"
        " exit 1168\r\n"
        "}\r\n"
        "if($a.Status -ne 'Up'){Enable-NetAdapter -Name $a.Name -Confirm:$false -ErrorAction SilentlyContinue}\r\n"
        "for($i=0;$i -lt 20;$i++){\r\n"
        " $a=Get-NetAdapter -InterfaceIndex $a.ifIndex -IncludeHidden -ErrorAction SilentlyContinue\r\n"
        " if($a -and $a.Status -eq 'Up'){break}\r\n"
        " Start-Sleep -Milliseconds 250\r\n"
        "}\r\n"
        "if(-not $a -or $a.Status -ne 'Up'){\r\n"
        " Write-Output ('adapter not ready status=' + $a.Status + ' if=' + $a.ifIndex)\r\n"
        " exit 1168\r\n"
        "}\r\n"
        "Set-NetIPInterface -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -Dhcp Disabled -InterfaceMetric 9999 -ErrorAction Stop\r\n"
        /* Already right: leave it. A Game Streaming duplicate's user may be signing in
           over this link, and losing it mid-way signs them out again. */
        "if(-not (Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {$_.IPAddress -eq $env:ASB_NET_IP})){\r\n"
        " Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue\r\n"
        " New-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -IPAddress $env:ASB_NET_IP -PrefixLength 24 -PolicyStore ActiveStore -ErrorAction Stop | Out-Null\r\n"
        " Set-DnsClientServerAddress -InterfaceIndex $a.ifIndex -ResetServerAddresses -ErrorAction SilentlyContinue\r\n"
        "}\r\n"
        /* The share network is LAN-only: no route off it, and a firewall
           backstop in case DHCP or a stale config ever hands it a gateway.
           Scoped to this adapter, so a client VM's own NAT adapter is
           unaffected. Non-fatal: shares still work if the firewall is off. */
        "Get-NetRoute -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue\r\n"
        "try{\r\n"
        " function AsbN($s){$b=([ipaddress]$s).GetAddressBytes();[array]::Reverse($b);[BitConverter]::ToUInt32($b,0)}\r\n"
        " function AsbS($n){$b=[BitConverter]::GetBytes([uint32]$n);[array]::Reverse($b);([ipaddress]$b).ToString()}\r\n"
        " $lo=AsbN (($env:ASB_NET_IP -replace '\\.\\d+$','')+'.0'); $hi=$lo+255; $r=@()\r\n"
        " if($lo -gt 0){$r+=('0.0.0.0-'+(AsbS ($lo-1)))}\r\n"
        " if($hi -lt [uint32]::MaxValue){$r+=((AsbS ($hi+1))+'-255.255.255.255')}\r\n"
        " Get-NetFirewallRule -DisplayName 'AppSandbox Shared Network Egress' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue\r\n"
        " New-NetFirewallRule -DisplayName 'AppSandbox Shared Network Egress' -Direction Outbound -Action Block -InterfaceAlias $a.Name -RemoteAddress $r -Profile Any -ErrorAction Stop | Out-Null\r\n"
        "}catch{Write-Output ('egress block not applied: ' + $_.Exception.Message)}\r\n"
        "for($i=0;$i -lt 20;$i++){\r\n"
        " if(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {$_.IPAddress -eq $env:ASB_NET_IP}){exit 0}\r\n"
        " Start-Sleep -Milliseconds 250\r\n"
        "}\r\n"
        "Write-Output ('assigned ip not found=' + $env:ASB_NET_IP + ' if=' + $a.ifIndex)\r\n"
        "exit 1168\r\n";
    const wchar_t *path = L"C:\\ProgramData\\AppSandbox\\shared-net.ps1";
    wchar_t mac[64], ip[64]; int ec;
    if (!mac_a || !ip_a) return ERROR_INVALID_PARAMETER;
    strcpy_s(g_shared_nic_mac, sizeof(g_shared_nic_mac), mac_a);
    if (!MultiByteToWideChar(CP_UTF8, 0, mac_a, -1, mac, _countof(mac)) ||
        !MultiByteToWideChar(CP_UTF8, 0, ip_a, -1, ip, _countof(ip)))
        return (int)GetLastError();
    ec = write_agent_script(path, script); if (ec) return ec;
    SetEnvironmentVariableW(L"ASB_NET_MAC", mac);
    SetEnvironmentVariableW(L"ASB_NET_IP", ip);
    wcscpy_s(g_shared_management_ip, _countof(g_shared_management_ip), ip);
    {
        wchar_t *last_dot = wcsrchr(g_shared_management_ip, L'.');
        if (!last_dot) {
            SetEnvironmentVariableW(L"ASB_NET_MAC", NULL);
            SetEnvironmentVariableW(L"ASB_NET_IP", NULL);
            g_shared_management_ip[0] = L'\0';
            return ERROR_INVALID_ADDRESS;
        }
        wcscpy_s(last_dot + 1,
                 _countof(g_shared_management_ip) - (size_t)(last_dot + 1 - g_shared_management_ip),
                 L"1");
    }
    {
        char out[2048];
        ec = run_agent_powershell_out(path, 120000, out, sizeof(out));
        if (out[0]) {
            char *line = out, *nl;
            while (line && *line) {
                nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                if (*line) agent_log("shared_net: %s", line);
                if (!nl) break;
                line = nl + 1;
                while (*line == '\r' || *line == '\n') line++;
            }
        }
    }
    SetEnvironmentVariableW(L"ASB_NET_MAC", NULL);
    SetEnvironmentVariableW(L"ASB_NET_IP", NULL);
    return ec;
}

/* Configure the normal NAT adapter by excluding the private shared-resource
   adapter's MAC. The guest can have more than one adapter and Windows does
   not guarantee that the internet-facing one is named "Ethernet". */
static int configure_nat_nic(const char *ip_a, const char *prefix_a,
                             const char *gateway_a, const char *mac_a,
                             const char *dns_a)
{
    static const char script[] =
        "$ErrorActionPreference='Stop'\r\n"
        "Set-Location -LiteralPath ([Environment]::SystemDirectory)\r\n"
        "$shared=$env:ASB_SHARED_NIC_MAC -replace '[:-]',''\r\n"
        "$want=$env:ASB_NAT_NIC_MAC -replace '[:-]',''\r\n"
        "$a=$null\r\n"
        "$last=''\r\n"
        "$last_candidates=@()\r\n"
        "$fallback=$false\r\n"
        "for($i=0;$i -lt 60 -and -not $a;$i++){\r\n"
        " try{\r\n"
        "  $candidates=@(Get-NetAdapter -IncludeHidden -ErrorAction Stop | Where-Object {\r\n"
        "   $_.MacAddress -and ((($_.MacAddress) -replace '[:-]','') -ne $shared)\r\n"
        "  })\r\n"
        " }catch{$last=$_.Exception.Message;$candidates=@()}\r\n"
        " if($candidates.Count -gt 0){$last_candidates=$candidates}\r\n"
        " if($want){$a=$candidates | Where-Object {(($_.MacAddress) -replace '[:-]','') -eq $want} | Select-Object -First 1}\r\n"
        " else{\r\n"
        "  $a=$candidates | Where-Object {$_.Status -eq 'Up'} | Select-Object -First 1\r\n"
        "  if(-not $a){$a=$candidates | Where-Object {$_.Status -ne 'Disabled'} | Select-Object -First 1}\r\n"
        "  if(-not $a){$a=$candidates | Select-Object -First 1}\r\n"
        " }\r\n"
        " if(-not $a -and $want -and $candidates.Count -eq 1 -and $candidates[0].Status -eq 'Up' -and $i -ge 10){$a=$candidates[0];$fallback=$true}\r\n"
        " if(-not $a){Start-Sleep -Milliseconds 500}\r\n"
        "}\r\n"
        "if(-not $a -and $want){\r\n"
        " $fallbacks=@($last_candidates | Where-Object {$_.Status -ne 'Disabled'})\r\n"
        " $up=@($fallbacks | Where-Object {$_.Status -eq 'Up'})\r\n"
        " if($up.Count -eq 1){$a=$up[0];$fallback=$true}\r\n"
        " elseif($up.Count -eq 0 -and $fallbacks.Count -eq 1){$a=$fallbacks[0];$fallback=$true}\r\n"
        "}\r\n"
        "if(-not $a){if($want){Write-Output ('NAT adapter with MAC ' + $want + ' not found')}else{Write-Output 'normal NAT adapter not found'};if($last){Write-Output ('adapter query unavailable: ' + $last)};exit 1168}\r\n"
        "if($fallback){Write-Output ('requested NAT adapter MAC ' + $want + ' not present; using only non-shared adapter')}\r\n"
        "Write-Output ('selected adapter=' + $a.Name + ' mac=' + $a.MacAddress + ' status=' + $a.Status + ' if=' + $a.ifIndex)\r\n"
        "try{\r\n"
        " if($a.Status -eq 'Disabled'){Enable-NetAdapter -Name $a.Name -Confirm:$false -ErrorAction Stop;Start-Sleep -Milliseconds 500}\r\n"
        " Set-NetIPInterface -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -Dhcp Disabled -InterfaceMetric 10 -ErrorAction Stop\r\n"
        " Get-NetRoute -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue\r\n"
        " Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue\r\n"
        " New-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -IPAddress $env:ASB_NAT_IP -PrefixLength ([int]$env:ASB_NAT_PREFIX) -DefaultGateway $env:ASB_NAT_GATEWAY -PolicyStore ActiveStore -ErrorAction Stop | Out-Null\r\n"
        " $dns=@($env:ASB_NAT_GATEWAY,'8.8.8.8'); if($env:ASB_NAT_DNS){$dns=@($env:ASB_NAT_DNS -split ',')}\r\n"
        " Set-DnsClientServerAddress -InterfaceIndex $a.ifIndex -ServerAddresses $dns -ErrorAction Stop\r\n"
        " for($i=0;$i -lt 20;$i++){\r\n"
        "  $ip=Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {$_.IPAddress -eq $env:ASB_NAT_IP}\r\n"
        "  $route=Get-NetRoute -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Where-Object {$_.NextHop -eq $env:ASB_NAT_GATEWAY}\r\n"
        "  if($ip -and $route){Write-Output ('configured adapter=' + $a.Name + ' if=' + $a.ifIndex);exit 0}\r\n"
        "  Start-Sleep -Milliseconds 250\r\n"
        " }\r\n"
        " Write-Output ('NAT IP or gateway route not present on adapter=' + $a.Name + ' if=' + $a.ifIndex)\r\n"
        " exit 1168\r\n"
        "}catch{\r\n"
        " $code=([int]$_.Exception.HResult -band 0xffff);if($code -eq 0){$code=1}\r\n"
        " Write-Output ('NAT configuration failed adapter=' + $a.Name + ' if=' + $a.ifIndex + ' code=' + $code + ': ' + $_.Exception.Message)\r\n"
        " exit $code\r\n"
        "}\r\n";
    const wchar_t *path = L"C:\\ProgramData\\AppSandbox\\nat-net.ps1";
    wchar_t ip[64], prefix[16], gateway[64], mac[64];
    char out[2048];
    int ec;

    if (!ip_a || !prefix_a || !gateway_a ||
        !MultiByteToWideChar(CP_UTF8, 0, ip_a, -1, ip, _countof(ip)) ||
        !MultiByteToWideChar(CP_UTF8, 0, prefix_a, -1, prefix, _countof(prefix)) ||
        !MultiByteToWideChar(CP_UTF8, 0, gateway_a, -1, gateway, _countof(gateway)) ||
        !MultiByteToWideChar(CP_UTF8, 0, mac_a ? mac_a : "", -1, mac, _countof(mac)))
        return ERROR_INVALID_PARAMETER;
    ec = write_agent_script(path, script); if (ec) return ec;
    SetEnvironmentVariableA("ASB_SHARED_NIC_MAC", g_shared_nic_mac);
    SetEnvironmentVariableW(L"ASB_NAT_IP", ip);
    SetEnvironmentVariableW(L"ASB_NAT_PREFIX", prefix);
    SetEnvironmentVariableW(L"ASB_NAT_GATEWAY", gateway);
    SetEnvironmentVariableW(L"ASB_NAT_NIC_MAC", mac);
    SetEnvironmentVariableA("ASB_NAT_DNS", dns_a && dns_a[0] ? dns_a : NULL);
    ec = run_agent_powershell_out(path, 120000, out, sizeof(out));
    if (out[0]) {
        char *line = out, *nl;
        while (line && *line) {
            nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (*line) {
                agent_log("nat_net: %s", line);
                agent_log_to_host("nat_net: %s", line);
            }
            if (!nl) break;
            line = nl + 1;
            while (*line == '\r' || *line == '\n') line++;
        }
    }
    SetEnvironmentVariableA("ASB_SHARED_NIC_MAC", NULL);
    SetEnvironmentVariableW(L"ASB_NAT_IP", NULL);
    SetEnvironmentVariableW(L"ASB_NAT_PREFIX", NULL);
    SetEnvironmentVariableW(L"ASB_NAT_GATEWAY", NULL);
    SetEnvironmentVariableW(L"ASB_NAT_NIC_MAC", NULL);
    SetEnvironmentVariableA("ASB_NAT_DNS", NULL);
    return ec;
}

/* New-SmbGlobalMapping publishes the drive to every user and service inside
   the guest. The password exists only in this service's inherited child
   environment and is never placed on a command line or written to disk. */
static int map_smb_drive_global(const char *letter_a, const char *host_a,
                                const char *share_a, const char *user_a,
                                const char *password_a)
{
    static const char script[] =
        "$ErrorActionPreference='Stop'\r\n"
        "Set-Location -LiteralPath ([Environment]::SystemDirectory)\r\n"
        "$local=$env:ASB_SMB_LOCAL\r\n"
        "$remote=$env:ASB_SMB_REMOTE\r\n"
        "$root=$local+'\\'\r\n"
        "$last=''\r\n"
        "$h=($remote -split '\\\\')[2]\r\n"
        "$secure=ConvertTo-SecureString $env:ASB_SMB_PASSWORD -AsPlainText -Force\r\n"
        "$u=$env:ASB_SMB_USER\r\n"
        "if(-not $u.Contains('\\')){$u=$h+'\\'+$u}\r\n"
        "$cred=[System.Management.Automation.PSCredential]::new($u,$secure)\r\n"
        "$code=1\r\n"
        "for($attempt=0;$attempt -lt 8;$attempt++){\r\n"
        " try{\r\n"
        "  $existing=Get-SmbGlobalMapping -LocalPath $local -ErrorAction SilentlyContinue\r\n"
        "  if($existing){\r\n"
        "   if($existing.RemotePath -ieq $remote){\r\n"
        "    try{$null=Get-Item -LiteralPath $root -Force -ErrorAction Stop;exit 0}catch{$last=$_.Exception.Message}\r\n"
        "   }\r\n"
        "   Remove-SmbGlobalMapping -LocalPath $local -Force -ErrorAction Stop\r\n"
        "   for($i=0;$i -lt 20 -and (Get-SmbGlobalMapping -LocalPath $local -ErrorAction SilentlyContinue);$i++){Start-Sleep -Milliseconds 250}\r\n"
        "  }\r\n"
        "  if(Test-Path -LiteralPath $root -ErrorAction SilentlyContinue){exit 85}\r\n"
        "  New-SmbGlobalMapping -LocalPath $local -RemotePath $remote -Credential $cred -Persistent $false -RequireIntegrity $true -ErrorAction Stop | Out-Null\r\n"
        "  for($i=0;$i -lt 20;$i++){\r\n"
        "   try{$null=Get-Item -LiteralPath $root -Force -ErrorAction Stop;exit 0}catch{$last=$_.Exception.Message}\r\n"
        "   Start-Sleep -Milliseconds 250\r\n"
        "  }\r\n"
        "  Remove-SmbGlobalMapping -LocalPath $local -Force -ErrorAction SilentlyContinue\r\n"
        " }catch{\r\n"
        "  $code=([int]$_.Exception.HResult -band 0xffff);if($code -eq 0){$code=1};$last=$_.Exception.Message\r\n"
        "  Remove-SmbGlobalMapping -LocalPath $local -Force -ErrorAction SilentlyContinue\r\n"
        " }\r\n"
        " if($attempt -lt 7){Start-Sleep -Milliseconds 1000}\r\n"
        "}\r\n"
        "if(-not $last){$last='mapped drive was not readable'}\r\n"
        "Write-Output ('map failed user=' + $u + ' target=' + $remote + ': ' + $last)\r\n"
        "$t=Test-NetConnection -ComputerName $h -Port 445 -WarningAction SilentlyContinue\r\n"
        "Write-Output ('tcp445=' + $t.TcpTestSucceeded + ' src=' + $t.SourceAddress.IPAddress)\r\n"
        "foreach($ln in @('Microsoft-Windows-SMBClient/Connectivity','Microsoft-Windows-SMBClient/Security','Microsoft-Windows-SMBClient/Operational')){\r\n"
        " Get-WinEvent -LogName $ln -MaxEvents 3 -ErrorAction SilentlyContinue | ForEach-Object {\r\n"
        "  Write-Output ('evt ' + $ln.Split('/')[-1] + ' id=' + $_.Id + ' ' + (($_.Message -replace '\\s+',' ')).Substring(0,[Math]::Min(220,($_.Message -replace '\\s+',' ').Length)))\r\n"
        " }\r\n"
        "}\r\n"
        "exit $code\r\n";
    const wchar_t *path = L"C:\\ProgramData\\AppSandbox\\shared-smb-map.ps1";
    wchar_t local[4], remote[256], user[256], password[256]; int ec;
    if (!letter_a || strlen(letter_a) != 1 || !host_a || !share_a || !user_a || !password_a)
        return ERROR_INVALID_PARAMETER;
    swprintf_s(local, _countof(local), L"%c:", towupper((wchar_t)(unsigned char)letter_a[0]));
    if (!MultiByteToWideChar(CP_UTF8, 0, user_a, -1, user, _countof(user)) ||
        !MultiByteToWideChar(CP_UTF8, 0, password_a, -1, password, _countof(password)))
        return (int)GetLastError();
    swprintf_s(remote, _countof(remote), L"\\\\%S\\%S", host_a, share_a);
    ec = write_agent_script(path, script); if (ec) goto done;
    SetEnvironmentVariableW(L"ASB_SMB_LOCAL", local);
    SetEnvironmentVariableW(L"ASB_SMB_REMOTE", remote);
    SetEnvironmentVariableW(L"ASB_SMB_USER", user);
    SetEnvironmentVariableW(L"ASB_SMB_PASSWORD", password);
    {
        char out[8192];
        ec = run_agent_powershell_out(path, 90000, out, sizeof(out));
        if (out[0]) {
            char *line = out, *nl;
            while (line && *line) {
                nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                if (*line) {
                    agent_log("shared_smb_map: %s", line);
                    agent_log_to_host("shared_smb_map: %s", line);
                }
                if (!nl) break;
                line = nl + 1;
                while (*line == '\r' || *line == '\n') line++;
            }
        }
    }
    SetEnvironmentVariableW(L"ASB_SMB_LOCAL", NULL);
    SetEnvironmentVariableW(L"ASB_SMB_REMOTE", NULL);
    SetEnvironmentVariableW(L"ASB_SMB_USER", NULL);
    SetEnvironmentVariableW(L"ASB_SMB_PASSWORD", NULL);
done:
    if (ec == ERROR_SUCCESS)
        notify_shell_drive_added(letter_a);
    SecureZeroMemory(password, sizeof(password));
    SecureZeroMemory(user, sizeof(user));
    return ec;
}

/* Drops a mapping made by map_smb_drive_global. Reports success when the drive
   is already gone so the host can call it unconditionally (a resource that was
   never mapped, a VM that booted without it, a repeated request). */
static int unmap_smb_drive_global(const char *letter_a)
{
    static const char script[] =
        "$ErrorActionPreference='SilentlyContinue'\r\n"
        "$local=$env:ASB_SMB_LOCAL\r\n"
        "Remove-SmbGlobalMapping -LocalPath $local -Force -ErrorAction SilentlyContinue\r\n"
        "for($i=0;$i -lt 20 -and (Get-SmbGlobalMapping -LocalPath $local -ErrorAction SilentlyContinue);$i++){Start-Sleep -Milliseconds 250}\r\n"
        "if(Get-SmbGlobalMapping -LocalPath $local -ErrorAction SilentlyContinue){exit 1}\r\n"
        "exit 0\r\n";
    const wchar_t *path = L"C:\\ProgramData\\AppSandbox\\shared-smb-unmap.ps1";
    wchar_t local[4];
    int ec;
    if (!letter_a || strlen(letter_a) != 1) return ERROR_INVALID_PARAMETER;
    swprintf_s(local, _countof(local), L"%c:",
               towupper((wchar_t)(unsigned char)letter_a[0]));
    ec = write_agent_script(path, script);
    if (ec) return ec;
    SetEnvironmentVariableW(L"ASB_SMB_LOCAL", local);
    ec = run_agent_powershell(path, 45000);
    SetEnvironmentVariableW(L"ASB_SMB_LOCAL", NULL);
    return ec;
}

/* ---- Persistent client handler ---- */

/* Disable the Hyper-V synthetic video adapter (if present).
   Hardware ID: VMBUS\{da0a7802-e377-4aac-8e77-0558eb1073f8}
   Silently does nothing if the device doesn't exist. */
static void disable_hyperv_video(AsbConn *notify_sock)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    wchar_t hw_id[512];
    CONFIGRET cr;
    static const wchar_t *TARGET_HWID = L"VMBUS\\{DA0A7802-E377-4AAC-8E77-0558EB1073F8}";

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        agent_log("disable_hyperv_video: SetupDiGetClassDevs(PRESENT) failed, trying ALLCLASSES.");
        devs = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
        if (devs == INVALID_HANDLE_VALUE) return;
    }

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        ULONG dev_status = 0, dev_problem = 0;
        hw_id[0] = L'\0';
        CM_Get_Device_IDW(dev_info.DevInst, hw_id, 512, 0);

        if (_wcsnicmp(hw_id, TARGET_HWID, wcslen(TARGET_HWID)) != 0)
            continue;

        /* Check if already disabled */
        cr = CM_Get_DevNode_Status(&dev_status, &dev_problem, dev_info.DevInst, 0);
        if (cr == CR_SUCCESS && !(dev_status & DN_STARTED) && (dev_problem == CM_PROB_DISABLED)) {
            agent_log("Hyper-V Video adapter already disabled: %ls", hw_id);
            if (notify_sock != NULL)
                send_line(notify_sock, "hyperv_video:already_disabled");
            SetupDiDestroyDeviceInfoList(devs);
            return;
        }

        agent_log("Disabling Hyper-V Video adapter: %ls (status=0x%lX problem=%lu)", hw_id, dev_status, dev_problem);
        if (notify_sock != NULL)
            send_line(notify_sock, "hyperv_video:disabling");

        cr = CM_Disable_DevNode(dev_info.DevInst, 0);
        if (cr == CR_SUCCESS) {
            agent_log("Hyper-V Video adapter disabled.");
            if (notify_sock != NULL)
                send_line(notify_sock, "hyperv_video:disabled");
        } else {
            agent_log("Hyper-V Video disable failed (%lu).", cr);
        }
        SetupDiDestroyDeviceInfoList(devs);
        return;
    }

    agent_log("Hyper-V Video adapter not found (enumerated %lu devices).", idx);
    if (notify_sock != NULL)
        send_line(notify_sock, "hyperv_video:not_found");
    SetupDiDestroyDeviceInfoList(devs);
}

/* Forward declaration — defined after SSH proxy section */
static void handle_ssh_enable(AsbConn *client, const char *tag);

static BOOL grow_root_partition(void)
{
    wchar_t cmd[1024] =
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
        L"\"$d=$env:SystemDrive.TrimEnd(':'); "
        L"$p=Get-Partition -DriveLetter $d -ErrorAction Stop; "
        L"$s=Get-PartitionSupportedSize -DriveLetter $d -ErrorAction Stop; "
        L"if($p.Size -lt $s.SizeMax){Resize-Partition -DriveLetter $d -Size $s.SizeMax -ErrorAction Stop}\"";
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1, wait_result;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        agent_log("grow_root: PowerShell launch failed (%lu)", GetLastError());
        return FALSE;
    }
    wait_result = WaitForSingleObject(pi.hProcess, 60000);
    if (wait_result == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exit_code);
    else
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    agent_log("grow_root: wait=%lu exit=%lu", wait_result, exit_code);
    return wait_result == WAIT_OBJECT_0 && exit_code == 0;
}

static BOOL appliance_token_valid(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    if (!p || !*p) return FALSE;
    for (; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '$') return FALSE;
    return TRUE;
}

static DWORD run_powershell_wait(const wchar_t *script, DWORD timeout_ms)
{
    wchar_t command[4096];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    DWORD exit_code = ERROR_GEN_FAILURE;
    swprintf_s(command, _countof(command),
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"%s\"",
        script);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) return GetLastError();
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exit_code);
    else { TerminateProcess(pi.hProcess, ERROR_TIMEOUT); exit_code = ERROR_TIMEOUT; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return exit_code;
}

static DWORD appliance_prepare_storage(char *thumbprint, size_t thumbprint_chars)
{
    FILE *file = NULL;
    DWORD rc;
    char storage_out[2048];
    if (thumbprint && thumbprint_chars) thumbprint[0] = '\0';
    if (!g_shared_management_ip[0]) return ERROR_INVALID_ADDRESS;
    SetEnvironmentVariableW(L"ASB_MANAGEMENT_HOST", g_shared_management_ip);
    rc = run_powershell_capture(
        L"try{ "
        L"$root='C:\\AppSandboxData'; $d=Get-Disk | Where-Object PartitionStyle -eq 'RAW' | Sort-Object Number | Select-Object -Last 1; "
        L"if($d){Initialize-Disk -Number $d.Number -PartitionStyle GPT -ErrorAction Stop; "
        L"$p=New-Partition -DiskNumber $d.Number -UseMaximumSize -ErrorAction Stop; Format-Volume -Partition $p -FileSystem NTFS -NewFileSystemLabel AppSandboxShared -Confirm:$false -ErrorAction Stop | Out-Null}; "
        L"$v=Get-Volume -FileSystemLabel AppSandboxShared -ErrorAction Stop; "
        L"$part=$v | Get-Partition; "
        L"$have=@($part.AccessPaths | ForEach-Object {$_.TrimEnd('\\')}); "
        L"if($have -notcontains $root){ "
        L"Get-SmbShare -ErrorAction SilentlyContinue | Where-Object {$_.Path -like ($root+'*')} | Remove-SmbShare -Force -ErrorAction SilentlyContinue; "
        L"if(Test-Path $root){Write-Output 'storage: clearing unmounted directory on the OS disk'; "
        L"Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction Stop}; "
        L"New-Item -ItemType Directory -Force -Path $root | Out-Null; "
        L"Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AccessPath $root -ErrorAction Stop; "
        L"Write-Output 'storage: restarting LanmanServer so it sees the new mount point'; "
        L"Restart-Service LanmanServer -Force -ErrorAction SilentlyContinue}; "
        L"$part=Get-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber; "
        L"$have=@($part.AccessPaths | ForEach-Object {$_.TrimEnd('\\')}); "
        L"if($have -notcontains $root){Write-Output ('storage: mount verify failed; paths=' + ($have -join ',')); exit 4321}; "
        L"Write-Output ('storage: data volume ' + [math]::Round($v.Size/1GB) + ' GB mounted at ' + $root); "
        L"Set-SmbServerConfiguration -RequireSecuritySignature $true -EnableSecuritySignature $true -Confirm:$false -ErrorAction Stop; "
        L"$cert=Get-ChildItem Cert:\\LocalMachine\\My | Where-Object FriendlyName -eq 'AppSandbox Shared Appliance Management' | Select-Object -First 1; "
        L"if(-not $cert){$cert=New-SelfSignedCertificate -DnsName 'AppSandbox.SharedAppliance' -FriendlyName 'AppSandbox Shared Appliance Management' -CertStoreLocation 'Cert:\\LocalMachine\\My' -KeyExportPolicy NonExportable -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10) -ErrorAction Stop}; "
        /* Remote management is only for the Terminal button. On a domain
           controller WinRM's own local connection fails until Kerberos is up
           after boot; that must not fail readiness (and so the shares). */
        L"try{ "
        L"Enable-PSRemoting -SkipNetworkProfileCheck -Force -ErrorAction Stop; "
        L"Get-ChildItem WSMan:\\localhost\\Listener | Where-Object {$_.Keys -contains 'Transport=HTTPS'} | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue; "
        L"New-Item -Path WSMan:\\localhost\\Listener -Transport HTTPS -Address * -CertificateThumbPrint $cert.Thumbprint -Force -ErrorAction Stop | Out-Null "
        L"}catch{Write-Output 'storage: remote management is not ready yet; it is set up again at the next readiness check'}; "
        L"if(Get-NetFirewallRule -DisplayName 'AppSandbox Shared Appliance Management' -ErrorAction SilentlyContinue){Remove-NetFirewallRule -DisplayName 'AppSandbox Shared Appliance Management'}; "
        L"New-NetFirewallRule -DisplayName 'AppSandbox Shared Appliance Management' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 5986 -RemoteAddress $env:ASB_MANAGEMENT_HOST -Profile Any | Out-Null; "
        /* The private share adapter has no gateway, so Windows classifies it as
           a public network and the built-in File and Printer Sharing rules stay
           inactive. Without an explicit rule every client mapping times out. */
        L"$smbnet=($env:ASB_MANAGEMENT_HOST -replace '\\.\\d+$','.0')+'/24'; "
        L"if(Get-NetFirewallRule -DisplayName 'AppSandbox Shared Appliance SMB' -ErrorAction SilentlyContinue){Remove-NetFirewallRule -DisplayName 'AppSandbox Shared Appliance SMB'}; "
        L"New-NetFirewallRule -DisplayName 'AppSandbox Shared Appliance SMB' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 445 -RemoteAddress $smbnet -Profile Any | Out-Null; "
        L"New-Item -ItemType Directory -Force -Path 'C:\\ProgramData\\AppSandbox' | Out-Null; "
        L"[IO.File]::WriteAllText('C:\\ProgramData\\AppSandbox\\management-cert.thumbprint',$cert.Thumbprint,[Text.Encoding]::ASCII); "
        L"exit 0}catch{Write-Output ('storage error: ' + $_.Exception.Message); exit 1}",
        120000, storage_out, sizeof(storage_out));
    SetEnvironmentVariableW(L"ASB_MANAGEMENT_HOST", NULL);
    if (storage_out[0]) {
        char *line = storage_out, *nl;
        while (line && *line) {
            nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (*line) {
                agent_log("appliance_ready: %s", line);
                agent_log_to_host("appliance_ready: %s", line);
            }
            if (!nl) break;
            line = nl + 1;
            while (*line == '\r' || *line == '\n') line++;
        }
    }
    if (rc != ERROR_SUCCESS || !thumbprint || !thumbprint_chars) return rc;
    if (fopen_s(&file, "C:\\ProgramData\\AppSandbox\\management-cert.thumbprint", "rb") != 0 || !file)
        return ERROR_FILE_NOT_FOUND;
    if (!fgets(thumbprint, (int)thumbprint_chars, file)) rc = ERROR_INVALID_DATA;
    fclose(file);
    if (rc == ERROR_SUCCESS) {
        char *end = strpbrk(thumbprint, "\r\n");
        if (end) *end = '\0';
        if (!thumbprint[0]) rc = ERROR_INVALID_DATA;
    }
    return rc;
}

static DWORD appliance_set_account(const char *arguments)
{
    char copy[512], *separator;
    wchar_t user[128], password[256];
    DWORD rc;
    strcpy_s(copy, sizeof(copy), arguments);
    separator = strchr(copy, ':');
    if (!separator) return ERROR_INVALID_PARAMETER;
    *separator++ = '\0';
    if (!appliance_token_valid(copy) || !*separator) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_UTF8, 0, copy, -1, user, _countof(user));
    MultiByteToWideChar(CP_UTF8, 0, separator, -1, password, _countof(password));
    SetEnvironmentVariableW(L"ASB_APPLIANCE_USER", user);
    SetEnvironmentVariableW(L"ASB_APPLIANCE_PASSWORD", password);
    /* Uses the WinNT ADSI provider rather than New-LocalUser/Set-LocalUser:
       those live in the Microsoft.PowerShell.LocalAccounts module, which is
       absent from trimmed Windows images, and a missing cmdlet exits the whole
       script with a bare 1 that says nothing. ADSI is part of the OS.
       No double quotes: the caller wraps this script in them.
       0x10000 = DONT_EXPIRE_PASSWD, 0x2 = ACCOUNTDISABLE.
       On a domain controller this lands in Active Directory, which is still
       starting for a minute or two after boot, so it retries for ~150 s. */
    rc = run_powershell_wait(
        L"$ok=$false; $err=$null; for($k=0;$k -lt 30 -and -not $ok;$k++){ try{"
        L"$n=$env:ASB_APPLIANCE_USER; $p=$env:ASB_APPLIANCE_PASSWORD; $c=$env:COMPUTERNAME; "
        L"$root=[ADSI]('WinNT://'+$c); $u=$null; "
        L"try{$t=[ADSI]('WinNT://'+$c+'/'+$n+',user'); if($t.Name){$u=$t}}catch{$u=$null}; "
        L"if(-not $u){$u=$root.Create('User',$n)}; "
        L"$u.SetPassword($p); $u.SetInfo(); "
        L"$f=[int]$u.UserFlags.Value; "
        L"$u.UserFlags.Value=(($f -bor 0x10000) -band (-bnot 0x2)); $u.SetInfo(); $ok=$true"
        L"}catch{$err=$_; Start-Sleep -Seconds 5} }; "
        L"if(-not $ok){$e=([int]$err.Exception.HResult -band 0xffff); if($e -eq 0){$e=1}; exit $e}; "
        L"try{$g=[ADSI]('WinNT://'+$env:COMPUTERNAME+'/Administrators,group'); "
        L"$g.Remove(('WinNT://'+$env:COMPUTERNAME+'/'+$env:ASB_APPLIANCE_USER+',user'))}catch{}; "
        L"exit 0", 180000);
    if (rc == ERROR_SUCCESS) {
        BYTE sid[SECURITY_MAX_SID_SIZE];
        wchar_t domain[256];
        DWORD sid_bytes = sizeof(sid), domain_chars = _countof(domain);
        SID_NAME_USE sid_type;
        LSA_OBJECT_ATTRIBUTES attributes;
        LSA_HANDLE policy = NULL;
        LSA_UNICODE_STRING rights[2];
        wchar_t deny_interactive[] = L"SeDenyInteractiveLogonRight";
        wchar_t deny_remote[] = L"SeDenyRemoteInteractiveLogonRight";
        if (!LookupAccountNameW(NULL, user, sid, &sid_bytes, domain, &domain_chars, &sid_type)) {
            rc = GetLastError();
        } else {
            ZeroMemory(&attributes, sizeof(attributes));
            attributes.Length = sizeof(attributes);
            rights[0].Buffer = deny_interactive;
            rights[0].Length = (USHORT)(wcslen(deny_interactive) * sizeof(wchar_t));
            rights[0].MaximumLength = rights[0].Length + sizeof(wchar_t);
            rights[1].Buffer = deny_remote;
            rights[1].Length = (USHORT)(wcslen(deny_remote) * sizeof(wchar_t));
            rights[1].MaximumLength = rights[1].Length + sizeof(wchar_t);
            {
                NTSTATUS status = LsaOpenPolicy(NULL, &attributes,
                    POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT, &policy);
                if (status == 0)
                    status = LsaAddAccountRights(policy, sid, rights, _countof(rights));
                if (policy) LsaClose(policy);
                if (status != 0) rc = LsaNtStatusToWinError(status);
            }
        }
    }
    SetEnvironmentVariableW(L"ASB_APPLIANCE_PASSWORD", NULL);
    SetEnvironmentVariableW(L"ASB_APPLIANCE_USER", NULL);
    SecureZeroMemory(password, sizeof(password));
    SecureZeroMemory(copy, sizeof(copy));
    return rc;
}

static DWORD appliance_reconcile_share(const char *arguments)
{
    char copy[1024], *separator, *groups;
    wchar_t share[128], directory[128], script[4096];
    BOOL read_only;
    strcpy_s(copy, sizeof(copy), arguments);
    separator = strchr(copy, ':');
    if (!separator) return ERROR_INVALID_PARAMETER;
    *separator++ = '\0';
    /* Optional: base64 list of the domain groups allowed (domain controller only). */
    groups = strchr(separator, ':');
    if (groups) *groups++ = '\0';
    if (!appliance_token_valid(copy) ||
        (strcmp(separator, "ro") != 0 && strcmp(separator, "rw") != 0) ||
        (groups && !domain_field_ok(groups, "+/=", 800)))
        return ERROR_INVALID_PARAMETER;
    SetEnvironmentVariableA("ASB_SHARE_GROUPS", groups && *groups ? groups : NULL);
    read_only = strcmp(separator, "ro") == 0;
    MultiByteToWideChar(CP_UTF8, 0, copy, -1, share, _countof(share));
    wcscpy_s(directory, _countof(directory), share);
    { wchar_t *dollar = wcschr(directory, L'$'); if (dollar) *dollar = L'\0'; }
    swprintf_s(script, _countof(script),
        L"try{ "
        L"$p='C:\\AppSandboxData\\%s'; $rights='%s'; New-Item -ItemType Directory -Force -Path $p | Out-Null; "
        L"$acl=Get-Acl -LiteralPath $p; "
        L"$rule=New-Object System.Security.AccessControl.FileSystemAccessRule("
        L"'AppSandboxShare',$rights,'ContainerInherit,ObjectInherit','None','Allow'); "
        L"$acl.SetAccessRule($rule); $who=@('AppSandboxShare'); "
        /* Group-restricted: the share admits only these groups (and the
           host's service account), so members map it with their own login. */
        L"if($env:ASB_SHARE_GROUPS){ foreach($g in (([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:ASB_SHARE_GROUPS))) -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })){ "
        L"$q=$env:USERDOMAIN+'\\'+$g; $acl.SetAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($q,$rights,'ContainerInherit,ObjectInherit','None','Allow'))); $who+=$q } }; "
        L"Set-Acl -LiteralPath $p -AclObject $acl; "
        L"if(Get-SmbShare -Name '%s' -ErrorAction SilentlyContinue){Remove-SmbShare -Name '%s' -Force}; "
        L"New-SmbShare -Name '%s' -Path $p -%sAccess $who -EncryptData:$false | Out-Null; "
        L"$s=Get-SmbShare -Name '%s' -ErrorAction Stop; "
        L"Write-Output ('share=' + $s.Name + ' path=' + $s.Path + ' scope=' + $s.ScopeName + ' pathok=' + (Test-Path -LiteralPath $s.Path)); "
        L"exit 0}catch{Write-Output ('share error: ' + $_.Exception.Message); exit 1}",
        directory, read_only ? L"ReadAndExecute" : L"Modify",
        share, share, share, read_only ? L"Read" : L"Full", share);
    {
        char out[1024];
        DWORD rc = (DWORD)run_powershell_capture(script, 30000, out, sizeof(out));
        SetEnvironmentVariableA("ASB_SHARE_GROUPS", NULL);
        if (out[0]) {
            char *line = out, *nl;
            while (line && *line) {
                nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                if (*line) {
                    agent_log("appliance_reconcile: %s", line);
                    agent_log_to_host("appliance_reconcile: %s", line);
                }
                if (!nl) break;
                line = nl + 1;
                while (*line == '\r' || *line == '\n') line++;
            }
        }
        return rc;
    }
}

static DWORD appliance_purge_share(const char *share_arg)
{
    wchar_t share[128], directory[128], script[1024];
    if (!appliance_token_valid(share_arg)) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_UTF8, 0, share_arg, -1, share, _countof(share));
    wcscpy_s(directory, _countof(directory), share);
    { wchar_t *dollar = wcschr(directory, L'$'); if (dollar) *dollar = L'\0'; }
    swprintf_s(script, _countof(script),
        L"if(Get-SmbShare -Name '%s' -ErrorAction SilentlyContinue){Remove-SmbShare -Name '%s' -Force}; Remove-Item -LiteralPath 'C:\\AppSandboxData\\%s' -Recurse -Force -ErrorAction SilentlyContinue",
        share, share, directory);
    return run_powershell_wait(script, 60000);
}

static DWORD appliance_remove_share(const char *share_arg)
{
    wchar_t share[128], script[512];
    if (!appliance_token_valid(share_arg)) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_UTF8, 0, share_arg, -1, share, _countof(share));
    swprintf_s(script, _countof(script),
        L"if(Get-SmbShare -Name '%s' -ErrorAction SilentlyContinue){Remove-SmbShare -Name '%s' -Force}",
        share, share);
    return run_powershell_wait(script, 30000);
}

static void handle_client(AsbConn *client)
{
    char buf[2048];
    int n;
    DWORD heartbeat_interval = 5000;     /* ms between heartbeats */
    DWORD device_check_interval = 20000; /* ms between VDD/Hyper-V device checks */
    DWORD last_heartbeat;
    DWORD last_device_check;

    agent_log("Client connected.");
    g_client_sock = client;

    /* Set timeouts so recv/send don't block forever if host disconnects (no-op on ivshmem) */
    asb_set_timeout(client, 10000, 10000);

    /* Send hello */
    if (send_line(client, "hello") <= 0) {
        agent_log("Failed to send hello.");
        asb_close(client);
        g_client_sock = NULL;
        return;
    }

    /* Report IDD driver status to host (force: always send on connect so a reconnect re-syncs) */
    report_idd_status(client, 1);

    /* Initial device checks */
    ensure_vdd_running();
    ensure_hyperv_video_disabled(client);
    keep_only_appsandbox_display();

    last_heartbeat = GetTickCount();
    last_device_check = last_heartbeat;

    /* Persistent connection loop */
    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        int ret;
        DWORD now;

        ret = asb_poll(client, 1000);   /* 1s: 1=data, 0=timeout, <0=closed/error */
        if (ret < 0) break;

        /* Send heartbeat if interval elapsed */
        now = GetTickCount();
        if (now - last_heartbeat >= heartbeat_interval) {
            if (send_line(client, "heartbeat") <= 0) {
                agent_log("Heartbeat send failed, client disconnected.");
                break;
            }
            last_heartbeat = now;
        }

        /* Device checks on a slower cadence (VDD recovery is expensive) */
        if (now - last_device_check >= device_check_interval) {
            last_device_check = now;
            ensure_vdd_running();
            ensure_hyperv_video_disabled(client);
            keep_only_appsandbox_display();
            report_idd_status(client, 0);   /* re-report the (possibly self-healed) VDD state; on-change only */
        }


        if (ret == 0) continue; /* select timeout, no data */

        /* Read command */
        n = recv_line(client, buf, sizeof(buf));
        if (n <= 0) {
            agent_log("Client disconnected.");
            break;
        }

        /* Extract sequence tag (e.g. "42:ping" → tag="42:", cmd="ping") */
        {
            char *cmd = buf;
            char tag[32] = {0};
            char *colon = strchr(buf, ':');
            if (colon && colon > buf && colon - buf < 16) {
                /* Check all chars before colon are digits */
                char *p;
                BOOL is_seq = TRUE;
                for (p = buf; p < colon; p++) {
                    if (*p < '0' || *p > '9') { is_seq = FALSE; break; }
                }
                if (is_seq) {
                    int tlen = (int)(colon - buf + 1);
                    memcpy(tag, buf, tlen);
                    tag[tlen] = '\0';
                    cmd = colon + 1;
                }
            }

        if (strncmp(cmd, "shared_smb_map:", 15) == 0 ||
            strncmp(cmd, "appliance_account:", 18) == 0)
            agent_log("Command: shared_smb_map:<redacted>");
        else if (strncmp(cmd, "domain_", 7) == 0)
            agent_log("Command: %.20s<redacted>", cmd);
        else if (strncmp(cmd, "lab_chunk:", 10) == 0 ||
                 strcmp(cmd, "session_status") == 0 ||
                 strncmp(cmd, "session_catalog_read:", 21) == 0 ||
                 strcmp(cmd, "session_image_status") == 0)
            ;   /* transfers and polls, not worth a log line each */
        else
            agent_log("Command: %s", buf);

        /* Helper macro: send response with tag prefix */
        #define REPLY(msg) send_reply(client, tag, msg)

        if (strcmp(cmd, "ping") == 0) {
            REPLY("ok");
        }
        else if (strcmp(cmd, "appliance_ready") == 0) {
            char thumbprint[128];
            DWORD ec = appliance_prepare_storage(thumbprint, sizeof(thumbprint));
            if (ec == 0) {
                char reply[160];
                sprintf_s(reply, sizeof(reply), "ok:%s", thumbprint);
                REPLY(reply);
            }
            else { char reply[64]; sprintf_s(reply, sizeof(reply), "error:%lu", ec); REPLY(reply); }
        }
        else if (strncmp(cmd, "appliance_account:", 18) == 0) {
            DWORD ec = appliance_set_account(cmd + 18);
            if (ec == 0) REPLY("ok");
            else { char reply[64]; sprintf_s(reply, sizeof(reply), "error:%lu", ec); REPLY(reply); }
        }
        else if (strncmp(cmd, "appliance_reconcile:", 20) == 0) {
            DWORD ec = appliance_reconcile_share(cmd + 20);
            if (ec == 0) REPLY("ok");
            else { char reply[64]; sprintf_s(reply, sizeof(reply), "error:%lu", ec); REPLY(reply); }
        }
        else if (strncmp(cmd, "appliance_purge:", 16) == 0) {
            DWORD ec = appliance_purge_share(cmd + 16);
            if (ec == 0) REPLY("ok");
            else { char reply[64]; sprintf_s(reply, sizeof(reply), "error:%lu", ec); REPLY(reply); }
        }
        else if (strncmp(cmd, "appliance_remove:", 17) == 0) {
            DWORD ec = appliance_remove_share(cmd + 17);
            if (ec == 0) REPLY("ok");
            else { char reply[64]; sprintf_s(reply, sizeof(reply), "error:%lu", ec); REPLY(reply); }
        }
        else if (strncmp(cmd, "appliance_grow:", 15) == 0) {
            DWORD ec = run_powershell_wait(
                L"$v=Get-Volume -FileSystemLabel AppSandboxShared -ErrorAction Stop; $p=$v|Get-Partition; $s=$p|Get-PartitionSupportedSize; Resize-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -Size $s.SizeMax -ErrorAction Stop", 120000);
            REPLY(ec == 0 ? "ok" : "error:grow_failed");
        }
        else if (strcmp(cmd, "appliance_update") == 0) {
            DWORD ec = run_powershell_wait(
                L"$s=New-Object -ComObject Microsoft.Update.Session; $q=$s.CreateUpdateSearcher().Search('IsInstalled=0 and IsHidden=0'); "
                L"$c=New-Object -ComObject Microsoft.Update.UpdateColl; foreach($u in $q.Updates){if(-not $u.EulaAccepted){$u.AcceptEula()};[void]$c.Add($u)}; "
                L"if($c.Count){$d=$s.CreateUpdateDownloader();$d.Updates=$c;[void]$d.Download();$i=$s.CreateUpdateInstaller();$i.Updates=$c;$r=$i.Install();if($r.ResultCode -gt 3){exit 1}}",
                30 * 60 * 1000);
            REPLY(ec == 0 ? "ok" : "error:update_failed");
        }
        else if (strncmp(cmd, "shared_net:", 11) == 0) {
            char *arg = cmd + 11, *sep = strrchr(arg, ':');
            int ec = ERROR_INVALID_PARAMETER;
            if (sep) {
                *sep = '\0';
                ec = configure_shared_nic(arg, sep + 1);
                if (ec == ERROR_SUCCESS) REPLY("ok");
                else { char failure[64]; sprintf_s(failure, sizeof(failure),
                       "net_failed:%lu", (DWORD)ec); REPLY(failure); }
            } else REPLY("invalid");
        }
        else if (strncmp(cmd, "shared_smb_map:", 15) == 0) {
            char *ctx = NULL, *arg = cmd + 15;
            char *letter = strtok_s(arg, ":", &ctx);
            char *host = strtok_s(NULL, ":", &ctx);
            char *share = strtok_s(NULL, ":", &ctx);
            char *user = strtok_s(NULL, ":", &ctx);
            char *password = strtok_s(NULL, ":", &ctx);
            int ec = ERROR_INVALID_PARAMETER;
            if (letter && strlen(letter) == 1 && host && share && user && password) {
                ec = map_smb_drive_global(letter, host, share, user, password);
                SecureZeroMemory(password, strlen(password));
                if (ec == ERROR_SUCCESS)
                    REPLY("ok");
                else if (ec == ERROR_ALREADY_ASSIGNED)
                    REPLY("drive_collision");
                else {
                    char failure[64];
                    sprintf_s(failure, sizeof(failure), "map_failed:%lu", (DWORD)ec);
                    REPLY(failure);
                }
            } else REPLY("invalid");
        }
        else if (strncmp(cmd, "shared_smb_unmap:", 17) == 0) {
            int ec = unmap_smb_drive_global(cmd + 17);
            if (ec == ERROR_SUCCESS) REPLY("ok");
            else {
                char failure[64];
                sprintf_s(failure, sizeof(failure), "unmap_failed:%lu", (DWORD)ec);
                REPLY(failure);
            }
        }
        else if (strncmp(cmd, "ssh_deploy_key ", 15) == 0) {
            REPLY(deploy_ssh_key(cmd + 15) ? "ssh_key_deployed" : "ssh_key_failed");
        }
        else if (strcmp(cmd, "shutdown") == 0) {
            REPLY("ok");
            agent_log("Initiating shutdown... killing input helper first.");
            stop_input_monitor();
            agent_log("Input monitor stopped, calling InitiateSystemShutdownExW...");
            if (!enable_privilege(SE_SHUTDOWN_NAME))
                agent_log("Warning: could not enable SeShutdownPrivilege (%lu)", GetLastError());
            if (!InitiateSystemShutdownExW(NULL, L"AppSandbox shutdown", 0, TRUE, FALSE,
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                agent_log("InitiateSystemShutdownExW failed: %lu", GetLastError());
            /* Keep connection open — host will see it drop when VM powers off */
        }
        else if (strcmp(cmd, "restart") == 0) {
            REPLY("ok");
            agent_log("Initiating restart...");
            stop_input_monitor();
            if (!enable_privilege(SE_SHUTDOWN_NAME))
                agent_log("Warning: could not enable SeShutdownPrivilege (%lu)", GetLastError());
            if (!InitiateSystemShutdownExW(NULL, L"AppSandbox restart", 0, TRUE, TRUE,
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                agent_log("InitiateSystemShutdownExW failed: %lu", GetLastError());
        }
        else if (strncmp(cmd, "gpu_query_response:", 19) == 0) {
            int share_count = atoi(cmd + 19);
            agent_log("Received GPU share list (%d shares).", share_count);
            handle_gpu_query_response(client, share_count);
        }
        else if (strcmp(cmd, "gpu_none") == 0) {
            agent_log("Host reports no GPU-PV assigned.");
        }
        else if (strcmp(cmd, "ssh_enable") == 0) {
            handle_ssh_enable(client, tag);
        }
        else if (strncmp(cmd, "set_ip:", 7) == 0) {
            /* set_ip:172.20.0.X/PREFIX:GATEWAY[:MAC[:DNS,DNS]] */
            char ip[32] = {0}, prefix[8] = {0}, gateway[32] = {0}, nat_mac[64] = {0};
            char dns[160] = {0};
            char *slash, *colon2, *colon3;
            char *arg = cmd + 7;

            /* Parse IP/prefix:gateway */
            slash = strchr(arg, '/');
            colon2 = slash ? strchr(slash, ':') : NULL;
            if (slash && colon2) {
                colon3 = strchr(colon2 + 1, ':');
                int ip_len = (int)(slash - arg);
                int pfx_len = (int)(colon2 - slash - 1);
                int gateway_len = colon3 ? (int)(colon3 - colon2 - 1) : (int)strlen(colon2 + 1);
                if (ip_len > 0 && ip_len < (int)sizeof(ip))
                    strncpy_s(ip, sizeof(ip), arg, ip_len);
                if (pfx_len > 0 && pfx_len < (int)sizeof(prefix))
                    strncpy_s(prefix, sizeof(prefix), slash + 1, pfx_len);
                if (gateway_len > 0 && gateway_len < (int)sizeof(gateway))
                    strncpy_s(gateway, sizeof(gateway), colon2 + 1, gateway_len);
                if (colon3) {
                    char *colon4 = strchr(colon3 + 1, ':');
                    int mac_len = colon4 ? (int)(colon4 - colon3 - 1) : (int)strlen(colon3 + 1);
                    if (mac_len > 0 && mac_len < (int)sizeof(nat_mac))
                        strncpy_s(nat_mac, sizeof(nat_mac), colon3 + 1, mac_len);
                    if (colon4 && strlen(colon4 + 1) < sizeof(dns)) {
                        const char *p;
                        for (p = colon4 + 1; *p; p++)
                            if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ',')) break;
                        if (!*p) strcpy_s(dns, sizeof(dns), colon4 + 1);
                    }
                }
            }

            if (ip[0] && prefix[0] && gateway[0]) {
                int exit_code = configure_nat_nic(ip, prefix, gateway, nat_mac, dns);
                if (exit_code == 0) REPLY("ok");
                else REPLY("error:nat_net_failed");
            } else {
                agent_log("set_ip: bad format: %s", cmd);
                REPLY("error:bad_format");
            }
        }
        else if (strcmp(cmd, "gpu_copy") == 0) {
            /* Host re-triggered GPU copy — ask for share list */
            REPLY("gpu_query");
        }
        else if (strcmp(cmd, "idd_connect") == 0) {
            handle_idd_connect(client, tag);
        }
        else if (strncmp(cmd, "domain_", 7) == 0) {
            char *reply = (char *)malloc(DOMAIN_OUT_MAX + 64);
            if (reply) {
                handle_domain_command(cmd, reply, DOMAIN_OUT_MAX + 64);
                REPLY(reply);
                free(reply);
            } else {
                REPLY("error:out of memory");
            }
        }
        else if (strcmp(cmd, "backstage") == 0 || strcmp(cmd, "backstage_stop") == 0) {
            char reply[256];
            if (strcmp(cmd, "backstage") == 0) backstage_start(reply, sizeof(reply));
            else backstage_stop(reply, sizeof(reply));
            REPLY(reply);
        }
        else if (strcmp(cmd, "seamless") == 0) {
            char reply[256];
            seamless_start(reply, sizeof(reply));
            REPLY(reply);
        }
        else if (strcmp(cmd, "lab_start") == 0 || strcmp(cmd, "lab_stop") == 0 ||
                 strncmp(cmd, "lab_chunk:", 10) == 0) {
            /* Malware lab recorder (lab_recorder.c). */
            char *reply = (char *)malloc(LAB_CHUNK_BYTES * 2);
            if (!reply) { REPLY("error:out of memory"); }
            else {
                if (strcmp(cmd, "lab_start") == 0) lab_start(reply, LAB_CHUNK_BYTES * 2);
                else if (strcmp(cmd, "lab_stop") == 0) lab_stop(reply, LAB_CHUNK_BYTES * 2);
                else lab_chunk((size_t)_strtoui64(cmd + 10, NULL, 10), reply, LAB_CHUNK_BYTES * 2);
                REPLY(reply);
                free(reply);
            }
        }
        else if (strncmp(cmd, "user_drives:", 12) == 0) {
            if (set_user_drives(cmd + 12)) {
                REPLY("ok");
                map_user_drives(WTSGetActiveConsoleSessionId());
            } else {
                REPLY("error:write_failed");
            }
        }
        else if (strncmp(cmd, "install_apps:", 13) == 0) {
            if (queue_apps(cmd + 13)) {
                REPLY("ok");
                start_app_install();
            } else {
                REPLY("error:queue_failed");
            }
        }
        else if (strncmp(cmd, "stream_", 7) == 0) {
            char reply[1024];
            stream_command(cmd, reply, sizeof(reply));
            REPLY(reply);
        }
        else if (strncmp(cmd, "session_", 8) == 0) {
            char *reply = (char *)malloc(DOMAIN_OUT_MAX);
            if (reply) {
                session_command(cmd, reply, DOMAIN_OUT_MAX);
                REPLY(reply);
                free(reply);
            } else {
                REPLY("error:out of memory");
            }
        }
        else if (strncmp(cmd, "grow_root:", 10) == 0) {
            REPLY(grow_root_partition() ? "ok" : "error:guest_grow_failed");
        }
        else {
            REPLY("error:unknown");
        }

        #undef REPLY
        }
    }

    /* If GPU copy is still running, wait for it to finish */
    if (g_gpu_copy.copying) {
        agent_log("Waiting for GPU copy thread to finish...");
        /* Invalidate notify socket so it doesn't try to send on closed socket */
        g_gpu_copy.notify_sock = NULL;
        {
            int wait;
            for (wait = 0; wait < 10000 && g_gpu_copy.copying; wait += 500)
                Sleep(500);
        }
    }

    g_client_sock = NULL;
    asb_close(client);
    agent_log("Client handler exiting.");
}

/* ---- Socket listener ---- */

static DWORD WINAPI listener_thread(LPVOID param)
{
    AsbListener *l;
    (void)param;

    if (asb_transport_init() != 0) {
        agent_log("asb_transport_init failed.");
        return 1;
    }

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        l = asb_listen(ASB_CH_AGENT);
        if (!l) {
            agent_log("asb_listen(ASB_CH_AGENT) failed, retrying in 3s");
            if (WaitForSingleObject(g_stop_event, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        agent_log("Listening on agent control channel (transport=%s)",
                  asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

        /* Accept loop — one client at a time */
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
            AsbConn *client = asb_accept(l, 1000);   /* 1s timeout so we re-check g_stop_event */
            if (client) handle_client(client);
        }
        asb_close_listener(l);
    }

    agent_log("Listener stopped.");
    return 0;
}

/* ---- SSH: sshd detection + HV proxy (guest-side: HV socket → localhost:22) ---- */

/* SSH proxy service GUID: {A5B0CAFE-0007-4000-8000-000000000001} */
static const GUID SSH_SERVICE_GUID =
    { 0xa5b0cafe, 0x0007, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static HANDLE  g_ssh_proxy_thread = NULL;
static volatile BOOL g_ssh_proxy_running = FALSE;
static HANDLE  g_ssh_wait_thread = NULL;
static volatile BOOL g_ssh_waiting = FALSE;

#define SSH_RELAY_BUF 8192

typedef struct SshRelayCtx {
    AsbConn *hv;        /* transport connection to the host (AF_HYPERV on PC, ivshmem on Mac) */
    SOCKET   tcp_sock;  /* localhost:22 (sshd) */
} SshRelayCtx;

static DWORD WINAPI ssh_relay_thread(LPVOID param)
{
    SshRelayCtx *ctx = (SshRelayCtx *)param;
    char buf[SSH_RELAY_BUF];
    int n;
    SOCKET hv = (SOCKET)asb_conn_socket_u64(ctx->hv);

    if (hv != INVALID_SOCKET) {
        /* PC (AF_HYPERV): the connection exposes a real SOCKET, so relay both directions with a single dual-fd select(). */
        fd_set rfds;
        struct timeval tv;
        for (;;) {
            FD_ZERO(&rfds);
            FD_SET(hv, &rfds);
            FD_SET(ctx->tcp_sock, &rfds);
            tv.tv_sec  = 5;
            tv.tv_usec = 0;

            n = select(0, &rfds, NULL, NULL, &tv);
            if (n < 0) break;
            if (n == 0) continue;

            if (FD_ISSET(hv, &rfds)) {
                n = recv(hv, buf, SSH_RELAY_BUF, 0);
                if (n <= 0) break;
                if (send(ctx->tcp_sock, buf, n, 0) != n) break;
            }
            if (FD_ISSET(ctx->tcp_sock, &rfds)) {
                n = recv(ctx->tcp_sock, buf, SSH_RELAY_BUF, 0);
                if (n <= 0) break;
                if (send(hv, buf, n, 0) != n) break;
            }
        }
    } else {
        /* ivshmem: the connection has no fd to select() on, so relay each direction separately —
           poll the transport with a short timeout, non-blocking-check the TCP side. */
        fd_set rfds;
        struct timeval tv;
        for (;;) {
            int pr = asb_poll(ctx->hv, 5);
            if (pr < 0) break;
            if (pr > 0) {
                n = asb_recv(ctx->hv, buf, SSH_RELAY_BUF);
                if (n <= 0) break;
                { int off = 0; while (off < n) { int s = send(ctx->tcp_sock, buf + off, n - off, 0);
                    if (s <= 0) goto relay_done; off += s; } }
            }
            FD_ZERO(&rfds); FD_SET(ctx->tcp_sock, &rfds);
            tv.tv_sec = 0; tv.tv_usec = 0;
            if (select(0, &rfds, NULL, NULL, &tv) > 0) {
                n = recv(ctx->tcp_sock, buf, SSH_RELAY_BUF, 0);
                if (n <= 0) break;
                if (asb_send(ctx->hv, buf, n) != n) break;
            }
        }
    }
relay_done:
    asb_close(ctx->hv);
    closesocket(ctx->tcp_sock);
    free(ctx);
    return 0;
}

/* Check if sshd service is running. Returns TRUE if running. */
static BOOL is_sshd_running(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    BOOL running = FALSE;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return FALSE;

    svc = OpenServiceA(scm, "sshd", SERVICE_QUERY_STATUS);
    if (svc) {
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
            running = TRUE;
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return running;
}

/* Try to start sshd and set it to auto-start. Returns TRUE if running. */
static BOOL ensure_sshd_running(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    BOOL ok = FALSE;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return FALSE;

    svc = OpenServiceA(scm, "sshd", SERVICE_ALL_ACCESS);
    if (svc) {
        ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                             SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
            ok = TRUE;
        } else {
            if (StartServiceA(svc, 0, NULL))
                ok = TRUE;
            else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                ok = TRUE;
        }
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return ok;
}

/* HV proxy listener — runs after sshd is confirmed running */
static DWORD WINAPI ssh_proxy_thread(LPVOID param)
{
    AsbListener *l;
    struct sockaddr_in tcp_addr;
    (void)param;

    l = asb_listen(ASB_CH_SSH);
    if (!l) {
        agent_log("SSH proxy: asb_listen(ASB_CH_SSH) failed.");
        return 1;
    }
    agent_log("SSH proxy: listening on ssh channel (transport=%s)",
              asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    while (g_ssh_proxy_running) {
        AsbConn *client = asb_accept(l, 1000);   /* 1s so we re-check g_ssh_proxy_running */
        SOCKET tcp_s;
        if (!client) continue;

        /* Connect to local sshd */
        tcp_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_s == INVALID_SOCKET) { asb_close(client); continue; }

        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        tcp_addr.sin_port = htons(22);

        if (connect(tcp_s, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) != 0) {
            agent_log("SSH proxy: connect to localhost:22 failed: %d", WSAGetLastError());
            closesocket(tcp_s);
            asb_close(client);
            continue;
        }

        /* Spawn relay thread */
        {
            SshRelayCtx *ctx = (SshRelayCtx *)malloc(sizeof(SshRelayCtx));
            if (ctx) {
                HANDLE t;
                ctx->hv       = client;
                ctx->tcp_sock = tcp_s;
                t = CreateThread(NULL, 0, ssh_relay_thread, ctx, 0, NULL);
                if (t) CloseHandle(t);  /* detached — relay cleans up itself */
                else { free(ctx); asb_close(client); closesocket(tcp_s); }
            } else {
                asb_close(client);
                closesocket(tcp_s);
            }
        }
    }

    asb_close_listener(l);
    agent_log("SSH proxy: stopped.");
    return 0;
}

static void start_ssh_proxy(void)
{
    if (g_ssh_proxy_thread) return;  /* already running */
    g_ssh_proxy_running = TRUE;
    g_ssh_proxy_thread = CreateThread(NULL, 0, ssh_proxy_thread, NULL, 0, NULL);
}

static void stop_ssh_proxy(void)
{
    g_ssh_proxy_running = FALSE;
    g_ssh_waiting = FALSE;
    if (g_ssh_wait_thread) {
        WaitForSingleObject(g_ssh_wait_thread, 5000);
        CloseHandle(g_ssh_wait_thread);
        g_ssh_wait_thread = NULL;
    }
    if (g_ssh_proxy_thread) {
        WaitForSingleObject(g_ssh_proxy_thread, 5000);
        CloseHandle(g_ssh_proxy_thread);
        g_ssh_proxy_thread = NULL;
    }
}

/* Background thread: poll for sshd service to become running.
   SetupComplete.cmd installs OpenSSH via MSI during first boot — sshd may
   not be running yet when the agent first handles ssh_enable. */
static DWORD WINAPI ssh_wait_thread(LPVOID param)
{
    int attempts = 0;
    (void)param;

    agent_log("SSH: waiting for sshd to become available...");

    while (g_ssh_waiting && attempts < 120) {  /* up to ~2 minutes */
        Sleep(1000);
        attempts++;

        if (ensure_sshd_running()) {
            agent_log("SSH: sshd is running (after %d seconds).", attempts);
            start_ssh_proxy();
            EnterCriticalSection(&g_send_cs);
            if (g_client_sock != NULL)
                send_line(g_client_sock, "ssh_ready");
            LeaveCriticalSection(&g_send_cs);
            g_ssh_waiting = FALSE;
            return 0;
        }
    }

    agent_log("SSH: sshd did not start within timeout.");
    EnterCriticalSection(&g_send_cs);
    if (g_client_sock != NULL)
        send_line(g_client_sock, "ssh_failed");
    LeaveCriticalSection(&g_send_cs);
    g_ssh_waiting = FALSE;
    return 1;
}

/* Handle "ssh_enable" command from host.
   sshd is installed by SetupComplete.cmd (MSI). Agent just detects + proxies. */
static void handle_ssh_enable(AsbConn *client, const char *tag)
{
    /* Already running? */
    if (is_sshd_running()) {
        agent_log("SSH: sshd already running.");
        send_reply(client, tag, "ssh_ready");
        start_ssh_proxy();
        return;
    }

    /* Try to start it (may be installed but stopped) */
    if (ensure_sshd_running()) {
        agent_log("SSH: sshd started successfully.");
        send_reply(client, tag, "ssh_ready");
        start_ssh_proxy();
        return;
    }

    /* Not installed yet — SetupComplete.cmd may still be running.
       Poll on a background thread. */
    if (g_ssh_waiting) {
        send_reply(client, tag, "ssh_installing");
        return;
    }

    g_ssh_waiting = TRUE;
    send_reply(client, tag, "ssh_installing");
    g_ssh_wait_thread = CreateThread(NULL, 0, ssh_wait_thread, NULL, 0, NULL);
    if (!g_ssh_wait_thread) {
        agent_log("SSH: failed to create wait thread.");
        g_ssh_waiting = FALSE;
        send_reply(client, tag, "ssh_failed");
    }
}

/* ---- Windows service plumbing ---- */

static void set_service_status(DWORD state, DWORD exit_code)
{
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI service_ctrl_ex(DWORD ctrl, DWORD event_type, LPVOID event_data, LPVOID context)
{
    (void)context;
    agent_log("service_ctrl received: %lu (event_type=%lu)", ctrl, event_type);

    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        if (ctrl == SERVICE_CONTROL_SHUTDOWN)
            g_os_shutting_down = TRUE;

        /* Notify host */
        if (g_client_sock != NULL) {
            if (ctrl == SERVICE_CONTROL_SHUTDOWN)
                send_line(g_client_sock, "os_shutdown");
            else
                send_line(g_client_sock, "service_stopping");
            agent_log("Sent notification to host (ctrl=%lu).", ctrl);
        }

        set_service_status(SERVICE_STOP_PENDING, 0);
        agent_log("Setting stop event...");
        SetEvent(g_stop_event);
        return NO_ERROR;
    }

    if (ctrl == SERVICE_CONTROL_SESSIONCHANGE) {
        WTSSESSION_NOTIFICATION *sn = (WTSSESSION_NOTIFICATION *)event_data;
        {
            /* In the host's log too: a sign-in that doesn't last is otherwise invisible. */
            static const char *const names[] = { "?", "console connect", "console disconnect",
                "remote connect", "remote disconnect", "logon", "logoff", "lock", "unlock" };
            agent_log_to_host("Windows session %lu: %s (console is %lu).", sn ? sn->dwSessionId : 0,
                              event_type <= 8 ? names[event_type] : "other", WTSGetActiveConsoleSessionId());
        }
        if (event_type == WTS_SESSION_LOGOFF) {
            agent_log("Session logoff detected (session %lu).",
                       sn ? sn->dwSessionId : 0);
            /* Do NOT restart the VDD device here.  IddCx handles the DWM
               transition naturally: UnassignSwapChain (user DWM dies) then
               AssignSwapChain (login-screen DWM starts compositing).
               A devcon restart mid-transition destroys the adapter/monitor
               and prevents IddCx from completing the handoff. */

            /* Disable Hyper-V Video after logoff (may have been re-enabled) */
            ensure_hyperv_video_disabled(NULL);
        }
        if (event_type == WTS_SESSION_LOGON || event_type == WTS_SESSION_UNLOCK) {
            DWORD sid = sn ? sn->dwSessionId : WTSGetActiveConsoleSessionId();
            agent_log("Session %s detected (session %lu), respawning clipboard helpers.",
                       event_type == WTS_SESSION_LOGON ? "logon" : "unlock", sid);
            kill_clipboard_helper();
            if (sid != 0xFFFFFFFF)
                spawn_clipboard_in_session(sid);
            kill_clipboard_reader();
            if (sid != 0xFFFFFFFF)
                spawn_clipboard_reader_in_session(sid);
            if (event_type == WTS_SESSION_LOGON)
                map_user_drives(sid);
        }
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, LPSTR *argv)
{
    HANDLE thread;

    (void)argc; (void)argv;

    g_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_ctrl_ex, NULL);
    if (!g_status_handle) return;

    /* Init before accepting SESSIONCHANGE so the control handler and the
       clipboard monitor threads share this lock for the helper-process globals. */
    InitializeCriticalSection(&g_clip_proc_cs);

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    set_service_status(SERVICE_START_PENDING, 0);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&g_send_cs);

    /* Wire p9copy logging into agent_log */
    p9_set_log(p9copy_log);

    /* Start listener on a worker thread */
    thread = CreateThread(NULL, 0, listener_thread, NULL, 0, NULL);

    /* Start input helper monitor (spawns/respawns in console session as SYSTEM) */
    start_input_monitor();

    /* Start clipboard helper monitor (SYSTEM, :0005) */
    start_clipboard_monitor();

    /* Start clipboard reader monitor (user, :0006) */
    start_clipboard_reader_monitor();

    /* Start audio capture monitor (SYSTEM, :0004) */
    start_audio_monitor();

    /* Run in Sandbox: files the host copies in (:0010) */
    file_drop_start(p9copy_log, g_stop_event);

    set_service_status(SERVICE_RUNNING, 0);
    /* Stamp the build into the guest log. Without it there is no way to tell,
       from the log alone, whether the VM is running the agent you just built
       or one baked into its disk by an earlier install. */
    agent_log("Service started (built " __DATE__ " " __TIME__ ").");

    /* Install a display driver update the host staged, before checking it */
    apply_staged_vdd_update();

    /* Ensure VDD device is running (may need restart after logout teardown) */
    ensure_vdd_running();

    /* Ensure the virtual audio device exists and runs */
    ensure_vad_installed();

    /* Under Secure Boot it can't; give the VM VB-CABLE instead */
    ensure_fallback_audio();

    /* Finish installing apps chosen at creation that are still queued */
    start_app_install();

    lab_init(agent_log);

    /* Ensure Hyper-V Video adapter is disabled (VDD replaces it) */
    ensure_hyperv_video_disabled(NULL);

    /* Wait until stop is signaled */
    WaitForSingleObject(g_stop_event, INFINITE);

    /* Clean up */
    agent_log("Stop event signaled, cleaning up...");
    agent_log("Stopping SSH proxy...");
    stop_ssh_proxy();
    agent_log("SSH proxy stopped.");
    agent_log("Stopping audio monitor...");
    stop_audio_monitor();
    agent_log("Audio monitor stopped.");
    agent_log("Stopping clipboard reader monitor...");
    stop_clipboard_reader_monitor();
    agent_log("Clipboard reader monitor stopped.");
    agent_log("Stopping clipboard monitor...");
    stop_clipboard_monitor();
    agent_log("Clipboard monitor stopped.");
    agent_log("Stopping input monitor...");
    stop_input_monitor();
    agent_log("Input monitor stopped.");
    /* The control listener is owned by listener_thread (asb_close_listener); its accept loop
       polls g_stop_event every 1s and exits on its own. Nothing socket-level to close here. */
    if (thread) {
        agent_log("Waiting for listener thread...");
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        agent_log("Listener thread exited.");
    }
    CloseHandle(g_stop_event);
    DeleteCriticalSection(&g_clip_proc_cs);
    DeleteCriticalSection(&g_send_cs);

    agent_log("Service stopped.");
    set_service_status(SERVICE_STOPPED, 0);
}

/* ---- Install / Remove ---- */

static int install_service(void)
{
    SC_HANDLE scm, svc;
    char path[MAX_PATH];

    GetModuleFileNameA(NULL, path, MAX_PATH);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    svc = CreateServiceA(scm, SERVICE_NAME, DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            printf("Service already exists.\n");
            svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_START);
        } else {
            printf("CreateService failed: %lu\n", GetLastError());
            CloseServiceHandle(scm);
            return 1;
        }
    } else {
        printf("Service installed.\n");

        /* Set description */
        SERVICE_DESCRIPTIONA desc;
        desc.lpDescription = "AppSandbox guest agent for host-guest communication via Hyper-V sockets.";
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        /* Set recovery: restart on failure */
        SC_ACTION actions[3] = {
            { SC_ACTION_RESTART, 5000 },
            { SC_ACTION_RESTART, 10000 },
            { SC_ACTION_RESTART, 30000 }
        };
        SERVICE_FAILURE_ACTIONSA sfa = { 0 };
        sfa.dwResetPeriod = 86400;
        sfa.cActions = 3;
        sfa.lpsaActions = actions;
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);
    }

    /* Start the service */
    if (svc) {
        if (StartServiceA(svc, 0, NULL))
            printf("Service started.\n");
        else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
            printf("Service already running.\n");
        else
            printf("StartService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return 0;
}

static int remove_service(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!svc) {
        printf("Service not found.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    Sleep(1000);

    if (DeleteService(svc))
        printf("Service removed.\n");
    else
        printf("DeleteService failed: %lu\n", GetLastError());

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

/* ---- Entry point ---- */

int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--install") == 0)
            return install_service();
        if (strcmp(argv[1], "--remove") == 0)
            return remove_service();
        if (strcmp(argv[1], "--refresh-drive") == 0 && argc == 3)
            return refresh_drive_shell(argv[2]);
        if (strcmp(argv[1], "--open-file") == 0)
            return file_drop_open_main();
        printf("Usage: appsandbox-agent.exe [--install | --remove | --refresh-drive X]\n");
        return 1;
    }

    /* Launched by SCM — run as service */
    {
        SERVICE_TABLE_ENTRYA table[] = {
            { SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONA)service_main },
            { NULL, NULL }
        };
        if (!StartServiceCtrlDispatcherA(table)) {
            /* Not running as service — run listener directly for debugging */
            printf("Running in console mode (Ctrl+C to stop)...\n");
            p9_set_log(p9copy_log);
            InitializeCriticalSection(&g_send_cs);
            InitializeCriticalSection(&g_clip_proc_cs);
            g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            listener_thread(NULL);
            CloseHandle(g_stop_event);
            DeleteCriticalSection(&g_clip_proc_cs);
            DeleteCriticalSection(&g_send_cs);
        }
    }
    return 0;
}

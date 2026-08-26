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
#include <userenv.h>
#include <ntsecapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "p9copy.h"
#include "../transport/asb_transport.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

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

/* ---- Report display monitor info to host ---- */

/* Spawn appsandbox-displays.exe in the interactive session and capture its
   stdout.  The helper prints "N,WxH,WxH,..." and exits.  We use the same
   token-dup + SetTokenInformation pattern as spawn_input_in_session(). */
static void report_displays(AsbConn *notify)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    DWORD session_id;
    char output[512];
    DWORD bytes_read;
    char msg[512];

    output[0] = '\0';

    session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF || session_id == 0) {
        /* No interactive user session — skip display enumeration.
           Session 0 is the services session (no desktop).
           0xFFFFFFFF means no console session at all. */
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }

    /* Build path to appsandbox-displays.exe (same directory as agent) */
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-displays.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("report_displays: %ls not found.", exe_path);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }

    /* Create pipe for stdout capture */
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        agent_log("report_displays: CreatePipe failed (%lu).", GetLastError());
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Duplicate our token and set the interactive session */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("report_displays: OpenProcessToken failed (%lu).", GetLastError());
        CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("report_displays: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("report_displays: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("report_displays: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    CloseHandle(dup_token);
    CloseHandle(hWritePipe);  /* Close write end so ReadFile sees EOF */
    hWritePipe = NULL;

    /* Wait for process (max 2s) then read output.
       Timeout is normal at early boot before the desktop is ready. */
    if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) {
        agent_log("report_displays: helper timed out (desktop may not be ready).");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }

    {
        int total = 0;
        while (ReadFile(hReadPipe, output + total,
                        (DWORD)(sizeof(output) - 1 - total), &bytes_read, NULL) &&
               bytes_read > 0) {
            total += (int)bytes_read;
            if (total >= (int)sizeof(output) - 1) break;
        }
        output[total] = '\0';
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    /* Strip trailing whitespace */
    {
        int len = (int)strlen(output);
        while (len > 0 && (output[len-1] == '\r' || output[len-1] == '\n' || output[len-1] == ' '))
            output[--len] = '\0';
    }

    snprintf(msg, sizeof(msg), "displays:%s", output);
    agent_log("Displays: %s", output);
    if (notify != NULL) send_line(notify, msg);
}

/* ---- IDD connect: respawn helpers in console session ---- */

/* Send a response, prepending the sequence tag if present */
static void send_reply(AsbConn *s, const char *tag, const char *msg)
{
    if (tag[0]) {
        char rb[512];
        sprintf_s(rb, sizeof(rb), "%s%s", tag, msg);
        send_line(s, rb);
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
        "Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue\r\n"
        "New-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -IPAddress $env:ASB_NET_IP -PrefixLength 24 -PolicyStore ActiveStore -ErrorAction Stop | Out-Null\r\n"
        "Set-DnsClientServerAddress -InterfaceIndex $a.ifIndex -ResetServerAddresses -ErrorAction SilentlyContinue\r\n"
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
                             const char *gateway_a, const char *mac_a)
{
    static const char script[] =
        "$ErrorActionPreference='Stop'\r\n"
        "Set-Location -LiteralPath ([Environment]::SystemDirectory)\r\n"
        "$shared=$env:ASB_SHARED_NIC_MAC -replace '[:-]',''\r\n"
        "$want=$env:ASB_NAT_NIC_MAC -replace '[:-]',''\r\n"
        "$a=$null\r\n"
        "$last=''\r\n"
        "for($i=0;$i -lt 60 -and -not $a;$i++){\r\n"
        " try{\r\n"
        "  $candidates=@(Get-NetAdapter -IncludeHidden -ErrorAction Stop | Where-Object {\r\n"
        "   $_.MacAddress -and ((($_.MacAddress) -replace '[:-]','') -ne $shared)\r\n"
        "  })\r\n"
        " }catch{$last=$_.Exception.Message;$candidates=@()}\r\n"
        " if($want){$a=$candidates | Where-Object {(($_.MacAddress) -replace '[:-]','') -eq $want} | Select-Object -First 1}\r\n"
        " else{\r\n"
        "  $a=$candidates | Where-Object {$_.Status -eq 'Up'} | Select-Object -First 1\r\n"
        "  if(-not $a){$a=$candidates | Where-Object {$_.Status -ne 'Disabled'} | Select-Object -First 1}\r\n"
        "  if(-not $a){$a=$candidates | Select-Object -First 1}\r\n"
        " }\r\n"
        " if(-not $a){Start-Sleep -Milliseconds 500}\r\n"
        "}\r\n"
        "if(-not $a){if($want){Write-Output ('NAT adapter with MAC ' + $want + ' not found')}else{Write-Output 'normal NAT adapter not found'};if($last){Write-Output ('adapter query unavailable: ' + $last)};exit 1168}\r\n"
        "Write-Output ('selected adapter=' + $a.Name + ' mac=' + $a.MacAddress + ' status=' + $a.Status + ' if=' + $a.ifIndex)\r\n"
        "try{\r\n"
        " if($a.Status -eq 'Disabled'){Enable-NetAdapter -Name $a.Name -Confirm:$false -ErrorAction Stop;Start-Sleep -Milliseconds 500}\r\n"
        " Set-NetIPInterface -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -Dhcp Disabled -InterfaceMetric 10 -ErrorAction Stop\r\n"
        " Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue\r\n"
        " New-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -IPAddress $env:ASB_NAT_IP -PrefixLength ([int]$env:ASB_NAT_PREFIX) -DefaultGateway $env:ASB_NAT_GATEWAY -PolicyStore ActiveStore -ErrorAction Stop | Out-Null\r\n"
        " Set-DnsClientServerAddress -InterfaceIndex $a.ifIndex -ServerAddresses @($env:ASB_NAT_GATEWAY,'8.8.8.8') -ErrorAction Stop\r\n"
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
        "$cred=[System.Management.Automation.PSCredential]::new(($h+'\\'+$env:ASB_SMB_USER),$secure)\r\n"
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
        "Write-Output ('map failed user=' + $h + '\\' + $env:ASB_SMB_USER + ' target=' + $remote + ': ' + $last)\r\n"
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
        L"Enable-PSRemoting -SkipNetworkProfileCheck -Force -ErrorAction Stop; "
        L"Get-ChildItem WSMan:\\localhost\\Listener | Where-Object {$_.Keys -contains 'Transport=HTTPS'} | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue; "
        L"New-Item -Path WSMan:\\localhost\\Listener -Transport HTTPS -Address * -CertificateThumbPrint $cert.Thumbprint -Force -ErrorAction Stop | Out-Null; "
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
       0x10000 = DONT_EXPIRE_PASSWD, 0x2 = ACCOUNTDISABLE. */
    rc = run_powershell_wait(
        L"try{"
        L"$n=$env:ASB_APPLIANCE_USER; $p=$env:ASB_APPLIANCE_PASSWORD; $c=$env:COMPUTERNAME; "
        L"$root=[ADSI]('WinNT://'+$c); $u=$null; "
        L"try{$t=[ADSI]('WinNT://'+$c+'/'+$n+',user'); if($t.Name){$u=$t}}catch{$u=$null}; "
        L"if(-not $u){$u=$root.Create('User',$n)}; "
        L"$u.SetPassword($p); $u.SetInfo(); "
        L"$f=[int]$u.UserFlags.Value; "
        L"$u.UserFlags.Value=(($f -bor 0x10000) -band (-bnot 0x2)); $u.SetInfo()"
        L"}catch{$e=([int]$_.Exception.HResult -band 0xffff); if($e -eq 0){$e=1}; exit $e}; "
        L"try{$g=[ADSI]('WinNT://'+$env:COMPUTERNAME+'/Administrators,group'); "
        L"$g.Remove(('WinNT://'+$env:COMPUTERNAME+'/'+$env:ASB_APPLIANCE_USER+',user'))}catch{}; "
        L"exit 0", 30000);
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
    char copy[256], *separator;
    wchar_t share[128], directory[128], script[2048];
    BOOL read_only;
    strcpy_s(copy, sizeof(copy), arguments);
    separator = strchr(copy, ':');
    if (!separator) return ERROR_INVALID_PARAMETER;
    *separator++ = '\0';
    if (!appliance_token_valid(copy) ||
        (strcmp(separator, "ro") != 0 && strcmp(separator, "rw") != 0))
        return ERROR_INVALID_PARAMETER;
    read_only = strcmp(separator, "ro") == 0;
    MultiByteToWideChar(CP_UTF8, 0, copy, -1, share, _countof(share));
    wcscpy_s(directory, _countof(directory), share);
    { wchar_t *dollar = wcschr(directory, L'$'); if (dollar) *dollar = L'\0'; }
    swprintf_s(script, _countof(script),
        L"try{ "
        L"$p='C:\\AppSandboxData\\%s'; New-Item -ItemType Directory -Force -Path $p | Out-Null; "
        L"$acl=Get-Acl -LiteralPath $p; "
        L"$rule=New-Object System.Security.AccessControl.FileSystemAccessRule("
        L"'AppSandboxShare','%s','ContainerInherit,ObjectInherit','None','Allow'); "
        L"$acl.SetAccessRule($rule); Set-Acl -LiteralPath $p -AclObject $acl; "
        L"if(Get-SmbShare -Name '%s' -ErrorAction SilentlyContinue){Remove-SmbShare -Name '%s' -Force}; "
        L"New-SmbShare -Name '%s' -Path $p -%sAccess 'AppSandboxShare' -EncryptData:$false | Out-Null; "
        L"$s=Get-SmbShare -Name '%s' -ErrorAction Stop; "
        L"Write-Output ('share=' + $s.Name + ' path=' + $s.Path + ' scope=' + $s.ScopeName + ' pathok=' + (Test-Path -LiteralPath $s.Path)); "
        L"exit 0}catch{Write-Output ('share error: ' + $_.Exception.Message); exit 1}",
        directory, read_only ? L"ReadAndExecute" : L"Modify",
        share, share, share, read_only ? L"Read" : L"Full", share);
    {
        char out[1024];
        DWORD rc = (DWORD)run_powershell_capture(script, 30000, out, sizeof(out));
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
            /* set_ip:172.20.0.X/PREFIX:GATEWAY */
            char ip[32] = {0}, prefix[8] = {0}, gateway[32] = {0}, nat_mac[64] = {0};
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
                if (colon3)
                    strncpy_s(nat_mac, sizeof(nat_mac), colon3 + 1, sizeof(nat_mac) - 1);
            }

            if (ip[0] && prefix[0] && gateway[0]) {
                int exit_code = configure_nat_nic(ip, prefix, gateway, nat_mac);
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

    set_service_status(SERVICE_RUNNING, 0);
    /* Stamp the build into the guest log. Without it there is no way to tell,
       from the log alone, whether the VM is running the agent you just built
       or one baked into its disk by an earlier install. */
    agent_log("Service started (built " __DATE__ " " __TIME__ ").");

    /* Ensure VDD device is running (may need restart after logout teardown) */
    ensure_vdd_running();

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
        printf("Usage: appsandbox-agent.exe [--install | --remove]\n");
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

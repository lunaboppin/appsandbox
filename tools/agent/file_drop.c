/*
 * file_drop.c -- the guest end of Run in Sandbox (see file_drop.h).
 *
 * Wire protocol on ASB_CH_FILEDROP, mirrored in src/backend_win/sandbox_run.c
 * and tools/linux/agent/appsandbox-agent.c:
 *
 *   host:  "ASBDROP1 <size>\n<file name, UTF-8>\n"
 *   guest: "ready\n" | "error:<code>\n"
 *   host:  <size> bytes
 *   guest: "saved:<guest path, UTF-8>\n" | "error:<code>\n"
 *   host:  "open\n"
 *   guest: "opened\n" | "nohandler\n" | "nosession\n" | "error:<code>\n"
 */
#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>
#include <stdio.h>
#include <wchar.h>
#include "file_drop.h"
#include "../transport/asb_transport.h"

#define DROP_LINE_MAX     4096
#define DROP_NAME_CHARS   180       /* keeps the whole path well under MAX_PATH */
#define DROP_SPARE_BYTES  (64ull * 1024 * 1024)
#define DROP_SESSION_WAIT_MS 300000 /* long enough for someone to sign in at the display */
#define DROP_OPEN_WAIT_MS    20000

static FileDropLog g_log;
static HANDLE      g_stop;

/* Replace what Windows won't take in a file name, and shorten it keeping the
   extension. The host only ever sends a base name, but this runs as SYSTEM and
   trusts no one: separators can't survive this function. */
static void sanitize_name(const wchar_t *in, wchar_t *out, size_t chars)
{
    static const wchar_t *reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    size_t len = 0, i;
    const wchar_t *dot;

    for (i = 0; in[i] && len + 1 < chars; i++) {
        wchar_t c = in[i];
        out[len++] = (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    }
    out[len] = L'\0';
    while (len && (out[len - 1] == L'.' || out[len - 1] == L' ')) out[--len] = L'\0';
    if (!len) { wcscpy_s(out, chars, L"file"); len = 4; }

    if (len > DROP_NAME_CHARS) {
        dot = wcsrchr(out, L'.');
        if (dot && wcslen(dot) <= 32) {
            size_t ext = wcslen(dot);
            wmemmove(out + DROP_NAME_CHARS - ext, dot, ext + 1);
        } else {
            out[DROP_NAME_CHARS] = L'\0';
        }
    }

    {
        wchar_t stem[8];
        size_t stem_len = wcscspn(out, L".");
        if (stem_len < _countof(stem)) {
            wcsncpy_s(stem, _countof(stem), out, stem_len);
            for (i = 0; i < _countof(reserved); i++) {
                if (_wcsicmp(stem, reserved[i]) == 0) {
                    wmemmove(out + 1, out, wcslen(out) + 1);
                    out[0] = L'_';
                    break;
                }
            }
        }
    }
}

static BOOL drop_folder(wchar_t *out, size_t chars)
{
    wchar_t *docs = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_PublicDocuments, 0, NULL, &docs))) return FALSE;
    swprintf_s(out, chars, L"%s\\Run in Sandbox", docs);
    CoTaskMemFree(docs);
    return CreateDirectoryW(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* A new file that doesn't overwrite an earlier copy: "name (2).ext". */
static HANDLE create_unique(const wchar_t *folder, const wchar_t *name,
                            wchar_t *path, size_t chars)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    size_t stem = dot ? (size_t)(dot - name) : wcslen(name);
    int n;
    for (n = 1; n < 1000; n++) {
        HANDLE h;
        if (n == 1) swprintf_s(path, chars, L"%s\\%s", folder, name);
        else swprintf_s(path, chars, L"%s\\%.*s (%d)%s", folder, (int)stem, name, n, dot ? dot : L"");
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return h;
    }
    return INVALID_HANDLE_VALUE;
}

static void send_path_reply(AsbConn *c, const wchar_t *path)
{
    char utf8[1024], line[1100];
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL))
        strcpy_s(utf8, sizeof(utf8), "?");
    sprintf_s(line, sizeof(line), "saved:%s", utf8);
    asb_send_line(c, line);
}

/* Wait for someone to be signed in at the console. */
static HANDLE wait_user_token(DWORD *session_out)
{
    ULONGLONG deadline = GetTickCount64() + DROP_SESSION_WAIT_MS;
    for (;;) {
        DWORD session = WTSGetActiveConsoleSessionId();
        HANDLE token = NULL;
        if (session != 0xFFFFFFFF && WTSQueryUserToken(session, &token)) {
            *session_out = session;
            return token;
        }
        if (GetTickCount64() >= deadline) return NULL;
        if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) return NULL;
    }
}

/* Open in the user's session, as the user: the helper is this same exe. */
static const char *open_as_user(const wchar_t *path)
{
    HANDLE token;
    DWORD session = 0, exit_code = STILL_ACTIVE;
    LPVOID env = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH * 3];
    const char *result = "error:spawn";

    token = wait_user_token(&session);
    if (!token) return "nosession";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf_s(cmd, _countof(cmd), L"\"%s\" --open-file \"%s\"", exe, path);
    if (!CreateEnvironmentBlock(&env, token, FALSE)) env = NULL;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessAsUserW(token, exe, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                             env, NULL, &si, &pi)) {
        /* Still going after the wait is success: the handler is on screen asking
           something, such as a UAC prompt. */
        WaitForSingleObject(pi.hProcess, DROP_OPEN_WAIT_MS);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        if (exit_code == FILE_DROP_OPEN_NO_HANDLER) result = "nohandler";
        else if (exit_code == FILE_DROP_OPEN_FAILED) result = "error:open";
        else result = "opened";
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else if (g_log) {
        g_log("File drop: could not start the opener (%lu).", GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);
    return result;
}

static void handle_drop(AsbConn *c)
{
    char line[DROP_LINE_MAX];
    wchar_t wide[1024], name[256], folder[MAX_PATH], path[MAX_PATH * 2];
    unsigned long long size = 0, left;
    ULARGE_INTEGER free_bytes;
    HANDLE file;
    char *buf;
    BOOL ok = TRUE;

    if (asb_recv_line(c, line, sizeof(line)) <= 0) return;
    if (sscanf_s(line, "ASBDROP1 %llu", &size) != 1) { asb_send_line(c, "error:protocol"); return; }
    if (asb_recv_line(c, line, sizeof(line)) <= 0) return;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, wide, _countof(wide))) {
        asb_send_line(c, "error:name");
        return;
    }
    sanitize_name(wide, name, _countof(name));

    if (!drop_folder(folder, _countof(folder))) { asb_send_line(c, "error:folder"); return; }
    if (GetDiskFreeSpaceExW(folder, &free_bytes, NULL, NULL) &&
        free_bytes.QuadPart < size + DROP_SPARE_BYTES) {
        asb_send_line(c, "error:nospace");
        return;
    }
    file = create_unique(folder, name, path, _countof(path));
    if (file == INVALID_HANDLE_VALUE) { asb_send_line(c, "error:create"); return; }
    buf = (char *)malloc(1024 * 1024);
    if (!buf) { CloseHandle(file); DeleteFileW(path); asb_send_line(c, "error:memory"); return; }

    if (asb_send_line(c, "ready") <= 0) ok = FALSE;
    /* A generous per-read timeout: the copy can pause while the host's disk is busy. */
    asb_set_timeout(c, 300000, 30000);
    left = size;
    while (ok && left) {
        int want = left < 1024 * 1024 ? (int)left : 1024 * 1024;
        int got = asb_recv(c, buf, want);
        DWORD written = 0;
        if (got <= 0) { ok = FALSE; break; }
        if (!WriteFile(file, buf, (DWORD)got, &written, NULL) || written != (DWORD)got) {
            ok = FALSE;
            asb_send_line(c, "error:write");
            break;
        }
        left -= (unsigned long long)got;
    }
    free(buf);
    if (ok) ok = FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        /* Never leave half a file where someone could mistake it for the real one. */
        DeleteFileW(path);
        if (g_log) g_log("File drop: copy ended early with %llu bytes to go.", left);
        return;
    }
    send_path_reply(c, path);
    if (g_log) g_log("File drop: saved %llu bytes.", size);

    if (asb_recv_line(c, line, sizeof(line)) > 0 && strcmp(line, "open") == 0)
        asb_send_line(c, open_as_user(path));
}

static DWORD WINAPI file_drop_thread(LPVOID param)
{
    AsbListener *l;
    (void)param;
    if (asb_transport_init() != 0) return 1;
    /* The Mac (ivshmem) transport has no region for this channel. */
    if (asb_transport_is_ivshmem()) return 0;
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        l = asb_listen(ASB_CH_FILEDROP);
        if (!l) {
            if (WaitForSingleObject(g_stop, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
            AsbConn *c = asb_accept(l, 1000);
            if (c) {
                handle_drop(c);
                asb_close(c);
            }
        }
        asb_close_listener(l);
    }
    return 0;
}

void file_drop_start(FileDropLog log, HANDLE stop_event)
{
    HANDLE t;
    g_log = log;
    g_stop = stop_event;
    t = CreateThread(NULL, 0, file_drop_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---- --open-file: runs in the user's session ---- */

int file_drop_open_main(void)
{
    BOOL unknown_type = FALSE;
    int argc = 0, rc = FILE_DROP_OPEN_FAILED;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    SHELLEXECUTEINFOW sei;

    if (!argv || argc != 3) return FILE_DROP_OPEN_FAILED;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    /* The handler is a new window: let it come to the front. */
    AllowSetForegroundWindow(ASFW_ANY);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    /* NO_UI so that "no handler" comes back here as an error, not a dialog nobody
       can tell apart from success. */
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = argv[2];
    sei.nShow = SW_SHOWNORMAL;
    /* Windows 11 shows its app picker for an unknown type and calls that a success,
       so ask first. Only a type nothing has registered counts: plenty of real handlers
       have no plain open command. */
    {
        const wchar_t *dot = wcsrchr(argv[2], L'.'), *slash = wcsrchr(argv[2], L'\\');
        HKEY key = NULL;
        if (!dot || (slash && dot < slash) ||
            RegOpenKeyExW(HKEY_CLASSES_ROOT, dot, 0, KEY_READ, &key) != ERROR_SUCCESS)
            unknown_type = TRUE;
        if (key) RegCloseKey(key);
    }
    if (!unknown_type && ShellExecuteExW(&sei)) {
        rc = FILE_DROP_OPEN_OK;
    } else if (unknown_type || GetLastError() == ERROR_NO_ASSOCIATION || (INT_PTR)sei.hInstApp == SE_ERR_NOASSOC) {
        sei.fMask = SEE_MASK_NOASYNC;
        sei.lpVerb = L"openas";
        ShellExecuteExW(&sei);
        rc = FILE_DROP_OPEN_NO_HANDLER;
    }
    CoUninitialize();
    LocalFree(argv);
    return rc;
}

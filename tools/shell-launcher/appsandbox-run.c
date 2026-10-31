/*
 * appsandbox-run.exe -- what Explorer's "Run in Sandbox" entry runs.
 *
 * AppSandbox.exe requires elevation, so running it from a menu entry would raise a
 * UAC prompt on every click. This runs unelevated, hands the path to the app window
 * if there is one, and only starts the app (one prompt) if there isn't.
 *
 * It sends a path and nothing else: the app shows its picker, and nothing runs in a
 * VM until the user chooses there.
 */
#include <windows.h>
#include <winnetwk.h>
#include <shellapi.h>
#include <stdio.h>

#define MAIN_WINDOW_CLASS  L"AppSandbox_Main"
/* Must match ASB_COPYDATA_RUN_FILE in src/app_win/ui.c. */
#define COPYDATA_RUN_FILE  0x41534252
#define CORE_MUTEX         L"Global\\AppSandboxCoreHost"
#define APP_WAIT_MS        (2 * 60 * 1000)
#define APP_TITLE          L"Run in Sandbox"

static void complain(const wchar_t *text)
{
    MessageBoxW(NULL, text, APP_TITLE, MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
}

/* The elevated app can't see this user's mapped drives. */
static void resolve_mapped_drive(wchar_t *path, size_t chars)
{
    BYTE buffer[4096];
    DWORD size = sizeof(buffer);
    if (WNetGetUniversalNameW(path, UNIVERSAL_NAME_INFO_LEVEL, buffer, &size) == NO_ERROR) {
        const wchar_t *unc = ((UNIVERSAL_NAME_INFOW *)buffer)->lpUniversalName;
        if (wcslen(unc) < chars) wcscpy_s(path, chars, unc);
    }
}

/* The headless daemon holds the same single-instance mutex as the app. */
static BOOL other_core_running(void)
{
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, CORE_MUTEX);
    if (h) { CloseHandle(h); return TRUE; }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

static BOOL start_app(void)
{
    wchar_t exe[MAX_PATH + 16], *slash;
    SHELLEXECUTEINFOW sei;
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!len || len >= MAX_PATH) return FALSE;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return FALSE;
    wcscpy_s(slash + 1, _countof(exe) - (slash + 1 - exe), L"AppSandbox.exe");
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpFile = exe;
    sei.nShow = SW_SHOWNORMAL;
    /* The app writes its log to its current folder. */
    *slash = L'\0';
    {
        wchar_t folder[MAX_PATH + 16];
        wcscpy_s(folder, _countof(folder), exe);
        *slash = L'\\';
        sei.lpDirectory = folder;
        return ShellExecuteExW(&sei);
    }
}

/* The window's filter opens only partway through WM_CREATE, and the app answers with
   0 until it can take the path, so a refusal just means not yet. */
static BOOL deliver(const wchar_t *path, DWORD timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    COPYDATASTRUCT cds;
    cds.dwData = COPYDATA_RUN_FILE;
    cds.cbData = (DWORD)((wcslen(path) + 1) * sizeof(wchar_t));
    cds.lpData = (void *)path;
    for (;;) {
        HWND hwnd = FindWindowW(MAIN_WINDOW_CLASS, NULL);
        if (hwnd) {
            DWORD pid = 0;
            DWORD_PTR answer = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, (LPARAM)&cds,
                                    SMTO_ABORTIFHUNG, 10000, &answer) && answer)
                return TRUE;
        }
        if (GetTickCount64() >= deadline) return FALSE;
        Sleep(250);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t path[4096];
    HANDLE gate;
    int result = 0;
    (void)inst; (void)prev; (void)cmd; (void)show;

    if (!argv || argc < 2) {
        complain(L"Choose Run in Sandbox from a file's right-click menu.");
        return 2;
    }
    if (wcslen(argv[1]) >= _countof(path)) {
        complain(L"This file's path is too long.");
        return 2;
    }
    wcscpy_s(path, _countof(path), argv[1]);
    resolve_mapped_drive(path, _countof(path));

    /* Several selected files start several of these, which must not mean several
       UAC prompts: the first starts the app and the rest wait their turn. */
    gate = CreateMutexW(NULL, FALSE, L"Local\\AppSandboxRunLauncher");
    if (gate) WaitForSingleObject(gate, APP_WAIT_MS + 10000);

    if (!FindWindowW(MAIN_WINDOW_CLASS, NULL)) {
        if (other_core_running()) {
            complain(L"App Sandbox is already running without its window (the headless API, "
                     L"or another session). Stop that first, or use its API to run this file.");
            result = 1;
        } else if (!start_app()) {
            /* Most likely the UAC prompt was declined, which is an answer, not an error. */
            result = 1;
        }
    }
    if (!result && !deliver(path, APP_WAIT_MS)) {
        complain(L"App Sandbox didn't respond, so nothing was run. Try again once it has finished starting.");
        result = 1;
    }
    if (gate) { ReleaseMutex(gate); CloseHandle(gate); }
    LocalFree(argv);
    return result;
}

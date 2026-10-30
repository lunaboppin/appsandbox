/*
 * appsandbox-backstage.exe — Backstage session for AppSandbox.
 *
 * Like ScreenConnect's Backstage: a SYSTEM shell on a Windows desktop of its
 * own in the console session, for working on the VM beside the signed-in
 * user's desktop (which keeps running, untouched) or before anyone signs in.
 *
 * The agent starts it as SYSTEM in the console session. That first instance
 * creates the AppSandboxBackstage desktop and starts itself again on it
 * (--shell), so every program the shell opens lands there too. The shell
 * switches the screen and input to that desktop, shows a taskbar with tools
 * and an Explorer file browser, and switches back when Exit Backstage is
 * clicked or the agent sets the exit event. Programs left open stay on the
 * desktop and are there again the next time.
 *
 * Logs to C:\Windows\AppSandbox\backstage.log (beside agent.log).
 */

#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* Shared with the agent (agent.c). */
#define BACKSTAGE_DESKTOP       L"AppSandboxBackstage"
#define BACKSTAGE_EXIT_EVENT    L"Global\\AppSandboxBackstageExit"
#define BACKSTAGE_RUNNING_MUTEX L"Global\\AppSandboxBackstageRunning"

#define BAR_HEIGHT   44
#define BTN_HEIGHT   32
#define FILES_BAR    36

#define ID_FILES     100
#define ID_CMDPROMPT       101
#define ID_PS        102
#define ID_TASKMGR   103
#define ID_MORE      104
#define ID_EXIT      105
#define ID_REGEDIT   110
#define ID_SERVICES  111
#define ID_EVENTS    112
#define ID_COMPMGMT  113
#define ID_CONTROL   114
#define ID_NOTEPAD   115
#define ID_RUN       116
#define ID_WINDOW0   200        /* taskbar buttons of open windows */
#define MAX_WINDOWS  24

#define ID_BACK      300
#define ID_FORWARD   301
#define ID_UP        302
#define ID_ADDRESS   303
#define ID_GO        304

#define CLR_DESKTOP  RGB(0x0f, 0x17, 0x22)
#define CLR_BAR      RGB(0x16, 0x1f, 0x2c)
#define CLR_BUTTON   RGB(0x22, 0x2c, 0x3a)
#define CLR_PRESSED  RGB(0x3a, 0x4a, 0x60)
#define CLR_ACTIVE   RGB(0x2f, 0x4d, 0x73)
#define CLR_EXIT     RGB(0x8a, 0x2b, 0x2b)
#define CLR_TEXT     RGB(0xe6, 0xed, 0xf5)
#define CLR_DIM      RGB(0x8d, 0xa2, 0xb8)

static HDESK   g_desk;
static wchar_t g_prev_desktop[64] = L"Default";
static HWND    g_bg, g_bar;
static HFONT   g_font, g_font_bold, g_font_title;
static HANDLE  g_exit_event;
static BOOL    g_exiting;
static HWND    g_windows[MAX_WINDOWS];   /* windows the taskbar buttons stand for */
static HWND    g_window_buttons[MAX_WINDOWS];
static int     g_window_count;

/* ---- Logging ---- */

static void bs_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\backstage.log", "a") != 0 || !f)
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

/* ---- Desktops ---- */

static BOOL input_desktop_name(wchar_t *name, DWORD chars)
{
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    BOOL ok;
    name[0] = L'\0';
    if (!desk) return FALSE;
    ok = GetUserObjectInformationW(desk, UOI_NAME, name, chars * sizeof(wchar_t), NULL);
    CloseDesktop(desk);
    return ok;
}

/* Back to the desktop that was showing before, but only while ours is still
   the one showing: if Windows moved to its own (locking does) it stays there,
   so leaving Backstage never skips the lock screen. */
static void switch_back(void)
{
    wchar_t now[64];
    HDESK prev;
    if (!input_desktop_name(now, _countof(now)) || _wcsicmp(now, BACKSTAGE_DESKTOP) != 0) {
        bs_log("Leaving Backstage; %ls is showing, so it stays.", now[0] ? now : L"another desktop");
        return;
    }
    prev = OpenDesktopW(g_prev_desktop, 0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!prev || !SwitchDesktop(prev)) {
        bs_log("SwitchDesktop(%ls) failed: %lu", g_prev_desktop, GetLastError());
        if (prev) CloseDesktop(prev);
        return;
    }
    CloseDesktop(prev);
    bs_log("Switched back to %ls.", g_prev_desktop);
}

/* First instance: make the desktop and start the shell on it. */
static int launch_shell(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16], desktop[64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HDESK desk = CreateDesktopW(BACKSTAGE_DESKTOP, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desk) {
        bs_log("CreateDesktop failed: %lu", GetLastError());
        return 2;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf_s(cmd, _countof(cmd), L"\"%s\" --shell", exe);
    swprintf_s(desktop, _countof(desktop), L"WinSta0\\%s", BACKSTAGE_DESKTOP);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = desktop;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        bs_log("Starting the shell failed: %lu", GetLastError());
        CloseDesktop(desk);
        return 3;
    }
    /* Wait until the shell shows its desktop (it takes the running mutex) or
       gives up; its exit code becomes ours, so the agent can say why. */
    {
        DWORD code = 6;   /* timed out */
        int i;
        for (i = 0; i < 150; i++) {
            HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, BACKSTAGE_RUNNING_MUTEX);
            if (running) {
                CloseHandle(running);
                code = 0;
                break;
            }
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
                GetExitCodeProcess(pi.hProcess, &code);
                if (code == 0) code = 5;   /* ended without showing */
                break;
            }
        }
        if (code) bs_log("The shell did not come up (code %lu).", code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseDesktop(desk);
        return (int)code;
    }
}

/* ---- Launching tools ---- */

static void launch(const wchar_t *file, const wchar_t *params)
{
    HINSTANCE r = ShellExecuteW(NULL, L"open", file, params, L"C:\\", SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32)
        bs_log("Could not open %ls (%Id).", file, (INT_PTR)r);
}

typedef void (WINAPI *RunFileDlgFn)(HWND, HICON, LPCWSTR, LPCWSTR, LPCWSTR, UINT);

static void show_run_dialog(void)
{
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    RunFileDlgFn run = shell32 ? (RunFileDlgFn)GetProcAddress(shell32, MAKEINTRESOURCEA(61)) : NULL;
    if (run)
        run(g_bar, NULL, L"C:\\", L"Run as SYSTEM",
            L"Type the name of a program, folder, document or Internet resource. "
            L"It opens on the Backstage desktop as SYSTEM.", 0);
}

/* ---- Files window (Explorer's own file view, hosted here) ---- */

typedef struct {
    HWND hwnd, back, forward, up, address, go;
    IExplorerBrowser *browser;
    wchar_t shown[1024];
} FilesWindow;

static FilesWindow *files_of(HWND hwnd)
{
    wchar_t cls[32];
    if (!hwnd || !GetClassNameW(hwnd, cls, _countof(cls)) || wcscmp(cls, L"AsbBackstageFiles") != 0)
        return NULL;
    return (FilesWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void files_browse(FilesWindow *fw, PCIDLIST_ABSOLUTE pidl, UINT flags)
{
    if (fw->browser) IExplorerBrowser_BrowseToIDList(fw->browser, pidl, flags);
}

static void files_go(FilesWindow *fw)
{
    wchar_t typed[1024], path[1024];
    PIDLIST_ABSOLUTE pidl = NULL;
    GetWindowTextW(fw->address, typed, _countof(typed));
    if (!ExpandEnvironmentStringsW(typed, path, _countof(path)))
        wcscpy_s(path, _countof(path), typed);
    if (SUCCEEDED(SHParseDisplayName(path, NULL, &pidl, 0, NULL))) {
        files_browse(fw, pidl, SBSP_ABSOLUTE);
        CoTaskMemFree(pidl);
        SetFocus(fw->hwnd);
    } else {
        MessageBeep(MB_ICONWARNING);
    }
}

/* Keep the address bar and title on the folder being shown. */
static void files_sync_address(FilesWindow *fw)
{
    IFolderView *view = NULL;
    IPersistFolder2 *folder = NULL;
    PIDLIST_ABSOLUTE pidl = NULL;
    PWSTR path = NULL, label = NULL;
    wchar_t title[1100];

    if (!fw->browser ||
        FAILED(IExplorerBrowser_GetCurrentView(fw->browser, &IID_IFolderView, (void **)&view)))
        return;
    if (SUCCEEDED(IFolderView_GetFolder(view, &IID_IPersistFolder2, (void **)&folder)) &&
        SUCCEEDED(IPersistFolder2_GetCurFolder(folder, &pidl)) &&
        SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEEDITING, &path)) &&
        wcscmp(path, fw->shown) != 0) {
        wcscpy_s(fw->shown, _countof(fw->shown), path);
        if (GetFocus() != fw->address)
            SetWindowTextW(fw->address, path);
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &label))) {
            swprintf_s(title, _countof(title), L"%s - Files (SYSTEM)", label);
            SetWindowTextW(fw->hwnd, title);
            CoTaskMemFree(label);
        }
    }
    if (path) CoTaskMemFree(path);
    if (pidl) CoTaskMemFree(pidl);
    if (folder) IPersistFolder2_Release(folder);
    IFolderView_Release(view);
}

static void files_layout(FilesWindow *fw)
{
    RECT rc;
    int w;
    GetClientRect(fw->hwnd, &rc);
    w = rc.right - rc.left;
    MoveWindow(fw->back, 6, 5, 30, 26, TRUE);
    MoveWindow(fw->forward, 38, 5, 30, 26, TRUE);
    MoveWindow(fw->up, 70, 5, 30, 26, TRUE);
    MoveWindow(fw->address, 108, 6, w - 108 - 54, 24, TRUE);
    MoveWindow(fw->go, w - 48, 5, 42, 26, TRUE);
    rc.top = FILES_BAR;
    if (fw->browser) IExplorerBrowser_SetRect(fw->browser, NULL, rc);
}

static HWND files_button(HWND parent, const wchar_t *text, int id)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

static LRESULT CALLBACK files_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FilesWindow *fw = (FilesWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
    {
        FOLDERSETTINGS fs = { FVM_DETAILS, 0 };
        PIDLIST_ABSOLUTE start = NULL;
        RECT rc;
        fw = (FilesWindow *)calloc(1, sizeof(*fw));
        if (!fw) return -1;
        fw->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)fw);
        fw->back = files_button(hwnd, L"\u2190", ID_BACK);
        fw->forward = files_button(hwnd, L"\u2192", ID_FORWARD);
        fw->up = files_button(hwnd, L"\u2191", ID_UP);
        fw->go = files_button(hwnd, L"Go", ID_GO);
        fw->address = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_ADDRESS, NULL, NULL);
        SendMessageW(fw->address, WM_SETFONT, (WPARAM)g_font, TRUE);
        GetClientRect(hwnd, &rc);
        rc.top = FILES_BAR;
        if (SUCCEEDED(CoCreateInstance(&CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER,
                                       &IID_IExplorerBrowser, (void **)&fw->browser))) {
            if (FAILED(IExplorerBrowser_Initialize(fw->browser, hwnd, &rc, &fs))) {
                IExplorerBrowser_Release(fw->browser);
                fw->browser = NULL;
            }
        }
        if (!fw->browser) {
            bs_log("The Explorer file view could not be created.");
            return -1;
        }
        IExplorerBrowser_SetOptions(fw->browser, EBO_SHOWFRAMES);
        IExplorerBrowser_SetPropertyBag(fw->browser, L"AppSandbox.Backstage.Files");
        if (SUCCEEDED(SHGetKnownFolderIDList(&FOLDERID_ComputerFolder, 0, NULL, &start))) {
            files_browse(fw, start, SBSP_ABSOLUTE);
            CoTaskMemFree(start);
        }
        files_layout(fw);
        SetTimer(hwnd, 1, 300, NULL);
        return 0;
    }
    case WM_SIZE:
        if (fw) files_layout(fw);
        return 0;
    case WM_TIMER:
        if (fw) files_sync_address(fw);
        return 0;
    case WM_COMMAND:
        if (!fw) break;
        switch (LOWORD(wp)) {
        case ID_BACK:    files_browse(fw, NULL, SBSP_NAVIGATEBACK); return 0;
        case ID_FORWARD: files_browse(fw, NULL, SBSP_NAVIGATEFORWARD); return 0;
        case ID_UP:      files_browse(fw, NULL, SBSP_PARENT); return 0;
        case ID_GO:      files_go(fw); return 0;
        }
        break;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 420;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 260;
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (fw) {
            if (fw->browser) {
                IExplorerBrowser_Destroy(fw->browser);
                IExplorerBrowser_Release(fw->browser);
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            free(fw);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void open_files_window(void)
{
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) - BAR_HEIGHT };
    int w = (work.right * 3) / 4, h = (work.bottom * 3) / 4;
    static int cascade;
    int x = (work.right - w) / 2 + cascade, y = (work.bottom - h) / 2 + cascade;
    HWND hwnd;
    cascade = (cascade + 28) % 140;
    hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"AsbBackstageFiles", L"Files (SYSTEM)",
                           WS_OVERLAPPEDWINDOW, x, y, w, h, NULL, NULL, NULL, NULL);
    if (!hwnd) {
        MessageBoxW(g_bar, L"The file browser could not be opened. See backstage.log.",
                    L"Backstage", MB_ICONWARNING);
        return;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

/* Give the file view first go at keys (Ctrl+C, Delete, F2...), except while
   typing in the address bar, where Enter navigates. */
static BOOL files_pretranslate(MSG *msg)
{
    FilesWindow *fw;
    IShellView *view = NULL;
    BOOL handled = FALSE;
    if (msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST) return FALSE;
    fw = files_of(GetAncestor(msg->hwnd, GA_ROOT));
    if (!fw) return FALSE;
    if (msg->hwnd == fw->address) {
        if (msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN) {
            files_go(fw);
            return TRUE;
        }
        return FALSE;
    }
    if (fw->browser &&
        SUCCEEDED(IExplorerBrowser_GetCurrentView(fw->browser, &IID_IShellView, (void **)&view))) {
        handled = IShellView_TranslateAccelerator(view, msg) == S_OK;
        IShellView_Release(view);
    }
    return handled;
}

/* ---- Taskbar ---- */

static BOOL is_taskbar_window(HWND hwnd)
{
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (hwnd == g_bg || hwnd == g_bar || !IsWindowVisible(hwnd)) return FALSE;
    if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return FALSE;
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return FALSE;
    return GetWindowTextLengthW(hwnd) > 0;
}

typedef struct { HWND list[MAX_WINDOWS]; int count; } WindowList;

static BOOL CALLBACK collect_window(HWND hwnd, LPARAM lp)
{
    WindowList *wl = (WindowList *)lp;
    if (wl->count < MAX_WINDOWS && is_taskbar_window(hwnd))
        wl->list[wl->count++] = hwnd;
    return TRUE;
}

static int bar_windows_left(void)
{
    return 8 + 96 + (72 + 128 + 100 + 112 + 80) + 5 * 6 + 14;
}

static void layout_window_buttons(void)
{
    RECT rc;
    int left = bar_windows_left(), right, each, i;
    GetClientRect(g_bar, &rc);
    right = rc.right - 8 - 128 - 8 - 72;
    if (g_window_count == 0) return;
    each = (right - left) / g_window_count - 6;
    if (each > 200) each = 200;
    if (each < 48) each = 48;
    for (i = 0; i < g_window_count; i++)
        MoveWindow(g_window_buttons[i], left + i * (each + 6), (BAR_HEIGHT - BTN_HEIGHT) / 2,
                   each, BTN_HEIGHT, TRUE);
}

/* Rebuild the window buttons when the set of windows changes; repaint them
   (titles, which one is active) every tick. */
static void refresh_window_buttons(void)
{
    WindowList wl;
    int i;
    BOOL changed;
    ZeroMemory(&wl, sizeof(wl));
    EnumDesktopWindows(g_desk, collect_window, (LPARAM)&wl);
    changed = wl.count != g_window_count ||
              memcmp(wl.list, g_windows, sizeof(HWND) * (size_t)wl.count) != 0;
    if (changed) {
        for (i = 0; i < g_window_count; i++) DestroyWindow(g_window_buttons[i]);
        memcpy(g_windows, wl.list, sizeof(g_windows));
        g_window_count = wl.count;
        for (i = 0; i < g_window_count; i++)
            g_window_buttons[i] = CreateWindowExW(0, L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
                g_bar, (HMENU)(INT_PTR)(ID_WINDOW0 + i), NULL, NULL);
        layout_window_buttons();
    }
    for (i = 0; i < g_window_count; i++) InvalidateRect(g_window_buttons[i], NULL, FALSE);
}

static void activate_window(HWND hwnd)
{
    if (!IsWindow(hwnd)) return;
    if (GetForegroundWindow() == hwnd && !IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_MINIMIZE);
        return;
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

static void show_more_menu(HWND button)
{
    RECT r;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_REGEDIT, L"Registry Editor");
    AppendMenuW(m, MF_STRING, ID_SERVICES, L"Services");
    AppendMenuW(m, MF_STRING, ID_EVENTS, L"Event Viewer");
    AppendMenuW(m, MF_STRING, ID_COMPMGMT, L"Computer Management");
    AppendMenuW(m, MF_STRING, ID_CONTROL, L"Control Panel");
    AppendMenuW(m, MF_STRING, ID_NOTEPAD, L"Notepad");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_RUN, L"Run...");
    GetWindowRect(button, &r);
    SetForegroundWindow(g_bar);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN, r.left, r.top, 0, g_bar, NULL);
    PostMessageW(g_bar, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void draw_button(const DRAWITEMSTRUCT *di)
{
    wchar_t text[256];
    HBRUSH brush;
    COLORREF bg = CLR_BUTTON;
    RECT rc = di->rcItem;
    int id = (int)di->CtlID;
    if (id == ID_EXIT) bg = CLR_EXIT;
    if (id >= ID_WINDOW0 && id < ID_WINDOW0 + g_window_count) {
        HWND w = g_windows[id - ID_WINDOW0];
        GetWindowTextW(w, text, _countof(text));
        if (GetForegroundWindow() == w) bg = CLR_ACTIVE;
    } else {
        GetWindowTextW(di->hwndItem, text, _countof(text));
    }
    if (di->itemState & ODS_SELECTED) bg = CLR_PRESSED;
    brush = CreateSolidBrush(bg);
    FillRect(di->hDC, &rc, brush);
    DeleteObject(brush);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, CLR_TEXT);
    SelectObject(di->hDC, g_font);
    InflateRect(&rc, -8, 0);
    DrawTextW(di->hDC, text, -1, &rc, DT_SINGLELINE | DT_VCENTER |
              (id >= ID_WINDOW0 ? DT_LEFT : DT_CENTER) | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static HWND bar_button(const wchar_t *text, int id, int x, int w)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                           x, (BAR_HEIGHT - BTN_HEIGHT) / 2, w, BTN_HEIGHT,
                           g_bar, (HMENU)(INT_PTR)id, NULL, NULL);
}

static void place_shell_windows(void)
{
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_bg, HWND_BOTTOM, 0, 0, cx, cy, SWP_NOACTIVATE);
    SetWindowPos(g_bar, HWND_TOPMOST, 0, cy - BAR_HEIGHT, cx, BAR_HEIGHT, SWP_NOACTIVATE);
    {
        RECT rc;
        GetClientRect(g_bar, &rc);
        MoveWindow(GetDlgItem(g_bar, ID_EXIT), rc.right - 8 - 128, (BAR_HEIGHT - BTN_HEIGHT) / 2,
                   128, BTN_HEIGHT, TRUE);
    }
    layout_window_buttons();
}

static void leave_backstage(void)
{
    HWND w;
    if (g_exiting) return;
    g_exiting = TRUE;
    switch_back();
    /* Close our own file windows (the Explorer view needs its teardown);
       other programs stay on the desktop for next time. */
    while ((w = FindWindowExW(NULL, NULL, L"AsbBackstageFiles", NULL)) != NULL)
        DestroyWindow(w);
    DestroyWindow(g_bar);
    DestroyWindow(g_bg);
    PostQuitMessage(0);
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
    {
        int x = 8 + 96;
        g_bar = hwnd;
        bar_button(L"Files", ID_FILES, x, 72);              x += 72 + 6;
        bar_button(L"Command Prompt", ID_CMDPROMPT, x, 128);      x += 128 + 6;
        bar_button(L"PowerShell", ID_PS, x, 100);           x += 100 + 6;
        bar_button(L"Task Manager", ID_TASKMGR, x, 112);    x += 112 + 6;
        bar_button(L"More \u25B4", ID_MORE, x, 80);
        bar_button(L"Exit Backstage", ID_EXIT, 0, 128);
        SetTimer(hwnd, 1, 1000, NULL);
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, label, clock;
        HBRUSH brush = CreateSolidBrush(CLR_BAR);
        SYSTEMTIME t;
        wchar_t now[16];
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        label = rc; label.left = 12; label.right = 8 + 96;
        SelectObject(dc, g_font_bold);
        SetTextColor(dc, CLR_TEXT);
        DrawTextW(dc, L"BACKSTAGE", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        GetLocalTime(&t);
        swprintf_s(now, _countof(now), L"%02u:%02u", t.wHour, t.wMinute);
        clock = rc; clock.right = rc.right - 8 - 128 - 8; clock.left = clock.right - 72;
        SelectObject(dc, g_font);
        SetTextColor(dc, CLR_DIM);
        DrawTextW(dc, now, -1, &clock, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (!g_exiting) {
            refresh_window_buttons();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id >= ID_WINDOW0 && id < ID_WINDOW0 + g_window_count) {
            activate_window(g_windows[id - ID_WINDOW0]);
            return 0;
        }
        switch (id) {
        case ID_FILES:    open_files_window(); break;
        case ID_CMDPROMPT:      launch(L"cmd.exe", L"/k title Command Prompt (SYSTEM)"); break;
        case ID_PS:       launch(L"powershell.exe", L"-NoExit -NoLogo -Command \"$Host.UI.RawUI.WindowTitle='PowerShell (SYSTEM)'; Set-Location 'C:\\'\""); break;
        case ID_TASKMGR:  launch(L"taskmgr.exe", NULL); break;
        case ID_MORE:     show_more_menu((HWND)lp); break;
        case ID_REGEDIT:  launch(L"regedit.exe", L"-m"); break;
        case ID_SERVICES: launch(L"mmc.exe", L"services.msc"); break;
        case ID_EVENTS:   launch(L"mmc.exe", L"eventvwr.msc"); break;
        case ID_COMPMGMT: launch(L"mmc.exe", L"compmgmt.msc"); break;
        case ID_CONTROL:  launch(L"control.exe", NULL); break;
        case ID_NOTEPAD:  launch(L"notepad.exe", NULL); break;
        case ID_RUN:      show_run_dialog(); break;
        case ID_EXIT:     leave_backstage(); break;
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        place_shell_windows();
        return 0;
    case WM_CLOSE:
        leave_backstage();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK bg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_WINDOWPOSCHANGING:
        /* The backdrop always stays under everything else. */
        ((WINDOWPOS *)lp)->hwndInsertAfter = HWND_BOTTOM;
        ((WINDOWPOS *)lp)->flags &= ~SWP_NOZORDER;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, title, sub;
        HBRUSH brush = CreateSolidBrush(CLR_DESKTOP);
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = L"";
        wchar_t line[512];
        DWORD n = _countof(computer), session = 0;
        GetComputerNameW(computer, &n);
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        title = rc; title.top = rc.bottom / 2 - 70; title.bottom = title.top + 56;
        SelectObject(dc, g_font_title);
        SetTextColor(dc, CLR_TEXT);
        DrawTextW(dc, L"Backstage", -1, &title, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        swprintf_s(line, _countof(line),
                   L"NT AUTHORITY\\SYSTEM on %s (session %lu)\n"
                   L"The %s desktop keeps running unchanged. Exit Backstage returns to it.",
                   computer, session,
                   _wcsicmp(g_prev_desktop, L"Default") == 0 ? L"signed-in" : L"sign-in");
        sub = rc; sub.top = title.bottom + 8; sub.left += 40; sub.right -= 40;
        SelectObject(dc, g_font);
        SetTextColor(dc, CLR_DIM);
        DrawTextW(dc, line, -1, &sub, DT_CENTER | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI exit_watch_thread(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(g_exit_event, INFINITE) == WAIT_OBJECT_0)
        PostMessageW(g_bar, WM_CLOSE, 0, 0);
    return 0;
}

static void register_class(const wchar_t *name, WNDPROC proc, HBRUSH background)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

static int run_shell(void)
{
    HANDLE running, watcher;
    wchar_t name[64];
    MSG msg;

    running = OpenMutexW(SYNCHRONIZE, FALSE, BACKSTAGE_RUNNING_MUTEX);
    if (running) {                       /* one shell at a time */
        CloseHandle(running);
        return 0;
    }
    if (input_desktop_name(name, _countof(name)) && name[0] &&
        _wcsicmp(name, BACKSTAGE_DESKTOP) != 0)
        wcscpy_s(g_prev_desktop, _countof(g_prev_desktop), name);

    /* Opened by name for switch rights; the handle the process started with
       may not carry DESKTOP_SWITCHDESKTOP. */
    g_desk = OpenDesktopW(BACKSTAGE_DESKTOP, 0, FALSE, GENERIC_ALL);
    if (!g_desk) {
        bs_log("OpenDesktop(%ls) failed: %lu", BACKSTAGE_DESKTOP, GetLastError());
        return 7;
    }
    if (!SwitchDesktop(g_desk)) {
        bs_log("SwitchDesktop to Backstage failed: %lu (showing %ls)", GetLastError(), g_prev_desktop);
        return 4;
    }
    bs_log("Backstage shown (was %ls).", g_prev_desktop);
    running = CreateMutexW(NULL, FALSE, BACKSTAGE_RUNNING_MUTEX);

    OleInitialize(NULL);
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_title = CreateFontW(-44, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, 0, L"Segoe UI");
    register_class(L"AsbBackstageDesktop", bg_proc, NULL);
    register_class(L"AsbBackstageBar", bar_proc, NULL);
    register_class(L"AsbBackstageFiles", files_proc, (HBRUSH)(COLOR_BTNFACE + 1));

    g_bg = CreateWindowExW(WS_EX_TOOLWINDOW, L"AsbBackstageDesktop", L"Backstage",
                           WS_POPUP | WS_VISIBLE, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"AsbBackstageBar", L"Backstage taskbar",
                    WS_POPUP | WS_VISIBLE, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    place_shell_windows();
    refresh_window_buttons();

    g_exit_event = OpenEventW(SYNCHRONIZE, FALSE, BACKSTAGE_EXIT_EVENT);
    if (g_exit_event) {
        watcher = CreateThread(NULL, 0, exit_watch_thread, NULL, 0, NULL);
        if (watcher) CloseHandle(watcher);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (files_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    if (running) CloseHandle(running);
    bs_log("Backstage closed.");
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst; (void)prev; (void)show;
    if (cmd && wcsstr(cmd, L"--shell")) return run_shell();
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        bs_log("Opening Backstage in session %lu.", session);
    }
    return launch_shell();
}

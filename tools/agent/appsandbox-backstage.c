/*
 * appsandbox-backstage.exe — Backstage session for AppSandbox.
 *
 * Like ScreenConnect's Backstage: a SYSTEM shell in session 0, the services
 * session, which has a screen of its own that nothing in the VM shows. The
 * signed-in user (or the sign-in screen) never sees it, and everything it
 * opens runs there too, as SYSTEM.
 *
 * The agent starts it in session 0 on WinSta0\Default. It shows a taskbar
 * with tools, open windows and Exit, and an Explorer file browser, and it
 * streams that screen to the host's Backstage window over its own channel
 * (ASB_CH_BACKSTAGE): bands of rows that changed, as raw BGRA. The host's
 * mouse and keyboard come back on the same connection and are injected
 * here. Exit Backstage, or the agent's exit event, ends it; programs left
 * open keep running in session 0 and are there again next time.
 *
 * Logs to C:\Windows\AppSandbox\backstage.log (beside agent.log).
 */

#define COBJMACROS
#include "../transport/asb_transport.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* Shared with the agent (agent.c). */
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
static DWORD   g_session;
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

/* ---- SYSTEM profile ---- */

/* The SYSTEM profile has no Desktop, Documents or Downloads folder until
   something makes one, and Explorer's view reports Desktop "unavailable". */
static void ensure_profile_folders(void)
{
    const KNOWNFOLDERID *ids[] = { &FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads };
    int i;
    for (i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
        PWSTR path = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(ids[i], KF_FLAG_CREATE, NULL, &path)))
            SHCreateDirectoryExW(NULL, path, NULL);
        if (path) CoTaskMemFree(path);
    }
}

/* Session 0's screen starts small; take the largest mode up to 1920x1080. */
static void choose_screen_mode(void)
{
    DEVMODEW dm, best;
    DWORD i;
    BOOL found = FALSE;
    LONG r;
    ZeroMemory(&best, sizeof(best));
    for (i = 0; ; i++) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(NULL, i, &dm)) break;
        if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth > 1920 || dm.dmPelsHeight > 1080) continue;
        if (!found || dm.dmPelsWidth * dm.dmPelsHeight > best.dmPelsWidth * best.dmPelsHeight) {
            best = dm;
            found = TRUE;
        }
    }
    if (!found) return;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    r = ChangeDisplaySettingsW(&best, 0);
    bs_log("Screen %lux%lu (%s).", best.dmPelsWidth, best.dmPelsHeight,
           r == DISP_CHANGE_SUCCESSFUL ? "set" : "could not change; kept the current size");
}

/* ---- Streaming to the host's Backstage window ---- */

#define BS_FRAME_MAGIC  0x52465342u   /* "BSFR" little-endian */
#define BS_INPUT_MAGIC  0x4E495342u   /* "BSIN" little-endian */
#define BS_END_MAGIC    0x444E4542u   /* "BEND": the session is ending, don't restart it */
#define BS_BAND         32            /* rows per band compared and sent */
#define BS_FRAME_MS     50

#define BS_IN_MOVE      1   /* a=x b=y */
#define BS_IN_BUTTON    2   /* a=0 left,1 right,2 middle  b=1 down,0 up */
#define BS_IN_WHEEL     3   /* a=delta */
#define BS_IN_KEY       4   /* a=vk b=scan c=flags (1 up, 2 extended) */

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 width;
    UINT32 height;
    UINT32 y;
    UINT32 rows;     /* rows * width * 4 bytes of BGRA follow */
} BsFrameHeader;

typedef struct {
    UINT32 magic;
    UINT32 type;
    INT32  a, b, c;
} BsInput;
#pragma pack(pop)

/* Session 0 has no mouse or keyboard: SendInput "succeeds" there and nothing
   happens. So input is delivered as window messages, the way the input
   system would: mouse to the window holding capture or under the pointer,
   keys to the focus of the window last clicked. The input thread shares
   the target thread's input state (AttachThreadInput + SetKeyboardState),
   so Shift, Ctrl and Alt read as held and TranslateMessage makes the right
   characters. */
static POINT g_mouse;
static BYTE  g_keys[256];   /* keys and buttons the host holds, as GetKeyboardState has them */
static HWND  g_active;      /* top-level window last clicked; keys go to its focus */
static DWORD g_attached;    /* thread whose input state the input thread shares */
static struct { HWND hwnd; int button; DWORD time; POINT pt; } g_last_down;
static struct { HWND hwnd; LRESULT hit; POINT start; RECT rect; } g_drag;   /* title bar or border */

static BOOL send_all(AsbConn *c, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = asb_send(c, p, len);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
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

static void share_input_with(HWND w)
{
    DWORD tid = w ? GetWindowThreadProcessId(w, NULL) : 0;
    if (tid == g_attached) return;
    if (g_attached) AttachThreadInput(GetCurrentThreadId(), g_attached, FALSE);
    g_attached = 0;
    if (tid && tid != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), tid, TRUE))
        g_attached = tid;
}

static void apply_key_state(HWND w)
{
    share_input_with(w);
    SetKeyboardState(g_keys);
}

static WPARAM mouse_keys(void)
{
    WPARAM k = 0;
    if (g_keys[VK_LBUTTON] & 0x80) k |= MK_LBUTTON;
    if (g_keys[VK_RBUTTON] & 0x80) k |= MK_RBUTTON;
    if (g_keys[VK_MBUTTON] & 0x80) k |= MK_MBUTTON;
    if (g_keys[VK_SHIFT] & 0x80)   k |= MK_SHIFT;
    if (g_keys[VK_CONTROL] & 0x80) k |= MK_CONTROL;
    return k;
}

/* Bring the clicked window forward, as a real click would. */
static void activate(HWND w)
{
    HWND root = GetAncestor(w, GA_ROOT), fg = GetForegroundWindow();
    GUITHREADINFO gti;
    static BOOL logged;
    if (!root || (GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_NOACTIVATE)) return;
    g_active = root;
    if (fg != root) {
        if (fg) share_input_with(fg);   /* sharing the foreground's input lets it move */
        SetForegroundWindow(root);
        share_input_with(root);
        BringWindowToTop(root);
        SetActiveWindow(root);
        if (!logged) {
            logged = TRUE;
            bs_log(GetForegroundWindow() == root ? "Input: windows can take the foreground."
                                                 : "Input: SetForegroundWindow was refused.");
        }
    }
    /* Windows that don't take focus on a click (consoles) still need it
       for keys; the focus moves only if it isn't already inside. */
    share_input_with(root);
    ZeroMemory(&gti, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(root, NULL), &gti) ||
        !gti.hwndFocus || (gti.hwndFocus != root && !IsChild(root, gti.hwndFocus)))
        SetFocus(root);
}

static HWND capture_of(HWND w, BOOL *menu)
{
    GUITHREADINFO gti;
    ZeroMemory(&gti, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!w || !IsWindow(w) || !GetGUIThreadInfo(GetWindowThreadProcessId(w, NULL), &gti)) return NULL;
    *menu = (gti.flags & GUI_INMENUMODE) != 0;
    return gti.hwndCapture;
}

/* The window holding capture (a pressed button, a scroll bar, an open
   menu) gets the mouse wherever it is; menus take screen coordinates. */
static HWND mouse_capture(HWND under, BOOL *menu)
{
    HWND cap;
    *menu = FALSE;
    cap = capture_of(g_active, menu);
    if (!cap) cap = capture_of(under, menu);
    return cap;
}

static LRESULT hit_test(HWND w, POINT pt)
{
    DWORD_PTR hit = HTCLIENT;
    if (!SendMessageTimeoutW(w, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG, 100, &hit))
        return HTCLIENT;
    return (LRESULT)hit;
}

static BOOL in_client(LRESULT hit)
{
    return hit == HTCLIENT || hit == HTNOWHERE || hit == HTERROR || hit == HTTRANSPARENT;
}

static void post_mouse(HWND w, UINT msg, WPARAM wp, POINT pt, BOOL screen)
{
    if (!screen) ScreenToClient(w, &pt);
    PostMessageW(w, msg, wp, MAKELPARAM(pt.x, pt.y));
}

static void drag_to(POINT pt)
{
    RECT r = g_drag.rect;
    LRESULT h = g_drag.hit;
    int dx = pt.x - g_drag.start.x, dy = pt.y - g_drag.start.y;
    int minw = GetSystemMetrics(SM_CXMINTRACK), minh = GetSystemMetrics(SM_CYMINTRACK);
    if (h == HTCAPTION) {
        OffsetRect(&r, dx, dy);
    } else {
        if (h == HTLEFT || h == HTTOPLEFT || h == HTBOTTOMLEFT)
            r.left = min(r.left + dx, r.right - minw);
        if (h == HTRIGHT || h == HTTOPRIGHT || h == HTBOTTOMRIGHT)
            r.right = max(r.right + dx, r.left + minw);
        if (h == HTTOP || h == HTTOPLEFT || h == HTTOPRIGHT)
            r.top = min(r.top + dy, r.bottom - minh);
        if (h == HTBOTTOM || h == HTBOTTOMLEFT || h == HTBOTTOMRIGHT)
            r.bottom = max(r.bottom + dy, r.top + minh);
    }
    SetWindowPos(g_drag.hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (h == HTCAPTION ? SWP_NOSIZE : 0));
}

static void mouse_move(int x, int y)
{
    POINT pt;
    HWND w, cap;
    BOOL menu;
    LRESULT hit;
    static BOOL logged;
    g_mouse.x = x;
    g_mouse.y = y;
    /* Programs that ask where the pointer is (GetMessagePos: tree views,
       MMC) need it really there, not only in the messages. */
    if (!SetCursorPos(x, y) && !logged) {
        logged = TRUE;
        bs_log("Input: the pointer could not be moved (%lu).", GetLastError());
    }
    pt = g_mouse;
    if (g_drag.hwnd) {
        drag_to(pt);
        return;
    }
    w = WindowFromPoint(pt);
    if ((cap = mouse_capture(w, &menu)) != NULL) {
        post_mouse(cap, WM_MOUSEMOVE, mouse_keys(), pt, menu);
        return;
    }
    if (!w) return;
    hit = hit_test(w, pt);
    if (in_client(hit)) post_mouse(w, WM_MOUSEMOVE, mouse_keys(), pt, FALSE);
    else PostMessageW(w, WM_NCMOUSEMOVE, (WPARAM)hit, MAKELPARAM(pt.x, pt.y));
}

static void mouse_button(int b, BOOL down)
{
    static const int  vk[]    = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    static const UINT cdown[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
    static const UINT cup[]   = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
    static const UINT cdbl[]  = { WM_LBUTTONDBLCLK, WM_RBUTTONDBLCLK, WM_MBUTTONDBLCLK };
    static const UINT ndown[] = { WM_NCLBUTTONDOWN, WM_NCRBUTTONDOWN, WM_NCMBUTTONDOWN };
    static const UINT nup[]   = { WM_NCLBUTTONUP, WM_NCRBUTTONUP, WM_NCMBUTTONUP };
    static const UINT ndbl[]  = { WM_NCLBUTTONDBLCLK, WM_NCRBUTTONDBLCLK, WM_NCMBUTTONDBLCLK };
    POINT pt = g_mouse;
    HWND w, cap, root;
    BOOL menu, dbl = FALSE;
    LRESULT hit;

    g_keys[vk[b]] = down ? 0x80 : 0;
    if (g_drag.hwnd && b == 0 && !down) {
        g_drag.hwnd = NULL;
        return;
    }
    w = WindowFromPoint(pt);
    if ((cap = mouse_capture(w, &menu)) != NULL) {
        apply_key_state(cap);
        post_mouse(cap, down ? cdown[b] : cup[b], mouse_keys(), pt, menu);
        return;
    }
    if (!w) return;
    hit = hit_test(w, pt);
    if (down) {
        static int logged;
        DWORD now = GetTickCount();
        if (logged < 40) {   /* enough to see where clicks land */
            wchar_t cls[64] = L"";
            GetClassNameW(w, cls, _countof(cls));
            bs_log("Click %d,%d: %ls (hit %Id).", pt.x, pt.y, cls, (INT_PTR)hit);
            logged++;
        }
        dbl = g_last_down.hwnd == w && g_last_down.button == b &&
              now - g_last_down.time <= GetDoubleClickTime() &&
              abs(pt.x - g_last_down.pt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
              abs(pt.y - g_last_down.pt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        g_last_down.hwnd = dbl ? NULL : w;   /* a third click starts over */
        g_last_down.button = b;
        g_last_down.time = now;
        g_last_down.pt = pt;
        activate(w);
    }
    apply_key_state(w);
    if (in_client(hit)) {
        UINT msg = !down ? cup[b]
                 : (dbl && (GetClassLongPtrW(w, GCL_STYLE) & CS_DBLCLKS)) ? cdbl[b] : cdown[b];
        post_mouse(w, msg, mouse_keys(), pt, FALSE);
        return;
    }
    /* The title bar, borders and caption buttons track a real mouse in
       their own loops, so moving, sizing and the buttons are done here. */
    root = GetAncestor(w, GA_ROOT);
    if (b == 0 && (hit == HTCAPTION || (hit >= HTLEFT && hit <= HTBOTTOMRIGHT))) {
        if (down && dbl && hit == HTCAPTION)
            PostMessageW(root, WM_SYSCOMMAND, IsZoomed(root) ? SC_RESTORE : SC_MAXIMIZE, 0);
        else if (down && !IsZoomed(root)) {
            g_drag.hwnd = root;
            g_drag.hit = hit;
            g_drag.start = pt;
            GetWindowRect(root, &g_drag.rect);
        }
        return;
    }
    if (b == 0 && (hit == HTCLOSE || hit == HTMINBUTTON || hit == HTMAXBUTTON)) {
        if (!down)
            PostMessageW(root, WM_SYSCOMMAND, hit == HTCLOSE ? SC_CLOSE : hit == HTMINBUTTON ? SC_MINIMIZE
                                              : IsZoomed(root) ? SC_RESTORE : SC_MAXIMIZE, 0);
        return;
    }
    PostMessageW(w, !down ? nup[b] : dbl ? ndbl[b] : ndown[b], (WPARAM)hit, MAKELPARAM(pt.x, pt.y));
}

static void mouse_wheel(int delta)
{
    HWND w = WindowFromPoint(g_mouse), cap;
    BOOL menu;
    if ((cap = mouse_capture(w, &menu)) != NULL) w = cap;
    if (!w) return;
    apply_key_state(w);
    PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(mouse_keys(), (short)delta), MAKELPARAM(g_mouse.x, g_mouse.y));
}

static HWND key_window(void)
{
    GUITHREADINFO gti;
    HWND top = (g_active && IsWindow(g_active)) ? g_active : GetForegroundWindow();
    if (!top) return NULL;
    ZeroMemory(&gti, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(top, NULL), &gti)) {
        if (gti.hwndFocus) return gti.hwndFocus;
        if (gti.hwndActive) return gti.hwndActive;
    }
    return top;
}

/* Console windows ignore window messages for keys, so typing goes in as
   console input: the shell attaches to the console (one at a time) and
   writes key records, the way conhost would from a real keyboard. */
static DWORD  g_console_pid;
static HANDLE g_conin;

static void leave_console(void)
{
    if (g_conin) CloseHandle(g_conin);
    g_conin = NULL;
    if (g_console_pid) FreeConsole();
    g_console_pid = 0;
}

static BOOL console_key(HWND w, int vk, int scan, BOOL ext, BOOL up)
{
    wchar_t cls[32], ch[4];
    DWORD pid = 0, st = 0, n;
    INPUT_RECORD r;
    HWND root = GetAncestor(w, GA_ROOT);
    if (!root || !GetClassNameW(root, cls, _countof(cls)) || wcscmp(cls, L"ConsoleWindowClass") != 0)
        return FALSE;
    GetWindowThreadProcessId(root, &pid);   /* a console window reports a program using it */
    if (pid != g_console_pid) {
        static DWORD failed_pid;
        leave_console();
        FreeConsole();   /* AttachConsole refuses while attached to any console */
        if (!pid || !AttachConsole(pid)) {
            if (pid != failed_pid)
                bs_log("Input: could not attach to the console of process %lu (%lu).", pid, GetLastError());
            failed_pid = pid;
            return TRUE;
        }
        SetConsoleCtrlHandler(NULL, TRUE);   /* a Ctrl+C sent below must not end Backstage */
        g_console_pid = pid;
        g_conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
        if (g_conin == INVALID_HANDLE_VALUE) {
            g_conin = NULL;
            leave_console();
            return TRUE;
        }
    }
    /* Ctrl+C and Ctrl+Break are signals, not keys. */
    if (!up && (g_keys[VK_CONTROL] & 0x80) && (vk == 'C' || vk == VK_CANCEL)) {
        GenerateConsoleCtrlEvent(vk == 'C' ? CTRL_C_EVENT : CTRL_BREAK_EVENT, 0);
        return TRUE;
    }
    if (g_keys[VK_SHIFT] & 0x80)    st |= SHIFT_PRESSED;
    if (g_keys[VK_LCONTROL] & 0x80) st |= LEFT_CTRL_PRESSED;
    if (g_keys[VK_RCONTROL] & 0x80) st |= RIGHT_CTRL_PRESSED;
    if (g_keys[VK_LMENU] & 0x80)    st |= LEFT_ALT_PRESSED;
    if (g_keys[VK_RMENU] & 0x80)    st |= RIGHT_ALT_PRESSED;
    if (g_keys[VK_CAPITAL] & 1)     st |= CAPSLOCK_ON;
    if (g_keys[VK_NUMLOCK] & 1)     st |= NUMLOCK_ON;
    if (ext)                        st |= ENHANCED_KEY;
    ZeroMemory(&r, sizeof(r));
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = !up;
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.wVirtualKeyCode = (WORD)vk;
    r.Event.KeyEvent.wVirtualScanCode = (WORD)(scan & 0xFF);
    r.Event.KeyEvent.dwControlKeyState = st;
    /* 4: leave dead-key state alone */
    if (ToUnicodeEx((UINT)vk, (UINT)scan, g_keys, ch, _countof(ch), 4, GetKeyboardLayout(0)) == 1)
        r.Event.KeyEvent.uChar.UnicodeChar = ch[0];
    WriteConsoleInputW(g_conin, &r, 1, &n);
    return TRUE;
}

static void key_event(int vk, int scan, int flags)
{
    BOOL up = (flags & 1) != 0, ext = (flags & 2) != 0, was, alt, sys;
    BYTE held = up ? 0 : 0x80;
    HWND w;
    LPARAM lp;
    if (vk <= 0 || vk > 0xFE) return;
    was = (g_keys[vk] & 0x80) != 0;
    if (!up && !was) g_keys[vk] ^= 1;   /* the toggle bit, as Caps Lock keeps it */
    g_keys[vk] = (BYTE)((g_keys[vk] & 1) | held);
    if (vk == VK_SHIFT) g_keys[(scan & 0xFF) == 0x36 ? VK_RSHIFT : VK_LSHIFT] = held;
    else if (vk == VK_CONTROL) g_keys[ext ? VK_RCONTROL : VK_LCONTROL] = held;
    else if (vk == VK_MENU) g_keys[ext ? VK_RMENU : VK_LMENU] = held;
    if ((w = key_window()) == NULL) return;
    if (console_key(w, vk, scan, ext, up)) return;
    alt = (g_keys[VK_MENU] & 0x80) != 0 || vk == VK_MENU;
    sys = vk == VK_F10 || (alt && !(g_keys[VK_CONTROL] & 0x80));
    lp = 1 | ((LPARAM)(scan & 0xFF) << 16) | (ext ? (LPARAM)1 << 24 : 0) |
         (sys && alt ? (LPARAM)1 << 29 : 0) | (up || was ? (LPARAM)1 << 30 : 0) | (up ? (LPARAM)1 << 31 : 0);
    apply_key_state(w);
    PostMessageW(w, up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN), (WPARAM)vk, lp);
}

static void inject(const BsInput *in)
{
    switch (in->type) {
    case BS_IN_MOVE:   mouse_move(in->a, in->b); break;
    case BS_IN_BUTTON: if (in->a >= 0 && in->a <= 2) mouse_button(in->a, in->b != 0); break;
    case BS_IN_WHEEL:  mouse_wheel(in->a); break;
    case BS_IN_KEY:    key_event(in->a, in->b, in->c); break;
    }
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

/* 0 not checked yet, 1 the screen can be copied, 2 windows are painted one by one */
static int g_capture_mode;

/* True when nothing but black came back; our backdrop is never black. */
static BOOL frame_is_black(const BYTE *px, int w, int h)
{
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i += 97)
        if (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) return FALSE;
    return TRUE;
}

typedef struct { HWND list[96]; int count; } VisibleWindows;

static BOOL CALLBACK collect_visible(HWND hwnd, LPARAM lp)
{
    VisibleWindows *vw = (VisibleWindows *)lp;
    if (vw->count < (int)(sizeof(vw->list) / sizeof(vw->list[0])) &&
        IsWindowVisible(hwnd) && !IsIconic(hwnd))
        vw->list[vw->count++] = hwnd;
    return TRUE;
}

/* Session 0's screen is never drawn anywhere, so copying it gives black.
   Instead each window paints itself (PrintWindow) and they are stacked
   bottom to top, as they would appear. */
static void compose_windows(HDC mem, int w, int h)
{
    VisibleWindows vw;
    RECT all = { 0, 0, w, h };
    int i;
    FillRect(mem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    vw.count = 0;
    EnumDesktopWindows(g_desk, collect_visible, (LPARAM)&vw);   /* top first */
    for (i = vw.count - 1; i >= 0; i--) {
        RECT r;
        int ww, wh;
        BITMAPINFO bi;
        BYTE *bits = NULL;
        HBITMAP bmp;
        HDC dc;
        if (!GetWindowRect(vw.list[i], &r)) continue;
        ww = r.right - r.left;
        wh = r.bottom - r.top;
        if (ww <= 0 || wh <= 0 || ww > 8192 || wh > 8192) continue;
        if (r.right <= 0 || r.bottom <= 0 || r.left >= w || r.top >= h) continue;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = ww;
        bi.bmiHeader.biHeight = -wh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        if (!bmp) continue;
        dc = CreateCompatibleDC(mem);
        SelectObject(dc, bmp);
        if (!PrintWindow(vw.list[i], dc, PW_RENDERFULLCONTENT))
            PrintWindow(vw.list[i], dc, 0);
        BitBlt(mem, r.left, r.top, ww, wh, dc, 0, 0, SRCCOPY);
        DeleteDC(dc);
        DeleteObject(bmp);
    }
}

typedef struct {
    AsbConn *conn;
    volatile LONG closed;
} BsViewer;

static DWORD WINAPI viewer_input_thread(LPVOID param)
{
    BsViewer *v = (BsViewer *)param;
    BsInput in;
    MSG m;
    SetThreadDesktop(g_desk);
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);   /* a message queue, to share input state */
    bs_log("Input: delivered as window messages (session %lu has no mouse or keyboard).", g_session);
    while (recv_all(v->conn, &in, sizeof(in))) {
        if (in.magic == BS_INPUT_MAGIC) inject(&in);
    }
    share_input_with(NULL);
    leave_console();
    ZeroMemory(g_keys, sizeof(g_keys));
    g_drag.hwnd = NULL;
    InterlockedExchange(&v->closed, 1);
    return 0;
}

/* Send the screen until the viewer goes away: everything first, then only
   the bands of rows that changed. */
static void serve_viewer(AsbConn *conn)
{
    BsViewer v;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = NULL;
    BYTE *cur = NULL, *prev = NULL;
    int w = 0, h = 0, y;
    HANDLE input;

    v.conn = conn;
    v.closed = 0;
    input = CreateThread(NULL, 0, viewer_input_thread, &v, 0, NULL);
    while (!v.closed && !g_exiting) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        BOOL full = FALSE;
        if (sw <= 0 || sh <= 0) { Sleep(BS_FRAME_MS); continue; }
        if (!bmp || sw != w || sh != h) {
            BITMAPINFO bi;
            if (bmp) DeleteObject(bmp);
            free(prev);
            ZeroMemory(&bi, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = sw;
            bi.bmiHeader.biHeight = -sh;           /* top-down */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&cur, NULL, 0);
            prev = (BYTE *)malloc((size_t)sw * sh * 4);
            if (!bmp || !prev) break;
            SelectObject(mem, bmp);
            w = sw;
            h = sh;
            full = TRUE;
        }
        if (g_capture_mode != 2) {
            BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY | CAPTUREBLT);
            GdiFlush();
            if (g_capture_mode == 0) {
                g_capture_mode = frame_is_black(cur, w, h) ? 2 : 1;
                bs_log(g_capture_mode == 2
                       ? "Capture: the screen copies as black; painting each window instead."
                       : "Capture: copying the screen.");
            }
        }
        if (g_capture_mode == 2) {
            compose_windows(mem, w, h);
            GdiFlush();
        }
        for (y = 0; y < h; y += BS_BAND) {
            int rows = (h - y < BS_BAND) ? h - y : BS_BAND;
            size_t off = (size_t)y * w * 4, bytes = (size_t)rows * w * 4;
            BsFrameHeader hdr;
            if (!full && memcmp(cur + off, prev + off, bytes) == 0) continue;
            hdr.magic = BS_FRAME_MAGIC;
            hdr.width = (UINT32)w;
            hdr.height = (UINT32)h;
            hdr.y = (UINT32)y;
            hdr.rows = (UINT32)rows;
            if (!send_all(conn, &hdr, sizeof(hdr)) || !send_all(conn, cur + off, (int)bytes)) {
                InterlockedExchange(&v.closed, 1);
                break;
            }
            memcpy(prev + off, cur + off, bytes);
        }
        Sleep(BS_FRAME_MS);
    }
    if (g_exiting && !v.closed) {   /* tell the host window to close rather than start a new session */
        BsFrameHeader end;
        ZeroMemory(&end, sizeof(end));
        end.magic = BS_END_MAGIC;
        send_all(conn, &end, sizeof(end));
    }
    /* The input thread ends when the host closes its side. */
    if (input) {
        WaitForSingleObject(input, 5000);
        CloseHandle(input);
    }
    asb_close(conn);
    if (bmp) DeleteObject(bmp);
    free(prev);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static DWORD WINAPI stream_thread(LPVOID unused)
{
    AsbListener *l;
    (void)unused;
    if (asb_transport_init() != 0 || (l = asb_listen(ASB_CH_BACKSTAGE)) == NULL) {
        bs_log("Could not listen for the host's Backstage window.");
        return 0;
    }
    while (!g_exiting) {
        AsbConn *c = asb_accept(l, 1000);
        if (!c) continue;
        asb_set_timeout(c, 0, 10000);
        bs_log("Host Backstage window connected.");
        serve_viewer(c);
        bs_log("Host Backstage window disconnected.");
    }
    asb_close_listener(l);
    return 0;
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
                   L"Hidden from the VM's own screen: the signed-in user and the sign-in "
                   L"screen never see Backstage. Programs opened here run here.",
                   computer, session);
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

/* Consoles started here must use the classic console window: the terminal
   Windows would hand them to can't run in session 0, and only the classic
   window takes posted keys. Applies to SYSTEM's consoles in this VM. */
static void use_console_host(void)
{
    static const wchar_t conhost[] = L"{B23D10C0-E52E-411E-9D5B-C09FDF709C7D}";
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Console\\%%Startup", L"DelegationConsole",
                    REG_SZ, conhost, sizeof(conhost));
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Console\\%%Startup", L"DelegationTerminal",
                    REG_SZ, conhost, sizeof(conhost));
}

/* Session 0 has no input desktop until one is chosen; without it the
   pointer can't be placed and windows can't take the foreground. Nobody
   sees session 0, so choosing ours affects no one. */
static void become_input_desktop(void)
{
    HDESK desk;
    BOOL ok;
    if (g_session != 0) return;
    desk = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_SWITCHDESKTOP);
    ok = desk && SwitchDesktop(desk);
    bs_log(ok ? "This is now session 0's input desktop."
              : "Could not make this session 0's input desktop (%lu).", GetLastError());
    if (desk) CloseDesktop(desk);
}

static int run_shell(void)
{
    HANDLE running, thread, stream;
    MSG msg;

    running = OpenMutexW(SYNCHRONIZE, FALSE, BACKSTAGE_RUNNING_MUTEX);
    if (running) {                       /* one shell at a time */
        CloseHandle(running);
        return 0;
    }
    running = CreateMutexW(NULL, FALSE, BACKSTAGE_RUNNING_MUTEX);
    ProcessIdToSessionId(GetCurrentProcessId(), &g_session);
    g_desk = GetThreadDesktop(GetCurrentThreadId());
    bs_log("Backstage started in session %lu.", g_session);
    choose_screen_mode();
    become_input_desktop();

    OleInitialize(NULL);
    ensure_profile_folders();
    use_console_host();
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
        thread = CreateThread(NULL, 0, exit_watch_thread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    stream = CreateThread(NULL, 0, stream_thread, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (files_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (stream) {   /* lets it tell the host window the session ended */
        WaitForSingleObject(stream, 8000);
        CloseHandle(stream);
    }
    OleUninitialize();
    if (running) CloseHandle(running);
    bs_log("Backstage closed.");
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst; (void)prev; (void)cmd; (void)show;
    return run_shell();
}

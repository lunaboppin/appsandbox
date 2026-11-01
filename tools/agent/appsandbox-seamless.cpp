/*
 * appsandbox-seamless.exe -- seamless mode's guest side.
 *
 * The agent starts it in the signed-in user's session, with the user's full
 * token where there is one so input reaches elevated windows too (see
 * spawn_seamless in agent.c). It serves one host connection on ASB_CH_SEAMLESS
 * and exits when that connection ends.
 *
 *  - The main thread watches the session's top-level windows (WinEvent hooks
 *    mark the list dirty, a timer rescans) and reports new, changed and closed
 *    windows, the foreground, the pointer's shape and whether something secure
 *    (the sign-in screen, a lock, UAC) has the input.
 *  - The capture thread gets each window's own picture, whatever is on top of
 *    it, from Windows.Graphics.Capture, or PrintWindow where that can't (and
 *    for short-lived popups, whose first frame must be immediate). It sends
 *    only the rectangles of rows that changed, and it polls, so a slow host
 *    makes it skip frames rather than queue them.
 *  - The receive thread injects the host's mouse and keyboard with SendInput
 *    and carries out window commands.
 *
 * Guest windows stay where they are on the guest's screen when the host window
 * is moved; the two are tied together by offset on the host, so a host window
 * can go anywhere, including another monitor.
 *
 * Logs to C:\Windows\AppSandbox\seamless.log (beside agent.log).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../transport/asb_transport.h"
#include "seamless_proto.h"

#include <winsock2.h>
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <vector>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

#define POLL_MS        16
#define GDI_POLL_MS    66     /* windows PrintWindow paints: costly, so less often */
#define BAND_ROWS      32
#define SCAN_MS        33
#define FULL_SCAN_MS   1000   /* catches anything no event told us about */

struct Win {
    HWND     hwnd = NULL;
    /* main thread, under g_cs */
    bool     gone = false;
    bool     noactivate = false;
    bool     have_rect = false;
    SlWindow sent = {};
    std::wstring title;
    HICON    icon = NULL;
    bool     self_pending = false;
    int      self_x = 0, self_y = 0;
    /* capture thread only */
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    bool     wgc_tried = false;
    ID3D11Texture2D *staging = NULL;
    int      tex_w = 0, tex_h = 0;
    std::vector<BYTE> prev;
    int      prev_w = 0, prev_h = 0;
    DWORD    gdi_tick = 0;
    std::vector<BYTE> gdi;
};

CRITICAL_SECTION g_cs;        /* g_windows */
CRITICAL_SECTION g_send_cs;   /* one whole message at a time on the connection */
std::vector<Win *> g_windows;
AsbConn *g_conn;
volatile bool g_dirty = true;
volatile bool g_secure;
bool g_wgc_supported;
DWORD g_main_thread;

ID3D11Device *g_device;
ID3D11DeviceContext *g_context;
IDirect3DDevice g_rt_device{ nullptr };

/* ---- Logging ---- */

void sl_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;
    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\seamless.log", "a") != 0 || !f)
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

/* ---- Connection ---- */

bool send_bytes(const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int chunk = len > (1u << 30) ? (1 << 30) : (int)len;
        int n = asb_send(g_conn, p, chunk);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool recv_bytes(void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = asb_recv(g_conn, p, (int)len);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* The host going away ends the helper, whichever thread notices. */
void send_msg(UINT32 type, HWND hwnd, const void *a, size_t alen, const void *b = NULL, size_t blen = 0)
{
    SlHeader h;
    bool ok;
    h.magic = SL_MAGIC;
    h.type = type;
    h.window = (UINT32)(UINT_PTR)hwnd;
    h.len = (UINT32)(alen + blen);
    EnterCriticalSection(&g_send_cs);
    ok = send_bytes(&h, sizeof(h)) && (!alen || send_bytes(a, alen)) && (!blen || send_bytes(b, blen));
    LeaveCriticalSection(&g_send_cs);
    if (!ok) {
        sl_log("The host connection failed while sending; exiting.");
        ExitProcess(0);
    }
}

/* ---- Which windows are shown ---- */

bool process_is_shell(HWND hwnd)
{
    static const wchar_t *names[] = {
        L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"SearchApp.exe",
        L"TextInputHost.exe", L"LockApp.exe", L"ShellHost.exe", L"Widgets.exe", L"LogonUI.exe"
    };
    DWORD pid = 0, cap = MAX_PATH;
    wchar_t path[MAX_PATH] = L"", *base;
    HANDLE p;
    GetWindowThreadProcessId(hwnd, &pid);
    p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    QueryFullProcessImageNameW(p, 0, path, &cap);
    CloseHandle(p);
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    for (const wchar_t *n : names)
        if (_wcsicmp(base, n) == 0) return true;
    return false;
}

bool is_lock_screen(HWND hwnd)
{
    DWORD pid = 0, cap = MAX_PATH;
    wchar_t path[MAX_PATH] = L"", *base;
    HANDLE p;
    if (!hwnd) return false;
    GetWindowThreadProcessId(hwnd, &pid);
    p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    QueryFullProcessImageNameW(p, 0, path, &cap);
    CloseHandle(p);
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"LockApp.exe") == 0;
}

bool is_shell_class(const wchar_t *cls)
{
    static const wchar_t *names[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
        L"TaskListThumbnailWnd", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
        L"TopLevelWindowForOverflowXamlIsland", L"ForegroundStaging", L"Shell_InputSwitchTopLevelWindow",
        L"Windows.UI.Core.CoreWindow", L"Windows.UI.Input.InputSite.WindowClass", L"EdgeUiInputTopWndClass",
        L"EdgeUiInputWndClass", L"ApplicationManager_DesktopShellWindow", L"IME", L"MSCTFIME UI",
        L"Internet Explorer_Hidden", L"PseudoConsoleWindow", L"DummyDWMListenerWindow", L"Shell_SecondaryTrayWnd"
    };
    for (const wchar_t *n : names)
        if (_wcsicmp(cls, n) == 0) return true;
    return false;
}

bool is_menu_class(const wchar_t *cls)
{
    return wcscmp(cls, L"#32768") == 0 || _wcsicmp(cls, L"tooltips_class32") == 0 ||
           _wcsicmp(cls, L"ComboLBox") == 0 || _wcsicmp(cls, L"Xaml_WindowedPopupClass") == 0 ||
           _wcsicmp(cls, L"SysShadow") == 0 || _wcsicmp(cls, L"Chrome_WidgetWin_2") == 0;
}

void frame_rect(HWND hwnd, RECT *r)
{
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r))))
        GetWindowRect(hwnd, r);
}

bool should_show(HWND hwnd, const wchar_t *cls)
{
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    BOOL cloaked = FALSE;
    RECT r;
    if (!IsWindowVisible(hwnd)) return false;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    if (is_shell_class(cls) || wcscmp(cls, L"SysShadow") == 0) return false;
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return false;   /* click-through overlays */
    if (!IsIconic(hwnd)) {
        frame_rect(hwnd, &r);
        if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return false;
        /* Parked out of sight: apps start windows at -32000 and bring them in later. */
        if (r.right <= GetSystemMetrics(SM_XVIRTUALSCREEN) || r.bottom <= GetSystemMetrics(SM_YVIRTUALSCREEN))
            return false;
    }
    return true;
}

Win *find_win(HWND hwnd)
{
    for (Win *w : g_windows)
        if (w->hwnd == hwnd && !w->gone) return w;
    return NULL;
}

/* ---- Images (icons and pointers) ---- */

/* Paint an icon or pointer into BGRA. Masked (old-style) ones have no alpha of
   their own, so it comes from comparing them on black and on white. */
bool icon_to_bgra(HICON icon, int w, int h, std::vector<BYTE> &out)
{
    BITMAPINFO bi = {};
    BYTE *on_black = NULL, *on_white = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP black, white;
    bool ok = false;
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    black = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&on_black, NULL, 0);
    white = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&on_white, NULL, 0);
    if (black && white) {
        size_t i, px = (size_t)w * h;
        memset(on_black, 0, px * 4);
        memset(on_white, 0xFF, px * 4);
        SelectObject(dc, black);
        DrawIconEx(dc, 0, 0, icon, w, h, 0, NULL, DI_NORMAL);
        SelectObject(dc, white);
        DrawIconEx(dc, 0, 0, icon, w, h, 0, NULL, DI_NORMAL);
        GdiFlush();
        out.resize(px * 4);
        for (i = 0; i < px; i++) {
            const BYTE *b = on_black + i * 4, *c = on_white + i * 4;
            int alpha = 255 - (c[1] - b[1]);
            BYTE *d = &out[i * 4];
            if (alpha < 0) alpha = 0;
            if (alpha > 255) alpha = 255;
            /* premultiplied on black: undo it */
            d[0] = alpha ? (BYTE)min(255, b[0] * 255 / alpha) : 0;
            d[1] = alpha ? (BYTE)min(255, b[1] * 255 / alpha) : 0;
            d[2] = alpha ? (BYTE)min(255, b[2] * 255 / alpha) : 0;
            d[3] = (BYTE)alpha;
        }
        ok = true;
    }
    if (black) DeleteObject(black);
    if (white) DeleteObject(white);
    DeleteDC(dc);
    return ok;
}

HICON window_icon(HWND hwnd)
{
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &r) && r) return (HICON)r;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &r) && r) return (HICON)r;
    r = GetClassLongPtrW(hwnd, GCLP_HICON);
    if (r) return (HICON)r;
    return (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
}

void send_icon(Win *w)
{
    HICON icon = window_icon(w->hwnd);
    std::vector<BYTE> px;
    SlImage img = {};
    if (icon == w->icon) return;
    w->icon = icon;
    if (!icon) return;
    img.width = img.height = 32;
    if (icon_to_bgra(icon, 32, 32, px))
        send_msg(SL_G_ICON, w->hwnd, &img, sizeof(img), px.data(), px.size());
}

void send_cursor(void)
{
    static HCURSOR last;
    static bool last_shown = true, first = true;
    static const LPCWSTR standard[] = {
        IDC_ARROW, IDC_IBEAM, IDC_WAIT, IDC_CROSS, IDC_UPARROW, IDC_SIZENWSE, IDC_SIZENESW,
        IDC_SIZEWE, IDC_SIZENS, IDC_SIZEALL, IDC_NO, IDC_HAND, IDC_APPSTARTING, IDC_HELP
    };
    CURSORINFO ci = {};
    SlImage img = {};
    bool shown;
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    shown = (ci.flags & CURSOR_SHOWING) != 0;
    if (!first && shown == last_shown && ci.hCursor == last) return;
    first = false;
    last_shown = shown;
    last = ci.hCursor;
    if (!shown || !ci.hCursor) {
        send_msg(SL_G_CURSOR, NULL, &img, sizeof(img));
        return;
    }
    for (LPCWSTR id : standard) {
        if (LoadCursorW(NULL, id) == ci.hCursor) {
            img.hot_x = (INT32)(UINT_PTR)id;
            send_msg(SL_G_CURSOR, NULL, &img, sizeof(img));
            return;
        }
    }
    {
        ICONINFO info;
        BITMAP bm = {};
        std::vector<BYTE> px;
        if (!GetIconInfo(ci.hCursor, &info)) return;
        GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bm), &bm);
        img.width = (UINT32)bm.bmWidth;
        img.height = (UINT32)(info.hbmColor ? bm.bmHeight : bm.bmHeight / 2);
        img.hot_x = (INT32)info.xHotspot;
        img.hot_y = (INT32)info.yHotspot;
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        if (img.width && img.height && img.width <= SL_MAX_CURSOR_SIDE && img.height <= SL_MAX_CURSOR_SIDE &&
            icon_to_bgra(ci.hCursor, (int)img.width, (int)img.height, px))
            send_msg(SL_G_CURSOR, NULL, &img, sizeof(img), px.data(), px.size());
    }
}

/* ---- Watching the windows ---- */

struct ScanItem { HWND hwnd; };

BOOL CALLBACK collect_top_level(HWND hwnd, LPARAM lp)
{
    ((std::vector<HWND> *)lp)->push_back(hwnd);
    return TRUE;
}

void describe(HWND hwnd, const wchar_t *cls, SlWindow *sw, std::wstring &title)
{
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE), style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    HWND owner = GetWindow(hwnd, GW_OWNER);
    wchar_t text[SL_MAX_TITLE];
    bool has_caption = (style & WS_CAPTION) == WS_CAPTION;
    RECT r;
    int len = GetWindowTextW(hwnd, text, SL_MAX_TITLE);
    title.assign(text, len > 0 ? (size_t)len : 0);
    sw->owner = (UINT32)(UINT_PTR)owner;
    sw->flags = 0;
    if ((ex & WS_EX_APPWINDOW) || (!owner && !(ex & WS_EX_TOOLWINDOW) && !is_menu_class(cls) &&
                                   (has_caption || !title.empty())))
        sw->flags |= SL_WF_APP;
    if (is_menu_class(cls) || (ex & WS_EX_NOACTIVATE) || (!(sw->flags & SL_WF_APP) && !has_caption))
        sw->flags |= SL_WF_NOACTIVATE;
    sw->state = IsIconic(hwnd) ? SL_STATE_MINIMIZED : IsZoomed(hwnd) ? SL_STATE_MAXIMIZED : SL_STATE_NORMAL;
    if (sw->state == SL_STATE_MINIMIZED) {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        GetWindowPlacement(hwnd, &wp);
        r = wp.rcNormalPosition;
    } else {
        frame_rect(hwnd, &r);
    }
    sw->x = r.left;
    sw->y = r.top;
    sw->w = r.right - r.left;
    sw->h = r.bottom - r.top;
}

void send_window(Win *w)
{
    send_msg(SL_G_WINDOW, w->hwnd, &w->sent, sizeof(w->sent), w->title.data(), w->title.size() * sizeof(wchar_t));
}

/* Walk the top-level windows, top first, and report what changed. */
void scan_windows(void)
{
    std::vector<HWND> list;
    list.reserve(128);
    EnumWindows(collect_top_level, (LPARAM)&list);

    EnterCriticalSection(&g_cs);
    for (Win *w : g_windows)
        if (!w->gone && !IsWindow(w->hwnd)) {
            w->gone = true;
            send_msg(SL_G_CLOSED, w->hwnd, NULL, 0);
        }
    for (HWND hwnd : list) {
        wchar_t cls[80] = L"";
        Win *w = find_win(hwnd);
        GetClassNameW(hwnd, cls, _countof(cls));
        if (!should_show(hwnd, cls)) {
            if (w) {
                w->gone = true;
                send_msg(SL_G_CLOSED, hwnd, NULL, 0);
            }
            continue;
        }
        if (!w) {
            if (process_is_shell(hwnd) || (int)g_windows.size() >= SL_MAX_WINDOWS) continue;
            w = new Win();
            w->hwnd = hwnd;
            g_windows.push_back(w);
            sl_log("New window %p: class %ls style %08llx ex %08llx owner %p.", hwnd, cls,
                   (unsigned long long)GetWindowLongPtrW(hwnd, GWL_STYLE),
                   (unsigned long long)GetWindowLongPtrW(hwnd, GWL_EXSTYLE), GetWindow(hwnd, GW_OWNER));
        }
        {
            SlWindow now = {};
            std::wstring title;
            describe(hwnd, cls, &now, title);
            w->noactivate = (now.flags & SL_WF_NOACTIVATE) != 0;
            if (w->self_pending && now.x == w->self_x && now.y == w->self_y) {
                now.flags |= SL_WF_SELF_MOVED;
                w->self_pending = false;
            }
            if (!w->have_rect || title != w->title ||
                memcmp(&now, &w->sent, sizeof(now)) != 0) {
                w->sent = now;
                w->title = title;
                w->have_rect = true;
                send_window(w);
                w->sent.flags &= ~SL_WF_SELF_MOVED;   /* one report; later ones compare without it */
            }
            if (now.flags & SL_WF_APP) send_icon(w);
        }
    }
    LeaveCriticalSection(&g_cs);
}

void CALLBACK on_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG obj, LONG child, DWORD, DWORD)
{
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !hwnd) return;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        bool known;
        EnterCriticalSection(&g_cs);
        known = find_win(hwnd) != NULL;
        LeaveCriticalSection(&g_cs);
        g_dirty = true;
        if (known) send_msg(SL_G_FOREGROUND, hwnd, NULL, 0);
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        /* A title-bar drag is better done by the host: it can leave the guest's
           screen, and the window follows the pointer without a round trip. */
        POINT pt;
        DWORD_PTR hit = 0;
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !IsZoomed(hwnd) && GetCursorPos(&pt) &&
            SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG, 50, &hit) &&
            hit == HTCAPTION)
            send_msg(SL_G_MOVE_START, hwnd, NULL, 0);
        return;
    }
    if (GetAncestor(hwnd, GA_ROOT) == hwnd) g_dirty = true;
}

/* ---- Secure desktop ---- */

void check_secure(void)
{
    static int disagree;
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    bool secure = true;
    if (desk) {
        wchar_t name[64] = L"";
        GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), NULL);
        secure = _wcsicmp(name, L"Default") != 0;
        CloseDesktop(desk);
    }
    if (!secure && is_lock_screen(GetForegroundWindow())) secure = true;
    if (secure == g_secure) {
        disagree = 0;
        return;
    }
    if (++disagree < 2) return;   /* one failed open is not a change of desktop */
    disagree = 0;
    g_secure = secure;
    {
        SlValue v = {};
        v.a = secure ? 1 : 0;
        sl_log("Input is now on %s.", secure ? "a secure desktop" : "the user's desktop");
        send_msg(SL_G_SECURE, NULL, &v, sizeof(v));
    }
}

/* ---- Capture ---- */

/* Send the rectangles that differ from what the host has. Bands of rows are
   compared, then narrowed to the columns that changed: typing in a text box is
   a few kilobytes, not a screen. */
void send_changes(Win *w, const BYTE *px, int pitch, int width, int height)
{
    size_t row_bytes = (size_t)width * 4;
    bool full = width != w->prev_w || height != w->prev_h;
    int y;
    SlFrame f = {};
    if (full) {
        w->prev.resize(row_bytes * height);
        w->prev_w = width;
        w->prev_h = height;
    }
    f.width = (UINT32)width;
    f.height = (UINT32)height;
    for (y = 0; y < height; y += BAND_ROWS) {
        int rows = height - y < BAND_ROWS ? height - y : BAND_ROWS, r;
        int first = width, last = -1;
        std::vector<BYTE> out;
        for (r = 0; r < rows; r++) {
            const UINT32 *cur = (const UINT32 *)(px + (size_t)(y + r) * pitch);
            const UINT32 *old = (const UINT32 *)(w->prev.data() + (size_t)(y + r) * row_bytes);
            int x;
            if (full) {
                first = 0;
                last = width - 1;
                break;
            }
            if (memcmp(cur, old, row_bytes) == 0) continue;
            for (x = 0; x < first; x++)
                if (cur[x] != old[x]) { first = x; break; }
            for (x = width - 1; x > last; x--)
                if (cur[x] != old[x]) { last = x; break; }
        }
        if (last < first) continue;
        f.x = (UINT32)first;
        f.y = (UINT32)y;
        f.w = (UINT32)(last - first + 1);
        f.h = (UINT32)rows;
        out.resize((size_t)f.w * 4 * rows);
        for (r = 0; r < rows; r++) {
            const BYTE *cur = px + (size_t)(y + r) * pitch;
            memcpy(&out[(size_t)r * f.w * 4], cur + (size_t)first * 4, (size_t)f.w * 4);
            memcpy(w->prev.data() + (size_t)(y + r) * row_bytes, cur, row_bytes);
        }
        send_msg(SL_G_FRAME, w->hwnd, &f, sizeof(f), out.data(), out.size());
    }
}

void wgc_stop(Win *w)
{
    try {
        if (w->session) w->session.Close();
        if (w->pool) w->pool.Close();
    } catch (...) {}
    w->session = nullptr;
    w->pool = nullptr;
    w->item = nullptr;
}

void win_free(Win *w)
{
    wgc_stop(w);
    if (w->staging) w->staging->Release();
    delete w;
}

void wgc_start(Win *w)
{
    w->wgc_tried = true;
    if (!g_wgc_supported) return;
    try {
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{ nullptr };
        check_hresult(interop->CreateForWindow(w->hwnd, guid_of<GraphicsCaptureItem>(),
                                               put_abi(item)));
        w->item = item;
        w->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        w->session = w->pool.CreateCaptureSession(item);
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
            w->session.IsCursorCaptureEnabled(false);
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
            w->session.IsBorderRequired(false);
        w->session.StartCapture();
    } catch (const hresult_error &e) {
        sl_log("Window %p can't use Windows.Graphics.Capture (0x%08X); painting it instead.",
               w->hwnd, (unsigned)e.code().value);
        wgc_stop(w);
    }
}

/* The newest frame of this window, if there's one we haven't sent. */
void wgc_poll(Win *w)
{
    try {
        Direct3D11CaptureFrame frame{ nullptr };
        for (;;) {
            auto next = w->pool.TryGetNextFrame();
            if (!next) break;
            frame = next;
        }
        if (!frame) return;
        {
            SizeInt32 size = frame.ContentSize();
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            com_ptr<ID3D11Texture2D> tex;
            D3D11_TEXTURE2D_DESC desc;
            D3D11_MAPPED_SUBRESOURCE map;
            D3D11_BOX box = {};
            int width, height;
            check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), tex.put_void()));
            tex->GetDesc(&desc);
            width = min(size.Width, (int)desc.Width);
            height = min(size.Height, (int)desc.Height);
            if (width <= 0 || height <= 0 || width > (int)SL_MAX_SIDE || height > (int)SL_MAX_SIDE) return;
            if (!w->staging || w->tex_w != (int)desc.Width || w->tex_h != (int)desc.Height) {
                D3D11_TEXTURE2D_DESC sd = desc;
                if (w->staging) { w->staging->Release(); w->staging = NULL; }
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                sd.ArraySize = 1;
                sd.MipLevels = 1;
                check_hresult(g_device->CreateTexture2D(&sd, NULL, &w->staging));
                w->tex_w = (int)desc.Width;
                w->tex_h = (int)desc.Height;
            }
            box.right = (UINT)width;
            box.bottom = (UINT)height;
            box.back = 1;
            g_context->CopySubresourceRegion(w->staging, 0, 0, 0, 0, tex.get(), 0, &box);
            if (SUCCEEDED(g_context->Map(w->staging, 0, D3D11_MAP_READ, 0, &map))) {
                send_changes(w, (const BYTE *)map.pData, (int)map.RowPitch, width, height);
                g_context->Unmap(w->staging, 0);
            }
            if (size.Width != (int)desc.Width || size.Height != (int)desc.Height)
                w->pool.Recreate(g_rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
    } catch (const hresult_error &e) {
        sl_log("Capture of window %p failed (0x%08X); painting it instead.", w->hwnd, (unsigned)e.code().value);
        wgc_stop(w);
    }
}

/* PrintWindow: slower, but no capture session to start, so a menu's first
   frame is there at once, and it works on windows capture refuses. */
void gdi_poll(Win *w)
{
    DWORD now = GetTickCount();
    RECT wr, fr;
    int ww, wh, fw, fh, dx, dy;
    BITMAPINFO bi = {};
    BYTE *bits = NULL;
    HBITMAP bmp;
    HDC dc;
    if (now - w->gdi_tick < GDI_POLL_MS || IsIconic(w->hwnd) || IsHungAppWindow(w->hwnd)) return;
    w->gdi_tick = now;
    if (!GetWindowRect(w->hwnd, &wr)) return;
    frame_rect(w->hwnd, &fr);
    ww = wr.right - wr.left;
    wh = wr.bottom - wr.top;
    fw = fr.right - fr.left;
    fh = fr.bottom - fr.top;
    if (ww <= 0 || wh <= 0 || ww > (int)SL_MAX_SIDE || wh > (int)SL_MAX_SIDE || fw <= 0 || fh <= 0) return;
    dx = fr.left - wr.left;
    dy = fr.top - wr.top;
    if (dx < 0 || dy < 0 || dx + fw > ww || dy + fh > wh) { dx = dy = 0; fw = ww; fh = wh; }
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = ww;
    bi.bmiHeader.biHeight = -wh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp) {
        SelectObject(dc, bmp);
        if (PrintWindow(w->hwnd, dc, PW_RENDERFULLCONTENT) || PrintWindow(w->hwnd, dc, 0)) {
            GdiFlush();
            send_changes(w, bits + (size_t)dy * ww * 4 + (size_t)dx * 4, ww * 4, fw, fh);
        }
        DeleteObject(bmp);
    }
    DeleteDC(dc);
}

bool d3d_init(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, _countof(levels), D3D11_SDK_VERSION, &g_device, &got, &g_context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, _countof(levels), D3D11_SDK_VERSION, &g_device, &got, &g_context);
    if (FAILED(hr)) {
        sl_log("No Direct3D device (0x%08X).", (unsigned)hr);
        return false;
    }
    {
        com_ptr<IDXGIDevice> dxgi;
        com_ptr<::IInspectable> rt;
        check_hresult(g_device->QueryInterface(guid_of<IDXGIDevice>(), dxgi.put_void()));
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), rt.put()));
        g_rt_device = rt.as<IDirect3DDevice>();
    }
    return true;
}

DWORD WINAPI capture_thread(LPVOID)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    init_apartment(apartment_type::multi_threaded);
    try {
        g_wgc_supported = d3d_init() && GraphicsCaptureSession::IsSupported();
    } catch (const hresult_error &e) {
        sl_log("Windows.Graphics.Capture is not available (0x%08X).", (unsigned)e.code().value);
        g_wgc_supported = false;
    }
    sl_log(g_wgc_supported ? "Capture: Windows.Graphics.Capture." : "Capture: PrintWindow only.");
    {
        SlHello hello = {};
        hello.version = SL_VERSION;
        hello.screen_w = (UINT32)GetSystemMetrics(SM_CXSCREEN);
        hello.screen_h = (UINT32)GetSystemMetrics(SM_CYSCREEN);
        hello.capture_flags = g_wgc_supported ? SL_CAP_WGC : 0;
        send_msg(SL_G_HELLO, NULL, &hello, sizeof(hello));
    }
    if (g_wgc_supported) {
        /* Without the guest's yellow capture border. Refusal costs nothing: it
           isn't in the frames, only on the guest's own screen. */
        try {
            GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
        } catch (...) {}
    }
    for (;;) {
        std::vector<Win *> now;
        EnterCriticalSection(&g_cs);
        for (size_t i = 0; i < g_windows.size();) {
            if (g_windows[i]->gone) {
                Win *dead = g_windows[i];
                g_windows.erase(g_windows.begin() + (ptrdiff_t)i);
                LeaveCriticalSection(&g_cs);
                win_free(dead);
                EnterCriticalSection(&g_cs);
                continue;
            }
            now.push_back(g_windows[i]);
            i++;
        }
        LeaveCriticalSection(&g_cs);
        if (!g_secure) {
            for (Win *w : now) {
                if (w->gone) continue;   /* only this thread frees it */
                if (!w->wgc_tried && !w->noactivate) wgc_start(w);
                if (w->pool) wgc_poll(w);
                else gdi_poll(w);
            }
        }
        Sleep(POLL_MS);
    }
}

/* ---- Input ---- */

void inject_mouse(DWORD flags, DWORD data, LONG dx = 0, LONG dy = 0)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    in.mi.dx = dx;
    in.mi.dy = dy;
    SendInput(1, &in, sizeof(in));
}

void inject_key(WORD vk, WORD scan, DWORD flags)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

void mouse_to(LONG x, LONG y)
{
    LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    LONG vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;
    if (x < vx) x = vx;
    if (y < vy) y = vy;
    if (x >= vx + vw) x = vx + vw - 1;
    if (y >= vy + vh) y = vy + vh - 1;
    inject_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, 0,
                 (LONG)((LONGLONG)(x - vx) * 65535 / (vw - 1)),
                 (LONG)((LONGLONG)(y - vy) * 65535 / (vh - 1)));
}

void inject_button(int button, bool down)
{
    switch (button) {
    case SL_BTN_LEFT:   inject_mouse(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0); break;
    case SL_BTN_RIGHT:  inject_mouse(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0); break;
    case SL_BTN_MIDDLE: inject_mouse(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP, 0); break;
    case SL_BTN_X1:     inject_mouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON1); break;
    case SL_BTN_X2:     inject_mouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON2); break;
    }
}

/* ---- Window commands ---- */

/* The rectangle a window may be clicked in: its monitor's, so every pixel of
   it the host shows can take input. Keep it fully on the guest's screen. */
void keep_on_screen(HWND hwnd)
{
    RECT fr, wr;
    HMONITOR mon;
    MONITORINFO mi = {};
    int dx = 0, dy = 0;
    if (IsIconic(hwnd) || IsZoomed(hwnd) || !GetWindowRect(hwnd, &wr)) return;
    frame_rect(hwnd, &fr);
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    if (fr.right > mi.rcMonitor.right) dx = mi.rcMonitor.right - fr.right;
    if (fr.left + dx < mi.rcMonitor.left) dx = mi.rcMonitor.left - fr.left;
    if (fr.bottom > mi.rcMonitor.bottom) dy = mi.rcMonitor.bottom - fr.bottom;
    if (fr.top + dy < mi.rcMonitor.top) dy = mi.rcMonitor.top - fr.top;
    if (!dx && !dy) return;
    EnterCriticalSection(&g_cs);
    {
        Win *w = find_win(hwnd);
        if (w) {
            w->self_pending = true;
            w->self_x = fr.left + dx;
            w->self_y = fr.top + dy;
        }
    }
    LeaveCriticalSection(&g_cs);
    SetWindowPos(hwnd, NULL, wr.left + dx, wr.top + dy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void activate_window(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), theirs;
    if (!IsWindow(hwnd)) return;
    keep_on_screen(hwnd);
    if (fg == hwnd) return;
    /* The foreground rule lets the process that had the last input move the
       foreground, and a key nothing uses makes that us. It can still lose a race
       with whatever else is changing the foreground, and the keys that follow
       would go to the wrong window, so try again. */
    for (int attempt = 0; attempt < 4 && GetForegroundWindow() != hwnd; attempt++) {
        if (attempt) Sleep(25);
        inject_key(VK_F24, 0, 0);
        inject_key(VK_F24, 0, KEYEVENTF_KEYUP);
        fg = GetForegroundWindow();
        theirs = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
        if (theirs && theirs != me) AttachThreadInput(me, theirs, TRUE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (theirs && theirs != me) AttachThreadInput(me, theirs, FALSE);
    }
    if (GetForegroundWindow() != hwnd) sl_log("Window %p could not take the foreground.", hwnd);
}

void resize_window(HWND hwnd, int width, int height)
{
    RECT wr, fr;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    int extra_w, extra_h;
    if (!GetWindowRect(hwnd, &wr)) return;
    frame_rect(hwnd, &fr);
    extra_w = (wr.right - wr.left) - (fr.right - fr.left);
    extra_h = (wr.bottom - wr.top) - (fr.bottom - fr.top);
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (width < 16 || height < 16) return;
    SetWindowPos(hwnd, NULL, 0, 0, width + extra_w, height + extra_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (fr.left + width > mi.rcMonitor.right || fr.top + height > mi.rcMonitor.bottom) {
        /* Grown past the screen's edge: move back when the new size has landed. */
        Sleep(30);
        keep_on_screen(hwnd);
    }
}

void run_target(const std::wstring &target)
{
    wchar_t exe[MAX_PATH], found[MAX_PATH];
    DWORD found_size = sizeof(found);
    std::wstring cmd, registered = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + target;
    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    /* Started by the shell, so it gets the user's normal token rather than this
       helper's elevated one. */
    GetWindowsDirectoryW(exe, MAX_PATH);
    wcscat_s(exe, MAX_PATH, L"\\explorer.exe");
    cmd = L"explorer.exe \"";
    if (target.find(L":\\") == std::wstring::npos && target.rfind(L"\\\\", 0) != 0 &&
        target.rfind(L"shell:", 0) != 0) {
        /* A program by name (on the path or registered, as msedge.exe is), or an
           entry from the app list. Explorer answers anything else by opening
           Documents, so look before asking it. */
        if (SearchPathW(NULL, target.c_str(), L".exe", MAX_PATH, found, NULL) ||
            RegGetValueW(HKEY_LOCAL_MACHINE, registered.c_str(), NULL, RRF_RT_REG_SZ, NULL, found, &found_size) == ERROR_SUCCESS) {
            cmd += found;
        } else {
            std::wstring app = L"shell:AppsFolder\\" + target;
            IShellItem *item = NULL;
            HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            HRESULT hr = SHCreateItemFromParsingName(app.c_str(), NULL, IID_PPV_ARGS(&item));
            if (item) item->Release();
            if (SUCCEEDED(com)) CoUninitialize();
            if (FAILED(hr)) {
                sl_log("Nothing called %ls to open (0x%08lx).", target.c_str(), hr);
                return;
            }
            cmd += app;
        }
    } else {
        cmd += target;
    }
    cmd += L"\"";
    si.cb = sizeof(si);
    if (CreateProcessW(exe, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        sl_log("Could not open %ls (%lu).", target.c_str(), GetLastError());
    }
}

/* Everything the Start menu lists, packaged apps included. */
DWORD WINAPI list_apps_thread(LPVOID)
{
    std::wstring out;
    IShellItem *folder = NULL;
    IEnumShellItems *items = NULL;
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&folder))) &&
        SUCCEEDED(folder->BindToHandler(NULL, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
        IShellItem *item = NULL;
        while (items->Next(1, &item, NULL) == S_OK && item) {
            PWSTR name = NULL, parse = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parse)) &&
                out.size() + wcslen(name) + wcslen(parse) + 2 < SL_MAX_APPS_BYTES / sizeof(wchar_t)) {
                out.append(name);
                out.push_back(L'\0');
                out.append(parse);
                out.push_back(L'\0');
            }
            if (name) CoTaskMemFree(name);
            if (parse) CoTaskMemFree(parse);
            item->Release();
            item = NULL;
        }
        items->Release();
    }
    if (folder) folder->Release();
    CoUninitialize();
    send_msg(SL_G_APPS, NULL, out.data(), out.size() * sizeof(wchar_t));
    return 0;
}

/* ---- Receiving from the host ---- */

DWORD WINAPI recv_thread(LPVOID)
{
    SlHeader h;
    std::vector<BYTE> body;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    while (recv_bytes(&h, sizeof(h))) {
        SlValue v = {};
        HWND hwnd = (HWND)(UINT_PTR)h.window;
        if (h.magic != SL_MAGIC || h.len > SL_MAX_APPS_BYTES) break;
        body.resize(h.len);
        if (h.len && !recv_bytes(body.data(), h.len)) break;
        if (h.len >= sizeof(v)) memcpy(&v, body.data(), sizeof(v));
        switch (h.type) {
        case SL_H_MOUSE_MOVE: mouse_to(v.a, v.b); break;
        case SL_H_BUTTON:     inject_button(v.a, v.b != 0); break;
        case SL_H_WHEEL:
            inject_mouse(v.b ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, (DWORD)v.a);
            break;
        case SL_H_KEY:
            inject_key((WORD)v.a, (WORD)v.b, ((v.c & SL_KEY_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0) |
                                             ((v.c & SL_KEY_UP) ? KEYEVENTF_KEYUP : 0));
            break;
        case SL_H_ACTIVATE:   activate_window(hwnd); break;
        case SL_H_CLOSE:      PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case SL_H_SHOW:
            ShowWindowAsync(hwnd, v.a == SL_STATE_MINIMIZED ? SW_MINIMIZE
                                  : v.a == SL_STATE_MAXIMIZED ? SW_MAXIMIZE : SW_RESTORE);
            break;
        case SL_H_RESIZE:     resize_window(hwnd, v.a, v.b); break;
        case SL_H_MOVE_CANCEL:
            /* Escape puts the window back; the button is the host's now. */
            inject_key(VK_ESCAPE, 0, 0);
            inject_key(VK_ESCAPE, 0, KEYEVENTF_KEYUP);
            inject_mouse(MOUSEEVENTF_LEFTUP, 0);
            break;
        case SL_H_BLUR:
        {
            HWND fg = GetForegroundWindow();
            if (fg) PostMessageW(fg, WM_CANCELMODE, 0, 0);
            break;
        }
        case SL_H_LIST_APPS:
        {
            HANDLE t = CreateThread(NULL, 0, list_apps_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            break;
        }
        case SL_H_RUN:
            run_target(std::wstring((const wchar_t *)body.data(), h.len / sizeof(wchar_t)));
            break;
        }
    }
    sl_log("The host disconnected; exiting.");
    ExitProcess(0);
}

} /* namespace */

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AsbListener *l;
    HANDLE t;
    MSG msg;
    DWORD last_full = 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_send_cs);
    g_main_thread = GetCurrentThreadId();
    sl_log("Starting (PID=%lu).", GetCurrentProcessId());

    if (asb_transport_init() != 0 || (l = asb_listen(ASB_CH_SEAMLESS)) == NULL) {
        sl_log("Could not listen for the host. A VM started before this version of App Sandbox "
               "needs a restart for this channel.");
        return 1;
    }
    g_conn = asb_accept(l, 30000);
    if (!g_conn) {
        sl_log("No host connection arrived; exiting.");
        return 0;
    }
    asb_set_timeout(g_conn, 0, 30000);
    sl_log("Host connected.");

    /* Keep the guest's screen on: with it off, nothing is drawn to capture. */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    t = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    t = CreateThread(NULL, 0, recv_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);

    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, NULL, on_win_event, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_UNCLOAKED, NULL, on_win_event, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(NULL, 1, SCAN_MS, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            DWORD now = GetTickCount();
            static DWORD last_secure;
            if (now - last_secure >= 250) {
                last_secure = now;
                check_secure();
            }
            send_cursor();
            if (g_dirty || now - last_full >= FULL_SCAN_MS) {
                g_dirty = false;
                last_full = now;
                if (!g_secure) scan_windows();
            }
        }
        DispatchMessageW(&msg);
    }
    return 0;
}

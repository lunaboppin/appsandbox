/*
 * vm_backstage.c -- the host's Backstage window (see vm_backstage.h).
 *
 * Two threads per window: the window's own (message loop, painting, input)
 * and a network thread that starts the session through the agent, connects
 * to :0008 and receives screen bands into a BGRA buffer, reconnecting if
 * the session restarts. Both hold a reference to the view; the last one out
 * frees it.
 */

#include "vm_backstage.h"
#include "vm_agent.h"
#include "ui.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifndef AF_HYPERV
#define AF_HYPERV       34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif

#pragma pack(push, 1)
typedef struct {
    unsigned short Family;
    unsigned short Reserved;
    GUID VmId;
    GUID ServiceId;
} BsSockAddrHv;

/* Wire protocol: mirror of tools/agent/appsandbox-backstage.c */
typedef struct {
    UINT32 magic;
    UINT32 width;
    UINT32 height;
    UINT32 y;
    UINT32 rows;
} BsFrameHeader;

typedef struct {
    UINT32 magic;
    UINT32 type;
    INT32  a, b, c;
} BsInput;
#pragma pack(pop)

#define BS_CHANNEL      8
#define BS_FRAME_MAGIC  0x52465342u   /* "BSFR" */
#define BS_INPUT_MAGIC  0x4E495342u   /* "BSIN" */
#define BS_IN_MOVE      1
#define BS_IN_BUTTON    2
#define BS_IN_WHEEL     3
#define BS_IN_KEY       4
#define BS_MAX_SIDE     8192

#define WM_BS_STATUS    (WM_APP + 40)   /* lp = malloc'd status text */
#define WM_BS_FRONT     (WM_APP + 41)
#define IDM_BS_END      0x2010

static const wchar_t *BS_CLASS = L"AppSandboxBackstageView";

typedef struct BsView {
    VmInstance       *vm;
    GUID              runtime_id;
    wchar_t           vm_name[256];
    wchar_t           os_type[32];
    HINSTANCE         hinst;
    HWND              hwnd;
    volatile LONG     refs;
    volatile BOOL     closing;
    CRITICAL_SECTION  cs;          /* pixels, size, sock */
    SOCKET            sock;
    BYTE             *pixels;
    int               width, height;
    wchar_t           status[200];
    struct BsView    *next;
} BsView;

static CRITICAL_SECTION g_views_cs;
static INIT_ONCE        g_views_once = INIT_ONCE_STATIC_INIT;
static BsView          *g_views;

static BOOL CALLBACK init_views(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_views_cs);
    return TRUE;
}

static void view_release(BsView *v)
{
    if (InterlockedDecrement(&v->refs) != 0) return;
    DeleteCriticalSection(&v->cs);
    free(v->pixels);
    free(v);
}

static void view_status(BsView *v, const wchar_t *fmt, ...)
{
    wchar_t *text = (wchar_t *)calloc(200, sizeof(wchar_t));
    va_list ap;
    if (!text) return;
    va_start(ap, fmt);
    vswprintf_s(text, 200, fmt, ap);
    va_end(ap);
    if (!v->hwnd || !PostMessageW(v->hwnd, WM_BS_STATUS, 0, (LPARAM)text)) free(text);
}

/* ---- Network ---- */

static SOCKET bs_connect(BsView *v)
{
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    BsSockAddrHv addr;
    DWORD timeout = 10000;
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = v->runtime_id;
    hcs_service_guid(v->os_type, BS_CHANNEL, &addr.ServiceId);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    return s;
}

static BOOL bs_recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

static void bs_send_input(BsView *v, UINT32 type, INT32 a, INT32 b, INT32 c)
{
    BsInput in;
    in.magic = BS_INPUT_MAGIC;
    in.type = type;
    in.a = a; in.b = b; in.c = c;
    EnterCriticalSection(&v->cs);
    if (v->sock != INVALID_SOCKET)
        send(v->sock, (const char *)&in, sizeof(in), 0);
    LeaveCriticalSection(&v->cs);
}

/* Receive bands until the connection drops. */
static void bs_receive(BsView *v, SOCKET s)
{
    BsFrameHeader hdr;
    BYTE *band = NULL;
    size_t band_cap = 0;
    while (!v->closing && bs_recv_all(s, &hdr, sizeof(hdr))) {
        size_t bytes;
        if (hdr.magic != BS_FRAME_MAGIC || !hdr.width || !hdr.height ||
            hdr.width > BS_MAX_SIDE || hdr.height > BS_MAX_SIDE ||
            hdr.y >= hdr.height || hdr.rows == 0 || hdr.rows > hdr.height - hdr.y)
            break;
        bytes = (size_t)hdr.rows * hdr.width * 4;
        if (bytes > band_cap) {
            BYTE *grown = (BYTE *)realloc(band, bytes);
            if (!grown) break;
            band = grown;
            band_cap = bytes;
        }
        if (!bs_recv_all(s, band, (int)bytes)) break;
        EnterCriticalSection(&v->cs);
        if ((int)hdr.width != v->width || (int)hdr.height != v->height) {
            BYTE *fresh = (BYTE *)calloc((size_t)hdr.width * hdr.height, 4);
            if (fresh) {
                free(v->pixels);
                v->pixels = fresh;
                v->width = (int)hdr.width;
                v->height = (int)hdr.height;
            }
        }
        if (v->pixels && (int)hdr.width == v->width)
            memcpy(v->pixels + (size_t)hdr.y * hdr.width * 4, band, bytes);
        LeaveCriticalSection(&v->cs);
        {
            HWND hwnd = v->hwnd;   /* NULL once the window is gone; never invalidate "all" */
            if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
        }
    }
    free(band);
}

static DWORD WINAPI bs_net_thread(LPVOID param)
{
    BsView *v = (BsView *)param;
    char reply[256];
    while (!v->closing) {
        SOCKET s = INVALID_SOCKET;
        int tries;
        view_status(v, L"Starting the Backstage session in the VM...");
        reply[0] = '\0';
        if (!v->vm->agent_online) {
            view_status(v, L"Waiting for the VM's agent...");
            Sleep(2000);
            continue;
        }
        if (!vm_agent_send(v->vm, "backstage", reply, sizeof(reply), 30000)) {
            view_status(v, L"Backstage could not start: %S",
                        strncmp(reply, "error:", 6) == 0 ? reply + 6
                        : reply[0] ? reply : "the VM's agent did not answer.");
            Sleep(5000);
            continue;
        }
        for (tries = 0; tries < 30 && !v->closing; tries++) {
            s = bs_connect(v);
            if (s != INVALID_SOCKET) break;
            Sleep(500);
        }
        if (s == INVALID_SOCKET) {
            if (!v->closing)
                view_status(v, L"Could not connect to the Backstage session. Restart the VM if it was "
                               L"running before this AppSandbox update.");
            Sleep(5000);
            continue;
        }
        EnterCriticalSection(&v->cs);
        v->sock = s;
        LeaveCriticalSection(&v->cs);
        view_status(v, L"");
        bs_receive(v, s);
        EnterCriticalSection(&v->cs);
        v->sock = INVALID_SOCKET;
        LeaveCriticalSection(&v->cs);
        closesocket(s);
        if (!v->closing) {
            view_status(v, L"Disconnected; reconnecting...");
            Sleep(1000);
        }
    }
    view_release(v);
    return 0;
}

/* ---- Window ---- */

/* Where the picture goes: as large as fits, keeping its shape. */
static RECT bs_dest(BsView *v, const RECT *client)
{
    RECT r = *client;
    int cw = client->right - client->left, ch = client->bottom - client->top;
    if (v->width > 0 && v->height > 0 && cw > 0 && ch > 0) {
        int w = cw, h = (int)((LONGLONG)cw * v->height / v->width);
        if (h > ch) { h = ch; w = (int)((LONGLONG)ch * v->width / v->height); }
        r.left = client->left + (cw - w) / 2;
        r.top = client->top + (ch - h) / 2;
        r.right = r.left + w;
        r.bottom = r.top + h;
    }
    return r;
}

static BOOL bs_to_guest(BsView *v, LPARAM lp, INT32 *gx, INT32 *gy)
{
    RECT client, dst;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    GetClientRect(v->hwnd, &client);
    EnterCriticalSection(&v->cs);
    dst = bs_dest(v, &client);
    if (v->width <= 0 || dst.right <= dst.left || dst.bottom <= dst.top) {
        LeaveCriticalSection(&v->cs);
        return FALSE;
    }
    if (x < dst.left) x = dst.left;
    if (x >= dst.right) x = dst.right - 1;
    if (y < dst.top) y = dst.top;
    if (y >= dst.bottom) y = dst.bottom - 1;
    *gx = (INT32)((LONGLONG)(x - dst.left) * v->width / (dst.right - dst.left));
    *gy = (INT32)((LONGLONG)(y - dst.top) * v->height / (dst.bottom - dst.top));
    LeaveCriticalSection(&v->cs);
    return TRUE;
}

static DWORD WINAPI bs_end_thread(LPVOID param)
{
    VmInstance *vm = (VmInstance *)param;
    char reply[64];
    vm_agent_send(vm, "backstage_stop", reply, sizeof(reply), 10000);
    return 0;
}

static LRESULT CALLBACK bs_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BsView *v = (BsView *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    INT32 gx, gy;

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_BS_STATUS:
    {
        wchar_t *text = (wchar_t *)lp, title[400];
        if (v && text) {
            wcscpy_s(v->status, _countof(v->status), text);
            swprintf_s(title, _countof(title), L"Backstage (SYSTEM, hidden session) - %s%s%s",
                       v->vm_name, text[0] ? L" - " : L"", text);
            SetWindowTextW(hwnd, title);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        free(text);
        return 0;
    }
    case WM_BS_FRONT:
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == IDM_BS_END && v) {
            if (MessageBoxW(hwnd, L"End the Backstage session? Programs opened in it keep running "
                                  L"in the VM's hidden session until it restarts.",
                            L"Backstage", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                HANDLE t = CreateThread(NULL, 0, bs_end_thread, v->vm, 0, NULL);
                if (t) CloseHandle(t);
                DestroyWindow(hwnd);
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client, dst;
        GetClientRect(hwnd, &client);
        if (v) {
            EnterCriticalSection(&v->cs);
            dst = bs_dest(v, &client);
            if (v->pixels) {
                BITMAPINFO bi;
                HRGN outside = CreateRectRgnIndirect(&client), inner = CreateRectRgnIndirect(&dst);
                CombineRgn(outside, outside, inner, RGN_DIFF);
                FillRgn(dc, outside, (HBRUSH)GetStockObject(BLACK_BRUSH));
                DeleteObject(outside);
                DeleteObject(inner);
                ZeroMemory(&bi, sizeof(bi));
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = v->width;
                bi.bmiHeader.biHeight = -v->height;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                              0, 0, v->width, v->height, v->pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            LeaveCriticalSection(&v->cs);
            if (v->status[0]) {
                RECT band = client;
                HBRUSH bg = CreateSolidBrush(RGB(0x1b, 0x22, 0x2c));
                band.bottom = band.top + 32;
                FillRect(dc, &band, bg);
                DeleteObject(bg);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0xe6, 0xed, 0xf5));
                SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                band.left += 12;
                DrawTextW(dc, v->status, -1, &band, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (v && bs_to_guest(v, lp, &gx, &gy)) bs_send_input(v, BS_IN_MOVE, gx, gy, 0);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP:
    {
        BOOL down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0
                   : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
        if (!v) return 0;
        if (down) SetCapture(hwnd); else ReleaseCapture();
        if (bs_to_guest(v, lp, &gx, &gy)) bs_send_input(v, BS_IN_MOVE, gx, gy, 0);
        bs_send_input(v, BS_IN_BUTTON, button, down, 0);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (v) bs_send_input(v, BS_IN_WHEEL, GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (v) {
            BOOL up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
            bs_send_input(v, BS_IN_KEY, (INT32)wp, (INT32)((lp >> 16) & 0xFF),
                          (up ? 1 : 0) | (((lp >> 24) & 1) ? 2 : 0));
        }
        return 0;   /* everything goes to the VM, Alt included */
    case WM_CHAR: case WM_SYSCHAR:
        return 0;
    case WM_DESTROY:
        if (v) {
            BsView **p;
            v->closing = TRUE;
            EnterCriticalSection(&v->cs);
            if (v->sock != INVALID_SOCKET) shutdown(v->sock, SD_BOTH);   /* wakes the receiver */
            LeaveCriticalSection(&v->cs);
            EnterCriticalSection(&g_views_cs);
            for (p = &g_views; *p; p = &(*p)->next)
                if (*p == v) { *p = v->next; break; }
            LeaveCriticalSection(&g_views_cs);
            v->hwnd = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI bs_window_thread(LPVOID param)
{
    BsView *v = (BsView *)param;
    static volatile LONG registered;
    wchar_t title[400];
    HMENU sysmenu;
    HANDLE net;
    MSG msg;

    if (InterlockedCompareExchange(&registered, 1, 0) == 0) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = bs_wnd_proc;
        wc.hInstance = v->hinst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(NULL, IDI_SHIELD);
        wc.lpszClassName = BS_CLASS;
        RegisterClassExW(&wc);
    }
    swprintf_s(title, _countof(title), L"Backstage (SYSTEM, hidden session) - %s", v->vm_name);
    v->hwnd = CreateWindowExW(0, BS_CLASS, title, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1296, 768, NULL, NULL, v->hinst, v);
    if (!v->hwnd) {
        EnterCriticalSection(&g_views_cs);
        if (g_views == v) g_views = v->next;
        LeaveCriticalSection(&g_views_cs);
        view_release(v);
        view_release(v);   /* the network thread's reference, never started */
        return 0;
    }
    sysmenu = GetSystemMenu(v->hwnd, FALSE);
    if (sysmenu) {
        AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sysmenu, MF_STRING, IDM_BS_END, L"End Backstage session");
    }
    {
        BOOL dark = TRUE;
        typedef HRESULT (WINAPI *DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        DwmSetAttr set = dwm ? (DwmSetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        if (set) set(v->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
    ShowWindow(v->hwnd, SW_SHOW);
    SetForegroundWindow(v->hwnd);

    net = CreateThread(NULL, 0, bs_net_thread, v, 0, NULL);
    if (net) CloseHandle(net);
    else view_release(v);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    view_release(v);
    return 0;
}

void vm_backstage_open(VmInstance *vm, HINSTANCE hinst)
{
    BsView *v;
    HANDLE thread;
    if (!vm) return;
    InitOnceExecuteOnce(&g_views_once, init_views, NULL, NULL);

    EnterCriticalSection(&g_views_cs);
    for (v = g_views; v; v = v->next) {
        if (v->vm == vm && v->hwnd) {
            PostMessageW(v->hwnd, WM_BS_FRONT, 0, 0);
            LeaveCriticalSection(&g_views_cs);
            return;
        }
    }
    v = (BsView *)calloc(1, sizeof(*v));
    if (!v) {
        LeaveCriticalSection(&g_views_cs);
        return;
    }
    v->vm = vm;
    v->runtime_id = vm->runtime_id;
    wcscpy_s(v->vm_name, _countof(v->vm_name), vm->name);
    wcscpy_s(v->os_type, _countof(v->os_type), vm->os_type);
    v->hinst = hinst;
    v->sock = INVALID_SOCKET;
    v->refs = 2;                          /* window thread + network thread */
    InitializeCriticalSection(&v->cs);
    v->next = g_views;
    g_views = v;
    LeaveCriticalSection(&g_views_cs);

    thread = CreateThread(NULL, 0, bs_window_thread, v, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        EnterCriticalSection(&g_views_cs);
        if (g_views == v) g_views = v->next;
        LeaveCriticalSection(&g_views_cs);
        DeleteCriticalSection(&v->cs);
        free(v);
    }
}

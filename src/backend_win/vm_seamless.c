/*
 * vm_seamless.c -- the host side of seamless mode (see vm_seamless.h).
 *
 * Two threads per session. The window thread owns every HWND: the launcher and
 * one window per guest window. The network thread starts the helper through the
 * agent, connects to :0009 and copies pictures into each window's buffer; what
 * it can't do itself it posts to the launcher. Both hold a reference to the
 * session; the last one out frees it.
 *
 * Nothing from the guest is trusted: it can't size anything past the limits in
 * seamless_proto.h, its titles can't hide the VM's name, and its windows are
 * never topmost and only come forward under Windows' own foreground rules.
 *
 * A host window and its guest window are tied together by offset, not position:
 * the guest window sits somewhere on the guest's screen, the host window can be
 * anywhere, and a pointer position is the guest window's corner plus its place
 * in the host window. Dragging a title bar is handed to the host (SL_G_MOVE_START),
 * and the guest's window stays put. Anything else that moves the guest window
 * moves the host window by the same amount.
 */

#define COBJMACROS
#include <winsock2.h>
#include "vm_seamless.h"
#include "vm_agent.h"
#include "vm_clipboard.h"
#include "vm_display_auto.h"
#include "asb_core.h"
#include "ui.h"
#include "../../tools/agent/seamless_proto.h"
#include <initguid.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#ifndef AF_HYPERV
#define AF_HYPERV       34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

#pragma pack(push, 1)
typedef struct {
    unsigned short Family;
    unsigned short Reserved;
    GUID VmId;
    GUID ServiceId;
} SlSockAddrHv;
#pragma pack(pop)

#define SL_CHANNEL        9
#define SL_BORDER         2      /* the VM's colour round every window that has a frame */
#define SL_GRIP           6      /* how far in from the edge the host's resize begins */
#define SL_RESIZE_MS      60     /* how often a window being resized asks the guest for the new size */
#define SL_SHOW_LATE_MS   400    /* a window with no picture yet is shown anyway after this */

#define WM_SL_WINDOW      (WM_APP + 60)   /* lp = SlPosted */
#define WM_SL_CLOSED      (WM_APP + 61)   /* wp = guest window */
#define WM_SL_ICON        (WM_APP + 62)   /* lp = SlPosted */
#define WM_SL_CURSOR      (WM_APP + 63)   /* lp = SlPosted */
#define WM_SL_FOREGROUND  (WM_APP + 64)   /* wp = guest window */
#define WM_SL_MOVE_START  (WM_APP + 65)   /* wp = guest window */
#define WM_SL_SECURE      (WM_APP + 66)   /* wp = 1 secure, 0 back */
#define WM_SL_APPS        (WM_APP + 67)   /* lp = SlPosted */
#define WM_SL_RESET       (WM_APP + 68)   /* the connection ended: every host window goes */
#define WM_SL_STATUS      (WM_APP + 69)   /* lp = malloc'd text */
#define WM_SL_FIRST_FRAME (WM_APP + 70)   /* wp = guest window */
#define WM_SL_FRONT       (WM_APP + 71)
#define WM_SL_RUN         (WM_APP + 72)   /* lp = malloc'd target */
#define WM_SL_CONNECTED   (WM_APP + 73)
#define WM_SL_FOCUS_CHECK (WM_APP + 74)
#define WM_CLIP_READER_APPLY (WM_APP + 11)   /* vm_clipboard.h */

#define IDC_SL_FILTER     100
#define IDC_SL_LIST       101
#define IDC_SL_OPEN       102
#define IDC_SL_EXIT       103
#define IDC_SL_STATUS     104

#define IDT_SL_RESIZE     1
#define IDT_SL_SHOW       2
#define IDT_SL_LIST       3

static const wchar_t *SL_LAUNCHER_CLASS = L"AppSandboxSeamlessLauncher";
static const wchar_t *SL_WINDOW_CLASS   = L"AppSandboxSeamlessWindow";

typedef struct SlHostWin {
    struct SlSession *s;
    UINT32   guest;
    HWND     hwnd;
    SlWindow info;              /* last report; x,y are where the guest has it */
    wchar_t  title[SL_MAX_TITLE + 1];
    BOOL     have_info;
    int      border;
    /* under s->cs */
    BYTE    *pixels;
    int      pw, ph;
    /* window thread */
    BOOL     shown;
    BOOL     want_active;
    BOOL     in_sizemove;
    BOOL     move_pending;      /* guest entered a title-bar drag; hand it over once the pointer moves */
    POINT    down_at;           /* screen position of the left press */
    int      asked_w, asked_h;
    UINT32   guest_state;   /* what the guest last reported; info.state also follows the host */
    HICON    icon;
    unsigned frames;
    unsigned long long bytes;
} SlHostWin;

typedef struct { wchar_t *name; wchar_t *target; } SlApp;

typedef struct SlSession {
    UINT64            vm_id;
    GUID              runtime_id;
    wchar_t           vm_name[256];
    wchar_t           os_type[32];
    BOOL              lab_mode;
    HINSTANCE         hinst;
    HWND              launcher, status, filter, list, open_btn, exit_btn;
    HFONT             font;
    HBRUSH            bg_brush, edit_brush;
    COLORREF          colour;
    volatile LONG     refs;
    volatile BOOL     closing;
    volatile BOOL     connected;
    volatile BOOL     secure;
    CRITICAL_SECTION  cs;       /* wins, pixels, sock, screen, status_text */
    SOCKET            sock;
    SlHostWin        *wins[SL_MAX_WINDOWS];
    int               win_count;
    int               screen_w, screen_h;
    wchar_t           status_text[200];
    unsigned long long frames_received, bytes_received;
    /* window thread */
    SlHostWin        *active;
    UINT32            guest_active;
    DWORD             activated_tick;
    HCURSOR           cursor;
    BOOL              cursor_owned;
    UINT16            keys_down[256];   /* 0 up; else 0x100 | scan | (0x200 if extended) */
    int               buttons_down;
    SlApp            *apps;
    int               app_count;
    UINT32            list_guest[SL_MAX_WINDOWS];
    int               list_windows;
    int              *list_apps;
    VmDisplayAuto    *fallback;
    VmClipboard       clip;
    BOOL              clip_on;
    struct SlSession *next;
} SlSession;

typedef struct {
    UINT32  guest;
    SlWindow info;
    wchar_t title[SL_MAX_TITLE + 1];
    SlImage image;
    BYTE   *data;
    size_t  data_len;
} SlPosted;

static CRITICAL_SECTION g_sessions_cs;
static INIT_ONCE        g_sessions_once = INIT_ONCE_STATIC_INIT;
static SlSession       *g_sessions;

static BOOL CALLBACK init_sessions(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_sessions_cs);
    return TRUE;
}

static void session_release(SlSession *s)
{
    int i;
    if (InterlockedDecrement(&s->refs) != 0) return;
    for (i = 0; i < s->win_count; i++) {
        free(s->wins[i]->pixels);
        free(s->wins[i]);
    }
    for (i = 0; i < s->app_count; i++) {
        free(s->apps[i].name);
        free(s->apps[i].target);
    }
    free(s->apps);
    free(s->list_apps);
    DeleteCriticalSection(&s->cs);
    free(s);
}

/* A colour of the VM's own, always the same one, bright enough to be seen. */
static COLORREF vm_colour(const wchar_t *name)
{
    static const COLORREF palette[] = {
        RGB(0xF2, 0x8C, 0x28), RGB(0x2E, 0xB8, 0x8A), RGB(0x3D, 0x9B, 0xF2), RGB(0xE0, 0x4F, 0x8E),
        RGB(0xA3, 0x7C, 0xF2), RGB(0xE5, 0xC0, 0x2A), RGB(0x38, 0xC6, 0xD9), RGB(0xE8, 0x5D, 0x4A)
    };
    unsigned h = 5381;
    for (; *name; name++) h = h * 33 + (unsigned)*name;
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

static void set_status(SlSession *s, const wchar_t *fmt, ...)
{
    wchar_t *text = (wchar_t *)calloc(200, sizeof(wchar_t));
    va_list ap;
    if (!text) return;
    va_start(ap, fmt);
    vswprintf_s(text, 200, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&s->cs);
    wcscpy_s(s->status_text, _countof(s->status_text), text);
    LeaveCriticalSection(&s->cs);
    if (!s->launcher || !PostMessageW(s->launcher, WM_SL_STATUS, 0, (LPARAM)text)) free(text);
}

/* ---- Network ---- */

static void sl_send(SlSession *s, UINT32 type, UINT32 window, const void *body, UINT32 len)
{
    SlHeader h;
    h.magic = SL_MAGIC;
    h.type = type;
    h.window = window;
    h.len = len;
    EnterCriticalSection(&s->cs);
    if (s->sock != INVALID_SOCKET) {
        send(s->sock, (const char *)&h, sizeof(h), 0);
        if (len) send(s->sock, (const char *)body, (int)len, 0);
    }
    LeaveCriticalSection(&s->cs);
}

static void sl_send_value(SlSession *s, UINT32 type, UINT32 window, INT32 a, INT32 b, INT32 c)
{
    SlValue v;
    v.a = a; v.b = b; v.c = c;
    sl_send(s, type, window, &v, sizeof(v));
}

static BOOL sl_recv_all(SOCKET sock, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(sock, p, len > 0x40000000 ? 0x40000000 : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= (size_t)n;
    }
    return TRUE;
}

static SOCKET sl_connect(SlSession *s)
{
    SOCKET sock = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    SlSockAddrHv addr;
    DWORD timeout = 10000;
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = s->runtime_id;
    hcs_service_guid(s->os_type, SL_CHANNEL, &addr.ServiceId);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    return sock;
}

static SlHostWin *win_by_guest(SlSession *s, UINT32 guest)
{
    int i;
    for (i = 0; i < s->win_count; i++)
        if (s->wins[i]->guest == guest) return s->wins[i];
    return NULL;
}

/* Copy one rectangle of a window's picture into its buffer. */
static void net_frame(SlSession *s, UINT32 guest, const BYTE *body, size_t len)
{
    const SlFrame *f = (const SlFrame *)body;
    const BYTE *px = body + sizeof(SlFrame);
    SlHostWin *w;
    HWND hwnd = NULL;
    int border = 0;
    BOOL first = FALSE;
    UINT32 row;

    if (len < sizeof(SlFrame) || !f->width || !f->height || f->width > SL_MAX_SIDE || f->height > SL_MAX_SIDE ||
        !f->w || !f->h || f->x > f->width || f->w > f->width - f->x || f->y > f->height || f->h > f->height - f->y ||
        len != sizeof(SlFrame) + (size_t)f->w * f->h * 4)
        return;

    EnterCriticalSection(&s->cs);
    w = win_by_guest(s, guest);
    if (w) {
        if ((int)f->width != w->pw || (int)f->height != w->ph) {
            BYTE *fresh = (BYTE *)calloc((size_t)f->width * f->height, 4);
            if (fresh) {
                if (w->pixels) {
                    int y, rows = min(w->ph, (int)f->height), cols = min(w->pw, (int)f->width);
                    for (y = 0; y < rows; y++)
                        memcpy(fresh + (size_t)y * f->width * 4, w->pixels + (size_t)y * w->pw * 4, (size_t)cols * 4);
                }
                free(w->pixels);
                w->pixels = fresh;
                w->pw = (int)f->width;
                w->ph = (int)f->height;
            }
        }
        if (w->pixels && (int)f->width == w->pw && (int)f->height == w->ph) {
            for (row = 0; row < f->h; row++)
                memcpy(w->pixels + ((size_t)(f->y + row) * f->width + f->x) * 4,
                       px + (size_t)row * f->w * 4, (size_t)f->w * 4);
            first = w->frames == 0;
            w->frames++;
            w->bytes += len;
            hwnd = w->hwnd;
            border = w->border;
        }
    }
    s->frames_received++;
    s->bytes_received += len;
    LeaveCriticalSection(&s->cs);
    if (hwnd) {
        RECT r;
        r.left = (LONG)f->x + border;
        r.top = (LONG)f->y + border;
        r.right = r.left + (LONG)f->w;
        r.bottom = r.top + (LONG)f->h;
        InvalidateRect(hwnd, &r, FALSE);
    }
    if (first) PostMessageW(s->launcher, WM_SL_FIRST_FRAME, (WPARAM)guest, 0);
}

static void post_image(SlSession *s, UINT msg, UINT32 guest, const BYTE *body, size_t len, UINT32 max_side)
{
    const SlImage *img = (const SlImage *)body;
    SlPosted *p;
    if (len < sizeof(SlImage)) return;
    if (img->width > max_side || img->height > max_side ||
        len != sizeof(SlImage) + (size_t)img->width * img->height * 4)
        return;
    p = (SlPosted *)calloc(1, sizeof(*p));
    if (!p) return;
    p->guest = guest;
    p->image = *img;
    p->data_len = len - sizeof(SlImage);
    if (p->data_len) {
        p->data = (BYTE *)malloc(p->data_len);
        if (!p->data) { free(p); return; }
        memcpy(p->data, body + sizeof(SlImage), p->data_len);
    }
    if (!PostMessageW(s->launcher, msg, 0, (LPARAM)p)) {
        free(p->data);
        free(p);
    }
}

/* The guest picks a window's title. It can't hide who it comes from with text
   direction marks or control characters, or by being very long. */
static void clean_title(wchar_t *dst, size_t cap, const wchar_t *src, size_t chars)
{
    size_t i, n = 0;
    for (i = 0; i < chars && n + 1 < cap; i++) {
        wchar_t c = src[i];
        if (c < 0x20 || c == 0x7F || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
            (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF)
            continue;
        dst[n++] = c;
    }
    dst[n] = L'\0';
}

static void net_message(SlSession *s, const SlHeader *h, const BYTE *body)
{
    HWND launcher = s->launcher;
    switch (h->type) {
    case SL_G_HELLO:
        if (h->len >= sizeof(SlHello)) {
            const SlHello *hello = (const SlHello *)body;
            EnterCriticalSection(&s->cs);
            s->screen_w = hello->screen_w <= SL_MAX_SIDE ? (int)hello->screen_w : 0;
            s->screen_h = hello->screen_h <= SL_MAX_SIDE ? (int)hello->screen_h : 0;
            LeaveCriticalSection(&s->cs);
        }
        break;
    case SL_G_WINDOW:
        if (h->len >= sizeof(SlWindow)) {
            const SlWindow *in = (const SlWindow *)body;
            SlPosted *p;
            if (in->w < 0 || in->h < 0 || in->w > (int)SL_MAX_SIDE || in->h > (int)SL_MAX_SIDE) break;
            p = (SlPosted *)calloc(1, sizeof(*p));
            if (!p) break;
            p->guest = h->window;
            p->info = *in;
            clean_title(p->title, _countof(p->title), (const wchar_t *)(body + sizeof(SlWindow)),
                        (h->len - sizeof(SlWindow)) / sizeof(wchar_t));
            if (!PostMessageW(launcher, WM_SL_WINDOW, 0, (LPARAM)p)) free(p);
        }
        break;
    case SL_G_CLOSED:     PostMessageW(launcher, WM_SL_CLOSED, h->window, 0); break;
    case SL_G_FOREGROUND: PostMessageW(launcher, WM_SL_FOREGROUND, h->window, 0); break;
    case SL_G_MOVE_START: PostMessageW(launcher, WM_SL_MOVE_START, h->window, 0); break;
    case SL_G_SECURE:
        if (h->len >= sizeof(SlValue))
            PostMessageW(launcher, WM_SL_SECURE, ((const SlValue *)body)->a ? 1 : 0, 0);
        break;
    case SL_G_ICON:   post_image(s, WM_SL_ICON, h->window, body, h->len, SL_MAX_ICON_SIDE); break;
    case SL_G_CURSOR: post_image(s, WM_SL_CURSOR, 0, body, h->len, SL_MAX_CURSOR_SIDE); break;
    case SL_G_FRAME:  net_frame(s, h->window, body, h->len); break;
    case SL_G_APPS:
        if (h->len <= SL_MAX_APPS_BYTES) {
            SlPosted *p = (SlPosted *)calloc(1, sizeof(*p));
            if (!p) break;
            p->data_len = h->len;
            p->data = (BYTE *)malloc(h->len + sizeof(wchar_t));
            if (!p->data) { free(p); break; }
            memcpy(p->data, body, h->len);
            memset(p->data + h->len, 0, sizeof(wchar_t));
            if (!PostMessageW(launcher, WM_SL_APPS, 0, (LPARAM)p)) { free(p->data); free(p); }
        }
        break;
    }
}

static void net_receive(SlSession *s, SOCKET sock)
{
    BYTE *body = NULL;
    size_t cap = 0;
    SlHeader h;
    while (!s->closing && sl_recv_all(sock, &h, sizeof(h))) {
        if (h.magic != SL_MAGIC || h.len > SL_MAX_PAYLOAD) break;
        if (h.len + sizeof(wchar_t) > cap) {
            BYTE *grown = (BYTE *)realloc(body, h.len + sizeof(wchar_t));
            if (!grown) break;
            body = grown;
            cap = h.len + sizeof(wchar_t);
        }
        if (h.len && !sl_recv_all(sock, body, h.len)) break;
        net_message(s, &h, body);
    }
    free(body);
}

static void net_sleep(SlSession *s, DWORD ms)
{
    DWORD waited;
    for (waited = 0; waited < ms && !s->closing; waited += 100) Sleep(100);
}

static DWORD WINAPI net_thread(LPVOID param)
{
    SlSession *s = (SlSession *)param;
    char reply[256];
    while (!s->closing) {
        VmInstance *vm = asb_find_vm_by_id(s->vm_id);
        SOCKET sock = INVALID_SOCKET;
        int tries;
        SlHostHello hello;
        if (!vm || !vm->running) {
            /* The VM shut down: nothing to wait for. */
            PostMessageW(s->launcher, WM_CLOSE, 0, 0);
            break;
        }
        if (!vm->agent_online) {
            set_status(s, L"Waiting for the VM's agent...");
            net_sleep(s, 2000);
            continue;
        }
        reply[0] = '\0';
        if (!vm_agent_send(vm, "seamless", reply, sizeof(reply), 30000)) {
            if (strncmp(reply, "error:no_user", 13) == 0) {
                set_status(s, L"Nobody is signed in to the VM yet. Sign in in the display window.");
                PostMessageW(s->launcher, WM_SL_SECURE, 1, 0);
                net_sleep(s, 3000);
            } else {
                set_status(s, L"Seamless mode could not start: %S",
                           strncmp(reply, "error:", 6) == 0 ? reply + 6
                           : reply[0] ? reply : "the VM's agent did not answer.");
                net_sleep(s, 5000);
            }
            continue;
        }
        for (tries = 0; tries < 30 && !s->closing; tries++) {
            sock = sl_connect(s);
            if (sock != INVALID_SOCKET) break;
            Sleep(500);
        }
        if (sock == INVALID_SOCKET) {
            if (!s->closing)
                set_status(s, L"Could not connect to the VM's seamless helper. Restart the VM if it was "
                              L"running before this App Sandbox update.");
            net_sleep(s, 5000);
            continue;
        }
        EnterCriticalSection(&s->cs);
        s->sock = sock;
        LeaveCriticalSection(&s->cs);
        hello.version = SL_VERSION;
        sl_send(s, SL_H_HELLO, 0, &hello, sizeof(hello));
        sl_send(s, SL_H_LIST_APPS, 0, NULL, 0);
        s->connected = TRUE;
        PostMessageW(s->launcher, WM_SL_CONNECTED, 0, 0);
        PostMessageW(s->launcher, WM_SL_SECURE, 0, 0);
        set_status(s, L"Connected.");
        net_receive(s, sock);
        s->connected = FALSE;
        EnterCriticalSection(&s->cs);
        s->sock = INVALID_SOCKET;
        LeaveCriticalSection(&s->cs);
        closesocket(sock);
        /* A guest restart, sign-out or crash: its windows are gone with the helper. */
        if (!s->closing) {
            PostMessageW(s->launcher, WM_SL_RESET, 0, 0);
            set_status(s, L"Disconnected from the VM; reconnecting...");
            net_sleep(s, 2000);
        }
    }
    session_release(s);
    return 0;
}

/* ---- Images ---- */

/* An icon or pointer from BGRA. */
static HICON icon_from_bgra(const BYTE *px, int w, int h, int hot_x, int hot_y, BOOL is_icon)
{
    BITMAPV5HEADER bi;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP colour, mask;
    HICON icon = NULL;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    colour = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    mask = CreateBitmap(w, h, 1, 1, NULL);
    if (colour && mask) {
        ICONINFO ii;
        memcpy(bits, px, (size_t)w * h * 4);
        ii.fIcon = is_icon;
        ii.xHotspot = (DWORD)hot_x;
        ii.yHotspot = (DWORD)hot_y;
        ii.hbmMask = mask;
        ii.hbmColor = colour;
        icon = CreateIconIndirect(&ii);
    }
    if (colour) DeleteObject(colour);
    if (mask) DeleteObject(mask);
    return icon;
}

static void set_cursor(SlSession *s, const SlPosted *p)
{
    static const INT32 standard[] = {
        32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32650, 32651
    };
    HCURSOR next = NULL;
    BOOL owned = FALSE;
    if (p->image.width && p->image.height && p->data) {
        INT32 hx = p->image.hot_x, hy = p->image.hot_y;
        if (hx < 0 || hx >= (INT32)p->image.width) hx = 0;
        if (hy < 0 || hy >= (INT32)p->image.height) hy = 0;
        next = (HCURSOR)icon_from_bgra(p->data, (int)p->image.width, (int)p->image.height, hx, hy, FALSE);
        owned = next != NULL;
    } else if (p->image.hot_x) {
        int i;
        for (i = 0; i < (int)(sizeof(standard) / sizeof(standard[0])); i++)
            if (standard[i] == p->image.hot_x)
                next = LoadCursorW(NULL, MAKEINTRESOURCEW(p->image.hot_x));
        if (!next) next = LoadCursorW(NULL, IDC_ARROW);
    }
    if (s->cursor_owned && s->cursor) DestroyIcon((HICON)s->cursor);
    s->cursor = next;
    s->cursor_owned = owned;
    /* Takes effect now if the pointer is over one of ours. */
    {
        POINT pt;
        HWND under;
        wchar_t cls[64];
        GetCursorPos(&pt);
        under = WindowFromPoint(pt);
        if (under && GetClassNameW(under, cls, _countof(cls)) && wcscmp(cls, SL_WINDOW_CLASS) == 0)
            SetCursor(next);
    }
}

/* ---- Host windows ---- */

/* Where a guest window's top-left pixel goes in the host window's client area. */
static void content_size(SlHostWin *w, int *cw, int *ch)
{
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    *cw = rc.right - 2 * w->border;
    *ch = rc.bottom - 2 * w->border;
}

/* The guest's screen limits how large a host window can usefully be: every pixel
   the host shows has to be somewhere a click can reach. */
static void clamp_to_guest(SlSession *s, int *cw, int *ch)
{
    int sw, sh;
    EnterCriticalSection(&s->cs);
    sw = s->screen_w;
    sh = s->screen_h;
    LeaveCriticalSection(&s->cs);
    if (sw > 0 && *cw > sw) *cw = sw;
    if (sh > 0 && *ch > sh) *ch = sh;
}

/* A taskbar button for each window, even with "combine buttons" on, instead of
   every guest app stacking under App Sandbox. */
static void give_own_taskbar_entry(HWND hwnd, const SlHostWin *w)
{
    IPropertyStore *store = NULL;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, &IID_IPropertyStore, (void **)&store))) {
        wchar_t id[128];
        PROPVARIANT pv;
        swprintf_s(id, _countof(id), L"AppSandbox.Seamless.%llu.%08X", w->s->vm_id, w->guest);
        PropVariantInit(&pv);
        pv.vt = VT_LPWSTR;
        pv.pwszVal = id;
        IPropertyStore_SetValue(store, &PKEY_AppUserModel_ID, &pv);
        IPropertyStore_Commit(store);
        IPropertyStore_Release(store);
    }
}

static void update_title(SlHostWin *w)
{
    wchar_t text[SL_MAX_TITLE + 300];
    if (w->title[0])
        swprintf_s(text, _countof(text), L"%s  [%s]", w->title, w->s->vm_name);
    else
        swprintf_s(text, _countof(text), L"[%s]", w->s->vm_name);
    SetWindowTextW(w->hwnd, text);
}

static BOOL in_our_windows(SlSession *s, HWND hwnd)
{
    int i;
    if (!hwnd) return FALSE;
    if (hwnd == s->launcher) return TRUE;
    for (i = 0; i < s->win_count; i++)
        if (s->wins[i]->hwnd == hwnd) return TRUE;
    return FALSE;
}

static void schedule_list_refresh(SlSession *s)
{
    if (s->launcher) SetTimer(s->launcher, IDT_SL_LIST, 150, NULL);
}

static void release_held_input(SlSession *s)
{
    int vk;
    for (vk = 0; vk < 256; vk++) {
        if (!s->keys_down[vk]) continue;
        sl_send_value(s, SL_H_KEY, 0, vk, s->keys_down[vk] & 0xFF,
                      SL_KEY_UP | ((s->keys_down[vk] & 0x200) ? SL_KEY_EXTENDED : 0));
        s->keys_down[vk] = 0;
    }
    if (s->buttons_down) {
        int b;
        for (b = 0; b < 5; b++)
            if (s->buttons_down & (1 << b)) sl_send_value(s, SL_H_BUTTON, 0, b, 0, 0);
        s->buttons_down = 0;
    }
}

static void clipboard_sync(SlSession *s, BOOL on)
{
    if (!s->clip || on == s->clip_on) return;
    s->clip_on = on;
    vm_clipboard_set_sync_enabled(s->clip, on);
}

/* A host window just came to the front: the guest's should too, before any of
   the input that follows it. */
static void note_active(SlHostWin *w)
{
    SlSession *s = w->s;
    s->active = w;
    s->activated_tick = GetTickCount();
    if (s->guest_active != w->guest) {
        s->guest_active = w->guest;
        sl_send(s, SL_H_ACTIVATE, w->guest, NULL, 0);
    }
    clipboard_sync(s, TRUE);
}

/* Does this window get the pointer while nothing is pressed? Only the active
   one and its popups: the guest's pointer is in one place, and hovering a window
   behind would hover whatever covers it in the guest. */
static BOOL takes_hover(SlHostWin *w)
{
    return (w->info.flags & SL_WF_NOACTIVATE) || w->s->active == w;
}

static void send_pointer(SlHostWin *w, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    sl_send_value(w->s, SL_H_MOUSE_MOVE, 0, w->info.x + x - w->border, w->info.y + y - w->border, 0);
}

static void begin_host_move(SlHostWin *w)
{
    POINT pt;
    w->move_pending = FALSE;
    sl_send(w->s, SL_H_MOVE_CANCEL, w->guest, NULL, 0);
    w->s->buttons_down &= ~1;
    ReleaseCapture();
    GetCursorPos(&pt);
    SendMessageW(w->hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(pt.x, pt.y));
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SlHostWin *w = (SlHostWin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    SlSession *s = w ? w->s : NULL;

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_NCCALCSIZE:
        /* The whole window is client area: the guest draws its own title bar. */
        if (wp && w && !(w->info.flags & SL_WF_NOACTIVATE)) {
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            if (IsZoomed(hwnd)) {
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                InflateRect(&p->rgrc[0], -fx, -fy);
            }
            return 0;
        }
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wp, -1);
    case WM_NCHITTEST:
        if (w && !(w->info.flags & SL_WF_NOACTIVATE) && !IsZoomed(hwnd)) {
            RECT r;
            POINT pt;
            int edge = 0;
            pt.x = (short)LOWORD(lp);
            pt.y = (short)HIWORD(lp);
            GetWindowRect(hwnd, &r);
            if (pt.x < r.left + SL_GRIP) edge |= 1;
            else if (pt.x >= r.right - SL_GRIP) edge |= 2;
            if (pt.y < r.top + SL_GRIP) edge |= 4;
            else if (pt.y >= r.bottom - SL_GRIP) edge |= 8;
            switch (edge) {
            case 1:  return HTLEFT;
            case 2:  return HTRIGHT;
            case 4:  return HTTOP;
            case 8:  return HTBOTTOM;
            case 5:  return HTTOPLEFT;
            case 6:  return HTTOPRIGHT;
            case 9:  return HTBOTTOMLEFT;
            case 10: return HTBOTTOMRIGHT;
            }
        }
        return HTCLIENT;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && s) {
            SetCursor(s->cursor);
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO:
        if (s) {
            /* Maximized on whichever host monitor it's on, no larger than the guest's screen. */
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            int cw, ch, fx, fy;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                cw = mi.rcWork.right - mi.rcWork.left - 2 * w->border;
                ch = mi.rcWork.bottom - mi.rcWork.top - 2 * w->border;
                clamp_to_guest(s, &cw, &ch);
                mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left - fx;
                mm->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top - fy;
                mm->ptMaxSize.x = cw + 2 * w->border + 2 * fx;
                mm->ptMaxSize.y = ch + 2 * w->border + 2 * fy;
                mm->ptMinTrackSize.x = 160;
                mm->ptMinTrackSize.y = 80;
                cw = SL_MAX_SIDE;
                ch = SL_MAX_SIDE;
                clamp_to_guest(s, &cw, &ch);
                mm->ptMaxTrackSize.x = cw + 2 * w->border;
                mm->ptMaxTrackSize.y = ch + 2 * w->border;
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
        if (w) {
            RECT client;
            int border = w->border;
            GetClientRect(hwnd, &client);
            EnterCriticalSection(&s->cs);
            if (w->pixels) {
                BITMAPINFO bi;
                ZeroMemory(&bi, sizeof(bi));
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w->pw;
                bi.bmiHeader.biHeight = -w->ph;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(dc, border, border, (DWORD)w->pw, (DWORD)w->ph, 0, 0, 0, (UINT)w->ph,
                                  w->pixels, &bi, DIB_RGB_COLORS);
            }
            {
                /* What the picture doesn't reach yet while a resize is under way. */
                int pw = w->pixels ? w->pw : 0, ph = w->pixels ? w->ph : 0;
                HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
                RECT r;
                r.left = border + pw; r.top = border; r.right = client.right - border; r.bottom = client.bottom - border;
                if (r.right > r.left) FillRect(dc, &r, bg);
                r.left = border; r.top = border + ph; r.right = client.right - border; r.bottom = client.bottom - border;
                if (r.bottom > r.top) FillRect(dc, &r, bg);
            }
            LeaveCriticalSection(&s->cs);
            if (border) {
                HBRUSH c = CreateSolidBrush(s->colour);
                RECT r = client;
                r.bottom = border; FillRect(dc, &r, c);
                r = client; r.top = client.bottom - border; FillRect(dc, &r, c);
                r = client; r.right = border; FillRect(dc, &r, c);
                r = client; r.left = client.right - border; FillRect(dc, &r, c);
                DeleteObject(c);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        if (w && (w->info.flags & SL_WF_NOACTIVATE)) return MA_NOACTIVATE;
        break;
    case WM_ACTIVATE:
        if (!w) break;
        if (LOWORD(wp) != WA_INACTIVE) {
            note_active(w);
        } else {
            release_held_input(s);
            if (s->active == w) s->active = NULL;
            /* Focus may just be moving to another of ours; look once it has settled. */
            PostMessageW(s->launcher, WM_SL_FOCUS_CHECK, 0, 0);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        if (w) w->in_sizemove = TRUE;
        break;
    case WM_EXITSIZEMOVE:
        if (w) {
            int cw, ch;
            w->in_sizemove = FALSE;
            KillTimer(hwnd, IDT_SL_RESIZE);
            content_size(w, &cw, &ch);
            if ((cw != w->info.w || ch != w->info.h) && w->info.state != SL_STATE_MINIMIZED) {
                w->asked_w = cw;
                w->asked_h = ch;
                sl_send_value(s, SL_H_RESIZE, w->guest, cw, ch, 0);
            }
        }
        break;
    case WM_SIZE:
        if (!w) return 0;
        if (wp == SIZE_MINIMIZED) {
            if (w->info.state != SL_STATE_MINIMIZED) {
                w->info.state = SL_STATE_MINIMIZED;
                sl_send_value(s, SL_H_SHOW, w->guest, SL_STATE_MINIMIZED, 0, 0);
            }
            return 0;
        }
        if (wp == SIZE_MAXIMIZED && w->info.state != SL_STATE_MAXIMIZED) {
            w->info.state = SL_STATE_MAXIMIZED;
            sl_send_value(s, SL_H_SHOW, w->guest, SL_STATE_MAXIMIZED, 0, 0);
        } else if (wp == SIZE_RESTORED && w->info.state != SL_STATE_NORMAL) {
            w->info.state = SL_STATE_NORMAL;
            sl_send_value(s, SL_H_SHOW, w->guest, SL_STATE_NORMAL, 0, 0);
        }
        if (wp == SIZE_MAXIMIZED) {
            /* The guest maximizes to its own screen; fit it to this one. */
            SetTimer(hwnd, IDT_SL_RESIZE, SL_RESIZE_MS, NULL);
        } else if (w->in_sizemove) {
            SetTimer(hwnd, IDT_SL_RESIZE, SL_RESIZE_MS, NULL);
        }
        return 0;
    case WM_TIMER:
        if (!w) return 0;
        if (wp == IDT_SL_RESIZE) {
            int cw, ch;
            KillTimer(hwnd, IDT_SL_RESIZE);
            content_size(w, &cw, &ch);
            if ((cw != w->asked_w || ch != w->asked_h) && (cw != w->info.w || ch != w->info.h)) {
                w->asked_w = cw;
                w->asked_h = ch;
                sl_send_value(s, SL_H_RESIZE, w->guest, cw, ch, 0);
            }
        } else if (wp == IDT_SL_SHOW) {
            KillTimer(hwnd, IDT_SL_SHOW);
            PostMessageW(s->launcher, WM_SL_FIRST_FRAME, (WPARAM)w->guest, 0);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!w) return 0;
        if (w->move_pending) {
            POINT pt;
            GetCursorPos(&pt);
            if (!(GetKeyState(VK_LBUTTON) & 0x8000)) {
                w->move_pending = FALSE;
            } else if (abs(pt.x - w->down_at.x) >= GetSystemMetrics(SM_CXDRAG) ||
                       abs(pt.y - w->down_at.y) >= GetSystemMetrics(SM_CYDRAG)) {
                begin_host_move(w);
                return 0;
            }
        }
        if (s->buttons_down || takes_hover(w)) send_pointer(w, lp);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
    {
        BOOL down = msg != WM_LBUTTONUP && msg != WM_RBUTTONUP && msg != WM_MBUTTONUP && msg != WM_XBUTTONUP;
        int button;
        if (!w) return 0;
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: button = SL_BTN_LEFT; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: button = SL_BTN_RIGHT; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: button = SL_BTN_MIDDLE; break;
        default: button = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? SL_BTN_X1 : SL_BTN_X2; break;
        }
        if (down) {
            SetCapture(hwnd);
            s->buttons_down |= 1 << button;
            if (button == SL_BTN_LEFT) GetCursorPos(&w->down_at);
        } else {
            s->buttons_down &= ~(1 << button);
            if (!s->buttons_down) ReleaseCapture();
            w->move_pending = FALSE;
        }
        send_pointer(w, lp);
        sl_send_value(s, SL_H_BUTTON, 0, button, down ? 1 : 0, 0);
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
    }
    case WM_CAPTURECHANGED:
        if (s && (HWND)lp != hwnd) release_held_input(s);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (w) sl_send_value(s, SL_H_WHEEL, 0, GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL ? 1 : 0, 0);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (w) {
            BOOL up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
            int vk = (int)wp & 0xFF, scan = (int)((lp >> 16) & 0xFF);
            BOOL ext = ((lp >> 24) & 1) != 0;
            s->keys_down[vk] = up ? 0 : (UINT16)(0x100 | scan | (ext ? 0x200 : 0));
            sl_send_value(s, SL_H_KEY, 0, vk, scan, (up ? SL_KEY_UP : 0) | (ext ? SL_KEY_EXTENDED : 0));
        }
        return 0;   /* everything goes to the VM, Alt included */
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return 0;
    case WM_SYSCOMMAND:
        /* The taskbar's menu. The window itself goes when the guest's does. */
        if (w && (wp & 0xFFF0) == SC_CLOSE) {
            sl_send(s, SL_H_CLOSE, w->guest, NULL, 0);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (w) {
            sl_send(s, SL_H_CLOSE, w->guest, NULL, 0);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (w) {
            if (w->icon) DestroyIcon(w->icon);
            w->icon = NULL;
            if (s->active == w) s->active = NULL;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Keeping host windows in step with the guest's ---- */

static HWND owner_hwnd(SlSession *s, const SlWindow *info)
{
    SlHostWin *o = info->owner ? win_by_guest(s, info->owner) : NULL;
    if (o && o->hwnd) return o->hwnd;
    /* A menu or tooltip with no owner of its own floats above the active window. */
    if (!(info->flags & SL_WF_APP) && s->active) return s->active->hwnd;
    return NULL;
}

/* A popup stays at the same offset from its owner as it has in the guest. */
static void initial_position(SlSession *s, const SlWindow *info, HWND owner, int border, int *x, int *y)
{
    RECT work;
    SlHostWin *o = NULL;
    int i;
    for (i = 0; owner && i < s->win_count; i++)
        if (s->wins[i]->hwnd == owner) o = s->wins[i];
    if (o) {
        RECT or;
        GetWindowRect(o->hwnd, &or);
        *x = or.left + o->border + (info->x - o->info.x) - border;
        *y = or.top + o->border + (info->y - o->info.y) - border;
        return;
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    *x = work.left + info->x;
    *y = work.top + info->y;
    if (*x + info->w > work.right) *x = max(work.left, work.right - info->w);
    if (*y + info->h > work.bottom) *y = max(work.top, work.bottom - info->h);
}

static void on_window(SlSession *s, SlPosted *p)
{
    SlHostWin *w;
    SlWindow old;
    BOOL created = FALSE;
    UINT32 guest_state;
    int cw, ch;

    EnterCriticalSection(&s->cs);
    w = win_by_guest(s, p->guest);
    if (!w && s->win_count < SL_MAX_WINDOWS) {
        w = (SlHostWin *)calloc(1, sizeof(*w));
        if (w) {
            w->s = s;
            w->guest = p->guest;
            s->wins[s->win_count++] = w;
            created = TRUE;
        }
    }
    LeaveCriticalSection(&s->cs);
    if (!w) return;
    old = w->info;

    if (created) {
        w->guest_state = p->info.state;
        int x, y;
        DWORD style, ex = 0;
        HWND owner;
        BOOL frame = !(p->info.flags & SL_WF_NOACTIVATE);
        w->info = p->info;
        wcscpy_s(w->title, _countof(w->title), p->title);
        w->border = frame ? SL_BORDER : 0;
        owner = owner_hwnd(s, &p->info);
        style = WS_POPUP | (frame ? WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU : 0);
        if (p->info.flags & SL_WF_APP) ex |= WS_EX_APPWINDOW;
        if (!frame) ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        initial_position(s, &p->info, owner, w->border, &x, &y);
        w->hwnd = CreateWindowExW(ex, SL_WINDOW_CLASS, L"", style, x, y,
                                  p->info.w + 2 * w->border, p->info.h + 2 * w->border,
                                  owner, NULL, s->hinst, w);
        if (!w->hwnd) {
            ui_log(L"\"%s\": could not create a seamless window (%lu).", s->vm_name, GetLastError());
            return;
        }
        if (frame) {
            COLORREF colour = s->colour;
            BOOL dark = TRUE;
            DwmSetWindowAttribute(w->hwnd, DWMWA_BORDER_COLOR, &colour, sizeof(colour));
            DwmSetWindowAttribute(w->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        }
        if (p->info.flags & SL_WF_APP) give_own_taskbar_entry(w->hwnd, w);
        SetWindowPos(w->hwnd, NULL, x, y, p->info.w + 2 * w->border, p->info.h + 2 * w->border,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        update_title(w);
        if (p->info.state == SL_STATE_MINIMIZED) {
            ShowWindow(w->hwnd, SW_SHOWMINNOACTIVE);
            w->shown = TRUE;
        } else if (p->info.state == SL_STATE_MAXIMIZED) {
            ShowWindow(w->hwnd, SW_SHOWMAXIMIZED);
            w->shown = TRUE;
        } else {
            SetTimer(w->hwnd, IDT_SL_SHOW, SL_SHOW_LATE_MS, NULL);
        }
        schedule_list_refresh(s);
        /* The guest lists a window above its owner, so a dialog can arrive first. It
           has to stay above this window and at the right place on it. */
        {
            int i;
            for (i = 0; i < s->win_count; i++) {
                SlHostWin *child = s->wins[i];
                if (child == w || child->info.owner != p->guest || !child->hwnd) continue;
                SetWindowLongPtrW(child->hwnd, GWLP_HWNDPARENT, (LONG_PTR)w->hwnd);
                initial_position(s, &child->info, w->hwnd, child->border, &x, &y);
                SetWindowPos(child->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        return;
    }

    if (!w->hwnd) return;
    guest_state = w->guest_state;
    w->guest_state = p->info.state;
    /* Guest-side changes of owner or class of window don't happen often enough
       to rebuild for; the flags and title follow. */
    if (wcscmp(w->title, p->title) != 0) {
        wcscpy_s(w->title, _countof(w->title), p->title);
        update_title(w);
        schedule_list_refresh(s);
    }
    w->info.flags = p->info.flags;
    w->info.owner = p->info.owner;

    /* State first: what the size means depends on it. */
    if (p->info.state != w->info.state) {
        w->info.state = p->info.state;
        switch (p->info.state) {
        case SL_STATE_MINIMIZED:
            if (!IsIconic(w->hwnd)) ShowWindow(w->hwnd, SW_SHOWMINNOACTIVE);
            break;
        case SL_STATE_MAXIMIZED:
            if (!IsZoomed(w->hwnd)) ShowWindow(w->hwnd, s->active == w ? SW_SHOWMAXIMIZED : SW_MAXIMIZE);
            break;
        default:
            if (IsIconic(w->hwnd) || IsZoomed(w->hwnd))
                ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
            break;
        }
    }

    w->info.x = p->info.x;
    w->info.y = p->info.y;
    w->info.w = p->info.w;
    w->info.h = p->info.h;
    if (w->in_sizemove || p->info.state != SL_STATE_NORMAL) return;

    content_size(w, &cw, &ch);
    {
        int dx = (p->info.flags & SL_WF_SELF_MOVED) ? 0 : p->info.x - old.x;
        int dy = (p->info.flags & SL_WF_SELF_MOVED) ? 0 : p->info.y - old.y;
        /* Coming back from minimized or maximized isn't a move. */
        if (guest_state != SL_STATE_NORMAL || p->info.state != SL_STATE_NORMAL) dx = dy = 0;
        /* Not a move anyone made: never follow a guest window off into the distance. */
        if (abs(dx) >= s->screen_w || abs(dy) >= s->screen_h) dx = dy = 0;
        if (dx || dy || cw != p->info.w || ch != p->info.h) {
            RECT r;
            GetWindowRect(w->hwnd, &r);
            w->asked_w = p->info.w;
            w->asked_h = p->info.h;
            SetWindowPos(w->hwnd, NULL, r.left + dx, r.top + dy,
                         p->info.w + 2 * w->border, p->info.h + 2 * w->border,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

static void window_free(SlSession *s, SlHostWin *w)
{
    int i;
    HWND hwnd = w->hwnd;
    EnterCriticalSection(&s->cs);
    for (i = 0; i < s->win_count; i++) {
        if (s->wins[i] == w) {
            s->wins[i] = s->wins[--s->win_count];
            break;
        }
    }
    w->hwnd = NULL;
    LeaveCriticalSection(&s->cs);
    if (hwnd) DestroyWindow(hwnd);
    if (s->active == w) s->active = NULL;
    free(w->pixels);
    free(w);
}

static void destroy_all_windows(SlSession *s)
{
    while (s->win_count > 0) window_free(s, s->wins[s->win_count - 1]);
    s->guest_active = 0;
    schedule_list_refresh(s);
}

static void on_first_frame(SlSession *s, UINT32 guest)
{
    SlHostWin *w = win_by_guest(s, guest);
    if (!w || !w->hwnd || w->shown) return;
    w->shown = TRUE;
    KillTimer(w->hwnd, IDT_SL_SHOW);
    ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
    if (w->want_active) {
        w->want_active = FALSE;
        SetForegroundWindow(w->hwnd);
    }
}

static void on_foreground(SlSession *s, UINT32 guest)
{
    SlHostWin *w = win_by_guest(s, guest);
    HWND fg;
    s->guest_active = guest;
    if (!w || !w->hwnd || (w->info.flags & SL_WF_NOACTIVATE)) return;
    /* Our own request coming back, or two windows racing: the host's choice stands. */
    if (GetTickCount() - s->activated_tick < 300 && s->active && s->active != w) {
        s->guest_active = s->active->guest;
        sl_send(s, SL_H_ACTIVATE, s->guest_active, NULL, 0);
        return;
    }
    fg = GetForegroundWindow();
    if (fg == w->hwnd) return;
    /* An app opening a window only takes the host's foreground if the user is
       already in this VM's windows. Otherwise Windows' rules stop it anyway. */
    if (!in_our_windows(s, fg)) return;
    if (w->shown) SetForegroundWindow(w->hwnd);
    else w->want_active = TRUE;
}

/* ---- Secure desktop: the display window takes over ---- */

static void clipboard_stop(SlSession *s)
{
    if (s->clip) {
        vm_clipboard_destroy(s->clip);
        s->clip = NULL;
        s->clip_on = FALSE;
    }
}

static void clipboard_log(const wchar_t *msg, void *ud)
{
    ui_log(L"\"%s\" (seamless): %s", ((SlSession *)ud)->vm_name, msg);
}

/* The guest's clipboard helpers serve one viewer. If a display window has them,
   it keeps them; a malware lab VM gets none at all. */
static void clipboard_start(SlSession *s)
{
    VmInstance *vm = asb_find_vm_by_id(s->vm_id);
    if (s->clip || s->lab_mode || !vm || vm->idd_window) return;
    s->clip = vm_clipboard_create(&s->runtime_id, s->os_type, s->launcher, clipboard_log, s);
    if (s->clip && s->active) {
        s->clip_on = TRUE;
        vm_clipboard_set_sync_enabled(s->clip, TRUE);
    }
}

static void on_secure(SlSession *s, BOOL secure)
{
    VmInstance *vm = asb_find_vm_by_id(s->vm_id);
    if (secure == s->secure && (secure || !s->fallback)) return;
    s->secure = secure;
    if (secure) {
        HWND open = vm ? vm->idd_window : NULL;
        set_status(s, L"The VM's sign-in screen or an approval prompt has its input. Answer it in the display window.");
        release_held_input(s);
        clipboard_stop(s);
        if (open && IsWindow(open)) {
            if (IsIconic(open)) ShowWindow(open, SW_RESTORE);
            SetForegroundWindow(open);
        } else if (vm && !s->fallback) {
            s->fallback = vm_display_auto_create(vm, s->hinst, NULL, VDA_VIEW_AUTO);
        }
    } else {
        if (s->fallback) {
            VmDisplayAuto *d = s->fallback;
            s->fallback = NULL;
            vm_display_auto_destroy(d);
        }
        set_status(s, L"Connected.");
        clipboard_start(s);
    }
}

/* ---- Launcher ---- */

static void refill_list(SlSession *s)
{
    wchar_t filter[128] = L"";
    int i, top;
    int cap;
    LRESULT sel = SendMessageW(s->list, LB_GETCURSEL, 0, 0);
    wchar_t lower_filter[128];
    GetWindowTextW(s->filter, filter, _countof(filter));
    wcscpy_s(lower_filter, _countof(lower_filter), filter);
    _wcslwr_s(lower_filter, _countof(lower_filter));
    top = (int)SendMessageW(s->list, LB_GETTOPINDEX, 0, 0);
    SendMessageW(s->list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
    s->list_windows = 0;
    for (i = 0; i < s->win_count; i++) {
        SlHostWin *w = s->wins[i];
        wchar_t text[SL_MAX_TITLE + 8], lower[SL_MAX_TITLE + 8];
        if (!(w->info.flags & SL_WF_APP)) continue;
        wcscpy_s(lower, _countof(lower), w->title);
        _wcslwr_s(lower, _countof(lower));
        if (lower_filter[0] && !wcsstr(lower, lower_filter)) continue;
        swprintf_s(text, _countof(text), L"â–£  %s", w->title[0] ? w->title : L"(untitled window)");
        SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)text);
        s->list_guest[s->list_windows++] = w->guest;
    }
    cap = s->app_count;
    free(s->list_apps);
    s->list_apps = cap ? (int *)malloc(sizeof(int) * cap) : NULL;
    {
        int n = 0;
        for (i = 0; i < s->app_count && s->list_apps; i++) {
            wchar_t lower[300];
            wcsncpy_s(lower, _countof(lower), s->apps[i].name, _TRUNCATE);
            _wcslwr_s(lower, _countof(lower));
            if (lower_filter[0] && !wcsstr(lower, lower_filter)) continue;
            SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)s->apps[i].name);
            s->list_apps[n++] = i;
        }
    }
    SendMessageW(s->list, LB_SETTOPINDEX, (WPARAM)top, 0);
    if (sel >= 0) SendMessageW(s->list, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageW(s->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s->list, NULL, TRUE);
}

static void run_in_guest(SlSession *s, const wchar_t *target)
{
    sl_send(s, SL_H_RUN, 0, target, (UINT32)(wcslen(target) * sizeof(wchar_t)));
}

static void open_selected(SlSession *s)
{
    LRESULT sel = SendMessageW(s->list, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    if (sel < s->list_windows) {
        SlHostWin *w = win_by_guest(s, s->list_guest[sel]);
        if (w && w->hwnd) {
            if (IsIconic(w->hwnd)) ShowWindow(w->hwnd, SW_RESTORE);
            SetForegroundWindow(w->hwnd);
        }
    } else if (s->list_apps) {
        run_in_guest(s, s->apps[s->list_apps[sel - s->list_windows]].target);
    }
}

static void set_apps(SlSession *s, const wchar_t *data, size_t chars)
{
    int i, n = 0;
    size_t pos;
    for (i = 0; i < s->app_count; i++) {
        free(s->apps[i].name);
        free(s->apps[i].target);
    }
    free(s->apps);
    s->apps = NULL;
    s->app_count = 0;
    for (pos = 0; pos < chars; pos++)
        if (data[pos] == L'\0') n++;
    n /= 2;
    if (!n) return;
    s->apps = (SlApp *)calloc((size_t)n, sizeof(SlApp));
    if (!s->apps) return;
    for (pos = 0; pos < chars && s->app_count < n;) {
        const wchar_t *name = data + pos, *target;
        size_t nlen = wcsnlen(name, chars - pos), tlen;
        if (pos + nlen + 1 >= chars) break;
        target = name + nlen + 1;
        tlen = wcsnlen(target, chars - pos - nlen - 1);
        if (nlen && tlen && !wcsstr(name, L"Uninstall")) {
            s->apps[s->app_count].name = _wcsdup(name);
            s->apps[s->app_count].target = _wcsdup(target);
            if (!s->apps[s->app_count].name || !s->apps[s->app_count].target) break;
            s->app_count++;
        }
        pos += nlen + tlen + 2;
    }
    /* Alphabetical, like the Start menu's list. */
    for (i = 1; i < s->app_count; i++) {
        SlApp a = s->apps[i];
        int j = i - 1;
        while (j >= 0 && _wcsicmp(s->apps[j].name, a.name) > 0) {
            s->apps[j + 1] = s->apps[j];
            j--;
        }
        s->apps[j + 1] = a;
    }
}

static void layout_launcher(SlSession *s)
{
    RECT rc;
    UINT dpi = GetDpiForWindow(s->launcher);
    int m = MulDiv(12, dpi, 96), row = MulDiv(30, dpi, 96), status_h = MulDiv(42, dpi, 96), y;
    GetClientRect(s->launcher, &rc);
    y = m;
    MoveWindow(s->status, m, y, rc.right - 2 * m, status_h, TRUE);
    y += status_h + m / 2;
    MoveWindow(s->filter, m, y, rc.right - 2 * m, row - MulDiv(4, dpi, 96), TRUE);
    y += row;
    MoveWindow(s->list, m, y, rc.right - 2 * m, rc.bottom - y - m * 2 - row, TRUE);
    MoveWindow(s->open_btn, m, rc.bottom - m - row, MulDiv(110, dpi, 96), row, TRUE);
    MoveWindow(s->exit_btn, rc.right - m - MulDiv(150, dpi, 96), rc.bottom - m - row, MulDiv(150, dpi, 96), row, TRUE);
}

static LRESULT CALLBACK launcher_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SlSession *s = (SlSession *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_SIZE:
        if (s && s->list) layout_launcher(s);
        return 0;
    case WM_ERASEBKGND:
        if (s) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, s->bg_brush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (s) {
            SetTextColor((HDC)wp, RGB(0xE6, 0xED, 0xF5));
            SetBkColor((HDC)wp, msg == WM_CTLCOLORSTATIC ? RGB(0x16, 0x1F, 0x2C) : RGB(0x22, 0x2C, 0x3A));
            return (LRESULT)(msg == WM_CTLCOLORSTATIC ? s->bg_brush : s->edit_brush);
        }
        break;
    case WM_COMMAND:
        if (!s) break;
        switch (LOWORD(wp)) {
        case IDC_SL_FILTER:
            if (HIWORD(wp) == EN_CHANGE) refill_list(s);
            return 0;
        case IDC_SL_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) open_selected(s);
            return 0;
        case IDOK:
        case IDC_SL_OPEN:
            open_selected(s);
            return 0;
        case IDCANCEL:
        case IDC_SL_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_TIMER:
        if (s && wp == IDT_SL_LIST) {
            KillTimer(hwnd, IDT_SL_LIST);
            refill_list(s);
        }
        return 0;
    case WM_SL_STATUS:
    {
        wchar_t *text = (wchar_t *)lp;
        if (s && text) SetWindowTextW(s->status, text);
        free(text);
        return 0;
    }
    case WM_SL_WINDOW:
    {
        SlPosted *p = (SlPosted *)lp;
        if (s) on_window(s, p);
        free(p);
        return 0;
    }
    case WM_SL_CLOSED:
        if (s) {
            SlHostWin *w = win_by_guest(s, (UINT32)wp);
            if (w) {
                window_free(s, w);
                schedule_list_refresh(s);
            }
        }
        return 0;
    case WM_SL_ICON:
    {
        SlPosted *p = (SlPosted *)lp;
        SlHostWin *w = s ? win_by_guest(s, p->guest) : NULL;
        if (w && w->hwnd && p->data && p->image.width && p->image.height) {
            HICON icon = icon_from_bgra(p->data, (int)p->image.width, (int)p->image.height, 0, 0, TRUE);
            if (icon) {
                SendMessageW(w->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
                SendMessageW(w->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
                if (w->icon) DestroyIcon(w->icon);
                w->icon = icon;
            }
        }
        free(p->data);
        free(p);
        return 0;
    }
    case WM_SL_CURSOR:
    {
        SlPosted *p = (SlPosted *)lp;
        if (s) set_cursor(s, p);
        free(p->data);
        free(p);
        return 0;
    }
    case WM_SL_FOREGROUND:
        if (s) on_foreground(s, (UINT32)wp);
        return 0;
    case WM_SL_FIRST_FRAME:
        if (s) on_first_frame(s, (UINT32)wp);
        return 0;
    case WM_SL_MOVE_START:
        if (s) {
            SlHostWin *w = win_by_guest(s, (UINT32)wp);
            if (w && w->hwnd && (s->buttons_down & 1) && GetCapture() == w->hwnd) {
                POINT pt;
                w->move_pending = TRUE;
                GetCursorPos(&pt);
                if (abs(pt.x - w->down_at.x) >= GetSystemMetrics(SM_CXDRAG) ||
                    abs(pt.y - w->down_at.y) >= GetSystemMetrics(SM_CYDRAG))
                    begin_host_move(w);
            }
        }
        return 0;
    case WM_SL_SECURE:
        if (s) on_secure(s, wp != 0);
        return 0;
    case WM_SL_APPS:
    {
        SlPosted *p = (SlPosted *)lp;
        if (s) {
            set_apps(s, (const wchar_t *)p->data, p->data_len / sizeof(wchar_t));
            refill_list(s);
        }
        free(p->data);
        free(p);
        return 0;
    }
    case WM_SL_RESET:
        if (s) {
            release_held_input(s);
            destroy_all_windows(s);
            clipboard_stop(s);
        }
        return 0;
    case WM_SL_CONNECTED:
        if (s) clipboard_start(s);
        return 0;
    case WM_SL_FOCUS_CHECK:
        /* Focus has settled. If it left this VM's windows, close any menu open in
           the guest, as clicking away from one does, and stop sharing the clipboard. */
        if (s && !in_our_windows(s, GetForegroundWindow())) {
            s->guest_active = 0;
            sl_send(s, SL_H_BLUR, 0, NULL, 0);
            clipboard_sync(s, FALSE);
        }
        return 0;
    case WM_SL_FRONT:
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 0;
    case WM_SL_RUN:
    {
        wchar_t *target = (wchar_t *)lp;
        if (s && target) run_in_guest(s, target);
        free(target);
        return 0;
    }
    case WM_ACTIVATE:
        if (s && LOWORD(wp) != WA_INACTIVE) clipboard_sync(s, TRUE);
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (s && s->clip) vm_clipboard_on_clipboard_update(s->clip);
        return 0;
    case WM_CLIP_READER_APPLY:
        if (s && s->clip) vm_clipboard_on_reader_apply(s->clip);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (s) {
            s->closing = TRUE;
            EnterCriticalSection(&s->cs);
            if (s->sock != INVALID_SOCKET) shutdown(s->sock, SD_BOTH);   /* wakes the receiver; the helper exits */
            LeaveCriticalSection(&s->cs);
            RemoveClipboardFormatListener(hwnd);
            clipboard_stop(s);
            destroy_all_windows(s);
            if (s->fallback) {
                VmDisplayAuto *d = s->fallback;
                s->fallback = NULL;
                vm_display_auto_destroy(d);
            }
            {
                SlSession **p;
                EnterCriticalSection(&g_sessions_cs);
                for (p = &g_sessions; *p; p = &(*p)->next)
                    if (*p == s) { *p = s->next; break; }
                LeaveCriticalSection(&g_sessions_cs);
            }
            s->launcher = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_control(SlSession *s, DWORD ex, const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             s->launcher, (HMENU)(INT_PTR)id, s->hinst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s->font, TRUE);
    return c;
}

static DWORD WINAPI window_thread(LPVOID param)
{
    SlSession *s = (SlSession *)param;
    static volatile LONG registered;
    wchar_t title[320];
    HANDLE net;
    MSG msg;
    NONCLIENTMETRICSW ncm;
    UINT dpi;

    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (InterlockedCompareExchange(&registered, 1, 0) == 0) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = launcher_proc;
        wc.hInstance = s->hinst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        wc.lpszClassName = SL_LAUNCHER_CLASS;
        RegisterClassExW(&wc);
        wc.lpfnWndProc = window_proc;
        wc.lpszClassName = SL_WINDOW_CLASS;
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
    }
    s->bg_brush = CreateSolidBrush(RGB(0x16, 0x1F, 0x2C));
    s->edit_brush = CreateSolidBrush(RGB(0x22, 0x2C, 0x3A));
    swprintf_s(title, _countof(title), L"Seamless - %s", s->vm_name);
    s->launcher = CreateWindowExW(WS_EX_APPWINDOW, SL_LAUNCHER_CLASS, title,
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 10, 10, NULL, NULL, s->hinst, s);
    if (!s->launcher) {
        EnterCriticalSection(&g_sessions_cs);
        if (g_sessions == s) g_sessions = s->next;
        LeaveCriticalSection(&g_sessions_cs);
        session_release(s);
        session_release(s);   /* the network thread's reference, never started */
        CoUninitialize();
        return 0;
    }
    dpi = GetDpiForWindow(s->launcher);
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))
        s->font = CreateFontIndirectW(&ncm.lfMessageFont);
    SetWindowPos(s->launcher, NULL, 0, 0, MulDiv(420, dpi, 96), MulDiv(560, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    {
        COLORREF colour = s->colour;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(s->launcher, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(s->launcher, DWMWA_BORDER_COLOR, &colour, sizeof(colour));
    }
    s->status = make_control(s, 0, L"STATIC", L"Starting seamless mode...", SS_LEFT, IDC_SL_STATUS);
    s->filter = make_control(s, 0, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, IDC_SL_FILTER);
    SendMessageW(s->filter, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search the VM's apps");
    s->list = make_control(s, 0, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, IDC_SL_LIST);
    s->open_btn = make_control(s, 0, L"BUTTON", L"Open", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_SL_OPEN);
    s->exit_btn = make_control(s, 0, L"BUTTON", L"Exit seamless mode", WS_TABSTOP, IDC_SL_EXIT);
    layout_launcher(s);
    AddClipboardFormatListener(s->launcher);
    ShowWindow(s->launcher, SW_SHOW);
    SetForegroundWindow(s->launcher);

    net = CreateThread(NULL, 0, net_thread, s, 0, NULL);
    if (net) CloseHandle(net);
    else session_release(s);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (s->launcher && IsDialogMessageW(s->launcher, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (s->font) DeleteObject(s->font);
    DeleteObject(s->bg_brush);
    DeleteObject(s->edit_brush);
    if (s->cursor_owned && s->cursor) DestroyIcon((HICON)s->cursor);
    CoUninitialize();
    session_release(s);
    return 0;
}

/* ---- Public ---- */

const wchar_t *vm_seamless_unavailable(VmInstance *vm)
{
    if (!vm || !vm->running) return L"The VM isn't running.";
    if (_wcsicmp(vm->os_type, L"Windows") != 0)
        return L"Seamless mode needs Windows guests: Ubuntu's Wayland desktop has no way for a helper "
               L"to capture other apps' windows one by one.";
    if (vm->lab_mode) return L"Seamless mode is off for malware lab VMs, so nothing in them reaches the host's desktop.";
    return NULL;
}

void vm_seamless_open(VmInstance *vm, HINSTANCE hinst)
{
    SlSession *s;
    HANDLE thread;
    if (vm_seamless_unavailable(vm)) return;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);

    EnterCriticalSection(&g_sessions_cs);
    for (s = g_sessions; s; s = s->next) {
        if (s->vm_id == vm->unique_id) {
            if (s->launcher) PostMessageW(s->launcher, WM_SL_FRONT, 0, 0);
            LeaveCriticalSection(&g_sessions_cs);
            return;
        }
    }
    s = (SlSession *)calloc(1, sizeof(*s));
    if (!s) {
        LeaveCriticalSection(&g_sessions_cs);
        return;
    }
    s->vm_id = vm->unique_id;
    s->runtime_id = vm->runtime_id;
    wcscpy_s(s->vm_name, _countof(s->vm_name), vm->name);
    wcscpy_s(s->os_type, _countof(s->os_type), vm->os_type);
    s->lab_mode = vm->lab_mode;
    s->hinst = hinst;
    s->sock = INVALID_SOCKET;
    s->colour = vm_colour(vm->name);
    s->cursor = LoadCursorW(NULL, IDC_ARROW);
    s->refs = 2;                          /* window thread + network thread */
    InitializeCriticalSection(&s->cs);
    s->next = g_sessions;
    g_sessions = s;
    LeaveCriticalSection(&g_sessions_cs);

    thread = CreateThread(NULL, 0, window_thread, s, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        EnterCriticalSection(&g_sessions_cs);
        if (g_sessions == s) g_sessions = s->next;
        LeaveCriticalSection(&g_sessions_cs);
        DeleteCriticalSection(&s->cs);
        free(s);
    }
}

static SlSession *session_find(UINT64 vm_id)   /* with g_sessions_cs held */
{
    SlSession *s;
    for (s = g_sessions; s; s = s->next)
        if (s->vm_id == vm_id) return s;
    return NULL;
}

void vm_seamless_close(UINT64 vm_id)
{
    SlSession *s;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);
    EnterCriticalSection(&g_sessions_cs);
    s = session_find(vm_id);
    if (s && s->launcher) PostMessageW(s->launcher, WM_CLOSE, 0, 0);
    LeaveCriticalSection(&g_sessions_cs);
}

BOOL vm_seamless_is_open(UINT64 vm_id)
{
    BOOL open;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);
    EnterCriticalSection(&g_sessions_cs);
    open = session_find(vm_id) != NULL;
    LeaveCriticalSection(&g_sessions_cs);
    return open;
}

BOOL vm_seamless_run(UINT64 vm_id, const wchar_t *target)
{
    SlSession *s;
    BOOL ok = FALSE;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);
    EnterCriticalSection(&g_sessions_cs);
    s = session_find(vm_id);
    if (s && s->launcher && s->connected) {
        wchar_t *copy = _wcsdup(target);
        if (copy) {
            ok = PostMessageW(s->launcher, WM_SL_RUN, 0, (LPARAM)copy);
            if (!ok) free(copy);
        }
    }
    LeaveCriticalSection(&g_sessions_cs);
    return ok;
}

BOOL vm_seamless_input(UINT64 vm_id, UINT32 guest, const wchar_t *event, int a, int b, int c)
{
    SlSession *s;
    HWND hwnd = NULL;
    int border = 0, i;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);
    EnterCriticalSection(&g_sessions_cs);
    s = session_find(vm_id);
    if (s) {
        EnterCriticalSection(&s->cs);
        for (i = 0; i < s->win_count; i++) {
            if (s->wins[i]->guest == guest && s->wins[i]->hwnd) {
                hwnd = s->wins[i]->hwnd;
                border = s->wins[i]->border;
            }
        }
        LeaveCriticalSection(&s->cs);
    }
    LeaveCriticalSection(&g_sessions_cs);
    if (!hwnd) return FALSE;

    if (wcscmp(event, L"move") == 0 || wcscmp(event, L"down") == 0 || wcscmp(event, L"up") == 0) {
        /* a, b: inside the guest window. c: 0 left, 1 right, 2 middle. */
        LPARAM lp = MAKELPARAM(a + border, b + border);
        static const UINT downs[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
        static const UINT ups[] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
        if (c < 0 || c > 2) return FALSE;
        if (event[0] == L'm') return PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
        return PostMessageW(hwnd, event[0] == L'd' ? downs[c] : ups[c], 0, lp);
    }
    if (wcscmp(event, L"wheel") == 0)
        return PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)a), 0);
    if (wcscmp(event, L"keydown") == 0 || wcscmp(event, L"keyup") == 0) {
        /* a: virtual key. Unlike a real key press these carry the scan code from the host's layout. */
        UINT scan = MapVirtualKeyW((UINT)a, MAPVK_VK_TO_VSC);
        LPARAM lp = 1 | ((LPARAM)scan << 16);
        if (event[3] == L'u') lp |= (LPARAM)0xC0000000u;
        return PostMessageW(hwnd, event[3] == L'u' ? WM_KEYUP : WM_KEYDOWN, (WPARAM)a, lp);
    }
    if (wcscmp(event, L"activate") == 0) {
        /* The real thing can be refused by the foreground lock, and the session has
           to act on being told either way. */
        SetForegroundWindow(hwnd);
        return PostMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
    }
    if (wcscmp(event, L"close") == 0) return PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
    if (wcscmp(event, L"minimize") == 0) return ShowWindowAsync(hwnd, SW_MINIMIZE);
    if (wcscmp(event, L"maximize") == 0) return ShowWindowAsync(hwnd, SW_MAXIMIZE);
    if (wcscmp(event, L"restore") == 0) return ShowWindowAsync(hwnd, SW_RESTORE);
    if (wcscmp(event, L"position") == 0)   /* a, b: the host window's top left */
        return SetWindowPos(hwnd, NULL, a, b, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (wcscmp(event, L"size") == 0) {
        /* a, b: the content. Goes through the same steps as dragging an edge. */
        BOOL ok;
        SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        ok = SetWindowPos(hwnd, NULL, 0, 0, a + 2 * border, b + 2 * border, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
        return ok;
    }
    return FALSE;
}

static size_t json_text(char *out, size_t cap, const wchar_t *in)
{
    size_t n = 0;
    for (; *in && n + 8 < cap; in++) {
        wchar_t c = *in;
        if (c == L'"' || c == L'\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20 || c > 0x7E) {
            n += (size_t)sprintf_s(out + n, cap - n, "\\u%04x", (unsigned)c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return n;
}

char *vm_seamless_picture_json(UINT64 vm_id, UINT32 guest)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    SlSession *s;
    char *out = NULL;
    int i;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);
    EnterCriticalSection(&g_sessions_cs);
    s = session_find(vm_id);
    if (s) {
        EnterCriticalSection(&s->cs);
        for (i = 0; i < s->win_count; i++) {
            SlHostWin *w = s->wins[i];
            if (w->guest == guest && w->pixels) {
                size_t bytes = (size_t)w->pw * w->ph * 4, n, pos;
                const BYTE *px = (const BYTE *)w->pixels;
                out = (char *)malloc(bytes / 3 * 4 + 128);
                if (out) {
                    pos = (size_t)sprintf_s(out, 128, "{\"width\":%d,\"height\":%d,\"bgra\":\"", w->pw, w->ph);
                    for (n = 0; n < bytes; n += 3) {
                        UINT32 v = (UINT32)px[n] << 16;
                        if (n + 1 < bytes) v |= (UINT32)px[n + 1] << 8;
                        if (n + 2 < bytes) v |= px[n + 2];
                        out[pos++] = digits[v >> 18];
                        out[pos++] = digits[(v >> 12) & 63];
                        out[pos++] = n + 1 < bytes ? digits[(v >> 6) & 63] : '=';
                        out[pos++] = n + 2 < bytes ? digits[v & 63] : '=';
                    }
                    out[pos++] = '"';
                    out[pos++] = '}';
                    out[pos] = '\0';
                }
            }
        }
        LeaveCriticalSection(&s->cs);
    }
    LeaveCriticalSection(&g_sessions_cs);
    return out;
}

BOOL vm_seamless_status_json(UINT64 vm_id, char *buf, size_t cap)
{
    SlSession *s;
    size_t pos = 0;
    char text[900];
    int i;
    InitOnceExecuteOnce(&g_sessions_once, init_sessions, NULL, NULL);
    EnterCriticalSection(&g_sessions_cs);
    s = session_find(vm_id);
    if (!s) {
        LeaveCriticalSection(&g_sessions_cs);
        return sprintf_s(buf, cap, "{\"open\":false,\"windows\":[]}") > 0;
    }
    EnterCriticalSection(&s->cs);
    json_text(text, sizeof(text), s->status_text);
    pos += (size_t)sprintf_s(buf + pos, cap - pos,
        "{\"open\":true,\"connected\":%s,\"secure\":%s,\"fallbackDisplay\":%s,\"status\":\"%s\","
        "\"guestScreen\":[%d,%d],\"apps\":%d,\"framesReceived\":%llu,\"bytesReceived\":%llu,\"windows\":[",
        s->connected ? "true" : "false", s->secure ? "true" : "false", s->fallback ? "true" : "false", text,
        s->screen_w, s->screen_h, s->app_count, s->frames_received, s->bytes_received);
    for (i = 0; i < s->win_count && pos + 1200 < cap; i++) {
        SlHostWin *w = s->wins[i];
        RECT r = { 0, 0, 0, 0 };
        if (w->hwnd) GetWindowRect(w->hwnd, &r);
        json_text(text, sizeof(text), w->title);
        pos += (size_t)sprintf_s(buf + pos, cap - pos,
            "%s{\"guest\":%u,\"host\":%llu,\"title\":\"%s\",\"app\":%s,\"popup\":%s,\"state\":%u,"
            "\"guestRect\":[%d,%d,%d,%d],\"hostRect\":[%ld,%ld,%ld,%ld],\"border\":%d,\"picture\":[%d,%d],"
            "\"frames\":%u,\"bytes\":%llu}",
            i ? "," : "", w->guest, (unsigned long long)(UINT_PTR)w->hwnd, text,
            (w->info.flags & SL_WF_APP) ? "true" : "false",
            (w->info.flags & SL_WF_NOACTIVATE) ? "true" : "false", w->info.state,
            w->info.x, w->info.y, w->info.w, w->info.h, r.left, r.top, r.right, r.bottom, w->border,
            w->pw, w->ph, w->frames, w->bytes);
    }
    LeaveCriticalSection(&s->cs);
    LeaveCriticalSection(&g_sessions_cs);
    if (pos + 3 >= cap) return FALSE;
    buf[pos++] = ']';
    buf[pos++] = '}';
    buf[pos] = '\0';
    return TRUE;
}

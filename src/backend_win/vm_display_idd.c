/*
 * vm_display_idd.c -- Host-side IDD frame receiver + D3D11 renderer.
 *
 * Connects to the guest VM over AF_HYPERV sockets:
 *   :0002  Frame channel — receives frames, renders via D3D11 textured quad
 *   :0003  Input channel — forwards keyboard/mouse events to guest
 *   :0004  Audio channel — receives audio from guest, renders via WASAPI
 *
 * Clipboard sync (:0005/:0006) is handled by vm_clipboard.c.
 *
 * Pure C, compiled as C.
 */

#include <winsock2.h>
#include <windows.h>

#define COBJMACROS
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union in SDK headers */
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#pragma warning(pop)

#include <stdio.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "vm_clipboard.h"
#include "vm_agent.h"
#include "vm_backstage.h"
#include "hcs_vm.h"
#include "ui.h"
#include "resource.h"
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif


/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* These three are superseded by hcs_service_guid(os_type, port, ...) — kept
   for grep. Windows VMs reach byte-identical GUIDs via the helper; Linux
   VMs reach vsock-template GUIDs. */
static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Input channel service GUID — connects to agent for SendInput injection */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Audio capture channel — connects to guest audio helper (guest→host) */
static const GUID AUDIO_SERVICE_GUID =
    { 0xa5b0cafe, 0x0004, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Audio wire protocol (mirror of tools/agent/appsandbox-audio.c) ---- */

#define AUDIO_HEADER_MAGIC  0x31415341  /* 'ASA1' */

#pragma pack(push, 1)
typedef struct AudioHeader {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;      /* 1=PCM, 3=IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct AudioFrameHeader {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

/* Clipboard reader-apply message (posted by vm_clipboard.c to our wndproc) */
#define WM_CLIP_READER_APPLY (WM_APP + 11)

/* ---- Frame protocol constants ---- */

#define FRAME_MAGIC         0x52465341  /* "ASFR" little-endian */
#define FRAME_NV12_MAGIC    0x3231564E  /* "NV12": same header, full frame as NV12 */
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MAX_DIRTY_RECTS     64

/* The guest display takes the size of the window's client area, exactly. These
   are the only limits, and the driver applies the same ones (VDD_* in vdd.h).
   Below 640x480 Windows offers no mode, so a smaller window shows the picture
   scaled down. The upper limit is what a frame may cost: buffers per frame here
   and in the guest, and the link's throughput. A bigger window scales up. */
#define IDD_MIN_WIDTH       640
#define IDD_MIN_HEIGHT      480
#define IDD_MAX_SIDE        8192
#define IDD_MAX_PIXELS      (7680u * 4320u)
#define MAX_FRAME_DATA_SIZE (IDD_MAX_PIXELS * 4)

/* Host -> driver on the frame connection: please use this size (VDD_WIRE_RESIZE). */
#define RESIZE_MAGIC        0x5A525341  /* "ASRZ" little-endian */

/* ---- Input protocol (host → guest) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3
#define INPUT_MOUSE_REL     4   /* param1/param2: signed dx/dy (raw mouse counts) */
#define INPUT_MOUSE_HWHEEL  5

/* Button IDs for INPUT_MOUSE_BUTTON */
#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2
#define INPUT_BTN_X1        3
#define INPUT_BTN_X2        4

#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" little-endian */

#pragma pack(push, 1)
typedef struct InputPacket {
    UINT32 magic;   /* INPUT_MAGIC */
    UINT32 type;    /* INPUT_MOUSE_MOVE / BUTTON / WHEEL / KEY */
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;
#pragma pack(pop)

/* ---- Window messages ---- */

#define WM_VM_DISPLAY_CLOSED    (WM_APP + 5)
#define WM_IDD_FRAME_READY      (WM_USER + 100)
#define WM_IDD_FOCUS            (WM_USER + 101)
#define WM_IDD_CURSOR_VIS       (WM_USER + 102)   /* wp = guest cursor visible */
#define WM_IDD_RESIZE_CHECK     (WM_USER + 103)   /* the frame size or connection changed */

/* Timer for Present cadence when no frames arrive */
#define IDT_PRESENT     2001
#define PRESENT_MS      16   /* ~60 fps */
#define IDT_INPUT       2002
#define MOUSE_MOVE_MIN_INTERVAL_MS 8
/* A mode change makes the guest's monitor depart and arrive, so it waits for the
   window to settle: the end of a drag, or a pause after maximize and the like. */
#define IDT_RESIZE      2003
#define RESIZE_SETTLE_MS 300
#define RESIZE_POLL_MS   250
/* How long a requested size has to appear. Longer than the driver takes to
   go back to the last size that worked (VDD_ASSIGN_WAIT_MS and friends). */
#define RESIZE_WAIT_MS   15000

/* Debug log window */
#define IDC_LOG_LIST      3001
#define MAX_LOG_LINES     200
#define LOG_WINDOW_W      800
#define LOG_WINDOW_H      300

/* ---- HLSL shaders (inline strings) ---- */

static const char g_vs_hlsl[] =
    "struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    VS_OUT o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char g_ps_hlsl[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    return tex.Sample(samp, uv);\n"
    "}\n";

/* ---- Frame header from wire ---- */

#pragma pack(push, 1)
typedef struct FrameHeader {
    UINT32 magic;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT64 frame_seq;
    UINT32 dirty_rect_count;
} FrameHeader;

/* ---- Cursor header from wire (must match VDD_WIRE_CURSOR_HEADER) ---- */

#define CURSOR_MAGIC        0x52435341  /* "ASCR" little-endian */
#define MAX_CURSOR_SIZE     (256 * 256 * 4 * 2)  /* 2x for MASKED_COLOR double-height */

typedef struct CursorHeader {
    UINT32 magic;
    INT32  x;
    INT32  y;
    UINT32 visible;
    UINT32 shape_updated;
    UINT32 shape_id;
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 xhot;
    UINT32 yhot;
    UINT32 cursor_type;     /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32 shape_data_size;
} CursorHeader;
#pragma pack(pop)

/* ---- Display context ---- */

struct VmDisplayIdd {
    VmInstance  *vm;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    wchar_t      os_type[32];      /* copy of vm->os_type for picking the service GUID variant */
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    volatile BOOL open;
    volatile BOOL stop;

    /* Owned by vm_display_auto: open at init_pos, offer "Show Hyper-V Screen" */
    BOOL          switchable;
    BOOL          has_init_pos;
    POINT         init_pos;
    volatile LONG switch_requested;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    ID3D11ShaderResourceView *frame_srv;
    UINT                     tex_width;     /* size of frame_tex */
    UINT                     tex_height;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11SamplerState      *sampler;

    /* Frame buffer (CPU-side, updated by recv thread) */
    BYTE          *frame_buf;
    UINT           frame_width;
    UINT           frame_height;
    UINT           frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL  frame_dirty;

    UINT           render_count;     /* number of renders (for one-shot logging) */
    volatile UINT  recv_count;       /* number of frames received over HvSocket */

    /* Guest display following the window (window thread, except as noted) */
    volatile SOCKET frame_socket;    /* frame connection; closed under frame_cs */
    volatile BOOL  frame_seen;       /* a frame arrived on this connection (recv thread) */
    BOOL           can_resize;       /* a Windows guest whose driver answers requests */
    BOOL           resize_worked;    /* the driver reached a requested size */
    BOOL           resize_even_only; /* an odd size didn't appear, an even one might */
    BOOL           resize_gave_up;   /* until the window's size changes again */
    BOOL           in_sizemove;      /* the user is dragging the frame */
    UINT           want_width;       /* the window's client area, within the limits */
    UINT           want_height;
    UINT           asked_width;      /* the size last requested and not yet seen */
    UINT           asked_height;
    DWORD          asked_tick;

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */
    BOOL           pending_mouse;   /* latest move waiting for the rate-limit timer */
    UINT           pending_mouse_x;
    UINT           pending_mouse_y;
    DWORD          last_mouse_move_tick;
    BOOL           have_last_mouse_move_tick;

    /* Game mode (Parsec-style mouse capture). While captured the host cursor
       is hidden and clipped to the render area, and raw mouse deltas are sent
       as relative moves, which is what games reading raw input expect.
       Ctrl+Alt or losing activation releases it. */
    volatile BOOL  game_mode;        /* capture automatically when the guest hides its cursor */
    BOOL           captured;
    BOOL           capture_auto;     /* captured by game mode, so a visible cursor releases it */
    DWORD          title_tick;
    UINT           title_frames;
    UINT           fps;

    /* Keyboard hotkey handling */
    wchar_t        vhdx_path[MAX_PATH]; /* copy of vm->vhdx_path for settings file */
    volatile BOOL  transmit_hotkeys; /* TRUE = capture host hotkeys + send to guest */
    volatile BOOL  input_focused;    /* TRUE while our top-level window is active */
    HHOOK          kbd_hook;          /* WH_KEYBOARD_LL handle, NULL when not installed */
    /* Held-key tracking (window-thread only, no lock). Indexed by virtual key.
       held_down[vk]=1 means we forwarded a down with no matching up yet; the
       saved scan/ext let us synthesize an accurate up when flushing. */
    BYTE           held_down[256];
    BYTE           held_scan[256];
    BYTE           held_ext[256];

    /* Guest cursor */
    HCURSOR        guest_cursor;    /* current cursor created from guest bitmap */
    UINT32         cursor_shape_id; /* tracks which shape is current */
    BOOL           cursor_visible;  /* guest cursor visibility */

    /* Debug log window (separate top-level window) */
    HWND           log_hwnd;        /* top-level log window */
    HWND           log_list_hwnd;   /* listbox inside log window */
    HWND           render_hwnd;     /* child window for D3D11 rendering */

    /* Clipboard (extracted to vm_clipboard.c) */
    VmClipboard      clipboard;

    /* Audio playback channel (:0004 — guest→host render) */
    volatile SOCKET  audio_socket;
    HANDLE           audio_recv_thread;
    volatile BOOL    audio_muted;

    /* Floating menu (Parsec-style): a small tab on the top edge of the
       picture that opens a panel with the display's options. Both are owned,
       non-activating popups, so clicking them never takes focus from the VM. */
    HWND           fab_hwnd;
    HWND           panel_hwnd;
    double         fab_pos;          /* tab position across the top, 0..1 */
    BOOL           fab_shown;
    BOOL           fab_hot;
    BOOL           fab_dragging;
    BOOL           fab_moved;
    int            fab_drag_x;       /* screen x where the drag started */
    int            fab_drag_left;    /* tab's left edge then */
    int            panel_hot;        /* hovered row, -1 for none */

    /* Threads */
    HANDLE         recv_thread;
    HANDLE         window_thread;
};

/* ---- Forward declarations ---- */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static DWORD WINAPI     idd_window_thread_proc(LPVOID param);
static DWORD WINAPI     idd_recv_thread_proc(LPVOID param);

/* ---- Window class ---- */

static const wchar_t *IDD_DISPLAY_CLASS = L"AppSandboxIddDisplay";
static const wchar_t *IDD_RENDER_CLASS  = L"AppSandboxIddRender";
static const wchar_t *IDD_LOG_CLASS     = L"AppSandboxIddLog";

/* System menu command IDs — must be < 0xF000 and have low 4 bits clear */
#define IDM_AUDIO_MUTE     0x1000
#define IDM_XMIT_HOTKEYS   0x1010
#define IDM_SHOW_LOG       0x1020
#define IDM_SHOW_CONSOLE   0x1030
#define IDM_GAME_MODE      0x1040
#define IDM_CAPTURE_MOUSE  0x1050
#define IDM_BACKSTAGE      0x1060
static BOOL g_idd_class_registered;

static WNDPROC g_orig_listbox_proc;

/* Listbox subclass — handles Ctrl+A / Ctrl+C */
static LRESULT CALLBACK idd_log_listbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (wp == 'C') {
            LRESULT sel_count = SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
            if (sel_count > 0) {
                int *indices = (int *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sel_count * sizeof(int));
                if (indices) {
                    LRESULT i;
                    SIZE_T total = 0;
                    wchar_t *text;
                    SendMessageW(hwnd, LB_GETSELITEMS, (WPARAM)sel_count, (LPARAM)indices);
                    for (i = 0; i < sel_count; i++)
                        total += (SIZE_T)SendMessageW(hwnd, LB_GETTEXTLEN, (WPARAM)indices[i], 0) + 2;
                    total++;
                    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(wchar_t));
                    if (text) {
                        wchar_t *p = text;
                        for (i = 0; i < sel_count; i++) {
                            LRESULT len = SendMessageW(hwnd, LB_GETTEXT, (WPARAM)indices[i], (LPARAM)p);
                            p += len;
                            *p++ = L'\r'; *p++ = L'\n';
                        }
                        if (OpenClipboard(hwnd)) {
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                            if (hMem) {
                                void *dst = GlobalLock(hMem);
                                if (dst) {
                                    memcpy(dst, text, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                                    GlobalUnlock(hMem);
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                } else {
                                    GlobalFree(hMem);
                                }
                            }
                            CloseClipboard();
                        }
                        HeapFree(GetProcessHeap(), 0, text);
                    }
                    HeapFree(GetProcessHeap(), 0, indices);
                }
            }
            return 0;
        }
        if (wp == 'A') {
            LRESULT cnt = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            SendMessageW(hwnd, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, (WORD)(cnt - 1)));
            return 0;
        }
    }
    return CallWindowProcW(g_orig_listbox_proc, hwnd, msg, wp, lp);
}

/* Log window proc — resizes listbox child to fill the window */
static LRESULT CALLBACK idd_log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH s_dark_brush = NULL;

    switch (msg) {
    case WM_SIZE: {
        HWND list = GetDlgItem(hwnd, IDC_LOG_LIST);
        if (list) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(list, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* Match AppSandbox #log-panel: --ctrl-bg #2d2d2d, text #ccc */
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(204, 204, 204));
        SetBkColor(hdc, RGB(45, 45, 45));
        if (!s_dark_brush) s_dark_brush = CreateSolidBrush(RGB(45, 45, 45));
        return (LRESULT)s_dark_brush;
    }
    case WM_CLOSE:
        /* Just hide — the main IDD window owns the lifecycle */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Render child window proc — forwards input + paint to parent for handling */
static LRESULT CALLBACK idd_render_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        /* Validate this window's update region; repaint once for expose/resize
           (rendering is otherwise push-driven by received frames). */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        SendMessageW(GetParent(hwnd), WM_IDD_FRAME_READY, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_MOUSELEAVE:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SETCURSOR:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_idd_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    if (g_idd_class_registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_wnd_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_DISPLAY_CLASS;
    RegisterClassExW(&wc);

    /* Render child — D3D11 swap chain targets this window.
       hCursor=NULL so WM_SETCURSOR can set the guest cursor. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_render_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_RENDER_CLASS;
    RegisterClassExW(&wc);

    /* Separate log window — dark background to match AppSandbox main window */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = idd_log_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = IDD_LOG_CLASS;
    RegisterClassExW(&wc);

    g_idd_class_registered = TRUE;
}

/* ---- Floating menu (Parsec-style tab + panel) ---- */

#define IDD_FAB_CLASS    L"AppSandboxIddFab"
#define IDD_PANEL_CLASS  L"AppSandboxIddPanel"
#define FAB_W            64
#define FAB_H            20
#define PANEL_W          300
#define PANEL_HEADER     40
#define PANEL_ROW        36
#define PANEL_MAX_ROWS   8

#define FAB_BG           RGB(0x1b, 0x22, 0x2c)
#define FAB_BG_HOT       RGB(0x2a, 0x36, 0x47)
#define FAB_BORDER       RGB(0x3b, 0x4a, 0x5e)
#define FAB_TEXT         RGB(0xe6, 0xed, 0xf5)
#define FAB_DIM          RGB(0x8d, 0xa2, 0xb8)
#define FAB_ON           RGB(0x5c, 0xc8, 0x8a)

typedef struct {
    UINT cmd;
    const wchar_t *label;
    const wchar_t *state;
    BOOL on;
} IddMenuRow;

static HFONT g_fab_font, g_fab_font_bold;

static void idd_panel_close(VmDisplayIdd *d);

/* What the panel offers: the same commands as the title-bar menu, which
   the panel sends as WM_SYSCOMMAND so both share one handler. */
static int idd_menu_rows(VmDisplayIdd *d, IddMenuRow *rows)
{
    int n = 0;
    rows[n].cmd = IDM_CAPTURE_MOUSE; rows[n].label = L"Capture mouse now";
    rows[n].state = L"Ctrl+Alt releases"; rows[n].on = FALSE; n++;
    rows[n].cmd = IDM_GAME_MODE; rows[n].label = L"Game mode";
    rows[n].state = d->game_mode ? L"On" : L"Off"; rows[n].on = d->game_mode; n++;
    rows[n].cmd = IDM_XMIT_HOTKEYS; rows[n].label = L"Send hotkeys to the VM";
    rows[n].state = d->transmit_hotkeys ? L"On" : L"Off"; rows[n].on = d->transmit_hotkeys; n++;
    rows[n].cmd = IDM_AUDIO_MUTE; rows[n].label = L"Mute audio";
    rows[n].state = d->audio_muted ? L"On" : L"Off"; rows[n].on = d->audio_muted; n++;
    if (_wcsicmp(d->os_type, L"Windows") == 0) {
        rows[n].cmd = IDM_BACKSTAGE; rows[n].label = L"Backstage session (SYSTEM)";
        rows[n].state = L"Hidden session";
        rows[n].on = FALSE; n++;
    }
    if (d->switchable) {
        rows[n].cmd = IDM_SHOW_CONSOLE; rows[n].label = L"Show Hyper-V screen";
        rows[n].state = L"Boot console"; rows[n].on = FALSE; n++;
    }
    rows[n].cmd = IDM_SHOW_LOG; rows[n].label = L"Show log";
    rows[n].state = L""; rows[n].on = FALSE; n++;
    return n;
}

static void idd_fab_fonts(void)
{
    if (g_fab_font) return;
    g_fab_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fab_font_bold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

/* The client area in screen coordinates. */
static RECT idd_client_screen(VmDisplayIdd *d)
{
    RECT rc;
    POINT origin = { 0, 0 };
    GetClientRect(d->hwnd, &rc);
    ClientToScreen(d->hwnd, &origin);
    OffsetRect(&rc, origin.x, origin.y);
    return rc;
}

/* Put the tab on the top edge at its saved position, or hide it while the
   mouse is captured (game mode) or the window is minimised. */
static void idd_fab_place(VmDisplayIdd *d)
{
    RECT rc;
    int x, span;
    BOOL want;
    if (!d || !d->fab_hwnd) return;
    want = !d->captured && IsWindowVisible(d->hwnd) && !IsIconic(d->hwnd);
    if (!want) {
        if (d->fab_shown) ShowWindow(d->fab_hwnd, SW_HIDE);
        d->fab_shown = FALSE;
        idd_panel_close(d);
        return;
    }
    rc = idd_client_screen(d);
    span = (rc.right - rc.left) - FAB_W;
    if (span < 0) span = 0;
    x = rc.left + (int)(d->fab_pos * span + 0.5);
    SetWindowPos(d->fab_hwnd, NULL, x, rc.top, FAB_W, FAB_H,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    d->fab_shown = TRUE;
    if (d->panel_hwnd) {
        RECT pr;
        int px;
        GetWindowRect(d->panel_hwnd, &pr);
        px = x + FAB_W / 2 - PANEL_W / 2;
        if (px + PANEL_W > rc.right) px = rc.right - PANEL_W;
        if (px < rc.left) px = rc.left;
        SetWindowPos(d->panel_hwnd, NULL, px, rc.top + FAB_H + 4, 0, 0,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    }
}

static void idd_panel_close(VmDisplayIdd *d)
{
    HWND panel;
    if (!d || !d->panel_hwnd) return;
    panel = d->panel_hwnd;
    d->panel_hwnd = NULL;     /* first, so losing capture doesn't come back here */
    if (GetCapture() == panel) ReleaseCapture();
    DestroyWindow(panel);
    if (d->fab_hwnd) InvalidateRect(d->fab_hwnd, NULL, FALSE);
}

static void idd_panel_open(VmDisplayIdd *d)
{
    IddMenuRow rows[PANEL_MAX_ROWS];
    int n = idd_menu_rows(d, rows);
    int h = PANEL_HEADER + n * PANEL_ROW + 8;
    HRGN rgn;
    d->panel_hot = -1;
    d->panel_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, IDD_PANEL_CLASS, L"",
                                    WS_POPUP, 0, 0, PANEL_W, h, d->hwnd, NULL,
                                    d->hInstance, d);
    if (!d->panel_hwnd) return;
    rgn = CreateRoundRectRgn(0, 0, PANEL_W + 1, h + 1, 12, 12);
    SetWindowRgn(d->panel_hwnd, rgn, FALSE);
    idd_fab_place(d);
    ShowWindow(d->panel_hwnd, SW_SHOWNOACTIVATE);
    SetCapture(d->panel_hwnd);    /* a click anywhere else closes it */
    InvalidateRect(d->fab_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK idd_fab_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (!d) break;
        if (!d->fab_hot) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            d->fab_hot = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (d->fab_dragging) {
            /* Drag along the top edge, like Parsec's tab. */
            POINT pt;
            RECT rc = idd_client_screen(d);
            int span = (rc.right - rc.left) - FAB_W, left;
            GetCursorPos(&pt);
            if (abs(pt.x - d->fab_drag_x) > 3) d->fab_moved = TRUE;
            if (d->fab_moved && span > 0) {
                left = d->fab_drag_left + (pt.x - d->fab_drag_x) - rc.left;
                if (left < 0) left = 0;
                if (left > span) left = span;
                d->fab_pos = (double)left / span;
                idd_fab_place(d);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d) {
            d->fab_hot = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (d) {
            POINT pt;
            RECT wr;
            GetCursorPos(&pt);
            GetWindowRect(hwnd, &wr);
            d->fab_dragging = TRUE;
            d->fab_moved = FALSE;
            d->fab_drag_x = pt.x;
            d->fab_drag_left = wr.left;
            SetCapture(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (d && d->fab_dragging) {
            d->fab_dragging = FALSE;
            ReleaseCapture();
            if (!d->fab_moved) idd_panel_open(d);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (d) d->fab_dragging = FALSE;
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        HBRUSH bg;
        HPEN pen;
        int i, cx;
        GetClientRect(hwnd, &rc);
        bg = CreateSolidBrush(d && (d->fab_hot || d->panel_hwnd) ? FAB_BG_HOT : FAB_BG);
        pen = CreatePen(PS_SOLID, 1, FAB_BORDER);
        SelectObject(dc, bg);
        SelectObject(dc, pen);
        RoundRect(dc, rc.left, rc.top - 8, rc.right, rc.bottom, 10, 10);
        DeleteObject(bg);
        DeleteObject(pen);
        /* three dots */
        bg = CreateSolidBrush(FAB_TEXT);
        cx = rc.right / 2;
        for (i = -1; i <= 1; i++) {
            RECT dot = { cx + i * 8 - 2, rc.bottom / 2 - 2, cx + i * 8 + 2, rc.bottom / 2 + 2 };
            FillRect(dc, &dot, bg);
        }
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int idd_panel_row_at(int y, int rows)
{
    int r;
    if (y < PANEL_HEADER) return -1;
    r = (y - PANEL_HEADER) / PANEL_ROW;
    return r < rows ? r : -1;
}

static LRESULT CALLBACK idd_panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    IddMenuRow rows[PANEL_MAX_ROWS];
    int n;
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (d) {
            int hot;
            n = idd_menu_rows(d, rows);
            hot = idd_panel_row_at((short)HIWORD(lp), n);
            if ((short)LOWORD(lp) < 0 || (short)LOWORD(lp) >= PANEL_W) hot = -1;
            if (hot != d->panel_hot) {
                d->panel_hot = hot;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    {
        RECT rc;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        GetClientRect(hwnd, &rc);
        if (d && !PtInRect(&rc, pt)) idd_panel_close(d);   /* clicked elsewhere */
        return 0;
    }
    case WM_LBUTTONUP:
        if (d) {
            int row;
            UINT cmd;
            n = idd_menu_rows(d, rows);
            row = idd_panel_row_at((short)HIWORD(lp), n);
            if ((short)LOWORD(lp) < 0 || (short)LOWORD(lp) >= PANEL_W || row < 0) return 0;
            cmd = rows[row].cmd;
            idd_panel_close(d);
            SetForegroundWindow(d->hwnd);
            PostMessageW(d->hwnd, WM_SYSCOMMAND, cmd, 0);
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (d) idd_panel_close(d);
        return 0;
    case WM_CAPTURECHANGED:
        if (d && d->panel_hwnd == hwnd) idd_panel_close(d);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, r;
        HBRUSH brush;
        HPEN pen;
        int i;
        GetClientRect(hwnd, &rc);
        brush = CreateSolidBrush(FAB_BG);
        pen = CreatePen(PS_SOLID, 1, FAB_BORDER);
        SelectObject(dc, brush);
        SelectObject(dc, pen);
        RoundRect(dc, 0, 0, rc.right, rc.bottom, 12, 12);
        DeleteObject(brush);
        DeleteObject(pen);
        if (!d) { EndPaint(hwnd, &ps); return 0; }
        SetBkMode(dc, TRANSPARENT);
        r = rc; r.left = 16; r.right -= 16; r.bottom = PANEL_HEADER;
        SelectObject(dc, g_fab_font_bold);
        SetTextColor(dc, FAB_TEXT);
        DrawTextW(dc, d->vm_name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        n = idd_menu_rows(d, rows);
        for (i = 0; i < n; i++) {
            RECT row = { 6, PANEL_HEADER + i * PANEL_ROW, rc.right - 6, PANEL_HEADER + (i + 1) * PANEL_ROW };
            if (i == d->panel_hot) {
                brush = CreateSolidBrush(FAB_BG_HOT);
                FillRect(dc, &row, brush);
                DeleteObject(brush);
            }
            r = row; r.left += 10; r.right -= 10;
            SelectObject(dc, g_fab_font);
            SetTextColor(dc, FAB_TEXT);
            DrawTextW(dc, rows[i].label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
            SetTextColor(dc, rows[i].on ? FAB_ON : FAB_DIM);
            DrawTextW(dc, rows[i].state, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void idd_fab_create(VmDisplayIdd *d)
{
    static BOOL registered;
    HRGN rgn;
    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.hInstance = d->hInstance;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpfnWndProc = idd_fab_proc;
        wc.lpszClassName = IDD_FAB_CLASS;
        RegisterClassExW(&wc);
        wc.lpfnWndProc = idd_panel_proc;
        wc.lpszClassName = IDD_PANEL_CLASS;
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        registered = TRUE;
    }
    idd_fab_fonts();
    d->fab_pos = 0.5;
    d->fab_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                  IDD_FAB_CLASS, L"", WS_POPUP, 0, 0, FAB_W, FAB_H,
                                  d->hwnd, NULL, d->hInstance, d);
    if (!d->fab_hwnd) return;
    SetLayeredWindowAttributes(d->fab_hwnd, 0, 235, LWA_ALPHA);
    rgn = CreateRoundRectRgn(0, -10, FAB_W + 1, FAB_H + 1, 10, 10);
    SetWindowRgn(d->fab_hwnd, rgn, FALSE);
    idd_fab_place(d);
}



/* ---- Debug log panel ---- */

static void idd_log(VmDisplayIdd *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    LRESULT count;

    if (!d || !d->log_list_hwnd || d->stop) return;

    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);

    count = SendMessageW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf);

    /* Trim old entries */
    while (count > MAX_LOG_LINES) {
        SendMessageW(d->log_list_hwnd, LB_DELETESTRING, 0, 0);
        count--;
    }

    /* Scroll to bottom and force repaint even when not focused */
    SendMessageW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
    UpdateWindow(d->log_list_hwnd);
}

/* ---- Send input packet to guest ---- */

static UINT g_input_send_count = 0;

static void send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    SOCKET s;
    int ret;

    s = d->input_socket;
    if (s == INVALID_SOCKET) return;

    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    /* Non-blocking send — drop packet if buffer full rather than stall the UI */
    ret = send(s, (const char *)&pkt, (int)sizeof(pkt), 0);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            /* Send buffer full: drop this packet (as the comment intends)
               without tearing down the socket. */
            return;
        }
        idd_log(d, L"INPUT SEND ERR %d - flagging for reconnect.", err);
        /* Mark dead — recv thread owns the socket and will close + reconnect */
        d->input_socket = INVALID_SOCKET;
        return;
    }
    if (ret != (int)sizeof(pkt)) {
        /* Partial send on a stream socket: the guest reads fixed-size 20-byte
           InputPackets, so a truncated packet permanently misaligns the wire.
           Flag for reconnect so the channel resynchronises. */
        idd_log(d, L"INPUT SEND short (%d/%d) - flagging for reconnect.",
                ret, (int)sizeof(pkt));
        d->input_socket = INVALID_SOCKET;
        return;
    }

    g_input_send_count++;

    /* Log non-move events only (moves are too noisy) */
    if (type != INPUT_MOUSE_MOVE && type != INPUT_MOUSE_REL) {
        static const wchar_t *type_names[] = {
            L"MOUSE_MOVE", L"MOUSE_BTN", L"MOUSE_WHEEL", L"KEY", L"MOUSE_REL", L"MOUSE_HWHEEL"
        };
        const wchar_t *name = type < 6 ? type_names[type] : L"?";
        idd_log(d, L"INPUT %s p1=%u p2=%u p3=%u (#%u)", name, p1, p2, p3, g_input_send_count);
    }
}

/* Mouse messages can arrive much faster than the guest display refreshes.
   Keep only the latest position and send it at a bounded rate so the window
   thread and the guest input helper cannot be flooded by cursor movement. */
static void flush_pending_mouse(VmDisplayIdd *d)
{
    DWORD now;

    if (!d || !d->pending_mouse || !d->mouse_in)
        return;
    now = GetTickCount();
    if (d->have_last_mouse_move_tick &&
        (DWORD)(now - d->last_mouse_move_tick) < MOUSE_MOVE_MIN_INTERVAL_MS)
        return;
    send_input(d, INPUT_MOUSE_MOVE, d->pending_mouse_x, d->pending_mouse_y, 0);
    d->pending_mouse = FALSE;
    d->last_mouse_move_tick = now;
    d->have_last_mouse_move_tick = TRUE;
    KillTimer(d->hwnd, IDT_INPUT);
}

/* ==================================================================
 * Per-VM display settings (display_settings.json beside disk.vhdx)
 *
 * Mirrors the vm_state.json pattern in asb_core.c but is owned entirely
 * by the IDD display: the file is created lazily the first time a VM's
 * display opens, so both new and pre-existing VMs get one on demand.
 * ================================================================== */

static void idd_display_settings_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_chars)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_chars, L"%s\\display_settings.json", dir);
}

static void idd_display_settings_save(const wchar_t *vhdx_path, BOOL transmit_hotkeys,
                                      BOOL game_mode)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"transmitKeyboardHotkeys\":%d,\"gameMode\":%d}\n",
            transmit_hotkeys ? 1 : 0, game_mode ? 1 : 0);
    fclose(f);
}

/* Read the persisted settings; if the file is absent, create it with the
   defaults (hotkeys off, game mode on). Returns the transmit-hotkeys value;
   game mode is on unless the file turns it off. */
static BOOL idd_display_settings_load_or_create(const wchar_t *vhdx_path, BOOL *game_mode)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    BOOL transmit = FALSE;

    *game_mode = TRUE;
    if (!vhdx_path || vhdx_path[0] == L'\0') return FALSE;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);

    if (_wfopen_s(&f, path, L"r") != 0 || !f) {
        /* Lazy creation: file doesn't exist yet (new or pre-existing VM). */
        idd_display_settings_save(vhdx_path, FALSE, TRUE);
        return FALSE;
    }
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"transmitKeyboardHotkeys\":1"))
            transmit = TRUE;
        if (strstr(buf, "\"gameMode\":0"))
            *game_mode = FALSE;
    }
    fclose(f);
    return transmit;
}

/* ==================================================================
 * Keyboard hotkey capture
 * ================================================================== */

/* Maximal reserved-hotkey set: keys the host shell would normally consume.
   In Default mode these are withheld from the guest (host handles them); in
   Transmit mode they are captured and forwarded to the guest instead.
   alt_down must reflect whether Alt is currently held (LLKHF_ALTDOWN from the
   low-level hook, or GetKeyState(VK_MENU) from the wndproc path).
   Note: Ctrl+Alt+Del and Win+L are secure (SAS) sequences that no user-mode
   hook can intercept — they always reach the host. */
static BOOL idd_is_reserved_hotkey(DWORD vk, BOOL alt_down)
{
    switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SNAPSHOT:   /* PrintScreen */
    case VK_APPS:       /* Menu / context key */
        return TRUE;
    case VK_TAB:
        return alt_down;                                  /* Alt+Tab */
    case VK_ESCAPE:
        /* Alt+Esc / Ctrl+Esc. GetAsyncKeyState reads real-time physical state,
           which is reliable both in the wndproc and inside the low-level hook
           (where the synchronized GetKeyState value may not be updated yet). */
        return alt_down || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    default:
        return FALSE;
    }
}

/* Forward a key event to the guest and track held state so a later focus
   change can release anything still down. Runs on the window thread only. */
static void idd_forward_key(VmDisplayIdd *d, DWORD vk, DWORD scan, BOOL ext, BOOL up)
{
    UINT32 flags = 0;
    if (ext) flags |= 1;
    if (up)  flags |= 2;
    send_input(d, INPUT_KEY, vk, scan, flags);

    if (vk < 256) {
        if (up) {
            d->held_down[vk] = 0;
        } else {
            d->held_down[vk] = 1;
            d->held_scan[vk] = (BYTE)scan;
            d->held_ext[vk]  = (BYTE)(ext ? 1 : 0);
        }
    }
}

/* Send key-up for every key we believe is still held in the guest, then
   clear tracking. Called when our window loses activation, when Transmit
   mode is turned off, and on teardown — this is the core stuck-key fix. */
static void idd_flush_held_keys(VmDisplayIdd *d)
{
    int vk;
    for (vk = 0; vk < 256; vk++) {
        if (d->held_down[vk]) {
            UINT32 flags = 2 | (d->held_ext[vk] ? 1 : 0);
            send_input(d, INPUT_KEY, (UINT32)vk, d->held_scan[vk], flags);
            d->held_down[vk] = 0;
        }
    }
}

/* Thread-local owner: a WH_KEYBOARD_LL callback runs on the thread that
   installed it (our window thread), so this resolves each display's context
   without a global registry, keeping multiple displays independent. */
static __declspec(thread) VmDisplayIdd *t_hook_display;

static LRESULT CALLBACK idd_ll_keyboard_proc(int code, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = t_hook_display;

    if (code == HC_ACTION && d && !d->stop &&
        d->input_focused && (d->transmit_hotkeys || d->captured)) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL up  = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        BOOL alt = (k->flags & LLKHF_ALTDOWN) != 0;
        if (idd_is_reserved_hotkey(k->vkCode, alt)) {
            idd_forward_key(d, k->vkCode, k->scanCode,
                            (k->flags & LLKHF_EXTENDED) != 0, up);
            return 1;  /* swallow so the host shell doesn't act on it */
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void idd_install_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) return;
    t_hook_display = d;
    d->kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, idd_ll_keyboard_proc,
                                     d->hInstance, 0);
    if (!d->kbd_hook)
        idd_log(d, L"Hotkey hook install failed (err %lu).", GetLastError());
    else
        idd_log(d, L"Hotkey capture enabled.");
}

static void idd_remove_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) {
        UnhookWindowsHookEx(d->kbd_hook);
        d->kbd_hook = NULL;
        idd_log(d, L"Hotkey capture disabled.");
    }
    t_hook_display = NULL;
}

/* ==================================================================
 * Game mode: mouse capture with relative movement
 * ================================================================== */

static void idd_update_title(VmDisplayIdd *d)
{
    wchar_t title[320];
    if (!d || !d->hwnd) return;
    swprintf_s(title, _countof(title), L"%s%s Display %ux%u - %u fps%s",
               d->audio_muted ? L"\U0001F507 " : L"",
               d->vm_name, d->frame_width, d->frame_height, d->fps,
               d->captured ? L" - Mouse captured (Ctrl+Alt releases)" : L"");
    SetWindowTextW(d->hwnd, title);
}

/* Keep the (hidden) cursor inside the render area so clicks land on it. */
static void idd_clip_cursor(VmDisplayIdd *d)
{
    RECT rc;
    if (!d->captured || !d->render_hwnd) return;
    GetClientRect(d->render_hwnd, &rc);
    MapWindowPoints(d->render_hwnd, NULL, (POINT *)&rc, 2);
    ClipCursor(&rc);
}

static void idd_begin_capture(VmDisplayIdd *d, BOOL automatic)
{
    RAWINPUTDEVICE rid;
    if (!d || d->captured || !d->hwnd || !d->render_hwnd || !d->input_focused) return;
    rid.usUsagePage = 0x01;   /* generic desktop */
    rid.usUsage     = 0x02;   /* mouse */
    rid.dwFlags     = 0;
    rid.hwndTarget  = d->hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        idd_log(d, L"Mouse capture: raw input registration failed (%lu).", GetLastError());
        return;
    }
    d->captured = TRUE;
    d->capture_auto = automatic;
    d->pending_mouse = FALSE;
    KillTimer(d->hwnd, IDT_INPUT);
    idd_clip_cursor(d);
    SetCursor(NULL);
    /* Win, Alt+Tab and friends go to the game while captured. */
    if (!d->transmit_hotkeys) idd_install_kbd_hook(d);
    idd_log(d, automatic ? L"Mouse captured (game hid the cursor). Ctrl+Alt releases."
                         : L"Mouse captured. Ctrl+Alt releases.");
    idd_update_title(d);
}

static void idd_end_capture(VmDisplayIdd *d)
{
    RAWINPUTDEVICE rid;
    if (!d || !d->captured) return;
    d->captured = FALSE;
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_REMOVE;
    rid.hwndTarget  = NULL;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ClipCursor(NULL);
    if (!d->transmit_hotkeys) idd_remove_kbd_hook(d);
    idd_log(d, L"Mouse released.");
    idd_update_title(d);
}

/* Compute letterboxed/pillarboxed viewport within client rect */
static void compute_letterbox(UINT client_w, UINT client_h,
                              UINT frame_w, UINT frame_h,
                              float *out_x, float *out_y,
                              float *out_w, float *out_h)
{
    float scale_x, scale_y, scale;
    if (client_w == 0 || client_h == 0 || frame_w == 0 || frame_h == 0) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return;
    }
    scale_x = (float)client_w / (float)frame_w;
    scale_y = (float)client_h / (float)frame_h;
    scale = scale_x < scale_y ? scale_x : scale_y;
    *out_w = (float)frame_w * scale;
    *out_h = (float)frame_h * scale;
    *out_x = ((float)client_w - *out_w) * 0.5f;
    *out_y = ((float)client_h - *out_h) * 0.5f;
}

/* Map window client coordinates to VM framebuffer coordinates */
static void window_to_vm_coords(HWND hwnd, int wx, int wy,
                                 UINT vm_w, UINT vm_h,
                                 UINT *vx, UINT *vy)
{
    RECT rc;
    float vp_x, vp_y, vp_w, vp_h;
    float local_x, local_y;

    GetClientRect(hwnd, &rc);
    compute_letterbox((UINT)rc.right, (UINT)rc.bottom, vm_w, vm_h,
                      &vp_x, &vp_y, &vp_w, &vp_h);

    if (vp_w <= 0 || vp_h <= 0) {
        *vx = 0;
        *vy = 0;
        return;
    }

    local_x = ((float)wx - vp_x) / vp_w * (float)vm_w;
    local_y = ((float)wy - vp_y) / vp_h * (float)vm_h;

    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    *vx = (UINT)local_x;
    *vy = (UINT)local_y;
    if (*vx >= vm_w) *vx = vm_w - 1;
    if (*vy >= vm_h) *vy = vm_h - 1;
}

/* NV12 (Y plane, then interleaved UV; full-range BT.601) -> BGRA. The
   inverse of VddPackNv12 in the guest driver. The planes have even sizes, so
   an odd width or height carries one column or row of padding, dropped here. */
#define IDD_ALIGN2(n) (((n) + 1) & ~1u)

static void idd_nv12_to_bgra(const BYTE *src, UINT w, UINT h, BYTE *dst, UINT dst_stride)
{
    const UINT aw = IDD_ALIGN2(w), ah = IDD_ALIGN2(h);
    const BYTE *uv_plane = src + (size_t)aw * ah;
    UINT x, y;
    for (y = 0; y < h; y++) {
        const BYTE *yrow = src + (size_t)y * aw;
        const BYTE *uv = uv_plane + (size_t)(y / 2) * aw;
        BYTE *out = dst + (size_t)y * dst_stride;
        for (x = 0; x < w; x += 2) {
            int u = uv[x] - 128, v = uv[x + 1] - 128;
            int dr = 359 * v, dg = -88 * u - 183 * v, db = 454 * u;
            int k;
            for (k = 0; k < 2 && x + k < w; k++) {
                int l = yrow[x + k] << 8;
                int r = (l + dr + 128) >> 8, g = (l + dg + 128) >> 8, b = (l + db + 128) >> 8;
                out[0] = (BYTE)(b < 0 ? 0 : b > 255 ? 255 : b);
                out[1] = (BYTE)(g < 0 ? 0 : g > 255 ? 255 : g);
                out[2] = (BYTE)(r < 0 ? 0 : r > 255 ? 255 : r);
                out[3] = 255;
                out += 4;
            }
        }
    }
}

/* ---- Malware lab screenshots ----
   While the VM records an analysis run, the newest frame is saved as a JPEG
   at most every 2 s and only when the screen changed, named by the time
   into the run (screenshots\shot_<ms>.jpg) for the report's timeline. */
static BOOL idd_save_jpeg(const wchar_t *path, const BYTE *bgra, UINT w, UINT h, UINT stride)
{
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    BYTE *bgr = (BYTE *)malloc((size_t)w * 3 * h);
    BOOL ok = FALSE;
    UINT x, y;
    if (!bgr) return FALSE;
    for (y = 0; y < h; y++) {
        const BYTE *s = bgra + (size_t)y * stride;
        BYTE *d = bgr + (size_t)y * w * 3;
        for (x = 0; x < w; x++, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    }
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IWICImagingFactory, (void **)&factory)) &&
        SUCCEEDED(factory->lpVtbl->CreateStream(factory, &stream)) &&
        SUCCEEDED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatJpeg, NULL, &encoder)) &&
        SUCCEEDED(encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &props))) {
        PROPBAG2 option;
        VARIANT value;
        ZeroMemory(&option, sizeof(option));
        option.pstrName = (LPOLESTR)L"ImageQuality";
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.8f;
        props->lpVtbl->Write(props, 1, &option, &value);
        ok = SUCCEEDED(frame->lpVtbl->Initialize(frame, props)) &&
             SUCCEEDED(frame->lpVtbl->SetSize(frame, w, h)) &&
             SUCCEEDED(frame->lpVtbl->SetPixelFormat(frame, &format)) &&
             IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(frame->lpVtbl->WritePixels(frame, h, w * 3, w * 3 * h, bgr)) &&
             SUCCEEDED(frame->lpVtbl->Commit(frame)) &&
             SUCCEEDED(encoder->lpVtbl->Commit(encoder));
    }
    if (props) props->lpVtbl->Release(props);
    if (frame) frame->lpVtbl->Release(frame);
    if (encoder) encoder->lpVtbl->Release(encoder);
    if (stream) stream->lpVtbl->Release(stream);
    if (factory) factory->lpVtbl->Release(factory);
    free(bgr);
    return ok;
}

static void idd_lab_screenshot(VmDisplayIdd *d, ULONGLONG *last_tick, UINT64 *last_hash)
{
    ULONGLONG now = GetTickCount64();
    BYTE *copy;
    UINT w, h, stride, y;
    UINT64 hash = 14695981039346656037ull;
    wchar_t path[MAX_PATH];
    VmInstance *vm = d->vm;
    if (!vm || !vm->lab_running || now - *last_tick < 2000) return;
    *last_tick = now;
    EnterCriticalSection(&d->frame_cs);
    w = d->frame_width; h = d->frame_height; stride = d->frame_stride;
    copy = (d->frame_buf && w && h) ? (BYTE *)malloc((size_t)stride * h) : NULL;
    if (copy) memcpy(copy, d->frame_buf, (size_t)stride * h);
    LeaveCriticalSection(&d->frame_cs);
    if (!copy) return;
    for (y = 0; y < h; y += 7) {       /* sampled rows: cheap change check */
        const BYTE *row = copy + (size_t)y * stride;
        UINT x;
        for (x = 0; x < stride; x += 64) { hash ^= row[x]; hash *= 1099511628211ull; }
    }
    if (hash != *last_hash) {
        *last_hash = hash;
        swprintf_s(path, MAX_PATH, L"%s\\screenshots\\shot_%07llu.jpg", vm->lab_dir, now - vm->lab_t0);
        if (idd_save_jpeg(path, copy, w, h, stride))
            InterlockedIncrement(&vm->lab_shots);
    }
    free(copy);
}

/* ---- Reliable recv: read exactly `len` bytes ---- */

static BOOL recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_hv_service(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family   = AF_HYPERV;
    addr.VmId     = *vm_runtime_id;
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with recv timeout */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ==================================================================
 * Audio playback — guest -> host
 * ================================================================== */

static const CLSID AUDIO_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID AUDIO_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID AUDIO_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID AUDIO_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

static DWORD WINAPI audio_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    HRESULT hr;
    BOOL com_ok = FALSE;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pAC = NULL;
    IAudioRenderClient *pRC = NULL;
    WAVEFORMATEX *renderfmt = NULL;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    UINT32 buf_frames = 0;
    UINT32 render_block_align = 0;
    AudioHeader hdr;
    BOOL stream_started = FALSE;
    BOOL session_logged = FALSE;
    int  header_misses = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        com_ok = TRUE;

    /* Outer retry loop — keep trying to connect to the guest capture helper */
    while (!d->stop) {
        GUID svc; hcs_service_guid(d->os_type, 4, &svc);
        SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
        if (s == INVALID_SOCKET) {
            int wait;
            for (wait = 0; wait < 2000 && !d->stop; wait += 200)
                Sleep(200);
            continue;
        }

        /* Blocking recv for the session */
        {
            DWORD no_timeout = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
        }
        d->audio_socket = s;

        /* Read one-shot AudioHeader.
           The guest closes without sending a header when the VAD endpoint
           isn't ready yet (e.g. before user login / audio stack init).
           Log only the first miss per run so we don't spam. */
        if (!recv_exact(s, &hdr, sizeof(hdr)) || hdr.magic != AUDIO_HEADER_MAGIC) {
            if (header_misses == 0)
                idd_log(d, L"Audio header bad/absent - guest not ready, retrying quietly.");
            header_misses++;
            goto session_cleanup;
        }

        idd_log(d, L"Audio connected (GUID :0004).");
        session_logged = TRUE;
        header_misses = 0;

        idd_log(d, L"Audio header: %lu Hz, %u ch, %u bits, tag=%u.",
                 hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);

        /* Set up WASAPI render on the default endpoint */
        hr = CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL,
                               CLSCTX_ALL, &AUDIO_IID_IMMDeviceEnumerator,
                               (void **)&pEnum);
        if (FAILED(hr)) { idd_log(d, L"Audio: CoCreateInstance failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDev);
        if (FAILED(hr)) { idd_log(d, L"Audio: GetDefaultAudioEndpoint failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDevice_Activate(pDev, &AUDIO_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC);
        if (FAILED(hr)) { idd_log(d, L"Audio: Activate failed 0x%08lX.", hr); goto session_cleanup; }

        /* Build the source format exactly matching what the guest is sending.
           We deliberately do NOT call GetMixFormat: we never use the host's
           mix format (Initialize runs with AUTOCONVERTPCM, so WASAPI converts
           our renderfmt to whatever the endpoint needs). On some endpoints
           (seen on AMD HD Audio) GetMixFormat returns
           AUDCLNT_E_UNSUPPORTED_FORMAT for the configured default format,
           which would needlessly kill the whole audio path. */
        renderfmt = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!renderfmt) goto session_cleanup;
        ZeroMemory(renderfmt, sizeof(WAVEFORMATEX));
        renderfmt->wFormatTag      = hdr.format_tag;
        renderfmt->nChannels       = hdr.channels;
        renderfmt->nSamplesPerSec  = hdr.sample_rate;
        renderfmt->wBitsPerSample  = hdr.bits_per_sample;
        renderfmt->nBlockAlign     = hdr.block_align ? hdr.block_align
                                     : (WORD)((hdr.channels * hdr.bits_per_sample) / 8);
        renderfmt->nAvgBytesPerSec = renderfmt->nSamplesPerSec * renderfmt->nBlockAlign;
        renderfmt->cbSize          = 0;
        render_block_align         = renderfmt->nBlockAlign;

        /* 40ms buffer. HV socket is effectively memcpy and the guest polls
           every 5ms, so scheduler jitter is the only thing we need to absorb. */
        hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000, 0, renderfmt, NULL);
        if (FAILED(hr)) {
            idd_log(d, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr);
            goto session_cleanup;
        }

        hr = IAudioClient_GetBufferSize(pAC, &buf_frames);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioRenderClient, (void **)&pRC);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_Start(pAC);
        if (FAILED(hr)) goto session_cleanup;
        stream_started = TRUE;

        idd_log(d, L"Audio render started: buf=%lu frames.", buf_frames);

        /* Receive loop */
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 padding;
            UINT32 free_frames;
            UINT32 frames_to_write;
            BYTE *dst;

            if (!recv_exact(s, &fh, sizeof(fh))) break;
            if (fh.bytes == 0 || fh.bytes > 4 * 1024 * 1024) break;

            if (fh.bytes > scratch_cap) {
                BYTE *nb = scratch
                    ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, fh.bytes)
                    : (BYTE *)HeapAlloc(GetProcessHeap(), 0, fh.bytes);
                if (!nb) break;
                scratch = nb;
                scratch_cap = fh.bytes;
            }
            if (!recv_exact(s, scratch, (int)fh.bytes)) break;

            /* Host-side mute: keep draining the socket but don't push to WASAPI */
            if (d->audio_muted) continue;

            frames_in_payload = fh.bytes / render_block_align;
            if (frames_in_payload == 0) continue;

            /* Push into WASAPI buffer; drop if no room (low-latency, no stalling) */
            hr = IAudioClient_GetCurrentPadding(pAC, &padding);
            if (FAILED(hr)) break;
            free_frames = buf_frames - padding;
            frames_to_write = frames_in_payload;
            if (frames_to_write > free_frames)
                frames_to_write = free_frames;
            if (frames_to_write == 0)
                continue;

            hr = IAudioRenderClient_GetBuffer(pRC, frames_to_write, &dst);
            if (FAILED(hr)) break;
            memcpy(dst, scratch, (size_t)frames_to_write * render_block_align);
            IAudioRenderClient_ReleaseBuffer(pRC, frames_to_write, 0);
        }

session_cleanup:
        if (stream_started && pAC) {
            IAudioClient_Stop(pAC);
            stream_started = FALSE;
        }
        if (pRC)    { IAudioRenderClient_Release(pRC);    pRC = NULL; }
        if (pAC)    { IAudioClient_Release(pAC);          pAC = NULL; }
        if (pDev)   { IMMDevice_Release(pDev);            pDev = NULL; }
        if (pEnum)  { IMMDeviceEnumerator_Release(pEnum); pEnum = NULL; }
        if (renderfmt) { CoTaskMemFree(renderfmt); renderfmt = NULL; }

        if (d->audio_socket != INVALID_SOCKET) {
            closesocket(d->audio_socket);
            d->audio_socket = INVALID_SOCKET;
        }
        if (session_logged) {
            idd_log(d, L"Audio session ended.");
            session_logged = FALSE;
        }

        if (d->stop) break;
        /* Back off when the guest keeps rejecting us (VAD not ready), in
           short slices so a stop is seen within 100 ms. */
        {
            int wait, backoff = header_misses > 3 ? 5000 : 500;
            for (wait = 0; wait < backoff && !d->stop; wait += 100)
                Sleep(100);
        }
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (com_ok)  CoUninitialize();
    /* The handle stays set: teardown waits on it before freeing d. */
    return 0;
}


/* ==================================================================
 * D3D11 initialization and teardown
 * ================================================================== */

static BOOL d3d_compile_shader(const char *hlsl, const char *entry,
                               const char *target, ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), NULL, NULL, NULL,
                            entry, target, 0, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            ui_log(L"Shader compile error: %S",
                   (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        return FALSE;
    }
    if (errors) errors->lpVtbl->Release(errors);
    return TRUE;
}

/* The dynamic, CPU-writable texture frames are uploaded to, sized to the frame. */
static BOOL d3d_create_frame_texture(VmDisplayIdd *d, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    HRESULT hr;

    if (d->frame_srv) { d->frame_srv->lpVtbl->Release(d->frame_srv); d->frame_srv = NULL; }
    if (d->frame_tex) { d->frame_tex->lpVtbl->Release(d->frame_tex); d->frame_tex = NULL; }
    d->tex_width = d->tex_height = 0;

    ZeroMemory(&td, sizeof(td));
    td.Width              = width;
    td.Height             = height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DYNAMIC;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

    hr = d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &d->frame_tex);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateTexture2D(%ux%u) failed (0x%08X)", width, height, hr);
        return FALSE;
    }

    /* Shader resource view for the frame texture */
    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    hr = d->device->lpVtbl->CreateShaderResourceView(d->device,
            (ID3D11Resource *)d->frame_tex, &srv_desc, &d->frame_srv);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateShaderResourceView failed (0x%08X)", hr);
        d->frame_tex->lpVtbl->Release(d->frame_tex);
        d->frame_tex = NULL;
        return FALSE;
    }

    d->tex_width  = width;
    d->tex_height = height;
    return TRUE;
}

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    HRESULT hr;

    /* Create device and swap chain */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = DEFAULT_WIDTH;
    scd.BufferDesc.Height                  = DEFAULT_HEIGHT;
    scd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = d->render_hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION,
        &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);

    if (FAILED(hr)) {
        ui_log(L"IDD: D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        return FALSE;
    }

    /* Create render target view from back buffer */
    {
        ID3D11Texture2D *back_buf = NULL;
        hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                       &IID_ID3D11Texture2D, (void **)&back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: GetBuffer failed (0x%08X)", hr);
            return FALSE;
        }
        hr = d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: CreateRenderTargetView failed (0x%08X)", hr);
            return FALSE;
        }
    }

    if (!d3d_create_frame_texture(d, DEFAULT_WIDTH, DEFAULT_HEIGHT))
        return FALSE;

    /* Compile and create vertex shader */
    if (!d3d_compile_shader(g_vs_hlsl, "main", "vs_4_0", &vs_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreateVertexShader(d->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob),
            NULL, &d->vs);
    vs_blob->lpVtbl->Release(vs_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateVertexShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create pixel shader */
    if (!d3d_compile_shader(g_ps_hlsl, "main", "ps_4_0", &ps_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreatePixelShader(d->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob),
            NULL, &d->ps);
    ps_blob->lpVtbl->Release(ps_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreatePixelShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Sampler state (linear filtering) */
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = d->device->lpVtbl->CreateSamplerState(d->device, &sd, &d->sampler);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateSamplerState failed (0x%08X)", hr);
        return FALSE;
    }

    return TRUE;
}

static void d3d_resize_swap_chain(VmDisplayIdd *d)
{
    RECT rc;
    HRESULT hr;
    ID3D11Texture2D *back_buf = NULL;

    if (!d->swap_chain) return;

    /* Release old render target */
    if (d->rtv) {
        d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 0, NULL, NULL);
        d->rtv->lpVtbl->Release(d->rtv);
        d->rtv = NULL;
    }

    GetClientRect(d->render_hwnd, &rc);
    idd_log(d, L"Resize: render_hwnd client=%dx%d, frame=%ux%u",
            rc.right, rc.bottom, d->frame_width, d->frame_height);
    if (rc.right == 0 || rc.bottom == 0) return;

    hr = d->swap_chain->lpVtbl->ResizeBuffers(d->swap_chain, 0,
            (UINT)rc.right, (UINT)rc.bottom,
            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        ui_log(L"IDD: ResizeBuffers failed (0x%08X)", hr);
        return;
    }

    hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                   &IID_ID3D11Texture2D, (void **)&back_buf);
    if (SUCCEEDED(hr)) {
        d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
    }
}

static void d3d_render_frame(VmDisplayIdd *d)
{
    D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
    D3D11_VIEWPORT vp;
    RECT rc;
    HRESULT hr;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    BOOL frame_uploaded = FALSE;

    if (!d->device || !d->ctx || !d->swap_chain || !d->rtv)
        return;

    /* Upload frame data to GPU texture if dirty */
    if (d->frame_dirty) {
        EnterCriticalSection(&d->frame_cs);
        if (d->frame_width != d->tex_width || d->frame_height != d->tex_height)
            d3d_create_frame_texture(d, d->frame_width, d->frame_height);
        hr = d->frame_tex ? d->ctx->lpVtbl->Map(d->ctx,
                (ID3D11Resource *)d->frame_tex, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped) : E_FAIL;
        if (SUCCEEDED(hr)) {
            UINT row;
            UINT copy_stride = d->frame_width * 4;
            if (copy_stride > mapped.RowPitch)
                copy_stride = mapped.RowPitch;
            if (copy_stride > d->frame_stride)
                copy_stride = d->frame_stride;

            for (row = 0; row < d->frame_height; row++) {
                memcpy((BYTE *)mapped.pData + row * mapped.RowPitch,
                       d->frame_buf + row * d->frame_stride,
                       copy_stride);
            }
            d->ctx->lpVtbl->Unmap(d->ctx,
                    (ID3D11Resource *)d->frame_tex, 0);
        }
        d->frame_dirty = FALSE;
        LeaveCriticalSection(&d->frame_cs);
        frame_uploaded = TRUE;
    }

    /* Compute letterboxed viewport within client area */
    GetClientRect(d->render_hwnd, &rc);
    {
        float vp_x, vp_y, vp_w, vp_h;
        compute_letterbox((UINT)rc.right, (UINT)rc.bottom,
                          d->frame_width, d->frame_height,
                          &vp_x, &vp_y, &vp_w, &vp_h);
        ZeroMemory(&vp, sizeof(vp));
        vp.TopLeftX = vp_x;
        vp.TopLeftY = vp_y;
        vp.Width    = vp_w;
        vp.Height   = vp_h;
        vp.MaxDepth = 1.0f;
    }

    /* Count uploaded frames and refresh the title (with the frame rate)
       twice a second; retitling every frame costs the window a redraw. */
    if (frame_uploaded) {
        DWORD now = GetTickCount();
        d->title_frames++;
        if ((DWORD)(now - d->title_tick) >= 500) {
            DWORD elapsed = (DWORD)(now - d->title_tick);
            d->fps = d->title_tick ? (d->title_frames * 1000 + elapsed / 2) / elapsed : 0;
            d->title_frames = 0;
            d->title_tick = now;
            idd_update_title(d);
        }
    }
    d->render_count++;

    d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
    d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &vp);
    d->ctx->lpVtbl->ClearRenderTargetView(d->ctx, d->rtv, clear_color);

    d->ctx->lpVtbl->IASetPrimitiveTopology(d->ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->lpVtbl->IASetInputLayout(d->ctx, NULL);

    d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
    d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps, NULL, 0);
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &d->frame_srv);
    d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);

    /* Draw fullscreen triangle (3 vertices, no vertex buffer) */
    d->ctx->lpVtbl->Draw(d->ctx, 3, 0);

    d->swap_chain->lpVtbl->Present(d->swap_chain, 0, 0);
}

static void d3d_cleanup(VmDisplayIdd *d)
{
    if (d->sampler)    { d->sampler->lpVtbl->Release(d->sampler);       d->sampler = NULL; }
    if (d->ps)         { d->ps->lpVtbl->Release(d->ps);                 d->ps = NULL; }
    if (d->vs)         { d->vs->lpVtbl->Release(d->vs);                 d->vs = NULL; }
    if (d->frame_srv)  { d->frame_srv->lpVtbl->Release(d->frame_srv);   d->frame_srv = NULL; }
    if (d->frame_tex)  { d->frame_tex->lpVtbl->Release(d->frame_tex);   d->frame_tex = NULL; }
    if (d->rtv)        { d->rtv->lpVtbl->Release(d->rtv);               d->rtv = NULL; }
    if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
    if (d->ctx)        { d->ctx->lpVtbl->Release(d->ctx);               d->ctx = NULL; }
    if (d->device)     { d->device->lpVtbl->Release(d->device);         d->device = NULL; }
}

/* ==================================================================
 * Guest cursor — create HCURSOR from received bitmap
 * ================================================================== */

/* Cursor types from IddCx IDDCX_CURSOR_SHAPE_TYPE */
#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

static HCURSOR create_cursor_from_bitmap(UINT width, UINT height,
                                          UINT xhot, UINT yhot,
                                          UINT cursor_type, UINT pitch,
                                          UINT shape_data_size,
                                          const BYTE *shape_data)
{
    HCURSOR result = NULL;
    BITMAPINFO bmi;
    HBITMAP hColor = NULL, hMask = NULL;
    ICONINFO ii;
    HDC hdc;

    if (width == 0 || height == 0 || width > 256 || height > 256)
        return NULL;

    /* shape_data holds exactly shape_data_size bytes; both copy paths read up
       to (height-1)*pitch + width*4 bytes. Require the stride to cover a row
       and the total to cover all rows, else a tiny buffer with large
       width/height/pitch would over-read. */
    if (pitch < width * 4 || (UINT64)pitch * height > (UINT64)shape_data_size)
        return NULL;

    hdc = GetDC(NULL);

    if (cursor_type == CURSOR_TYPE_MASKED_COLOR) {
        /* MASKED_COLOR (IddCx 1.10 / QueryHardwareCursor3): single 32bpp BGRA
           image where the alpha channel encodes the AND mask.
           Height is the ACTUAL cursor height (not doubled).
           Per pixel: A = AND mask (0xFF = transparent/XOR, 0x00 = opaque),
                      B,G,R = XOR color values.
           We extract A into a 1bpp monochrome hbmMask and BGR into hbmColor. */
        UINT mask_row_bytes = (width + 7) / 8;
        UINT mask_pitch = ((mask_row_bytes + 3) & ~3u);
        void *color_bits = NULL;
        BYTE *mask_buf;
        UINT row, col;

        /* Build 1bpp AND mask from alpha channel */
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      mask_pitch * height);
        if (!mask_buf) { ReleaseDC(NULL, hdc); return NULL; }

        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                BYTE alpha = src_row[col * 4 + 3];  /* A channel */
                if (alpha != 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        /* XOR color bitmap (32bpp, top-down) — copy full BGRA data */
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)width;
        bmi.bmiHeader.biHeight      = -(LONG)height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (hColor && color_bits) {
            UINT dst_pitch = width * 4;
            for (row = 0; row < height; row++) {
                const BYTE *src = shape_data + row * pitch;
                BYTE *dst = (BYTE *)color_bits + row * dst_pitch;
                for (col = 0; col < width; col++) {
                    dst[col * 4 + 0] = src[col * 4 + 0];  /* B */
                    dst[col * 4 + 1] = src[col * 4 + 1];  /* G */
                    dst[col * 4 + 2] = src[col * 4 + 2];  /* R */
                    dst[col * 4 + 3] = 0;                  /* A = 0, AND mask handles transparency */
                }
            }
        }

        ReleaseDC(NULL, hdc);

        if (!hColor || !hMask) {
            if (hColor) DeleteObject(hColor);
            if (hMask)  DeleteObject(hMask);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;

    } else {
        /* ALPHA (type 2): 32bpp BGRA with premultiplied alpha.
           Height is the real cursor height.

           The 32-bpp DIB section with BI_RGB doesn't tell Windows that
           the top byte is alpha — Windows treats it as 24-bit RGB +
           padding and, with an all-zero AND mask, renders EVERY pixel
           opaque. For cursors whose transparent regions are stored as
           (0,0,0,0) (e.g. ours from Linux), that paints a black square
           around the cursor. Windows-VDD cursors happen not to trigger
           this because their transparent regions have non-zero RGB.

           Fix: build the AND mask from the alpha channel — bit set
           (1) = transparent, bit cleared (0) = opaque. Threshold low
           (any alpha > 0 = opaque) preserves anti-aliased edges, the
           alpha channel then handles smooth blending of those edges.

           Use BITMAPV4HEADER with explicit alpha mask instead of
           BITMAPINFOHEADER+BI_RGB. The latter is ambiguous at 32-bpp:
           some Windows paths treat it as XRGB (alpha ignored, RGB
           rendered at full opacity → cursor body looks too bright).
           V4 with bV4AlphaMask = 0xFF000000 settles it. */
        void *color_bits = NULL;
        UINT row, col, dst_pitch;
        UINT mask_row_bytes, mask_pitch;
        BYTE *mask_buf;
        BITMAPV4HEADER bv4;

        ZeroMemory(&bv4, sizeof(bv4));
        bv4.bV4Size          = sizeof(BITMAPV4HEADER);
        bv4.bV4Width         = (LONG)width;
        bv4.bV4Height        = -(LONG)height;
        bv4.bV4Planes        = 1;
        bv4.bV4BitCount      = 32;
        bv4.bV4V4Compression = BI_BITFIELDS;
        bv4.bV4RedMask       = 0x00FF0000;
        bv4.bV4GreenMask     = 0x0000FF00;
        bv4.bV4BlueMask      = 0x000000FF;
        bv4.bV4AlphaMask     = 0xFF000000;

        hColor = CreateDIBSection(hdc, (BITMAPINFO *)&bv4,
                                  DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (!hColor || !color_bits) {
            ReleaseDC(NULL, hdc);
            return NULL;
        }

        dst_pitch = width * 4;
        for (row = 0; row < height; row++) {
            memcpy((BYTE *)color_bits + row * dst_pitch,
                   shape_data + row * pitch,
                   dst_pitch);
        }

        /* AND mask, 1bpp, DWORD-aligned rows. Bit set = transparent. */
        mask_row_bytes = (width + 7) / 8;
        mask_pitch     = (mask_row_bytes + 3) & ~3u;
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (size_t)mask_pitch * height);
        if (!mask_buf) {
            DeleteObject(hColor);
            ReleaseDC(NULL, hdc);
            return NULL;
        }
        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                if (src_row[col * 4 + 3] == 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        ReleaseDC(NULL, hdc);

        if (!hMask) {
            DeleteObject(hColor);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
    }

    result = (HCURSOR)CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);

    return result;
}

/* ==================================================================
 * Guest display following the window
 * ================================================================== */

/* The window's client area changed. Nothing is asked of the guest until it
   settles, and the picture is scaled to fit until the new size arrives. */
static void idd_resize_note_client(VmDisplayIdd *d, HWND hwnd, UINT width, UINT height)
{
    if (width < IDD_MIN_WIDTH)   width = IDD_MIN_WIDTH;
    if (height < IDD_MIN_HEIGHT) height = IDD_MIN_HEIGHT;
    if (width > IDD_MAX_SIDE)    width = IDD_MAX_SIDE;
    if (height > IDD_MAX_SIDE)   height = IDD_MAX_SIDE;
    if ((UINT64)width * height > IDD_MAX_PIXELS) height = IDD_MAX_PIXELS / width;
    if (width == d->want_width && height == d->want_height) return;
    d->want_width = width;
    d->want_height = height;
    d->resize_gave_up = FALSE;
    d->asked_width = d->asked_height = 0;
    SetTimer(hwnd, IDT_RESIZE, RESIZE_SETTLE_MS, NULL);
}

static BOOL idd_resize_send(VmDisplayIdd *d, UINT width, UINT height)
{
    UINT32 msg[3];
    BOOL sent = FALSE;
    msg[0] = RESIZE_MAGIC;
    msg[1] = width;
    msg[2] = height;
    EnterCriticalSection(&d->frame_cs);
    if (d->frame_socket != INVALID_SOCKET)
        sent = send(d->frame_socket, (const char *)msg, sizeof(msg), 0) == (int)sizeof(msg);
    LeaveCriticalSection(&d->frame_cs);
    return sent;
}

/* Bring the guest to the window's size, one request at a time. The frames are
   the answer: when they have the new size, it worked. A guest that never gets
   there (a driver from before this, or a size Windows won't use) keeps what it
   has, with the picture scaled to the window as before, and is not asked again
   until the window's size changes. */
static void idd_resize_step(VmDisplayIdd *d)
{
    UINT target_w = d->want_width, target_h = d->want_height;

    if (!d->can_resize || d->in_sizemove || d->resize_gave_up || !target_w) return;
    if (d->resize_even_only) {
        target_w = IDD_ALIGN2(target_w);
        target_h = IDD_ALIGN2(target_h);
    }

    if (d->frame_seen && d->frame_width == target_w && d->frame_height == target_h) {
        if (d->asked_width) {
            idd_log(d, L"Guest display is now %ux%u.", target_w, target_h);
            d->resize_worked = TRUE;
        }
        d->asked_width = d->asked_height = 0;
        return;
    }

    /* After a guest reboot this comes round again when frames return. */
    if (!d->frame_seen || d->frame_socket == INVALID_SOCKET) {
        SetTimer(d->hwnd, IDT_RESIZE, RESIZE_POLL_MS, NULL);
        return;
    }

    if (d->asked_width == target_w && d->asked_height == target_h) {
        if ((DWORD)(GetTickCount() - d->asked_tick) < RESIZE_WAIT_MS) {
            SetTimer(d->hwnd, IDT_RESIZE, RESIZE_POLL_MS, NULL);
            return;
        }
        if ((target_w | target_h) & 1) {
            idd_log(d, L"Guest display didn't change to %ux%u; trying the nearest even size.",
                    target_w, target_h);
            d->resize_even_only = TRUE;
            target_w = IDD_ALIGN2(target_w);
            target_h = IDD_ALIGN2(target_h);
        } else {
            idd_log(d, L"Guest display didn't change to %ux%u; staying at %ux%u, scaled to the window.",
                    target_w, target_h, d->frame_width, d->frame_height);
            d->resize_gave_up = TRUE;
            if (!d->resize_worked) d->can_resize = FALSE;   /* e.g. an older guest driver */
            return;
        }
    }

    idd_log(d, L"Asking for guest display size %ux%u.", target_w, target_h);
    if (idd_resize_send(d, target_w, target_h)) {
        d->asked_width = target_w;
        d->asked_height = target_h;
        d->asked_tick = GetTickCount();
    }
    SetTimer(d->hwnd, IDT_RESIZE, RESIZE_POLL_MS, NULL);
}

/* ==================================================================
 * Recv thread - connects to VM, receives frames, updates frame_buf
 * ================================================================== */

static void clip_log_callback(const wchar_t *msg, void *user_data)
{
    VmDisplayIdd *d = (VmDisplayIdd *)user_data;
    idd_log(d, L"%s", msg);
}

static DWORD WINAPI idd_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;
    UINT32 recv_cap = DEFAULT_WIDTH * DEFAULT_HEIGHT * 4;   /* grows with the frames */
    /* Frame pipeline timing, logged every 2 s: how long each frame takes to
       arrive once it starts (transfer) against how often frames start (the
       guest's rate). Slow transfer points at the link, slow starts at the
       guest (game, readback). */
    LARGE_INTEGER perf_freq, perf_window, perf_t0, perf_t1;
    double perf_recv_ms = 0.0;
    UINT64 perf_bytes = 0;
    UINT perf_frames = 0, perf_full = 0, perf_nv12 = 0;
    ULONGLONG lab_shot_tick = 0;      /* malware lab screenshots */
    UINT64 lab_shot_hash = 0;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    QueryPerformanceFrequency(&perf_freq);
    QueryPerformanceCounter(&perf_window);

    /* Allocate receive buffer for frame pixel data */
    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, recv_cap);
    if (!recv_buf) {
        ui_log(L"IDD recv: failed to allocate receive buffer");
        return 1;
    }

    /* The agent connection has one synchronous command slot.  Startup uses it
       for network and shared-resource configuration, so wait for that sequence
       to finish before asking the agent to respawn the console-session helper. */
    while (!d->stop && d->vm && d->vm->agent_online &&
           d->vm->agent_initializing) {
        Sleep(100);
    }

    /* Tell the agent to respawn input helper in console session. */
    if (!d->stop && d->vm && d->vm->agent_online &&
        !d->vm->agent_initializing) {
        idd_log(d, L"Sending idd_connect to agent...");
        vm_agent_send(d->vm, "idd_connect", NULL, 0, 30000);
    }

    /* Input socket lives independently of the frame channel — survives
       frame disconnections so the user can still send input to wake
       the VM screen if the display path goes inactive. */
    {
        SOCKET input_s = INVALID_SOCKET;

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;

        /* Ensure input channel is connected (independent of frame channel) */
        if (input_s == INVALID_SOCKET) {
            GUID svc; hcs_service_guid(d->os_type, 3, &svc);
            input_s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
            if (input_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(input_s, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == INPUT_READY_MAGIC) {
                    DWORD zero_timeout = 0;
                    u_long nb = 1;
                    setsockopt(input_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                    ioctlsocket(input_s, FIONBIO, &nb);
                    d->input_socket = input_s;
                    g_input_send_count = 0;
                    idd_log(d, L"Input connected + ready (GUID :0003).");
                } else {
                    idd_log(d, L"Input handshake failed - closing.");
                    closesocket(input_s);
                    input_s = INVALID_SOCKET;
                }
            }
        }

        /* Clipboard module (handles :0005 + :0006 internally). A malware lab
           VM gets none: nothing in it may read or fill the host clipboard. */
        if (!d->clipboard && !(d->vm && d->vm->lab_mode)) {
            d->clipboard = vm_clipboard_create(&d->runtime_id, d->os_type,
                                               d->hwnd, clip_log_callback, d);
            if (d->clipboard)
                idd_log(d, L"Clipboard module created.");
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        {
            GUID svc; hcs_service_guid(d->os_type, 2, &svc);
            s = connect_to_hv_service(&d->runtime_id, &svc, 3000);
        }
        if (s == INVALID_SOCKET) {
            int wait;
            idd_log(d, L"Connection failed, retrying in 3s.");
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        idd_log(d, L"Frame channel connected.");
        EnterCriticalSection(&d->frame_cs);
        d->frame_socket = s;
        d->frame_seen = FALSE;
        LeaveCriticalSection(&d->frame_cs);

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;
            BOOL nv12;
            BOOL size_changed = FALSE;

            /* Peek at magic to determine message type */
            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;

                if ((d->cursor_visible != 0) != (chdr.visible != 0) && d->hwnd)
                    PostMessageW(d->hwnd, WM_IDD_CURSOR_VIS, chdr.visible ? 1 : 0, 0);
                d->cursor_visible = chdr.visible;

                if (chdr.shape_updated && chdr.shape_data_size > 0) {
                    BYTE *cursor_buf;
                    if (chdr.shape_data_size > MAX_CURSOR_SIZE) {
                        idd_log(d, L"Cursor data too large (%u), reconnecting.",
                               chdr.shape_data_size);
                        break;
                    }
                    cursor_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                                    chdr.shape_data_size);
                    if (!cursor_buf) break;

                    if (!recv_exact(s, cursor_buf, (int)chdr.shape_data_size)) {
                        HeapFree(GetProcessHeap(), 0, cursor_buf);
                        break;
                    }

                    /* Create new cursor from bitmap */
                    {
                        HCURSOR new_cursor = create_cursor_from_bitmap(
                            chdr.width, chdr.height, chdr.xhot, chdr.yhot,
                            chdr.cursor_type, chdr.pitch,
                            chdr.shape_data_size, cursor_buf);
                        if (new_cursor) {
                            HCURSOR old = d->guest_cursor;
                            d->guest_cursor = new_cursor;
                            d->cursor_shape_id = chdr.shape_id;
                            if (old) DestroyCursor(old);
                            /* Force cursor update if mouse is in window */
                            if (d->render_hwnd)
                                PostMessageW(d->render_hwnd, WM_SETCURSOR,
                                             (WPARAM)d->render_hwnd,
                                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                        }
                    }

                    HeapFree(GetProcessHeap(), 0, cursor_buf);
                }
                continue;  /* back to message loop */
            }

            if (magic != FRAME_MAGIC && magic != FRAME_NV12_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }
            nv12 = (magic == FRAME_NV12_MAGIC);

            /* Read rest of frame header (already read magic) */
            QueryPerformanceCounter(&perf_t0);
            hdr.magic = magic;
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
                break;

            /* Sanity checks */
            if (hdr.width == 0 || hdr.height == 0 ||
                hdr.width > IDD_MAX_SIDE || hdr.height > IDD_MAX_SIDE ||
                (UINT64)hdr.width * hdr.height > IDD_MAX_PIXELS ||
                hdr.stride < hdr.width * (nv12 ? 1 : 4) ||
                (nv12 && (hdr.stride != IDD_ALIGN2(hdr.width) || hdr.dirty_rect_count != 0))) {
                idd_log(d, L"Invalid frame dimensions %ux%u stride %u.",
                       hdr.width, hdr.height, hdr.stride);
                break;
            }

            rect_count = hdr.dirty_rect_count;
            if (rect_count > MAX_DIRTY_RECTS) {
                idd_log(d, L"Too many dirty rects (%u), reconnecting.", rect_count);
                break;
            }

            /* Read dirty rects */
            if (rect_count > 0) {
                if (!recv_exact(s, dirty_rects, (int)(rect_count * sizeof(RECT))))
                    break;
            }

            /* Read data_size */
            if (!recv_exact(s, &data_size, 4))
                break;

            if (data_size > MAX_FRAME_DATA_SIZE ||
                (nv12 && data_size != IDD_ALIGN2(hdr.width) * IDD_ALIGN2(hdr.height) / 2 * 3)) {
                idd_log(d, L"Frame data size %u bytes is wrong, reconnecting.", data_size);
                break;
            }

            if (data_size > recv_cap) {
                BYTE *bigger = (BYTE *)HeapAlloc(GetProcessHeap(), 0, data_size);
                if (!bigger) {
                    idd_log(d, L"No memory for a %u byte frame, reconnecting.", data_size);
                    break;
                }
                HeapFree(GetProcessHeap(), 0, recv_buf);
                recv_buf = bigger;
                recv_cap = data_size;
            }

            /* Read pixel data */
            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
            }

            QueryPerformanceCounter(&perf_t1);
            perf_recv_ms += (double)(perf_t1.QuadPart - perf_t0.QuadPart) * 1000.0 /
                            (double)perf_freq.QuadPart;
            perf_bytes += data_size;
            perf_frames++;
            if (rect_count == 0) perf_full++;
            if (nv12) perf_nv12++;
            {
                double window_ms = (double)(perf_t1.QuadPart - perf_window.QuadPart) * 1000.0 /
                                   (double)perf_freq.QuadPart;
                if (window_ms >= 2000.0) {
                    idd_log(d, L"Frames: %.1f fps (%u full, %u NV12), transfer %.1f ms/frame, %.0f MB/s while sending.",
                            perf_frames * 1000.0 / window_ms, perf_full, perf_nv12,
                            perf_recv_ms / perf_frames,
                            perf_recv_ms > 0 ? (double)perf_bytes / 1048.576 / perf_recv_ms : 0.0);
                    perf_window = perf_t1;
                    perf_recv_ms = 0.0;
                    perf_bytes = 0;
                    perf_frames = perf_full = perf_nv12 = 0;
                }
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            /* Reallocate frame_buf if resolution changed */
            if (hdr.width != d->frame_width || hdr.height != d->frame_height) {
                UINT new_stride = hdr.width * 4;
                UINT new_size   = new_stride * hdr.height;
                BYTE *new_buf   = (BYTE *)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY, new_size);
                if (new_buf) {
                    if (d->frame_buf)
                        HeapFree(GetProcessHeap(), 0, d->frame_buf);
                    d->frame_buf    = new_buf;
                    d->frame_width  = hdr.width;
                    d->frame_height = hdr.height;
                    d->frame_stride = new_stride;
                    size_changed = TRUE;
                    idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                            hdr.width, hdr.height, hdr.stride);
                    idd_log(d, L"Resolution changed to %ux%u.", hdr.width, hdr.height);
                } else {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
            }

            if (nv12) {
                if (hdr.width == d->frame_width && hdr.height == d->frame_height)
                    idd_nv12_to_bgra(recv_buf, hdr.width, hdr.height,
                                     d->frame_buf, d->frame_stride);
            } else if (rect_count == 0) {
                /* Full frame update */
                UINT row;
                UINT copy_w = hdr.width * 4;
                BYTE *src = recv_buf;
                /* Bound source rows by the bytes actually received: width/height/
                   stride and data_size are independent guest-controlled fields, so
                   only data_size/stride rows of recv_buf hold valid pixels. */
                UINT src_rows = hdr.stride ? (UINT)(data_size / hdr.stride) : 0;
                if (copy_w > d->frame_stride) copy_w = d->frame_stride;
                if (copy_w > hdr.stride)      copy_w = hdr.stride;

                for (row = 0; row < hdr.height && row < d->frame_height &&
                              row < src_rows; row++) {
                    memcpy(d->frame_buf + row * d->frame_stride,
                           src + row * hdr.stride,
                           copy_w);
                }
            } else {
                /* Dirty rect updates — pixel data is per-rect rows concatenated */
                BYTE *src = recv_buf;
                for (i = 0; i < rect_count; i++) {
                    LONG left   = dirty_rects[i].left;
                    LONG top    = dirty_rects[i].top;
                    LONG right  = dirty_rects[i].right;
                    LONG bottom = dirty_rects[i].bottom;
                    UINT rect_w, rect_h, row;
                    UINT rect_row_bytes;

                    /* Clamp to frame bounds */
                    if (left < 0) left = 0;
                    if (top  < 0) top  = 0;
                    if (right  > (LONG)d->frame_width)  right  = (LONG)d->frame_width;
                    if (bottom > (LONG)d->frame_height) bottom = (LONG)d->frame_height;
                    if (left >= right || top >= bottom) continue;

                    rect_w = (UINT)(right - left);
                    rect_h = (UINT)(bottom - top);
                    rect_row_bytes = rect_w * 4;

                    /* Refuse to read past the bytes actually received into recv_buf. */
                    if ((size_t)(src - recv_buf) + (size_t)rect_row_bytes * rect_h >
                        (size_t)data_size)
                        break;

                    for (row = 0; row < rect_h; row++) {
                        UINT dst_y = (UINT)top + row;
                        memcpy(d->frame_buf + dst_y * d->frame_stride + (UINT)left * 4,
                               src + row * rect_row_bytes,
                               rect_row_bytes);
                    }
                    src += rect_row_bytes * rect_h;
                }
            }

            d->frame_dirty = TRUE;
            d->recv_count++;
            if (!d->frame_seen) {
                d->frame_seen = TRUE;
                size_changed = TRUE;
            }
            LeaveCriticalSection(&d->frame_cs);

            /* Signal the window thread to repaint */
            if (d->hwnd && IsWindow(d->hwnd)) {
                PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);
                if (size_changed)
                    PostMessageW(d->hwnd, WM_IDD_RESIZE_CHECK, 0, 0);
            }

            idd_lab_screenshot(d, &lab_shot_tick, &lab_shot_hash);

            /* Reconnect input socket if send_input flagged it dead */
            if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
                idd_log(d, L"Input socket closed, will reconnect...");
            }
            if (input_s == INVALID_SOCKET) {
                GUID svc; hcs_service_guid(d->os_type, 3, &svc);
                SOCKET new_s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
                if (new_s != INVALID_SOCKET) {
                    UINT32 ready_magic = 0;
                    if (recv_exact(new_s, &ready_magic, sizeof(ready_magic)) &&
                        ready_magic == INPUT_READY_MAGIC) {
                        DWORD zero_timeout = 0;
                        u_long nb = 1;
                        setsockopt(new_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                        ioctlsocket(new_s, FIONBIO, &nb);
                        input_s = new_s;
                        d->input_socket = new_s;
                        g_input_send_count = 0;
                        idd_log(d, L"Input reconnected + ready (GUID :0003).");
                    } else {
                        closesocket(new_s);
                    }
                }
                /* If connect/handshake fails, will retry next frame */
            }
        }

        /* Frame channel lost — close it but keep input alive */
        EnterCriticalSection(&d->frame_cs);
        d->frame_socket = INVALID_SOCKET;
        d->frame_seen = FALSE;
        LeaveCriticalSection(&d->frame_cs);
        closesocket(s);
        idd_log(d, L"Frame channel disconnected, reconnecting...");

        /* Check if input is still alive (send_input may have flagged it dead) */
        if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
            closesocket(input_s);
            input_s = INVALID_SOCKET;
            idd_log(d, L"Input socket flagged dead, will reconnect.");
        }

        /* Wait before reconnecting frame channel */
        {
            int wait;
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
        }
    }

    /* Final cleanup — close input socket on thread exit */
    d->input_socket = INVALID_SOCKET;
    if (input_s != INVALID_SOCKET) {
        closesocket(input_s);
    }
    idd_log(d, L"Input disconnected.");

    } /* end input_s scope */

    if (recv_buf)
        HeapFree(GetProcessHeap(), 0, recv_buf);

    WSACleanup();
    return 0;
}

/* ==================================================================
 * Window thread — creates window, initializes D3D11, runs message pump
 * ================================================================== */

static DWORD WINAPI idd_window_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    wchar_t title[300];
    MSG msg;

    ensure_idd_class(d->hInstance);

    swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);

    /* Compute outer window size so the client area is exactly 1920x1080 */
    {
        DWORD style   = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
        DWORD exstyle = 0;
        RECT wr = { 0, 0, 1920, 1080 };
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);

        d->hwnd = CreateWindowExW(
            exstyle, IDD_DISPLAY_CLASS, title, style,
            d->has_init_pos ? d->init_pos.x : CW_USEDEFAULT,
            d->has_init_pos ? d->init_pos.y : CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, d->hInstance, d);
    }

    if (!d->hwnd) {
        ui_log(L"IDD: CreateWindowEx failed (0x%08X)", GetLastError());
        d->open = FALSE;
        return 1;
    }

    /* Dark mode title bar to match AppSandbox main window */
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* Add options to the system menu (right-click title bar) */
    {
        HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
        if (sysmenu) {
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_AUDIO_MUTE, L"Mute audio");
            AppendMenuW(sysmenu, MF_STRING, IDM_XMIT_HOTKEYS, L"Transmit Keyboard Hotkeys");
            AppendMenuW(sysmenu, MF_STRING, IDM_GAME_MODE,
                        L"Game Mode (capture mouse when the VM hides it)");
            AppendMenuW(sysmenu, MF_STRING, IDM_CAPTURE_MOUSE,
                        L"Capture Mouse Now (Ctrl+Alt releases)");
            if (d->vm && _wcsicmp(d->vm->os_type, L"Windows") == 0)
                AppendMenuW(sysmenu, MF_STRING, IDM_BACKSTAGE,
                            L"Backstage Session (SYSTEM)...");
            AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_LOG, L"Show Log");
            if (d->switchable)
                AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_CONSOLE,
                            L"Show Hyper-V Screen (boot console)");
            CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                          MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(sysmenu, IDM_GAME_MODE,
                          MF_BYCOMMAND | (d->game_mode ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    /* Floating menu tab on the top edge (Parsec-style) */
    idd_fab_create(d);

    /* Bring the display window to the foreground on open */
    ShowWindow(d->hwnd, SW_SHOW);
    BringWindowToTop(d->hwnd);
    SetForegroundWindow(d->hwnd);

    /* Render child fills entire client area */
    {
        RECT rc;
        GetClientRect(d->hwnd, &rc);

        d->render_hwnd = CreateWindowExW(
            0, IDD_RENDER_CLASS, NULL,
            WS_CHILD | WS_VISIBLE,
            0, 0, rc.right, rc.bottom,
            d->hwnd, NULL, d->hInstance, NULL);
    }

    /* Separate top-level log window */
    {
        wchar_t log_title[300];
        HFONT font;
        swprintf_s(log_title, 300, L"%s - IDD Log", d->vm_name);

        /* Created hidden — shown on demand via the "Show Log" system-menu item. */
        d->log_hwnd = CreateWindowExW(
            0, L"AppSandboxIddLog", log_title,
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT, CW_USEDEFAULT, LOG_WINDOW_W, LOG_WINDOW_H,
            NULL, NULL, d->hInstance, NULL);

        if (d->log_hwnd) {
            /* Dark mode title bar to match AppSandbox main window */
            BOOL dark = TRUE;
            DwmSetWindowAttribute(d->log_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            /* Fill the log window with a listbox */
            RECT lrc;
            GetClientRect(d->log_hwnd, &lrc);
            d->log_list_hwnd = CreateWindowExW(
                0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_HASSTRINGS,
                0, 0, lrc.right, lrc.bottom,
                d->log_hwnd, (HMENU)(INT_PTR)IDC_LOG_LIST, d->hInstance, NULL);

            /* Subclass the listbox so Ctrl+A / Ctrl+C work when it has focus */
            if (d->log_list_hwnd)
                g_orig_listbox_proc = (WNDPROC)SetWindowLongPtrW(
                    d->log_list_hwnd, GWLP_WNDPROC, (LONG_PTR)idd_log_listbox_proc);

            font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
            if (font && d->log_list_hwnd)
                SendMessageW(d->log_list_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
        }
        idd_log(d, L"IDD display started.");
    }

    /* Initialize D3D11 */
    if (!d3d_init(d)) {
        ui_log(L"IDD: D3D11 initialization failed.");
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start the recv thread now that the window and D3D11 are ready */
    d->recv_thread = CreateThread(NULL, 0, idd_recv_thread_proc, d, 0, NULL);
    if (!d->recv_thread) {
        ui_log(L"IDD: Failed to create recv thread.");
        d3d_cleanup(d);
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start a present timer for steady rendering */
    SetTimer(d->hwnd, IDT_PRESENT, PRESENT_MS, NULL);

    /* Install the hotkey hook on this (message-pumping) thread if the
       persisted setting has Transmit mode enabled. */
    if (d->transmit_hotkeys)
        idd_install_kbd_hook(d);

    /* Message pump */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        AddClipboardFormatListener(hwnd);
        return 0;
    }

    d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_SYSCOMMAND:
        if (d && (wp & 0xFFF0) == IDM_AUDIO_MUTE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->audio_muted = !d->audio_muted;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_AUDIO_MUTE,
                              MF_BYCOMMAND | (d->audio_muted ? MF_CHECKED : MF_UNCHECKED));
            }
            idd_update_title(d);
            idd_log(d, d->audio_muted ? L"Audio muted." : L"Audio unmuted.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_XMIT_HOTKEYS) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->transmit_hotkeys = !d->transmit_hotkeys;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                              MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->transmit_hotkeys) {
                idd_install_kbd_hook(d);
            } else if (!d->captured) {
                idd_remove_kbd_hook(d);
                /* Release anything the guest may be holding from this mode. */
                idd_flush_held_keys(d);
            }
            idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys, d->game_mode);
            idd_log(d, d->transmit_hotkeys
                        ? L"Transmit Keyboard Hotkeys: ON."
                        : L"Transmit Keyboard Hotkeys: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_GAME_MODE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->game_mode = !d->game_mode;
            if (sysmenu)
                CheckMenuItem(sysmenu, IDM_GAME_MODE,
                              MF_BYCOMMAND | (d->game_mode ? MF_CHECKED : MF_UNCHECKED));
            if (!d->game_mode && d->capture_auto) idd_end_capture(d);
            idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys, d->game_mode);
            idd_log(d, d->game_mode ? L"Game Mode: ON." : L"Game Mode: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_CAPTURE_MOUSE) {
            d->input_focused = TRUE;   /* the menu was just used on this window */
            idd_begin_capture(d, FALSE);
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_BACKSTAGE) {
            /* its own window: the session runs hidden in the VM's session 0 */
            if (d->vm) vm_backstage_open(d->vm, d->hInstance);
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_SHOW_LOG) {
            /* Reveal the log window (hidden by default). Closing it via its
               own [X] just hides it again (see idd_log_proc WM_CLOSE), so
               this item can re-open it any number of times. */
            if (d->log_hwnd && IsWindow(d->log_hwnd)) {
                if (IsIconic(d->log_hwnd))
                    ShowWindow(d->log_hwnd, SW_RESTORE);
                else
                    ShowWindow(d->log_hwnd, SW_SHOW);
                BringWindowToTop(d->log_hwnd);
                SetForegroundWindow(d->log_hwnd);
            }
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_SHOW_CONSOLE) {
            /* vm_display_auto picks this up and swaps this window for the
               Hyper-V console. */
            InterlockedExchange(&d->switch_requested, 1);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            RemoveClipboardFormatListener(hwnd);

            /* Remove the hotkey hook and release any keys still held in the
               guest before tearing the window down. */
            idd_end_capture(d);
            idd_remove_kbd_hook(d);
            idd_flush_held_keys(d);

            /* Stop recv threads */
            d->stop = TRUE;

            /* Destroy clipboard module */
            if (d->clipboard) {
                vm_clipboard_destroy(d->clipboard);
                d->clipboard = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                if (d->audio_socket != INVALID_SOCKET) {
                    closesocket(d->audio_socket);
                    d->audio_socket = INVALID_SOCKET;
                }
                /* Leave the handle for vm_display_idd_destroy if the thread
                   hasn't exited yet: it must not free d under the thread. */
                if (WaitForSingleObject(d->audio_recv_thread, 2000) == WAIT_OBJECT_0) {
                    CloseHandle(d->audio_recv_thread);
                    d->audio_recv_thread = NULL;
                }
            }

            /* Wait briefly for recv thread to exit */
            if (d->recv_thread) {
                WaitForSingleObject(d->recv_thread, 2000);
                CloseHandle(d->recv_thread);
                d->recv_thread = NULL;
            }

            d->open = FALSE;

            /* Close the separate log window */
            if (d->log_hwnd && IsWindow(d->log_hwnd))
                DestroyWindow(d->log_hwnd);
            d->log_hwnd = NULL;
            d->log_list_hwnd = NULL;

            /* Clean up D3D11 */
            d3d_cleanup(d);

            /* Clean up guest cursor */
            if (d->guest_cursor) {
                DestroyCursor(d->guest_cursor);
                d->guest_cursor = NULL;
            }

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             1, (LPARAM)d->vm);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PRESENT);
        KillTimer(hwnd, IDT_INPUT);
        KillTimer(hwnd, IDT_RESIZE);
        if (d && d->captured) ClipCursor(NULL);
        if (d) idd_remove_kbd_hook(d);  /* safety net if WM_CLOSE was bypassed */
        if (d) d->hwnd = NULL;
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        RECT wr;
        /* Minimum: 320x180 client area */
        wr.left = 0; wr.top = 0; wr.right = 320; wr.bottom = 180;
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;
        /* Max: native frame size, for a guest whose display can't follow the window */
        if (d && !d->can_resize && d->frame_width > 0 && d->frame_height > 0) {
            wr.left = 0; wr.top = 0;
            wr.right = (LONG)d->frame_width; wr.bottom = (LONG)d->frame_height;
            AdjustWindowRectEx(&wr, style, FALSE, exstyle);
            mmi->ptMaxTrackSize.x = wr.right - wr.left;
            mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        }
        return 0;
    }

    case WM_SIZE:
        if (d) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (d->render_hwnd)
                MoveWindow(d->render_hwnd, 0, 0, rc.right, rc.bottom, TRUE);
            d3d_resize_swap_chain(d);
            idd_clip_cursor(d);
            idd_fab_place(d);
            if (wp != SIZE_MINIMIZED && rc.right > 0 && rc.bottom > 0)
                idd_resize_note_client(d, hwnd, (UINT)rc.right, (UINT)rc.bottom);
        }
        return 0;

    /* The guest changes size once at the end of a drag, not along the way. */
    case WM_ENTERSIZEMOVE:
        if (d) d->in_sizemove = TRUE;
        break;

    case WM_EXITSIZEMOVE:
        if (d) {
            d->in_sizemove = FALSE;
            idd_resize_step(d);
        }
        break;

    case WM_IDD_RESIZE_CHECK:
        if (d) idd_resize_step(d);
        return 0;

    case WM_MOVE:
        if (d) {
            idd_clip_cursor(d);
            idd_fab_place(d);
        }
        break;

    /* Raw mouse deltas while captured: sent as relative moves, unthrottled,
       so games get every count at the mouse's own polling rate. */
    case WM_INPUT:
        if (d && d->captured) {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE &&
                !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
                (ri.data.mouse.lLastX || ri.data.mouse.lLastY))
                send_input(d, INPUT_MOUSE_REL, (UINT32)ri.data.mouse.lLastX,
                           (UINT32)ri.data.mouse.lLastY, 0);
        }
        break;   /* DefWindowProc cleans up the raw input buffer */

    /* The guest showed or hid its cursor. A game hiding it means mouse-look:
       game mode captures on the next click in the window (not right away,
       since Windows also hides the pointer while you type). Showing it again
       (a menu, alt-tab in the guest) releases an automatic capture. */
    case WM_IDD_CURSOR_VIS:
        if (d && wp && d->captured && d->capture_auto)
            idd_end_capture(d);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (d) d3d_render_frame(d);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_INPUT && d) {
            flush_pending_mouse(d);
            if (d->pending_mouse)
                SetTimer(hwnd, IDT_INPUT, MOUSE_MOVE_MIN_INTERVAL_MS, NULL);
            return 0;
        }
        if (wp == IDT_RESIZE && d) {
            KillTimer(hwnd, IDT_RESIZE);
            idd_resize_step(d);
            return 0;
        }
        if (wp == IDT_PRESENT && d) {
            if (d->frame_dirty)
                d3d_render_frame(d);
            /* the tab hides while the mouse is captured */
            if (d->fab_hwnd && d->fab_shown != (!d->captured && !IsIconic(hwnd)))
                idd_fab_place(d);
        }
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) d3d_render_frame(d);
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (d && d->clipboard) {
            vm_clipboard_on_clipboard_update(d->clipboard);
        }
        return 0;

    case WM_CLIP_READER_APPLY:
        if (d && d->clipboard) {
            vm_clipboard_on_reader_apply(d->clipboard);
        }
        return 0;

    /* Posted by vm_display_idd_focus() from another thread to raise an
       already-open window. Runs on the window's own thread. Restores from
       minimized (the creation path never had to handle that) then brings
       the window forward; SetForegroundWindow succeeds because the user
       just clicked our foreground main window to trigger this. */
    case WM_IDD_FOCUS:
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        return 0;

    case WM_SETFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, TRUE);
        break;

    case WM_KILLFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, FALSE);
        break;

    /* Top-level activation gates keyboard forwarding. Tracking activation
       (not WM_KILLFOCUS) is correct because focus moves between this window
       and its render child without losing activation. Losing activation —
       e.g. the user clicks another window or Alt+Tabs away with a key held —
       flushes held keys so nothing sticks down in the guest. */
    case WM_ACTIVATE:
        if (d) {
            d->input_focused = (LOWORD(wp) != WA_INACTIVE);
            if (!d->input_focused) {
                idd_panel_close(d);
                idd_end_capture(d);
                idd_flush_held_keys(d);
            }
        }
        break;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
    case WM_MOUSEMOVE:
        if (d && d->captured) return 0;   /* relative moves come from WM_INPUT */
        if (d) {
            if (!d->tracking && d->render_hwnd) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = d->render_hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                d->tracking = TRUE;
            }
            d->mouse_in = TRUE;

            {
                UINT vx, vy;
                /* lp coords are relative to render child */
                window_to_vm_coords(d->render_hwnd,
                                    (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                                    d->frame_width, d->frame_height, &vx, &vy);
                d->pending_mouse_x = vx;
                d->pending_mouse_y = vy;
                d->pending_mouse = TRUE;
                flush_pending_mouse(d);
                if (d->pending_mouse)
                    SetTimer(hwnd, IDT_INPUT, MOUSE_MOVE_MIN_INTERVAL_MS, NULL);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (d) {
            d->mouse_in = FALSE;
            d->tracking = FALSE;
            d->pending_mouse = FALSE;
            KillTimer(hwnd, IDT_INPUT);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (d && d->captured)
                SetCursor(NULL);
            else if (d && d->guest_cursor)
                SetCursor(d->guest_cursor);
            else
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    /* ---- Mouse button/wheel forwarding (only when cursor is in render area) ---- */
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (d && d->mouse_in && !d->captured && d->game_mode && !d->cursor_visible)
            idd_begin_capture(d, TRUE);
        if (d && (d->mouse_in || d->captured)) {
            UINT32 button = msg == WM_LBUTTONDOWN ? INPUT_BTN_LEFT :
                            msg == WM_RBUTTONDOWN ? INPUT_BTN_RIGHT :
                            msg == WM_MBUTTONDOWN ? INPUT_BTN_MIDDLE :
                            GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? INPUT_BTN_X2 : INPUT_BTN_X1;
            if (msg == WM_LBUTTONDOWN && !d->captured && d->render_hwnd)
                SetCapture(d->render_hwnd);
            send_input(d, INPUT_MOUSE_BUTTON, button, 1, 0);
        }
        return msg == WM_XBUTTONDOWN ? TRUE : 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        if (d && (d->mouse_in || d->captured)) {
            UINT32 button = msg == WM_LBUTTONUP ? INPUT_BTN_LEFT :
                            msg == WM_RBUTTONUP ? INPUT_BTN_RIGHT :
                            msg == WM_MBUTTONUP ? INPUT_BTN_MIDDLE :
                            GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? INPUT_BTN_X2 : INPUT_BTN_X1;
            send_input(d, INPUT_MOUSE_BUTTON, button, 0, 0);
        }
        return msg == WM_XBUTTONUP ? TRUE : 0;

    case WM_MOUSEWHEEL:
        if (d && (d->mouse_in || d->captured))
            send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;
    case WM_MOUSEHWHEEL:
        if (d && (d->mouse_in || d->captured))
            send_input(d, INPUT_MOUSE_HWHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;

    /* ---- Keyboard input forwarding (gated on window activation) ----
       In Transmit mode reserved hotkeys are captured by the low-level hook
       and never reach here. The mode check below covers Default mode: a
       reserved hotkey is neither forwarded to the guest nor consumed — it
       falls through to DefWindowProc so the host handles it normally (this
       is what avoids the old stuck-key bug). Normal keys are forwarded while
       the window is the active foreground window. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        BOOL ext = (lp & (1 << 24)) != 0;
        BOOL up  = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
        if (!d) break;
        /* Ctrl+Alt releases a captured mouse. The keys already sent are
           released in the guest so neither sticks down there. */
        if (d->captured && !up && (wp == VK_CONTROL || wp == VK_MENU) &&
            (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000)) {
            idd_end_capture(d);
            idd_flush_held_keys(d);
            return 0;
        }
        if (!d->transmit_hotkeys && !d->captured &&
            idd_is_reserved_hotkey((DWORD)wp, (GetKeyState(VK_MENU) & 0x8000) != 0))
            break;  /* Default mode: let the host handle this hotkey. */
        /* A key only reaches this thread when our window has focus. If the
           activation that sets input_focused was missed (another of our
           windows being destroyed can take it), recover from the actual
           foreground window instead of dropping keys until a re-activate. */
        if (!d->input_focused && GetAncestor(GetForegroundWindow(), GA_ROOT) == hwnd)
            d->input_focused = TRUE;
        if (d->input_focused)
            idd_forward_key(d, (DWORD)wp, scan, ext, up);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Public API
 * ================================================================== */

static VmDisplayIdd *idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd,
                                BOOL switchable, const POINT *pos)
{
    VmDisplayIdd *d;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm           = vm;
    wcscpy_s(d->vm_name, 256, vm->name);
    wcscpy_s(d->vhdx_path, MAX_PATH, vm->vhdx_path);
    d->runtime_id   = vm->runtime_id;
    wcscpy_s(d->os_type, 32, vm->os_type);
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->switchable   = switchable;
    if (pos) { d->init_pos = *pos; d->has_init_pos = TRUE; }
    d->open         = TRUE;
    d->stop         = FALSE;
    d->input_socket       = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;
    d->frame_socket       = INVALID_SOCKET;
    /* The Linux guest's display (asb_drm) keeps its own size. */
    d->can_resize         = _wcsicmp(vm->os_type, L"Windows") == 0;
    d->clipboard          = NULL;

    /* Load the per-VM display setting, creating display_settings.json with
       the default (off) if this VM doesn't have one yet. The hook itself is
       installed later, on the window thread, once the window exists. */
    {
        BOOL game_mode = TRUE;
        d->transmit_hotkeys = idd_display_settings_load_or_create(vm->vhdx_path, &game_mode);
        d->game_mode = game_mode;
    }
    d->cursor_visible = TRUE;

    /* Initialize frame buffer at default resolution */
    d->frame_width  = DEFAULT_WIDTH;
    d->frame_height = DEFAULT_HEIGHT;
    d->frame_stride = DEFAULT_WIDTH * 4;
    d->frame_buf    = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         d->frame_stride * d->frame_height);
    if (!d->frame_buf) {
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    InitializeCriticalSection(&d->frame_cs);

    /* Start the window thread (which will then start the recv thread) */
    d->window_thread = CreateThread(NULL, 0, idd_window_thread_proc, d, 0, NULL);
    if (!d->window_thread) {
        ui_log(L"IDD: Failed to create window thread.");
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    return idd_create(vm, hInstance, main_hwnd, FALSE, NULL);
}

VmDisplayIdd *vm_display_idd_create_switchable(VmInstance *vm, HINSTANCE hInstance,
                                               HWND main_hwnd, const POINT *pos)
{
    return idd_create(vm, hInstance, main_hwnd, TRUE, pos);
}

BOOL vm_display_idd_take_switch_request(VmDisplayIdd *display)
{
    return display && InterlockedExchange(&display->switch_requested, 0) != 0;
}

BOOL vm_display_idd_get_pos(VmDisplayIdd *display, POINT *pos)
{
    RECT rc;
    if (!display || !pos || !display->hwnd || !GetWindowRect(display->hwnd, &rc))
        return FALSE;
    pos->x = rc.left;
    pos->y = rc.top;
    return TRUE;
}

void vm_display_idd_destroy(VmDisplayIdd *display)
{
    BOOL threads_alive = FALSE;
    if (!display) return;

    /* Signal stop */
    display->stop = TRUE;
    display->open = FALSE;

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(
                1, &display->window_thread, FALSE, 5000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
        CloseHandle(display->window_thread);
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        if (WaitForSingleObject(display->recv_thread, 10000) != WAIT_OBJECT_0)
            threads_alive = TRUE;
        CloseHandle(display->recv_thread);
    }

    /* Clipboard is cleaned up by WM_CLOSE handler, but guard */
    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    /* audio_recv_thread guard */
    if (display->audio_recv_thread) {
        if (display->audio_socket != INVALID_SOCKET) {
            closesocket(display->audio_socket);
            display->audio_socket = INVALID_SOCKET;
        }
        if (WaitForSingleObject(display->audio_recv_thread, 10000) != WAIT_OBJECT_0)
            threads_alive = TRUE;
        CloseHandle(display->audio_recv_thread);
    }

    /* A worker still running would read the struct after it is freed (the
       audio thread did exactly that and crashed the app). Leak it instead. */
    if (threads_alive) {
        ui_log(L"IDD: a display worker thread did not stop; keeping its state allocated.");
        return;
    }

    DeleteCriticalSection(&display->frame_cs);

    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    HeapFree(GetProcessHeap(), 0, display);
}

BOOL vm_display_idd_is_open(VmDisplayIdd *display)
{
    if (!display) return FALSE;
    return display->open && display->hwnd && IsWindow(display->hwnd);
}

void vm_display_idd_focus(VmDisplayIdd *display)
{
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd))
        return;
    /* Marshal to the window thread; that thread owns the window and runs
       the activation (restore-if-minimized + foreground) in WM_IDD_FOCUS. */
    PostMessageW(display->hwnd, WM_IDD_FOCUS, 0, 0);
}

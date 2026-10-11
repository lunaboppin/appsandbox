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
#include <dwmapi.h>

#include <stdio.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "asb_core.h"
#include "vm_clipboard.h"
#include "vm_agent.h"
#include "hcs_vm.h"
#include "ui.h"
#include "resource.h"

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

/* Guest -> host control frames share the frame stream: a frame header with
   AUDIO_CTRL_FLAG set carries flags instead of a PCM length (real frames
   are capped at 4 MiB, far below the flag bit). */
#define AUDIO_CTRL_FLAG           0x80000000u
#define AUDIO_CTRL_MIC_WANTED     0x00000001u  /* a guest app is recording */
#define AUDIO_CTRL_NO_MIC_SINK    0x00000002u  /* guest has no VB-CABLE */
#define AUDIO_CTRL_NO_SPEAKER     0x00000004u  /* guest has no playback device */

/* Host -> guest microphone stream on the same socket: one MicHeader (same
   layout as AudioHeader) per capture start, then AudioFrameHeader + PCM. */
#define MIC_HEADER_MAGIC          0x314D5341u  /* 'ASM1' */

/* Clipboard reader-apply message (posted by vm_clipboard.c to our wndproc) */
#define WM_CLIP_READER_APPLY (WM_APP + 11)

/* ---- Frame protocol constants ---- */

#define FRAME_MAGIC         0x52465341  /* "ASFR" little-endian */
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MAX_DIRTY_RECTS     64
/* Largest frame the guest may send (7680x4320 BGRA); the receive buffer
   grows on demand up to this. */
#define MAX_FRAME_DATA_SIZE (7680u * 4320u * 4u)

/* ---- Input protocol (host → guest) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3
#define INPUT_MOUSE_RELATIVE 4
#define INPUT_DISPLAY_REFRESH 5   /* p1 = host monitor Hz, p3 = display index */
#define INPUT_DISPLAY_COUNT   6   /* p1 = number of guest monitors (1 or 2) */
#define INPUT_DISPLAY_RESIZE  7   /* p1 = width, p2 = height, p3 = display index */

/* Button IDs for INPUT_MOUSE_BUTTON */
#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2
#define INPUT_BTN_X1        3
#define INPUT_BTN_X2        4

#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" little-endian */
#define INPUT_READY_VHID    0x44494856  /* "VHID" little-endian */
/* Newer guest helpers: same capabilities plus normalized (0..65535) absolute moves. */
#define INPUT_READY_MAGIC2  0x32445249  /* "IRD2" */
#define INPUT_READY_VHID2   0x32444856  /* "VHD2" */

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
#define WM_IDD_INPUT_CAPS       (WM_USER + 102)
#define WM_IDD_CURSOR_POS       (WM_USER + 103)
#define WM_IDD_BADGE_MENU       (WM_USER + 104)
#define WM_IDD_DISPLAY_COUNT    (WM_USER + 105)

/* Timer for Present cadence when no frames arrive */
#define IDT_PRESENT     2001
#define PRESENT_MS      16   /* ~60 fps */
#define IDT_INPUT       2002
#define IDT_RESIZE      2003   /* debounce: resize guest after the window settles */
#define RESIZE_SETTLE_MS 350
#define MOUSE_MOVE_MIN_INTERVAL_MS 8
#define IMMERSIVE_MOUSE_MIN_INTERVAL_MS 2
#define IDH_IMMERSIVE_INPUT  1

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
    UINT         display_index;   /* 0 or 1; maps to HvSocket channel 2 or 8 */
    BOOL         auxiliary;       /* display 2: frames only, shares primary I/O */
    struct VmDisplayIdd *primary;
    struct VmDisplayIdd *secondary;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    wchar_t      os_type[32];      /* copy of vm->os_type for picking the service GUID variant */
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    volatile BOOL open;
    volatile BOOL stop;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    UINT                     tex_width, tex_height;  /* frame_tex size */
    ID3D11ShaderResourceView *frame_srv;
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
    volatile LONG  frame_ready_posted; /* coalesce receive-thread UI messages */
    volatile BOOL  has_frame;     /* display 2 stays hidden until first frame */

    UINT           render_count;     /* number of renders (for one-shot logging) */
    volatile UINT  recv_count;       /* number of frames received over HvSocket */

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    CRITICAL_SECTION input_send_cs; /* Display 1 + 2 share one packet stream */
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */
    BOOL           pending_mouse;   /* latest move waiting for the rate-limit timer */
    UINT           pending_mouse_x;
    UINT           pending_mouse_y;
    DWORD          last_mouse_move_tick;
    BOOL           have_last_mouse_move_tick;
    volatile BOOL  input_vhid_ready; /* helper has a real virtual HID mouse */
    volatile BOOL  input_normalized; /* helper takes 0..65535 absolute moves (newer guests) */
    BOOL           immersive_input; /* raw host mouse -> guest virtual HID */
    BOOL           immersive_resume; /* paused by focus loss; re-capture on return */
    UINT           sent_refresh_hz;  /* host monitor rate last sent to the guest */
    UINT           sent_width, sent_height; /* guest resolution last requested */
    int            display_count;    /* primary only: 1 or 2 guest monitors */
    BOOL           fullscreen;       /* borderless on the current monitor */
    WINDOWPLACEMENT fs_placement;    /* windowed placement to restore */
    HWND           immersive_badge;  /* Parsec-style icon: drag to move, click for menu */
    int            badge_fx, badge_fy; /* position in 1/1000 of the picture; -1 = default */
    BOOL           badge_hover;
    BOOL           badge_pressing;   /* left button went down on the badge */
    BOOL           badge_dragged;    /* ...and moved far enough to be a drag */
    POINT          badge_grab;       /* cursor offset inside the badge */
    POINT          badge_press;      /* screen point of the press */
    volatile LONG  pending_relative_x;
    volatile LONG  pending_relative_y;
    DWORD          last_relative_tick;
    BOOL           have_last_relative_tick;

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
    volatile LONG  cursor_x;        /* guest pointer hotspot, display pixels */
    volatile LONG  cursor_y;
    UINT32         cursor_xhot;     /* hotspot of the current shape */
    UINT32         cursor_yhot;

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
    HANDLE           mic_thread;      /* host capture -> guest, while wanted */
    volatile LONG    mic_stop;

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
#define IDM_IMMERSIVE_INPUT 0x1030
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
/* Cross-window ownership is a HWND, never a borrowed display-state pointer. */
static HWND volatile g_immersive_hwnd;

static VmDisplayIdd *idd_input_owner(VmDisplayIdd *d)
{
    return d && d->primary ? d->primary : d;
}

static BOOL idd_virtual_hid_ready(VmDisplayIdd *d)
{
    VmDisplayIdd *owner = idd_input_owner(d);
    return owner && owner->input_vhid_ready;
}

/* Immersive mode needs only a connected input channel. With the guest's
   virtual HID mouse (test-signed kernel driver: Secure Boot guests cannot
   load it) motion goes through that device; otherwise the guest helper
   injects the same relative motion with SendInput, which Raw Input readers
   such as games still receive. */
static BOOL idd_immersive_available(VmDisplayIdd *d)
{
    VmDisplayIdd *owner = idd_input_owner(d);
    return owner && owner->input_socket != INVALID_SOCKET;
}

static void send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    VmDisplayIdd *owner;
    SOCKET s;
    int ret;
    int err = 0;

    owner = idd_input_owner(d);
    if (!owner) return;
    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    EnterCriticalSection(&owner->input_send_cs);
    s = owner->input_socket;
    if (s == INVALID_SOCKET) {
        LeaveCriticalSection(&owner->input_send_cs);
        return;
    }
    /* A lost key/button-up leaves guest input stuck. Fail the connection on
       backpressure or a partial send; the helper releases held input on EOF. */
    ret = send(s, (const char *)&pkt, (int)sizeof(pkt), 0);
    if (ret == SOCKET_ERROR) {
        err = WSAGetLastError();
        /* Mark dead — recv thread owns the socket and will close + reconnect */
        owner->input_socket = INVALID_SOCKET;
    } else if (ret != (int)sizeof(pkt)) {
        /* Partial send on a stream socket: the guest reads fixed-size 20-byte
           InputPackets, so a truncated packet permanently misaligns the wire.
           Flag for reconnect so the channel resynchronises. */
        owner->input_socket = INVALID_SOCKET;
    }
    if (owner->input_socket == INVALID_SOCKET) shutdown(s, SD_BOTH);
    LeaveCriticalSection(&owner->input_send_cs);
    if (ret == SOCKET_ERROR) {
        idd_log(d, L"INPUT SEND ERR %d - flagging for reconnect.", err);
        return;
    }
    if (ret != (int)sizeof(pkt)) {
        idd_log(d, L"INPUT SEND short (%d/%d) - flagging for reconnect.",
                ret, (int)sizeof(pkt));
        return;
    }

    g_input_send_count++;

    /* Log non-move events only (moves are too noisy) */
    if (type != INPUT_MOUSE_MOVE && type != INPUT_MOUSE_RELATIVE) {
        static const wchar_t *type_names[] = {
            L"MOUSE_MOVE", L"MOUSE_BTN", L"MOUSE_WHEEL", L"KEY", L"MOUSE_REL",
            L"DISPLAY_HZ", L"DISPLAY_COUNT", L"DISPLAY_RESIZE"
        };
        const wchar_t *name = type < _countof(type_names) ? type_names[type] : L"?";
        idd_log(d, L"INPUT %s p1=%u p2=%u p3=%u (#%u)", name, p1, p2, p3, g_input_send_count);
    }
}

static void flush_pending_relative_mouse(VmDisplayIdd *d)
{
    DWORD now;
    LONG dx, dy;
    if (!d || !d->immersive_input) return;
    now = GetTickCount();
    if (d->have_last_relative_tick &&
        (DWORD)(now - d->last_relative_tick) < IMMERSIVE_MOUSE_MIN_INTERVAL_MS)
        return;
    dx = InterlockedExchange(&d->pending_relative_x, 0);
    dy = InterlockedExchange(&d->pending_relative_y, 0);
    if (!dx && !dy) return;
    send_input(d, INPUT_MOUSE_RELATIVE, (UINT32)dx, (UINT32)dy, 0);
    d->last_relative_tick = now;
    d->have_last_relative_tick = TRUE;
}

static void idd_update_immersive_menu(VmDisplayIdd *d)
{
    HMENU menu;
    if (!d || !d->hwnd) return;
    menu = GetSystemMenu(d->hwnd, FALSE);
    if (!menu) return;
    EnableMenuItem(menu, IDM_IMMERSIVE_INPUT,
                   MF_BYCOMMAND | (idd_immersive_available(d) ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, IDM_IMMERSIVE_INPUT,
                  MF_BYCOMMAND | (d->immersive_input ? MF_CHECKED : MF_UNCHECKED));
    DrawMenuBar(d->hwnd);
}

static void idd_sync_immersive_cursor(VmDisplayIdd *d);
static void idd_update_immersive_badge(VmDisplayIdd *d);
static void idd_prefs_save_display_count(const wchar_t *vhdx_path, int count);
static void idd_prefs_save_badge(const wchar_t *vhdx_path, UINT display_index, int x, int y);

/* Tell the guest the refresh rate of the host monitor this viewer is on so
   its virtual monitor switches to the closest mode (60-240 Hz). Sent when
   input connects, after a move/resize, and on display changes; only when
   the rate actually changed. */
static void idd_send_host_refresh(VmDisplayIdd *d, BOOL force)
{
    MONITORINFOEXW mi;
    DEVMODEW dm;
    HMONITOR mon;
    if (!d || !d->hwnd || !idd_immersive_available(d)) return;
    mon = MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST);
    ZeroMemory(&mi, sizeof(mi)); mi.cbSize = sizeof(mi);
    ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (!mon || !GetMonitorInfoW(mon, (MONITORINFO *)&mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1)
        return;
    if (!force && dm.dmDisplayFrequency == d->sent_refresh_hz) return;
    d->sent_refresh_hz = dm.dmDisplayFrequency;
    send_input(d, INPUT_DISPLAY_REFRESH, dm.dmDisplayFrequency, 0, d->display_index);
    idd_log(d, L"Host monitor %s runs at %lu Hz; asked guest Display %u to match.",
            mi.szDevice, dm.dmDisplayFrequency, d->display_index + 1);
}

/* Guest resolution: 1920x1080, or 2560x1440 once the viewer's picture area is
   larger than 1080p (snapped large, maximized, full screen). The picture is
   scaled to the window either way. Both modes always exist in the display
   driver, so this is an ordinary mode change in the guest. */
static void idd_request_guest_resize(VmDisplayIdd *d, BOOL force)
{
    RECT rc;
    UINT w, h;
    if (!d || !d->hwnd || !d->render_hwnd || !idd_immersive_available(d)) return;
    if (!IsWindowVisible(d->hwnd) || IsIconic(d->hwnd)) return;
    GetClientRect(d->render_hwnd, &rc);
    if (rc.right > 1920 || rc.bottom > 1080) { w = 2560; h = 1440; }
    else { w = 1920; h = 1080; }
    if (!force && w == d->sent_width && h == d->sent_height) return;
    d->sent_width = w;
    d->sent_height = h;
    send_input(d, INPUT_DISPLAY_RESIZE, w, h, d->display_index);
    idd_log(d, L"Asked guest Display %u for %ux%u.", d->display_index + 1, w, h);
}
/* Borderless full screen on the monitor the window is on; restores the
   previous windowed placement (Windows snapping keeps working windowed). */
static void idd_set_fullscreen(VmDisplayIdd *d, BOOL on)
{
    if (!d || !d->hwnd || on == d->fullscreen) return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        d->fs_placement.length = sizeof(d->fs_placement);
        GetWindowPlacement(d->hwnd, &d->fs_placement);
        if (!GetMonitorInfoW(MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtrW(d->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPos(d->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(d->hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPlacement(d->hwnd, &d->fs_placement);
        SetWindowPos(d->hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    d->fullscreen = on;
    idd_update_immersive_badge(d);
}

/* 1 or 2 guest monitors: the guest helper detaches/re-attaches the second
   virtual monitor, and the second viewer window hides/shows to match.
   Remembered per VM. */
static void idd_set_display_count(VmDisplayIdd *d, int count)
{
    VmDisplayIdd *owner = idd_input_owner(d);
    if (!owner) return;
    owner->display_count = count == 1 ? 1 : 2;
    idd_prefs_save_display_count(owner->vhdx_path, owner->display_count);
    send_input(owner, INPUT_DISPLAY_COUNT, (UINT32)owner->display_count, 0, 0);
    idd_log(owner, L"Guest displays: %d.", owner->display_count);
    if (owner->secondary && owner->secondary->hwnd)
        PostMessageW(owner->secondary->hwnd, WM_IDD_DISPLAY_COUNT, 0, 0);
}

#define IMMERSIVE_BADGE_SIZE 38
#define IMMERSIVE_BADGE_ICON 24
#define IMMERSIVE_BADGE_DRAG_PX 4

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#define ASB_DWMWCP_ROUND 2

/* Popup menus follow the Windows light/dark setting. uxtheme exports
   SetPreferredAppMode / AllowDarkModeForWindow / FlushMenuThemes only by
   ordinal (135 / 133 / 136); they are absent on old builds, so each is
   optional. */
static void idd_enable_dark_menus(HWND hwnd)
{
    typedef int  (WINAPI *SetPreferredAppModeFn)(int);
    typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
    typedef void (WINAPI *FlushMenuThemesFn)(void);
    static BOOL done;
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    if (!done) {
        SetPreferredAppModeFn set_mode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        FlushMenuThemesFn flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (set_mode) set_mode(1 /* AllowDark: follow the system setting */);
        if (flush) flush();
        done = TRUE;
    }
    {
        AllowDarkModeForWindowFn allow = (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        if (allow) allow(hwnd, TRUE);
    }
}

static void idd_update_immersive_badge(VmDisplayIdd *d);
static void idd_badge_drag_end(VmDisplayIdd *d);
static void idd_drag_immersive_badge(VmDisplayIdd *d, POINT cursor);

/* Parsec-style control: the AppSandbox icon floats over the picture (top
   left by default; position remembered per display). Drag it to move it;
   click it for the menu (immersive mouse/keyboard, displays, full screen,
   audio, log). Keyboard capture can send Alt+Tab to the guest and the mouse
   is clipped in immersive mode, so it is also the always-available way out.
   An owned popup (not a child) so it draws above the flip-model swap chain;
   it never activates. In immersive mode legacy mouse messages are off, so
   its clicks arrive through raw input (WM_INPUT) instead of this proc. */
static LRESULT CALLBACK idd_badge_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        HBRUSH bg = CreateSolidBrush(d && (d->badge_hover || d->badge_pressing)
                                     ? RGB(62, 62, 66) : RGB(32, 32, 34));
        HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPSANDBOX),
                                       IMAGE_ICON, IMMERSIVE_BADGE_ICON, IMMERSIVE_BADGE_ICON, 0);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        if (icon) {
            DrawIconEx(dc, (rc.right - IMMERSIVE_BADGE_ICON) / 2, (rc.bottom - IMMERSIVE_BADGE_ICON) / 2,
                       icon, IMMERSIVE_BADGE_ICON, IMMERSIVE_BADGE_ICON, 0, NULL, DI_NORMAL);
            DestroyIcon(icon);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    case WM_LBUTTONDOWN:
        if (d) {
            RECT r;
            SetCapture(hwnd);
            GetCursorPos(&d->badge_press);
            GetWindowRect(hwnd, &r);
            d->badge_grab.x = d->badge_press.x - r.left;
            d->badge_grab.y = d->badge_press.y - r.top;
            d->badge_pressing = TRUE;
            d->badge_dragged = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (d && d->badge_pressing && GetCapture() == hwnd) {
            POINT pt;
            GetCursorPos(&pt);
            idd_drag_immersive_badge(d, pt);
        } else if (d && !d->badge_hover) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            d->badge_hover = TRUE;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d) { d->badge_hover = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (d && d->badge_pressing) {
            ReleaseCapture();
            d->badge_pressing = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
            if (d->badge_dragged) idd_badge_drag_end(d);
            else PostMessageW(d->hwnd, WM_IDD_BADGE_MENU, 0, 0);
        }
        return 0;
    case WM_RBUTTONUP:
        if (d) PostMessageW(d->hwnd, WM_IDD_BADGE_MENU, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Show the badge whenever its display window is visible; keep it inside the
   picture (stored position is relative, so it survives resizes). */
static void idd_update_immersive_badge(VmDisplayIdd *d)
{
    static BOOL registered;
    RECT rc;
    POINT pt;
    if (!d || !d->hwnd || !d->render_hwnd) return;
    if (!IsWindowVisible(d->hwnd) || IsIconic(d->hwnd)) {
        if (d->immersive_badge) ShowWindow(d->immersive_badge, SW_HIDE);
        return;
    }
    if (!d->immersive_badge) {
        BOOL dark = TRUE;
        int corner = ASB_DWMWCP_ROUND;
        COLORREF border = RGB(70, 70, 76);
        if (!registered) {
            WNDCLASSEXW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = idd_badge_proc;
            wc.hInstance = d->hInstance;
            wc.lpszClassName = L"AppSandboxIddBadge";
            registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        d->immersive_badge = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"AppSandboxIddBadge", L"AppSandbox",
            WS_POPUP, 0, 0, IMMERSIVE_BADGE_SIZE, IMMERSIVE_BADGE_SIZE,
            d->hwnd, NULL, d->hInstance, NULL);
        if (!d->immersive_badge) return;
        SetWindowLongPtrW(d->immersive_badge, GWLP_USERDATA, (LONG_PTR)d);
        /* Windows 11 rounds and shadows it; older builds ignore these. */
        DwmSetWindowAttribute(d->immersive_badge, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        DwmSetWindowAttribute(d->immersive_badge, DWMWA_BORDER_COLOR, &border, sizeof(border));
        DwmSetWindowAttribute(d->immersive_badge, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    GetClientRect(d->render_hwnd, &rc);
    pt.x = d->badge_fx >= 0 ? MulDiv(d->badge_fx, rc.right - IMMERSIVE_BADGE_SIZE, 1000) : 10;
    pt.y = d->badge_fy >= 0 ? MulDiv(d->badge_fy, rc.bottom - IMMERSIVE_BADGE_SIZE, 1000) : 10;
    if (pt.x > rc.right - IMMERSIVE_BADGE_SIZE) pt.x = rc.right - IMMERSIVE_BADGE_SIZE;
    if (pt.y > rc.bottom - IMMERSIVE_BADGE_SIZE) pt.y = rc.bottom - IMMERSIVE_BADGE_SIZE;
    if (pt.x < 0) pt.x = 0;
    if (pt.y < 0) pt.y = 0;
    ClientToScreen(d->render_hwnd, &pt);
    SetWindowPos(d->immersive_badge, HWND_TOP, pt.x, pt.y,
                 IMMERSIVE_BADGE_SIZE, IMMERSIVE_BADGE_SIZE,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* TRUE if the (guest-synced) cursor is over the visible badge. */
static BOOL idd_cursor_on_immersive_badge(VmDisplayIdd *d)
{
    RECT r;
    POINT pt;
    return d && d->immersive_badge && IsWindowVisible(d->immersive_badge) &&
           GetWindowRect(d->immersive_badge, &r) && GetCursorPos(&pt) &&
           PtInRect(&r, pt);
}

/* While the left button is held on the badge, it follows the cursor once the
   cursor has moved past a small threshold (otherwise it is a click). */
static void idd_drag_immersive_badge(VmDisplayIdd *d, POINT cursor)
{
    POINT pos;
    RECT rc;
    int span_x, span_y;
    if (!d->badge_pressing || !d->immersive_badge) return;
    if (!d->badge_dragged &&
        abs(cursor.x - d->badge_press.x) <= IMMERSIVE_BADGE_DRAG_PX &&
        abs(cursor.y - d->badge_press.y) <= IMMERSIVE_BADGE_DRAG_PX)
        return;
    d->badge_dragged = TRUE;
    pos.x = cursor.x - d->badge_grab.x;
    pos.y = cursor.y - d->badge_grab.y;
    ScreenToClient(d->render_hwnd, &pos);
    GetClientRect(d->render_hwnd, &rc);
    span_x = rc.right - IMMERSIVE_BADGE_SIZE;
    span_y = rc.bottom - IMMERSIVE_BADGE_SIZE;
    d->badge_fx = span_x > 0 ? MulDiv(max(0, min(pos.x, span_x)), 1000, span_x) : 0;
    d->badge_fy = span_y > 0 ? MulDiv(max(0, min(pos.y, span_y)), 1000, span_y) : 0;
    idd_update_immersive_badge(d);
}

static void idd_badge_drag_end(VmDisplayIdd *d)
{
    idd_prefs_save_badge(d->vhdx_path, d->display_index, d->badge_fx, d->badge_fy);
}
/* Window title: VM/display, frame size, receive counter, and the immersive
   state so the user can tell whether the mouse is captured or paused. */
static void idd_update_title(VmDisplayIdd *d)
{
    wchar_t title[300];
    if (!d || !d->hwnd) return;
    swprintf_s(title, _countof(title), L"%s%s Display %u %ux%u recv=%u%s",
               d->audio_muted ? L"\U0001F507 " : L"",
               d->vm_name, d->display_index + 1,
               d->frame_width, d->frame_height, d->recv_count,
               d->immersive_input ? L" — Immersive (Ctrl+Alt+Shift+I to exit)"
               : d->immersive_resume ? L" — Immersive paused (click to resume)" : L"");
    SetWindowTextW(d->hwnd, title);
}

static void idd_set_immersive_input(VmDisplayIdd *d, BOOL enabled)
{
    RAWINPUTDEVICE rid;
    RECT clip;
    POINT tl, br;
    if (!d || d->immersive_input == enabled) return;

    if (enabled) {
        if (!idd_immersive_available(d) || !d->render_hwnd) {
            idd_log(d, L"Immersive input requires the guest input channel.");
            return;
        }
        HWND current = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&g_immersive_hwnd, NULL, NULL);
        if (current && current != d->hwnd) {
            DWORD_PTR ignored;
            SendMessageTimeoutW(current, WM_SYSCOMMAND,
                                IDM_IMMERSIVE_INPUT, 0,
                                SMTO_ABORTIFHUNG, 1000, &ignored);
            current = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&g_immersive_hwnd, NULL, NULL);
            if (current && current != d->hwnd) {
                idd_log(d, L"Another display still owns immersive input.");
                return;
            }
        }
        ZeroMemory(&rid, sizeof(rid));
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE;
        rid.hwndTarget = d->hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            idd_log(d, L"Raw mouse registration failed (err %lu).", GetLastError());
            return;
        }
        GetClientRect(d->render_hwnd, &clip);
        tl.x = clip.left; tl.y = clip.top;
        br.x = clip.right; br.y = clip.bottom;
        ClientToScreen(d->render_hwnd, &tl);
        ClientToScreen(d->render_hwnd, &br);
        clip.left = tl.x; clip.top = tl.y;
        clip.right = br.x; clip.bottom = br.y;
        ClipCursor(&clip);
        SetCapture(d->render_hwnd);
        d->immersive_input = TRUE;
        d->mouse_in = TRUE;
        InterlockedExchangePointer((PVOID volatile *)&g_immersive_hwnd, d->hwnd);
        idd_sync_immersive_cursor(d);
        idd_log(d, idd_virtual_hid_ready(d)
                ? L"Immersive input ON (raw host mouse -> guest virtual HID). Hotkey: Ctrl+Alt+Shift+I."
                : L"Immersive input ON (raw host mouse -> guest SendInput relative). Hotkey: Ctrl+Alt+Shift+I.");
    } else {
        ZeroMemory(&rid, sizeof(rid));
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        ClipCursor(NULL);
        ReleaseCapture();
        d->immersive_input = FALSE;
        d->badge_pressing = FALSE;
        InterlockedExchange(&d->pending_relative_x, 0);
        InterlockedExchange(&d->pending_relative_y, 0);
        d->have_last_relative_tick = FALSE;
        send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
        send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
        send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
        send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_X1, 0, 0);
        send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_X2, 0, 0);
        InterlockedCompareExchangePointer((PVOID volatile *)&g_immersive_hwnd, NULL, d->hwnd);
        if (d->render_hwnd) InvalidateRect(d->render_hwnd, NULL, FALSE);
        idd_log(d, L"Immersive input OFF.");
    }
    idd_update_immersive_menu(d);
    idd_update_title(d);
    idd_update_immersive_badge(d);
}

/* Mouse messages can arrive much faster than the guest display refreshes.
   Keep only the latest position and send it at a bounded rate so the window
   thread and the guest input helper cannot be flooded by cursor movement. */
static void flush_pending_mouse(VmDisplayIdd *d)
{
    DWORD now;

    if (!d) return;
    if (d->immersive_input) {
        flush_pending_relative_mouse(d);
        return;
    }
    if (!d->pending_mouse || !d->mouse_in)
        return;
    now = GetTickCount();
    if (d->have_last_mouse_move_tick &&
        (DWORD)(now - d->last_mouse_move_tick) < MOUSE_MOVE_MIN_INTERVAL_MS)
        return;
    /* Normalized 0..65535 across the guest frame, so the guest helper maps
       correctly at any resolution, including mid-resize. A guest whose tools
       were not refreshed yet expects frame pixels (it assumes 1920x1080). */
    if (idd_input_owner(d)->input_normalized)
        send_input(d, INPUT_MOUSE_MOVE,
                   (UINT32)((UINT64)d->pending_mouse_x * 65535u / (d->frame_width > 1 ? d->frame_width - 1 : 1)),
                   (UINT32)((UINT64)d->pending_mouse_y * 65535u / (d->frame_height > 1 ? d->frame_height - 1 : 1)),
                   d->display_index);
    else
        send_input(d, INPUT_MOUSE_MOVE, d->pending_mouse_x, d->pending_mouse_y, d->display_index);
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

/* Per-VM viewer preferences (display_settings.json beside the VHDX). Both
   display windows read-modify-write it, so access is serialized. Badge
   positions are stored in thousandths of the picture so they survive window
   resizes; -1 = default corner. */
typedef struct {
    int transmit_hotkeys;
    int display_count;         /* 1 or 2 guest monitors */
    int badge_x[2], badge_y[2];
} ViewerPrefs;

static SRWLOCK g_prefs_lock = SRWLOCK_INIT;

static int prefs_int(const char *json, const char *key, int fallback)
{
    char pattern[64];
    const char *p;
    sprintf_s(pattern, sizeof(pattern), "\"%s\":", key);
    p = strstr(json, pattern);
    return p ? atoi(p + strlen(pattern)) : fallback;
}

static void prefs_read(const wchar_t *vhdx_path, ViewerPrefs *p, BOOL *exists)
{
    wchar_t path[MAX_PATH];
    char buf[512] = {0};
    FILE *f;
    p->transmit_hotkeys = 0;
    p->display_count = 2;
    p->badge_x[0] = p->badge_y[0] = p->badge_x[1] = p->badge_y[1] = -1;
    if (exists) *exists = FALSE;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    if (exists) *exists = TRUE;
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    p->transmit_hotkeys = prefs_int(buf, "transmitKeyboardHotkeys", 0) != 0;
    p->display_count = prefs_int(buf, "displayCount", 2) == 1 ? 1 : 2;
    p->badge_x[0] = prefs_int(buf, "badge1X", -1);
    p->badge_y[0] = prefs_int(buf, "badge1Y", -1);
    p->badge_x[1] = prefs_int(buf, "badge2X", -1);
    p->badge_y[1] = prefs_int(buf, "badge2Y", -1);
}

static void prefs_write(const wchar_t *vhdx_path, const ViewerPrefs *p)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"transmitKeyboardHotkeys\":%d,\"displayCount\":%d,"
               "\"badge1X\":%d,\"badge1Y\":%d,\"badge2X\":%d,\"badge2Y\":%d}\n",
            p->transmit_hotkeys ? 1 : 0, p->display_count,
            p->badge_x[0], p->badge_y[0], p->badge_x[1], p->badge_y[1]);
    fclose(f);
}

static void idd_display_settings_save(const wchar_t *vhdx_path, BOOL transmit_hotkeys)
{
    ViewerPrefs p;
    AcquireSRWLockExclusive(&g_prefs_lock);
    prefs_read(vhdx_path, &p, NULL);
    p.transmit_hotkeys = transmit_hotkeys;
    prefs_write(vhdx_path, &p);
    ReleaseSRWLockExclusive(&g_prefs_lock);
}

/* Read the persisted setting; if the file is absent, create it with the
   default (off) and return FALSE. Returns the transmit-hotkeys value. */
static BOOL idd_display_settings_load_or_create(const wchar_t *vhdx_path)
{
    ViewerPrefs p;
    BOOL exists;
    AcquireSRWLockExclusive(&g_prefs_lock);
    prefs_read(vhdx_path, &p, &exists);
    /* Lazy creation: file doesn't exist yet (new or pre-existing VM). */
    if (!exists) prefs_write(vhdx_path, &p);
    ReleaseSRWLockExclusive(&g_prefs_lock);
    return p.transmit_hotkeys;
}

static ViewerPrefs idd_prefs_load(const wchar_t *vhdx_path)
{
    ViewerPrefs p;
    AcquireSRWLockShared(&g_prefs_lock);
    prefs_read(vhdx_path, &p, NULL);
    ReleaseSRWLockShared(&g_prefs_lock);
    return p;
}

static void idd_prefs_save_display_count(const wchar_t *vhdx_path, int count)
{
    ViewerPrefs p;
    AcquireSRWLockExclusive(&g_prefs_lock);
    prefs_read(vhdx_path, &p, NULL);
    p.display_count = count == 1 ? 1 : 2;
    prefs_write(vhdx_path, &p);
    ReleaseSRWLockExclusive(&g_prefs_lock);
}

static void idd_prefs_save_badge(const wchar_t *vhdx_path, UINT display_index, int x, int y)
{
    ViewerPrefs p;
    if (display_index > 1) return;
    AcquireSRWLockExclusive(&g_prefs_lock);
    prefs_read(vhdx_path, &p, NULL);
    p.badge_x[display_index] = x;
    p.badge_y[display_index] = y;
    prefs_write(vhdx_path, &p);
    ReleaseSRWLockExclusive(&g_prefs_lock);
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
        d->input_focused && d->transmit_hotkeys) {
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

/* Immersive mode: the host cursor stands in for the guest's hardware cursor.
   Show the guest's shape (or nothing if the guest hid it, e.g. in a game) at
   the guest pointer's position. SetCursorPos produces no raw input, so the
   relative motion forwarded to the guest is unaffected. Window thread only. */
static void idd_sync_immersive_cursor(VmDisplayIdd *d)
{
    RECT rc;
    POINT pt;
    float vp_x, vp_y, vp_w, vp_h;
    if (!d || !d->immersive_input || !d->render_hwnd || !d->frame_width || !d->frame_height)
        return;
    GetClientRect(d->render_hwnd, &rc);
    compute_letterbox((UINT)rc.right, (UINT)rc.bottom, d->frame_width, d->frame_height,
                      &vp_x, &vp_y, &vp_w, &vp_h);
    pt.x = (LONG)(vp_x + (float)InterlockedCompareExchange(&d->cursor_x, 0, 0) * vp_w / (float)d->frame_width);
    pt.y = (LONG)(vp_y + (float)InterlockedCompareExchange(&d->cursor_y, 0, 0) * vp_h / (float)d->frame_height);
    if (pt.x < 0) pt.x = 0;
    if (pt.y < 0) pt.y = 0;
    if (pt.x >= rc.right) pt.x = rc.right - 1;
    if (pt.y >= rc.bottom) pt.y = rc.bottom - 1;
    ClientToScreen(d->render_hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
    SetCursor(d->cursor_visible && d->guest_cursor ? d->guest_cursor : NULL);
    idd_drag_immersive_badge(d, pt);
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

/* ==================================================================
 * Microphone passthrough — host -> guest
 * ================================================================== */

static const IID AUDIO_IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const PROPERTYKEY AUDIO_PKEY_Device_FriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const GUID AUDIO_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID AUDIO_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static UINT16 audio_wire_format_tag(const WAVEFORMATEX *wfx)
{
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        if (IsEqualGUID(&ext->SubFormat, &AUDIO_SUBTYPE_IEEE_FLOAT)) return WAVE_FORMAT_IEEE_FLOAT;
        if (IsEqualGUID(&ext->SubFormat, &AUDIO_SUBTYPE_PCM)) return WAVE_FORMAT_PCM;
    }
    return wfx->wFormatTag;
}

static BOOL send_all_socket(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* Enumerate active host capture endpoints (for the Settings device list). */
void idd_enum_capture_devices(void (*cb)(const wchar_t *id, const wchar_t *name, void *ud), void *ud)
{
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDeviceCollection *pColl = NULL;
    UINT count = 0, i;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &AUDIO_IID_IMMDeviceEnumerator, (void **)&pEnum)) &&
        SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pColl))) {
        IMMDeviceCollection_GetCount(pColl, &count);
        for (i = 0; i < count; i++) {
            IMMDevice *pDev = NULL;
            IPropertyStore *pProps = NULL;
            LPWSTR id = NULL;
            PROPVARIANT name;
            PropVariantInit(&name);
            if (FAILED(IMMDeviceCollection_Item(pColl, i, &pDev))) continue;
            if (SUCCEEDED(IMMDevice_GetId(pDev, &id)) &&
                SUCCEEDED(IMMDevice_OpenPropertyStore(pDev, STGM_READ, &pProps)) &&
                SUCCEEDED(IPropertyStore_GetValue(pProps, &AUDIO_PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR && name.pwszVal)
                cb(id, name.pwszVal, ud);
            PropVariantClear(&name);
            if (pProps) IPropertyStore_Release(pProps);
            if (id) CoTaskMemFree(id);
            IMMDevice_Release(pDev);
        }
    }
    if (pColl) IMMDeviceCollection_Release(pColl);
    if (pEnum) IMMDeviceEnumerator_Release(pEnum);
    if (SUCCEEDED(co)) CoUninitialize();
}

/* Capture the chosen host input (Windows' default when none is chosen) and
   stream it to the guest helper, which plays it into VB-CABLE's input so
   guest apps hear it on "CABLE Output". Runs only while the guest reports
   that an app is recording, so the host mic is not held open otherwise. */
static DWORD WINAPI mic_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    SOCKET s = d->audio_socket;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pAC = NULL;
    IAudioCaptureClient *pCC = NULL;
    WAVEFORMATEX *wfx = NULL;
    AudioHeader hdr;
    wchar_t device_id[512];
    BOOL enabled = FALSE, started = FALSE;
    static BYTE silence[16384];
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL com_ok = SUCCEEDED(hr);

    asb_get_mic_settings(&enabled, device_id, _countof(device_id));
    if (!enabled || s == INVALID_SOCKET) goto done;
    hr = CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &AUDIO_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) goto done;
    if (!device_id[0] || FAILED(IMMDeviceEnumerator_GetDevice(pEnum, device_id, &pDev))) {
        if (device_id[0]) idd_log(d, L"Mic: chosen input device missing; using the Windows default.");
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eCapture, eConsole, &pDev);
        if (FAILED(hr)) { idd_log(d, L"Mic: no host input device (0x%08lX).", hr); goto done; }
    }
    if (FAILED(hr = IMMDevice_Activate(pDev, &AUDIO_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC)) ||
        FAILED(hr = IAudioClient_GetMixFormat(pAC, &wfx)) ||
        FAILED(hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, wfx, NULL)) ||
        FAILED(hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioCaptureClient, (void **)&pCC)) ||
        FAILED(hr = IAudioClient_Start(pAC))) {
        idd_log(d, L"Mic: capture setup failed (0x%08lX).", hr);
        goto done;
    }
    started = TRUE;

    hdr.magic           = MIC_HEADER_MAGIC;
    hdr.sample_rate     = wfx->nSamplesPerSec;
    hdr.channels        = wfx->nChannels;
    hdr.bits_per_sample = wfx->wBitsPerSample;
    hdr.format_tag      = audio_wire_format_tag(wfx);
    hdr.block_align     = wfx->nBlockAlign;
    if (!send_all_socket(s, &hdr, sizeof(hdr))) goto done;
    idd_log(d, L"Mic: streaming host input to guest (%lu Hz, %u ch, %u bit).",
            hdr.sample_rate, hdr.channels, hdr.bits_per_sample);

    while (!InterlockedCompareExchange(&d->mic_stop, 0, 0) && !d->stop) {
        UINT32 packet = 0;
        Sleep(5);
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(pCC, &packet)) && packet) {
            BYTE *data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            AudioFrameHeader fh;
            if (FAILED(IAudioCaptureClient_GetBuffer(pCC, &data, &frames, &flags, NULL, NULL))) goto done;
            fh.bytes = frames * wfx->nBlockAlign;
            if (fh.bytes && fh.bytes < 4 * 1024 * 1024) {
                BOOL ok = send_all_socket(s, &fh, sizeof(fh));
                if (ok && (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    UINT32 left = fh.bytes;
                    while (ok && left) {
                        UINT32 chunk = left > sizeof(silence) ? (UINT32)sizeof(silence) : left;
                        ok = send_all_socket(s, silence, (int)chunk);
                        left -= chunk;
                    }
                } else if (ok) {
                    ok = send_all_socket(s, data, (int)fh.bytes);
                }
                if (!ok) { IAudioCaptureClient_ReleaseBuffer(pCC, frames); goto done; }
            }
            IAudioCaptureClient_ReleaseBuffer(pCC, frames);
        }
    }
done:
    if (started) IAudioClient_Stop(pAC);
    if (pCC) IAudioCaptureClient_Release(pCC);
    if (pAC) IAudioClient_Release(pAC);
    if (wfx) CoTaskMemFree(wfx);
    if (pDev) IMMDevice_Release(pDev);
    if (pEnum) IMMDeviceEnumerator_Release(pEnum);
    if (started) idd_log(d, L"Mic: stopped.");
    if (com_ok) CoUninitialize();
    return 0;
}

static void mic_stop_thread(VmDisplayIdd *d)
{
    if (!d->mic_thread) return;
    InterlockedExchange(&d->mic_stop, 1);
    WaitForSingleObject(d->mic_thread, 2000);
    CloseHandle(d->mic_thread);
    d->mic_thread = NULL;
}

static void mic_set_wanted(VmDisplayIdd *d, BOOL wanted)
{
    if (wanted && !d->mic_thread) {
        InterlockedExchange(&d->mic_stop, 0);
        d->mic_thread = CreateThread(NULL, 0, mic_thread_proc, d, 0, NULL);
    } else if (!wanted && d->mic_thread) {
        mic_stop_thread(d);
    } else if (wanted && d->mic_thread &&
               WaitForSingleObject(d->mic_thread, 0) == WAIT_OBJECT_0) {
        /* Previous capture ended on its own (device lost); start again. */
        mic_stop_thread(d);
        InterlockedExchange(&d->mic_stop, 0);
        d->mic_thread = CreateThread(NULL, 0, mic_thread_proc, d, 0, NULL);
    }
}

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

        /* sample_rate 0: the guest has no playback device to capture, but
           keeps the session for control frames (microphone passthrough). */
        if (!hdr.sample_rate) {
            idd_log(d, L"Audio: guest has no playback device; control channel only.");
            goto receive_loop;
        }

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

receive_loop:
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 padding;
            UINT32 free_frames;
            UINT32 frames_to_write;
            BYTE *dst;

            if (!recv_exact(s, &fh, sizeof(fh))) break;
            if (fh.bytes & AUDIO_CTRL_FLAG) {
                static BOOL warned_sink, warned_speaker;
                if ((fh.bytes & AUDIO_CTRL_NO_MIC_SINK) && !warned_sink) {
                    warned_sink = TRUE;
                    idd_log(d, L"Guest has no VB-CABLE: microphone passthrough is unavailable until it is installed in the VM.");
                }
                if ((fh.bytes & AUDIO_CTRL_NO_SPEAKER) && !warned_speaker) {
                    warned_speaker = TRUE;
                    idd_log(d, L"Guest has no playback device: install Hi-Fi Cable or VB-CABLE in the VM for sound.");
                }
                mic_set_wanted(d, (fh.bytes & AUDIO_CTRL_MIC_WANTED) != 0);
                continue;
            }
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

            /* Host-side mute (or no render stream): keep draining the socket */
            if (d->audio_muted || !pAC || !render_block_align) continue;

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
        mic_stop_thread(d);   /* it sends on this session's socket */
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
        /* Back off when the guest keeps rejecting us (VAD not ready) */
        Sleep(header_misses > 3 ? 5000 : 500);
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (com_ok)  CoUninitialize();
    d->audio_recv_thread = NULL;
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

/* (Re)create the CPU-writable frame texture + SRV at the guest frame size;
   the guest resolution follows the viewer window, so this changes at runtime. */
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
    d->tex_width = width;
    d->tex_height = height;
    return TRUE;
}

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    HRESULT hr;

    /* Create device and swap chain */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount                        = 2;
    scd.BufferDesc.Width                   = DEFAULT_WIDTH;
    scd.BufferDesc.Height                  = DEFAULT_HEIGHT;
    scd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = d->render_hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_FLIP_DISCARD;

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

    (void)td; (void)srv_desc;
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
    D3D11_MAPPED_SUBRESOURCE mapped;
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
        if ((d->frame_width != d->tex_width || d->frame_height != d->tex_height) &&
            !d3d_create_frame_texture(d, d->frame_width, d->frame_height)) {
            LeaveCriticalSection(&d->frame_cs);
            return;
        }
        hr = d->ctx->lpVtbl->Map(d->ctx,
                (ID3D11Resource *)d->frame_tex, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            UINT row;
            UINT copy_stride = d->frame_width * 4;
            if (copy_stride > mapped.RowPitch)
                copy_stride = mapped.RowPitch;
            if (copy_stride > d->frame_stride)
                copy_stride = d->frame_stride;

            for (row = 0; row < d->frame_height && row < d->tex_height; row++) {
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

    /* Refresh the title once per uploaded frame (~frame rate). */
    if (frame_uploaded) idd_update_title(d);
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

    /* Pace presentation to the desktop compositor instead of queueing frames
       as fast as the receiver can post them. */
    d->swap_chain->lpVtbl->Present(d->swap_chain, 1, 0);
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
    UINT32 recv_cap = DEFAULT_WIDTH * DEFAULT_HEIGHT * 4;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Receive buffer for frame pixel data; grows with the guest resolution. */
    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, recv_cap);
    if (!recv_buf) {
        ui_log(L"IDD recv: failed to allocate receive buffer");
        return 1;
    }

    /* The agent connection has one synchronous command slot.  Startup uses it
       for network and shared-resource configuration, so wait for that sequence
       to finish before asking the agent to respawn the console-session helper. */
    while (!d->auxiliary && !d->stop && d->vm && d->vm->agent_online &&
           d->vm->agent_initializing) {
        Sleep(100);
    }

    /* Tell the agent to respawn input helper in console session. */
    if (!d->auxiliary && !d->stop && d->vm && d->vm->agent_online &&
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
        const wchar_t *frame_step = L"connect";
        DWORD last_msg_tick = 0;

        /* Ensure input channel is connected (independent of frame channel) */
        if (!d->auxiliary && input_s == INVALID_SOCKET) {
            GUID svc; hcs_service_guid(d->os_type, 3, &svc);
            input_s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
            if (input_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(input_s, &ready_magic, sizeof(ready_magic)) &&
                    (ready_magic == INPUT_READY_MAGIC || ready_magic == INPUT_READY_VHID ||
                     ready_magic == INPUT_READY_MAGIC2 || ready_magic == INPUT_READY_VHID2)) {
                    DWORD zero_timeout = 0;
                    u_long nb = 1;
                    setsockopt(input_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                    ioctlsocket(input_s, FIONBIO, &nb);
                    d->input_socket = input_s;
                    d->input_vhid_ready = ready_magic == INPUT_READY_VHID || ready_magic == INPUT_READY_VHID2;
                    d->input_normalized = ready_magic == INPUT_READY_MAGIC2 || ready_magic == INPUT_READY_VHID2;
                    if (d->hwnd) PostMessageW(d->hwnd, WM_IDD_INPUT_CAPS, 0, 0);
                    g_input_send_count = 0;
                    idd_log(d, d->input_vhid_ready
                               ? L"Input connected: virtual HID immersive mode available."
                               : L"Input connected in legacy compatibility mode.");
                } else {
                    idd_log(d, L"Input handshake failed - closing.");
                    closesocket(input_s);
                    input_s = INVALID_SOCKET;
                }
            }
        }

        /* Clipboard module (handles :0005 + :0006 internally) */
        if (!d->auxiliary && !d->clipboard) {
            d->clipboard = vm_clipboard_create(&d->runtime_id, d->os_type,
                                               d->hwnd, clip_log_callback, d);
            if (d->clipboard)
                idd_log(d, L"Clipboard module created.");
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->auxiliary && !d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        {
            GUID svc;
            hcs_service_guid(d->os_type, d->display_index == 0 ? 2 : 8, &svc);
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

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        frame_step = L"select";
        last_msg_tick = GetTickCount();
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;

            /* Wait for the next message without a deadline: the guest sends
               nothing while the desktop is static (no keepalive frames), and
               the socket's 5 s receive timeout used to turn every quiet
               moment into a reconnect -- dropping cursor updates and forcing
               a full-frame resend. The timeout still guards stalls inside a
               message. Idle wakeups still service the input channel. */
            {
                fd_set rfds;
                struct timeval tv = { 1, 0 };
                int ready;
                FD_ZERO(&rfds);
                FD_SET(s, &rfds);
                frame_step = L"select";
                ready = select(0, &rfds, NULL, NULL, &tv);
                if (ready == SOCKET_ERROR) break;
                if (ready == 0) goto input_maintenance;
            }

            /* Peek at magic to determine message type */
            frame_step = L"magic";
            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                frame_step = L"cursor";
                last_msg_tick = GetTickCount();
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;

                d->cursor_visible = chdr.visible;
                if (chdr.shape_updated) {
                    d->cursor_xhot = chdr.xhot;
                    d->cursor_yhot = chdr.yhot;
                }
                /* IddCx reports the shape's top-left; the pointer tip is the
                   hotspot. Immersive mode mirrors it with the host cursor. */
                InterlockedExchange(&d->cursor_x, chdr.x + (LONG)d->cursor_xhot);
                InterlockedExchange(&d->cursor_y, chdr.y + (LONG)d->cursor_yhot);
                if (d->immersive_input && d->hwnd)
                    PostMessageW(d->hwnd, WM_IDD_CURSOR_POS, 0, 0);

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

            if (magic != FRAME_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }

            /* Read rest of frame header (already read magic) */
            hdr.magic = magic;
            frame_step = L"frame";
            last_msg_tick = GetTickCount();
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
                break;

            /* Sanity checks */
            if (hdr.width == 0 || hdr.height == 0 ||
                hdr.width > 7680 || hdr.height > 4320 ||
                hdr.stride < hdr.width * 4) {
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

            if (data_size > MAX_FRAME_DATA_SIZE) {
                idd_log(d, L"Frame data too large (%u bytes), reconnecting.", data_size);
                break;
            }

            /* Read pixel data */
            if (data_size > recv_cap) {
                BYTE *grown = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, recv_buf, data_size);
                if (!grown) { idd_log(d, L"Frame buffer allocation failed (%u bytes).", data_size); break; }
                recv_buf = grown;
                recv_cap = data_size;
            }
            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
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
                    idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                            hdr.width, hdr.height, hdr.stride);
                    idd_log(d, L"Resolution changed to %ux%u.", hdr.width, hdr.height);
                } else {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
            }

            if (rect_count == 0) {
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
            d->has_frame = TRUE;
            d->recv_count++;
            LeaveCriticalSection(&d->frame_cs);

            /* Signal the window thread to repaint */
            if (d->hwnd && IsWindow(d->hwnd) &&
                InterlockedExchange(&d->frame_ready_posted, 1) == 0)
                PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);

        input_maintenance:
            /* Reconnect input socket if send_input flagged it dead */
            if (!d->auxiliary && d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
                d->input_vhid_ready = FALSE;
                if (d->hwnd) PostMessageW(d->hwnd, WM_IDD_INPUT_CAPS, 0, 0);
                idd_log(d, L"Input socket closed, will reconnect...");
            }
            if (!d->auxiliary && input_s == INVALID_SOCKET) {
                GUID svc; hcs_service_guid(d->os_type, 3, &svc);
                SOCKET new_s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
                if (new_s != INVALID_SOCKET) {
                    UINT32 ready_magic = 0;
                    if (recv_exact(new_s, &ready_magic, sizeof(ready_magic)) &&
                        (ready_magic == INPUT_READY_MAGIC || ready_magic == INPUT_READY_VHID ||
                         ready_magic == INPUT_READY_MAGIC2 || ready_magic == INPUT_READY_VHID2)) {
                        DWORD zero_timeout = 0;
                        u_long nb = 1;
                        setsockopt(new_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                        ioctlsocket(new_s, FIONBIO, &nb);
                        input_s = new_s;
                        d->input_socket = new_s;
                        d->input_vhid_ready = ready_magic == INPUT_READY_VHID || ready_magic == INPUT_READY_VHID2;
                    d->input_normalized = ready_magic == INPUT_READY_MAGIC2 || ready_magic == INPUT_READY_VHID2;
                        if (d->hwnd) PostMessageW(d->hwnd, WM_IDD_INPUT_CAPS, 0, 0);
                        g_input_send_count = 0;
                        idd_log(d, d->input_vhid_ready
                                   ? L"Input reconnected: virtual HID immersive mode available."
                                   : L"Input reconnected in legacy compatibility mode.");
                    } else {
                        closesocket(new_s);
                    }
                }
                /* If connect/handshake fails, will retry next frame */
            }
        }

        /* Frame channel lost — close it but keep input alive */
        {
            int frame_wsa = WSAGetLastError();
            closesocket(s);
            idd_log(d, L"Frame channel disconnected at %s (WSA %d, %lu ms since last message), reconnecting...",
                    frame_step, frame_wsa, GetTickCount() - last_msg_tick);
        }

        /* Check if input is still alive (send_input may have flagged it dead) */
        if (!d->auxiliary && d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
            closesocket(input_s);
            input_s = INVALID_SOCKET;
            d->input_vhid_ready = FALSE;
            if (d->hwnd) PostMessageW(d->hwnd, WM_IDD_INPUT_CAPS, 0, 0);
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
    if (!d->auxiliary) {
        d->input_socket = INVALID_SOCKET;
        d->input_vhid_ready = FALSE;
        if (d->hwnd) PostMessageW(d->hwnd, WM_IDD_INPUT_CAPS, 0, 0);
    }
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

    swprintf_s(title, 300, L"%s - Display %u", d->vm_name,
               d->display_index + 1);

    /* Compute outer window size so the client area is exactly 1920x1080 */
    {
        DWORD style   = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        if (!d->auxiliary) style |= WS_VISIBLE;
        DWORD exstyle = 0;
        RECT wr = { 0, 0, 1920, 1080 };
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);

        d->hwnd = CreateWindowExW(
            exstyle, IDD_DISPLAY_CLASS, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
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
            AppendMenuW(sysmenu, d->auxiliary ? (MF_STRING | MF_GRAYED) : MF_STRING,
                        IDM_AUDIO_MUTE, L"Mute audio");
            AppendMenuW(sysmenu, MF_STRING, IDM_XMIT_HOTKEYS, L"Transmit Keyboard Hotkeys");
            AppendMenuW(sysmenu, MF_STRING | MF_GRAYED, IDM_IMMERSIVE_INPUT,
                        L"Immersive Mouse Input\tCtrl+Alt+Shift+I");
            AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_LOG, L"Show Log");
            CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                          MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    /* A deliberate escape chord works even while RIDEV_NOLEGACY suppresses
       ordinary mouse messages. */
    if (!d->auxiliary)
        RegisterHotKey(d->hwnd, IDH_IMMERSIVE_INPUT,
                       MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'I');
    idd_update_immersive_menu(d);

    /* Bring the display window to the foreground on open */
    if (!d->auxiliary) {
        ShowWindow(d->hwnd, SW_SHOW);
        BringWindowToTop(d->hwnd);
        SetForegroundWindow(d->hwnd);
    }

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
    idd_enable_dark_menus(d->hwnd);
    idd_update_immersive_badge(d);

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
        VmDisplayIdd *created = (VmDisplayIdd *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)created);
        if (!created || !created->auxiliary)
            AddClipboardFormatListener(hwnd);
        return 0;
    }

    d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_SYSCOMMAND:
        if (d && (wp & 0xFFF0) == IDM_IMMERSIVE_INPUT) {
            /* An explicit toggle (hotkey/menu) also cancels a paused resume. */
            BOOL enable = !d->immersive_input && !d->immersive_resume;
            d->immersive_resume = FALSE;
            if (d->immersive_input || enable) idd_set_immersive_input(d, enable);
            else idd_update_title(d);
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_AUDIO_MUTE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            wchar_t title[300];
            d->audio_muted = !d->audio_muted;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_AUDIO_MUTE,
                              MF_BYCOMMAND | (d->audio_muted ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->audio_muted)
                swprintf_s(title, 300, L"\U0001F507 %s - Display %u",
                           d->vm_name, d->display_index + 1);
            else
                swprintf_s(title, 300, L"%s - Display %u",
                           d->vm_name, d->display_index + 1);
            SetWindowTextW(hwnd, title);
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
            } else {
                idd_remove_kbd_hook(d);
                /* Release anything the guest may be holding from this mode. */
                idd_flush_held_keys(d);
            }
            idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys);
            idd_log(d, d->transmit_hotkeys
                        ? L"Transmit Keyboard Hotkeys: ON."
                        : L"Transmit Keyboard Hotkeys: OFF.");
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
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            if (!d->auxiliary) RemoveClipboardFormatListener(hwnd);
            if (!d->auxiliary && d->secondary && d->secondary->hwnd)
                PostMessageW(d->secondary->hwnd, WM_CLOSE, 0, 0);

            /* Remove the hotkey hook and release any keys still held in the
               guest before tearing the window down. */
            idd_remove_kbd_hook(d);
            idd_flush_held_keys(d);
            idd_set_immersive_input(d, FALSE);

            /* Stop recv threads */
            d->stop = TRUE;

            /* Destroy clipboard module */
            if (d->clipboard) {
                vm_clipboard_destroy(d->clipboard);
                d->clipboard = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                DWORD audio_wait;
                if (d->audio_socket != INVALID_SOCKET) {
                    closesocket(d->audio_socket);
                    d->audio_socket = INVALID_SOCKET;
                }
                audio_wait = WaitForSingleObject(d->audio_recv_thread, 7000);
                if (audio_wait == WAIT_OBJECT_0) {
                    CloseHandle(d->audio_recv_thread);
                    d->audio_recv_thread = NULL;
                }
            }

            /* Wait briefly for recv thread to exit */
            if (d->recv_thread) {
                DWORD recv_wait = WaitForSingleObject(d->recv_thread, 7000);
                if (recv_wait == WAIT_OBJECT_0) {
                    CloseHandle(d->recv_thread);
                    d->recv_thread = NULL;
                }
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
            if (user_initiated && !d->auxiliary && d->main_hwnd && d->vm)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             1, (LPARAM)d->vm);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PRESENT);
        KillTimer(hwnd, IDT_INPUT);
        UnregisterHotKey(hwnd, IDH_IMMERSIVE_INPUT);
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
        /* No maximum: the guest resolution follows the window (resize,
           Windows snapping, maximize and full screen all work). */
        return 0;
    }

    case WM_SIZE:
        if (d) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (d->render_hwnd)
                MoveWindow(d->render_hwnd, 0, 0, rc.right, rc.bottom, TRUE);
            d3d_resize_swap_chain(d);
            idd_update_immersive_badge(d);
            if (wp != SIZE_MINIMIZED)
                SetTimer(hwnd, IDT_RESIZE, RESIZE_SETTLE_MS, NULL);
        }
        return 0;

    case WM_MOVE:
        if (d) idd_update_immersive_badge(d);
        break;

    /* Window dropped on another monitor, or the host changed modes. */
    case WM_EXITSIZEMOVE:
    case WM_DISPLAYCHANGE:
        if (d) idd_send_host_refresh(d, FALSE);
        break;

    /* Badge menu. Immersive capture is released while the menu is open (it
       needs a normal cursor) and restored afterwards unless the choice was
       to turn it off. */
    case WM_IDD_BADGE_MENU:
        if (d && d->immersive_badge) {
            enum { BM_MOUSE = 1, BM_KEYBOARD, BM_BOTH, BM_ONE_DISPLAY, BM_TWO_DISPLAYS,
                   BM_FULLSCREEN, BM_MUTE, BM_LOG };
            VmDisplayIdd *owner = idd_input_owner(d);
            BOOL was_immersive = d->immersive_input;
            BOOL keyboard = d->transmit_hotkeys;
            BOOL available = idd_immersive_available(d);
            BOOL restore_immersive;
            wchar_t header[300];
            RECT br;
            HMENU menu, displays;
            UINT cmd;
            GetWindowRect(d->immersive_badge, &br);
            d->immersive_resume = FALSE;
            if (was_immersive) idd_set_immersive_input(d, FALSE);
            menu = CreatePopupMenu();
            displays = CreatePopupMenu();
            if (!menu || !displays) {
                if (menu) DestroyMenu(menu);
                if (displays) DestroyMenu(displays);
                if (was_immersive) idd_set_immersive_input(d, TRUE);
                return 0;
            }
            swprintf_s(header, _countof(header), L"%s  \u00b7  Display %u", d->vm_name, d->display_index + 1);
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, header);
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING | (was_immersive ? MF_CHECKED : 0) | (available ? 0 : MF_GRAYED),
                        BM_MOUSE, L"Immersive mouse\tCtrl+Alt+Shift+I");
            AppendMenuW(menu, MF_STRING | (keyboard ? MF_CHECKED : 0), BM_KEYBOARD,
                        L"Capture keyboard shortcuts");
            AppendMenuW(menu, MF_STRING | (was_immersive && keyboard ? MF_CHECKED : 0) |
                        (available ? 0 : MF_GRAYED), BM_BOTH, L"Immersive mouse && keyboard");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(displays, MF_STRING, BM_ONE_DISPLAY, L"1 display");
            AppendMenuW(displays, MF_STRING, BM_TWO_DISPLAYS, L"2 displays");
            CheckMenuRadioItem(displays, BM_ONE_DISPLAY, BM_TWO_DISPLAYS,
                               owner->display_count == 1 ? BM_ONE_DISPLAY : BM_TWO_DISPLAYS, MF_BYCOMMAND);
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)displays, L"Displays");
            AppendMenuW(menu, MF_STRING | (d->fullscreen ? MF_CHECKED : 0), BM_FULLSCREEN, L"Full screen");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING | (owner->audio_muted ? MF_CHECKED : 0), BM_MUTE, L"Mute VM audio");
            AppendMenuW(menu, MF_STRING, BM_LOG, L"Show log");
            SetForegroundWindow(hwnd);
            cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                       br.left, br.bottom + 4, 0, hwnd, NULL);
            DestroyMenu(menu);   /* also destroys the Displays submenu */
            restore_immersive = was_immersive;
            switch (cmd) {
            case BM_MOUSE:
                restore_immersive = !was_immersive;
                break;
            case BM_KEYBOARD:
                SendMessageW(hwnd, WM_SYSCOMMAND, IDM_XMIT_HOTKEYS, 0);
                break;
            case BM_BOTH: {
                BOOL on = !(was_immersive && keyboard);
                if (keyboard != on) SendMessageW(hwnd, WM_SYSCOMMAND, IDM_XMIT_HOTKEYS, 0);
                restore_immersive = on;
                break;
            }
            case BM_ONE_DISPLAY:
            case BM_TWO_DISPLAYS:
                idd_set_display_count(d, cmd == BM_ONE_DISPLAY ? 1 : 2);
                break;
            case BM_FULLSCREEN:
                idd_set_fullscreen(d, !d->fullscreen);
                break;
            case BM_MUTE:
                SendMessageW(owner->hwnd, WM_SYSCOMMAND, IDM_AUDIO_MUTE, 0);
                break;
            case BM_LOG:
                SendMessageW(hwnd, WM_SYSCOMMAND, IDM_SHOW_LOG, 0);
                break;
            }
            if (restore_immersive && available) idd_set_immersive_input(d, TRUE);
        }
        return 0;

    /* Primary changed the display count: show or hide this (second) window. */
    case WM_IDD_DISPLAY_COUNT:
        if (d && d->auxiliary) {
            if (idd_input_owner(d)->display_count < 2) {
                if (d->immersive_input) idd_set_immersive_input(d, FALSE);
                if (d->fullscreen) idd_set_fullscreen(d, FALSE);
                ShowWindow(hwnd, SW_HIDE);
                idd_update_immersive_badge(d);
            } else if (d->has_frame) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                idd_update_immersive_badge(d);
            }
        }
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
            if (d->immersive_input &&
                (InterlockedCompareExchange(&d->pending_relative_x, 0, 0) != 0 ||
                 InterlockedCompareExchange(&d->pending_relative_y, 0, 0) != 0))
                SetTimer(hwnd, IDT_INPUT, IMMERSIVE_MOUSE_MIN_INTERVAL_MS, NULL);
            else if (d->pending_mouse)
                SetTimer(hwnd, IDT_INPUT, MOUSE_MOVE_MIN_INTERVAL_MS, NULL);
            else
                KillTimer(hwnd, IDT_INPUT);
            return 0;
        }
        if (wp == IDT_RESIZE && d) {
            KillTimer(hwnd, IDT_RESIZE);
            idd_request_guest_resize(d, FALSE);
            return 0;
        }
        if (wp == IDT_PRESENT && d) {
            if (d->frame_dirty)
                d3d_render_frame(d);
        }
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) {
            InterlockedExchange(&d->frame_ready_posted, 0);
            if (d->auxiliary && d->has_frame && !IsWindowVisible(hwnd) &&
                idd_input_owner(d)->display_count >= 2) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                idd_update_immersive_badge(d);
            }
            d3d_render_frame(d);
        }
        return 0;

    case WM_IDD_CURSOR_POS:
        idd_sync_immersive_cursor(d);
        return 0;

    case WM_IDD_INPUT_CAPS:
        if (d) {
            if (!idd_immersive_available(d)) {
                d->immersive_resume = FALSE;
                if (d->immersive_input) idd_set_immersive_input(d, FALSE);
            }
            idd_update_immersive_menu(d);
            /* (Re)connected input: the guest helper may be new; resend. */
            idd_send_host_refresh(d, TRUE);
            if (!d->auxiliary && idd_immersive_available(d))
                send_input(d, INPUT_DISPLAY_COUNT, (UINT32)d->display_count, 0, 0);
            idd_request_guest_resize(d, TRUE);
            if (!d->auxiliary && d->secondary && d->secondary->hwnd)
                PostMessageW(d->secondary->hwnd, WM_IDD_INPUT_CAPS, 0, 0);
        }
        return 0;

    case WM_HOTKEY:
        if (d && wp == IDH_IMMERSIVE_INPUT) {
            HWND target = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&g_immersive_hwnd, NULL, NULL);
            PostMessageW(target ? target : d->hwnd, WM_SYSCOMMAND, IDM_IMMERSIVE_INPUT, 0);
            return 0;
        }
        break;

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
                idd_flush_held_keys(d);
                /* Release the mouse while another window is active, but
                   remember immersive mode so returning re-captures it. */
                if (d->immersive_input) d->immersive_resume = TRUE;
                idd_set_immersive_input(d, FALSE);
            } else if (d->immersive_resume && d->render_hwnd) {
                /* Keyboard return (Alt+Tab) or a click inside the picture
                   resumes now; a click elsewhere (title bar drag, resize)
                   waits for the next click inside the picture. */
                RECT rc;
                POINT pt;
                BOOL inside = FALSE;
                if (GetCursorPos(&pt) && GetClientRect(d->render_hwnd, &rc)) {
                    ScreenToClient(d->render_hwnd, &pt);
                    inside = PtInRect(&rc, pt);
                }
                if (LOWORD(wp) == WA_ACTIVE || inside) {
                    d->immersive_resume = FALSE;
                    idd_set_immersive_input(d, TRUE);
                }
            }
        }
        break;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
    case WM_MOUSEMOVE:
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
            if (d && d->immersive_input)
                SetCursor(d->cursor_visible && d->guest_cursor ? d->guest_cursor : NULL);
            else if (d && d->guest_cursor)
                SetCursor(d->guest_cursor);
            else
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_INPUT:
        if (d && d->immersive_input) {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) == size &&
                raw.header.dwType == RIM_TYPEMOUSE) {
                USHORT buttons = raw.data.mouse.usButtonFlags;
                /* Badge press/release is for the host: drag it, or click it
                   for the menu. Motion is still forwarded so the guest cursor
                   (which the host cursor and the badge follow) keeps moving. */
                if (d->badge_pressing) {
                    if (buttons & RI_MOUSE_LEFT_BUTTON_UP) {
                        d->badge_pressing = FALSE;
                        if (!d->badge_dragged)
                            PostMessageW(hwnd, WM_IDD_BADGE_MENU, 0, 0);
                        else
                            idd_badge_drag_end(d);
                        buttons &= ~RI_MOUSE_LEFT_BUTTON_UP;
                    }
                } else if ((buttons & RI_MOUSE_LEFT_BUTTON_DOWN) &&
                           idd_cursor_on_immersive_badge(d)) {
                    RECT r;
                    GetCursorPos(&d->badge_press);
                    GetWindowRect(d->immersive_badge, &r);
                    d->badge_grab.x = d->badge_press.x - r.left;
                    d->badge_grab.y = d->badge_press.y - r.top;
                    d->badge_pressing = TRUE;
                    d->badge_dragged = FALSE;
                    buttons &= ~RI_MOUSE_LEFT_BUTTON_DOWN;
                }
                if (!(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    InterlockedExchangeAdd(&d->pending_relative_x,
                                           raw.data.mouse.lLastX);
                    InterlockedExchangeAdd(&d->pending_relative_y,
                                           raw.data.mouse.lLastY);
                    flush_pending_relative_mouse(d);
                    if (InterlockedCompareExchange(&d->pending_relative_x, 0, 0) != 0 ||
                        InterlockedCompareExchange(&d->pending_relative_y, 0, 0) != 0)
                        SetTimer(hwnd, IDT_INPUT, IMMERSIVE_MOUSE_MIN_INTERVAL_MS, NULL);
                }
                if (buttons & RI_MOUSE_LEFT_BUTTON_DOWN)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
                if (buttons & RI_MOUSE_LEFT_BUTTON_UP)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
                if (buttons & RI_MOUSE_RIGHT_BUTTON_DOWN)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
                if (buttons & RI_MOUSE_RIGHT_BUTTON_UP)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
                if (buttons & RI_MOUSE_MIDDLE_BUTTON_DOWN)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
                if (buttons & RI_MOUSE_MIDDLE_BUTTON_UP)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
                if (buttons & RI_MOUSE_BUTTON_4_DOWN)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_X1, 1, 0);
                if (buttons & RI_MOUSE_BUTTON_4_UP)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_X1, 0, 0);
                if (buttons & RI_MOUSE_BUTTON_5_DOWN)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_X2, 1, 0);
                if (buttons & RI_MOUSE_BUTTON_5_UP)
                    send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_X2, 0, 0);
                if (buttons & RI_MOUSE_WHEEL)
                    send_input(d, INPUT_MOUSE_WHEEL,
                               (UINT32)(INT32)(SHORT)raw.data.mouse.usButtonData, 0, 0);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        break;

    /* ---- Mouse button/wheel forwarding (only when cursor is in render area) ---- */
    case WM_LBUTTONDOWN:
        if (d && d->mouse_in && d->immersive_resume && d->input_focused) {
            /* Deferred resume: this click re-captures; don't forward it. */
            d->immersive_resume = FALSE;
            idd_set_immersive_input(d, TRUE);
            return 0;
        }
        if (d && d->mouse_in) {
            if (d->render_hwnd) SetCapture(d->render_hwnd);
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
        return 0;

    case WM_RBUTTONDOWN:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
        return 0;
    case WM_RBUTTONUP:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
        return 0;

    case WM_MBUTTONDOWN:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
        return 0;
    case WM_MBUTTONUP:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
        return 0;

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (d && d->mouse_in)
            send_input(d, INPUT_MOUSE_BUTTON,
                       GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? INPUT_BTN_X1 : INPUT_BTN_X2,
                       msg == WM_XBUTTONDOWN, 0);
        return TRUE;

    case WM_MOUSEWHEEL:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
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
        if (!d->transmit_hotkeys &&
            idd_is_reserved_hotkey((DWORD)wp, (GetKeyState(VK_MENU) & 0x8000) != 0))
            break;  /* Default mode: let the host handle this hotkey. */
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

static VmDisplayIdd *vm_display_idd_create_one(VmInstance *vm,
                                                HINSTANCE hInstance,
                                                HWND main_hwnd,
                                                UINT display_index,
                                                VmDisplayIdd *primary)
{
    VmDisplayIdd *d;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm           = vm;
    d->display_index = display_index;
    d->auxiliary     = display_index != 0;
    d->primary       = primary;
    wcscpy_s(d->vm_name, 256, vm->name);
    wcscpy_s(d->vhdx_path, MAX_PATH, vm->vhdx_path);
    d->runtime_id   = vm->runtime_id;
    wcscpy_s(d->os_type, 32, vm->os_type);
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->open         = TRUE;
    d->stop         = FALSE;
    d->input_socket       = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;
    d->clipboard          = NULL;

    /* Load the per-VM display setting, creating display_settings.json with
       the default (off) if this VM doesn't have one yet. The hook itself is
       installed later, on the window thread, once the window exists. */
    d->transmit_hotkeys = primary ? primary->transmit_hotkeys
                                  : idd_display_settings_load_or_create(vm->vhdx_path);
    {
        ViewerPrefs prefs = idd_prefs_load(vm->vhdx_path);
        UINT slot = display_index > 1 ? 1 : display_index;
        d->badge_fx = prefs.badge_x[slot];
        d->badge_fy = prefs.badge_y[slot];
        d->display_count = prefs.display_count;
    }

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
    InitializeCriticalSection(&d->input_send_cs);

    /* Start the window thread (which will then start the recv thread) */
    d->window_thread = CreateThread(NULL, 0, idd_window_thread_proc, d, 0, NULL);
    if (!d->window_thread) {
        ui_log(L"IDD: Failed to create window thread.");
        DeleteCriticalSection(&d->frame_cs);
        DeleteCriticalSection(&d->input_send_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    VmDisplayIdd *primary, *secondary;
    primary = vm_display_idd_create_one(vm, hInstance, main_hwnd, 0, NULL);
    if (!primary) return NULL;
    /* Display 2 is created hidden.  It becomes visible only after channel 8
       delivers a frame, so an older single-monitor guest remains unchanged. */
    secondary = vm_display_idd_create_one(vm, hInstance, main_hwnd, 1, primary);
    if (secondary) primary->secondary = secondary;
    else ui_log(L"IDD: Display 2 viewer could not be created; Display 1 remains available.");
    return primary;
}

static BOOL vm_display_idd_destroy_one(VmDisplayIdd *display)
{
    if (!display) return TRUE;

    /* Signal stop */
    display->stop = TRUE;
    display->open = FALSE;

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        DWORD result;
        for (;;) {
            result = MsgWaitForMultipleObjects(
                1, &display->window_thread, FALSE, 15000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) break;
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                continue;
            }
            ui_log(L"IDD: window thread did not stop; preserving display state to avoid use-after-free.");
            return FALSE;
        }
        CloseHandle(display->window_thread);
        display->window_thread = NULL;
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        DWORD recv_wait = WaitForSingleObject(display->recv_thread, 7000);
        if (recv_wait != WAIT_OBJECT_0) {
            ui_log(L"IDD: receiver did not stop; preserving display state to avoid use-after-free.");
            return FALSE;
        }
        CloseHandle(display->recv_thread);
        display->recv_thread = NULL;
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
        if (WaitForSingleObject(display->audio_recv_thread, 7000) != WAIT_OBJECT_0) {
            ui_log(L"IDD: audio receiver did not stop; preserving display state to avoid use-after-free.");
            return FALSE;
        }
        CloseHandle(display->audio_recv_thread);
        display->audio_recv_thread = NULL;
    }

    DeleteCriticalSection(&display->frame_cs);
    DeleteCriticalSection(&display->input_send_cs);

    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    HeapFree(GetProcessHeap(), 0, display);
    return TRUE;
}

void vm_display_idd_destroy(VmDisplayIdd *display)
{
    if (!display) return;

    if (!display->auxiliary && display->secondary) {
        /* The auxiliary viewer borrows the primary input owner. Never free
           the primary while a timed-out secondary worker can still use it. */
        if (!vm_display_idd_destroy_one(display->secondary)) {
            ui_log(L"IDD: preserving primary state while Display 2 finishes stopping.");
            return;
        }
        display->secondary = NULL;
    }
    (void)vm_display_idd_destroy_one(display);
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

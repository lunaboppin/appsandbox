/*++

Module Name:

    vdd.h

Abstract:

    AppSandbox Virtual Display Driver - Single monitor of any size with
    direct HvSocket frame transport to the host (no agent middleman).

    The monitor offers one mode, the size the host's display window asks for,
    with framebuffer readback sent directly over Hyper-V sockets.

Environment:

    Windows User-Mode Driver Framework 2

--*/

#pragma once

#include <winsock2.h>   /* must precede windows.h */
#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>

#include "Trace.h"
#include "../transport/asb_transport.h"   /* AsbConn — host connection (AF_HYPERV on PC, ivshmem on Mac) */

/* ============================================================================
 *  Display constants
 * ============================================================================ */
#define VDD_DEFAULT_WIDTH    1920       /* until the host asks for a size */
#define VDD_DEFAULT_HEIGHT   1080
#define VDD_BPP              4          /* BGRA 8-bit */
#define VDD_MAX_DIRTY_RECTS  64

/* Limits on a requested size; the host clamps to the same ones (IDD_* in
   vm_display_idd.c). Below 640x480 Windows doesn't offer a mode. The upper
   limit is what a frame may cost, not what IddCx can do: per-frame buffers
   here and on the host (133 MB of BGRA) and the link's throughput. */
#define VDD_MIN_WIDTH        640
#define VDD_MIN_HEIGHT       480
#define VDD_MAX_SIDE         8192
#define VDD_MAX_PIXELS       (7680u * 4320u)

/* ============================================================================
 *  Wire protocol: ASFR frame header (sent over HvSocket to host)
 *  Must match FrameHeader in vm_display_idd.c
 * ============================================================================ */
#define VDD_FRAME_MAGIC     0x52465341  /* "ASFR" little-endian */
/* Same header, full frame as NV12 (Y plane then interleaved UV, full-range
   BT.601). Sent for frames where most of the screen changed: 2.7x fewer bytes
   than BGRA, which keeps games near 60 fps over the link. NV12 needs even
   planes, so an odd size is padded up by repeating its last column or row:
   stride = width rounded up to even, and the host crops the padding off. */
#define VDD_FRAME_NV12_MAGIC 0x3231564E  /* "NV12" little-endian */
#define VDD_ALIGN2(n)       (((n) + 1) & ~1u)
#define VDD_NV12_BYTES(w, h) (VDD_ALIGN2(w) * VDD_ALIGN2(h) * 3 / 2)
/* Dirty area (percent of the frame) from which a frame goes as NV12. */
#define VDD_NV12_AREA_PCT   40

#pragma pack(push, 1)
typedef struct _VDD_WIRE_FRAME_HEADER {
    UINT32  magic;              /* VDD_FRAME_MAGIC */
    UINT32  width;
    UINT32  height;
    UINT32  stride;
    UINT64  frame_seq;
    UINT32  dirty_rect_count;   /* 0 = full frame */
} VDD_WIRE_FRAME_HEADER;

/* Wire protocol: ASCR cursor update (sent over HvSocket to host)
 * Must match CursorHeader in vm_display_idd.c */
#define VDD_CURSOR_MAGIC    0x52435341  /* "ASCR" little-endian */

typedef struct _VDD_WIRE_CURSOR_HEADER {
    UINT32  magic;              /* VDD_CURSOR_MAGIC */
    INT32   x;                  /* cursor position X (can be negative) */
    INT32   y;                  /* cursor position Y (can be negative) */
    UINT32  visible;            /* 0 = hidden, 1 = visible */
    UINT32  shape_updated;      /* 0 = position only, 1 = shape data follows */
    UINT32  shape_id;           /* monotonic shape identifier */
    UINT32  width;              /* cursor width in pixels */
    UINT32  height;             /* cursor height in pixels */
    UINT32  pitch;              /* row pitch in bytes */
    UINT32  xhot;               /* hotspot X */
    UINT32  yhot;               /* hotspot Y */
    UINT32  cursor_type;        /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32  shape_data_size;    /* bytes of shape data following header (0 if !shape_updated) */
} VDD_WIRE_CURSOR_HEADER;

/* Wire protocol: ASRZ mode request, the one message the host sends this way on
 * the frame connection. Must match RESIZE_MAGIC in vm_display_idd.c */
#define VDD_RESIZE_MAGIC    0x5A525341  /* "ASRZ" little-endian */

typedef struct _VDD_WIRE_RESIZE {
    UINT32  magic;              /* VDD_RESIZE_MAGIC */
    UINT32  width;
    UINT32  height;
} VDD_WIRE_RESIZE;
#pragma pack(pop)

/* ============================================================================
 *  EDID - 128-byte block
 *
 *  Manufacturer: "ASB" (AppSandBox)
 *  Descriptor:   1920x1080 @ 60 Hz, 8-bit
 *  Checksum byte 127 is a placeholder - patched at runtime.
 *
 *  It identifies the monitor and never changes, so Windows keeps treating it
 *  as the same monitor (scale, window positions) at every size. The modes come
 *  from VddParseMonitorDescription2, not from this descriptor.
 * ============================================================================ */
static const BYTE VDD_EDID[] = {
    /* Header (bytes 0-7) */
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,

    /* Manufacturer "ASB" = 0x06 0x62  (5-bit packed: A=1,S=19,B=2) */
    /* Product code 0x0001, serial 0x00000001 */
    0x06, 0x62, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,

    /* Week 1, year 2024 (offset from 1990 = 34 = 0x22) */
    0x01, 0x22,

    /* EDID version 1.4 */
    0x01, 0x04,

    /* Digital input, 8-bit color depth, DisplayPort */
    0xA5,

    /* 53cm x 30cm (approx 24" diagonal for 1920x1080) */
    0x35, 0x1E,

    /* Gamma 2.2 (value = (gamma*100)-100 = 120 = 0x78) */
    0x78,

    /* Supported features: RGB color, preferred timing in DTD1 */
    0x22,

    /* Chromaticity coordinates (standard sRGB-ish values) */
    0xFC, 0x81, 0xA4, 0x55, 0x4D, 0x9D, 0x25, 0x12, 0x50, 0x54,

    /* Established timings */
    0x00, 0x00, 0x00,

    /* Standard timings (unused, filled with 0x01 0x01) */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,

    /* Detailed Timing Descriptor #1: 1920x1080 @ 60Hz */
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
    0x58, 0x2C, 0x45, 0x00, 0x12, 0x2C, 0x21, 0x00,
    0x00, 0x1E,

    /* Descriptor #2: Monitor name "AppSandboxVDD" */
    0x00, 0x00, 0x00, 0xFC, 0x00,
    'A', 'p', 'p', 'S', 'a', 'n', 'd', 'b', 'o', 'x', 'V', 'D', 'D',

    /* Descriptor #3: Monitor range limits */
    0x00, 0x00, 0x00, 0xFD, 0x00,
    0x38, 0x4C, 0x1E, 0x51, 0x11, 0x00, 0x0A,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20,

    /* Descriptor #4: Dummy (zero) */
    0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,

    /* Extension block count */
    0x00,

    /* Checksum placeholder - computed at runtime */
    0x00
};

/* ============================================================================
 *  Swap chain processor state (heap-allocated per AssignSwapChain)
 * ============================================================================ */
typedef struct _VDD_DEVICE_CONTEXT VDD_DEVICE_CONTEXT;

typedef struct _VDD_SWAP_PROC {
    VDD_DEVICE_CONTEXT* pCtx;
    IDDCX_SWAPCHAIN     hSwapChain;
    HANDLE              hAvailableEvent;    /* from pInArgs->hNextSurfaceAvailable */
    HANDLE              hTerminateEvent;
    HANDLE              hThread;

    /* Non-owning pointers to cached D3D11 device (owned by VDD_DEVICE_CONTEXT).
       The device is cached across swap chain transitions to avoid destroying it
       during IddCx teardown to avoid re-creation overhead. */
    ID3D11Device*       pDevice;
    ID3D11DeviceContext* pDeviceContext;

    /* Staging texture for CPU readback (owned, per-swap-chain) */
    ID3D11Texture2D*    pStagingTex;
    UINT                width;              /* size of pStagingTex and of the frames sent */
    UINT                height;

    /* Host connection via asb_transport — AF_HYPERV HvSocket on a Windows host, ivshmem stream
       slot on a macOS host. One emit path, two backends. */
    HANDLE              hNetworkThread;     /* listener/accept thread */
    AsbConn * volatile  hClientConn;        /* active client, owned by swap chain thread */
    AsbConn * volatile  hPendingConn;       /* new client from network thread, picked up by swap chain */
    volatile BOOL       bStopNetwork;
    UINT64              frameSeq;
    PBYTE               pSendBuf;           /* one packed frame: header, rects, size, pixels */
    VDD_WIRE_RESIZE     resizeMsg;          /* mode request being read from the host */
    UINT                resizeMsgLen;

    /* Sender thread: sends one packed frame while the swap chain thread reads
       back and packs the next, so the two stages overlap instead of adding up.
       The buffers swap on hand-off; sendLock keeps cursor messages from
       interleaving with a frame on the connection. */
    HANDLE              hSendThread;
    HANDLE              hSendWork;          /* auto-reset: a frame was handed off */
    HANDLE              hSendIdle;          /* manual-reset: set while nothing is in flight */
    HANDLE              hSendStop;
    CRITICAL_SECTION    sendLock;
    PBYTE               pSendBufOut;        /* frame being sent */
    int                 sendLen;
    AsbConn*            sendConn;
    volatile BOOL       sendFailed;         /* last hand-off failed to send */
    volatile double     perfSendMs;         /* send time, summed by the sender thread */

    /* Hardware cursor */
    IDDCX_MONITOR       hMonitor;           /* needed for QueryHardwareCursor */
    HANDLE              hCursorEvent;       /* signaled when cursor data changes */
    PBYTE               pCursorShapeBuffer; /* buffer for cursor bitmap */
    UINT                cursorBufSize;      /* size of pCursorShapeBuffer (2x for MASKED_COLOR) */
    DWORD               lastShapeId;        /* last received shape ID */
} VDD_SWAP_PROC;

/* ============================================================================
 *  Device context (heap-allocated, stored via IndirectDeviceContextWrapper)
 * ============================================================================ */
typedef enum _VDD_MODE_STATE {
    VDD_MODE_IDLE,                          /* the monitor has the size in force */
    VDD_MODE_DEPARTED,                      /* departed, waiting for its swap chain to go */
    VDD_MODE_WAIT_ASSIGN,                   /* arrived at the new size, waiting for a swap chain */
} VDD_MODE_STATE;

struct _VDD_DEVICE_CONTEXT {
    WDFDEVICE           wdfDevice;
    IDDCX_ADAPTER       hAdapter;
    IDDCX_MONITOR       hMonitor;
    VDD_SWAP_PROC*      pSwapProc;          /* Active swap chain processor */

    /* Cached D3D11 device — persists across swap chain transitions.
       Kept alive here so that WdfObjectDelete's IddCx teardown never triggers
       destruction of the D3D device (which crashes WUDFHost.exe). */
    ID3D11Device*       pCachedDevice;
    ID3D11DeviceContext* pCachedCtx;
    LUID                cachedDeviceLuid;

    /* Mode changes. Windows offers the intersection of the monitor's modes and
       the target modes, and the monitor's modes are read when it arrives, so a
       size it didn't arrive with can't be added by IddCxMonitorUpdateModes2.
       The monitor departs and arrives again offering only the requested size,
       which also leaves Windows nothing else to choose, at the sign-in screen
       as much as on a desktop. All of it runs on hModeTimer, a WDF timer like
       hRecoveryTimer, so only one step ever runs at a time. */
    WDFTIMER            hModeTimer;
    VDD_MODE_STATE      modeState;
    ULONGLONG           modeDeadline;       /* GetTickCount64 when the current wait gives up */
    volatile LONG64     wantedMode;         /* requested size: width << 32 | height */
    UINT                goodWidth;          /* last size a swap chain arrived at */
    UINT                goodHeight;
    GUID                containerId;        /* the same across arrivals */
    BOOL                hasContainerId;
    /* The frame connection outlives a mode change: the old swap chain's teardown
       parks it here for the next one, so the host sees frames of the new size
       on the connection it already has. */
    volatile BOOL       bReplugging;
    AsbConn * volatile  hCarriedConn;

    /* Recovery: if no AssignSwapChain arrives within 5s of Unassign,
       depart and re-arrive the monitor to force DWM re-engagement.
       Uses a WDF timer so the callback runs on a proper WDF thread
       (IddCx APIs crash when called from raw CreateThread threads). */
    WDFTIMER            hRecoveryTimer;
};

/* ============================================================================
 *  WDF Context Wrappers
 * ============================================================================ */

struct IndirectDeviceContextWrapper
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

struct MonitorContextWrapper
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

struct RecoveryTimerContext
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(RecoveryTimerContext);

/* ============================================================================
 *  IddCx Callback declarations
 * ============================================================================ */

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD           VddDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY            VddDeviceD0Entry;
EVT_WDF_DRIVER_UNLOAD              VddDriverUnload;

EVT_IDD_CX_ADAPTER_INIT_FINISHED                   VddAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES                    VddAdapterCommitModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2                   VddAdapterCommitModes2;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION               VddParseMonitorDescription;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2              VddParseMonitorDescription2;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES   VddMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES              VddMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2             VddMonitorQueryTargetModes2;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN                VddMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN              VddMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO               VddAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA        VddMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP                  VddMonitorSetGammaRamp;

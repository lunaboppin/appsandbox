/*++

Module Name:

    vdd.cpp

Abstract:

    AppSandbox Virtual Display Driver - Single monitor of any size with
    direct HvSocket frame transport to the host (no agent middleman).

    IddCx lifecycle:
    - DllMain
    - DriverEntry with WDF_OBJECT_ATTRIBUTES_INIT + EvtDriverUnload
    - DeviceAdd with PnP callbacks, IddCx config, EvtCleanupCallback,
      heap-allocated context (IndirectDeviceContextWrapper)
    - D0Entry -> InitAdapter (context type on adapter object)
    - AdapterInitFinished -> FinishInit -> CreateMonitor
    - ParseMonitorDescription, GetDefaultModes, QueryTargetModes
    - AssignSwapChain with direct HvSocket frame transport to host
    - UnassignSwapChain with cleanup
    - Mode changes: the host asks over the frame connection, the monitor
      departs and arrives again offering just that size (VddModeTimerCallback)

Environment:

    Windows User-Mode Driver Framework 2

--*/

#include "vdd.h"

#include <stdlib.h>  /* malloc, free */
#include <stdarg.h>  /* va_list, va_start, va_end */
#include <stdio.h>   /* _snprintf, _vsnprintf */
#include <cfgmgr32.h> /* CM_Locate_DevNode, CM_Disable_DevNode */
#include <d3dkmthk.h>  /* D3DKMTSetProcessSchedulingPriorityClass */
#include <setupapi.h>  /* SetupDiGetClassDevs, SetupDiEnumDeviceInfo */
#include "../transport/asb_transport.h"  /* ivshmem frame channel (Windows-VMs-on-Mac path only) */

/* ========================================================================= */
/*  File-based logging (persists across crashes, no debugger needed)         */
/* ========================================================================= */

#define VDD_LOG_PATH L"C:\\ProgramData\\AppSandboxVDD.log"

static void VddLog(const char* fmt, ...)
{
    HANDLE hFile = CreateFileW(VDD_LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    /* Timestamp */
    SYSTEMTIME sysTime;
    GetLocalTime(&sysTime);
    char buf[2048];
    int pos = sprintf_s(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                        sysTime.wYear, sysTime.wMonth, sysTime.wDay,
                        sysTime.wHour, sysTime.wMinute, sysTime.wSecond, sysTime.wMilliseconds);
    if (pos < 0) pos = 0;

    /* Format message */
    va_list args;
    va_start(args, fmt);
    int added = vsprintf_s(buf + pos, sizeof(buf) - (size_t)pos - 4, fmt, args);
    va_end(args);
    if (added > 0) pos += added;

    buf[pos++] = '\r';
    buf[pos++] = '\n';

    DWORD written;
    WriteFile(hFile, buf, (DWORD)pos, &written, NULL);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    /* Also send to debug output for good measure */
    buf[pos - 2] = '\n';
    buf[pos - 1] = '\0';
    OutputDebugStringA(buf);
}

/* ========================================================================= */
/*  EDID with computed checksum                                              */
/* ========================================================================= */

static const BYTE* VddGetEdid()
{
    static BYTE edid[128];
    static BOOL computed = FALSE;

    if (!computed)
    {
        memcpy(edid, VDD_EDID, 127);
        BYTE sum = 0;
        for (int i = 0; i < 127; i++)
            sum += edid[i];
        edid[127] = (BYTE)(256 - sum);
        computed = TRUE;
    }
    return edid;
}

/* ========================================================================= */
/*  Monitor mode helper                                                      */
/* ========================================================================= */

/* The size the monitor offers. Globals because the description callbacks get
   no monitor or device to find a context from; there is one device and one
   monitor. Changes only while the monitor is gone (VddModeTimerCallback). */
static volatile UINT g_ModeWidth  = VDD_DEFAULT_WIDTH;
static volatile UINT g_ModeHeight = VDD_DEFAULT_HEIGHT;

static void VddCreateMonitorMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, UINT vSyncDivider)
{
    const UINT w = g_ModeWidth, h = g_ModeHeight;
    /* totalSize == activeSize, pixelRate = refresh * w * h.
       IddCx validates these relationships; using actual blanking values causes
       STATUS_INVALID_PARAMETER from IddCxMonitorArrival. */
    sig->totalSize.cx                           = w;
    sig->totalSize.cy                           = h;
    sig->activeSize.cx                          = w;
    sig->activeSize.cy                          = h;
    sig->AdditionalSignalInfo.vSyncFreqDivider  = vSyncDivider;
    sig->AdditionalSignalInfo.videoStandard     = 255;
    sig->vSyncFreq.Numerator                    = 60;
    sig->vSyncFreq.Denominator                  = 1;
    sig->hSyncFreq.Numerator                    = 60 * h;
    sig->hSyncFreq.Denominator                  = 1;
    sig->scanLineOrdering                       = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    sig->pixelRate                               = (UINT64)60 * w * h;
}

/* ========================================================================= */
/*  Host frame transport                                                     */
/*                                                                           */
/*  The frame channel (listen/accept/send on the ch2 service a5b0cafe-0002)  */
/*  goes through asb_transport: asb_listen(ASB_CH_DISPLAY)/asb_accept/       */
/*  asb_send resolve to AF_HYPERV HvSocket on a Windows host and to the      */
/*  ivshmem ch2 stream slot on a macOS host.                                 */
/* ========================================================================= */

/* Send exactly len bytes over the transport. Returns 0 on success, -1 on failure
   (asb_send loops until all bytes are sent on both backends). */
static int VddSendAll(AsbConn* c, const char* data, int len)
{
    return asb_send(c, data, len) == len ? 0 : -1;
}

/* Close a client connection and clear *pc. PC: graceful shutdown(SD_BOTH) then closesocket; ivshmem:
   asb_close marks the slot CLOSING. */
static void VddCloseConn(AsbConn* volatile* pc)
{
    AsbConn* c = *pc;
    if (!c) return;
    {
        unsigned long long s = asb_conn_socket_u64(c);
        if (s != ~0ull) shutdown((SOCKET)s, SD_BOTH);   /* PC only; ivshmem returns ~0 */
    }
    asb_close(c);
    *pc = NULL;
}

/* ---- Sender thread (see VDD_SWAP_PROC) ---- */

static DWORD WINAPI VddSenderThread(LPVOID param)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)param;
    HANDLE waits[2] = { proc->hSendWork, proc->hSendStop };
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) break;
        QueryPerformanceCounter(&a);
        EnterCriticalSection(&proc->sendLock);
        if (VddSendAll(proc->sendConn, (const char*)proc->pSendBufOut, proc->sendLen) != 0)
            proc->sendFailed = TRUE;
        LeaveCriticalSection(&proc->sendLock);
        QueryPerformanceCounter(&b);
        proc->perfSendMs += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
        SetEvent(proc->hSendIdle);
    }
    return 0;
}

/* Wait for the frame in flight (if any). Must precede closing or replacing
   hClientConn, which the sender may be using. Returns FALSE if it failed. */
static BOOL VddWaitSendIdle(VDD_SWAP_PROC* proc)
{
    if (!proc->hSendIdle) return TRUE;
    if (WaitForSingleObject(proc->hSendIdle, 5000) != WAIT_OBJECT_0) {
        VddLog("Frame: sender stuck for 5 s");
        return FALSE;
    }
    if (proc->sendFailed) {
        proc->sendFailed = FALSE;
        return FALSE;
    }
    return TRUE;
}

/* Hand pSendBuf (len bytes) to the sender and take its other buffer for the
   next frame. Returns FALSE if the previous frame failed to send, so the
   caller drops the connection. */
static BOOL VddQueueSend(VDD_SWAP_PROC* proc, int len)
{
    PBYTE swap;
    if (!VddWaitSendIdle(proc)) return FALSE;
    swap = proc->pSendBufOut;
    proc->pSendBufOut = proc->pSendBuf;
    proc->pSendBuf = swap;            /* NULL at first: VddPackFrame allocates it */
    proc->sendLen = len;
    proc->sendConn = proc->hClientConn;
    ResetEvent(proc->hSendIdle);
    SetEvent(proc->hSendWork);
    return TRUE;
}

static void VddDropClient(VDD_SWAP_PROC* proc)
{
    VddWaitSendIdle(proc);
    VddCloseConn(&proc->hClientConn);
}

/* ========================================================================= */
/*  Network listener thread (accepts host connections over HvSocket)         */
/* ========================================================================= */

/* One-shot transport backend selection (PC AF_HYPERV vs Mac ivshmem). Done off the loader lock
   (in the network thread, never in DllMain) since the ivshmem probe enumerates device interfaces. */
static volatile LONG g_transportInited = 0;
static void VddEnsureTransportInit(void)
{
    if (InterlockedCompareExchange(&g_transportInited, 1, 0) == 0)
        asb_transport_init();   /* selects ivshmem if the host published the directory, else AF_HYPERV */
}

static DWORD WINAPI VddNetworkThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    AsbListener* l = NULL;

    VddLog("Network: starting");
    VddEnsureTransportInit();

    while (!proc->bStopNetwork) {
        /* (Re)create the listener. PC: socket(AF_HYPERV)+bind(ch2 GUID, wildcard)+listen;
           Mac: bind the ivshmem ch2 slot. */
        l = asb_listen(ASB_CH_DISPLAY);
        if (!l) {
            VddLog("Network: asb_listen(ch2) failed, retrying in 3s (transport=%s)",
                   asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");
            for (int w = 0; w < 3000 && !proc->bStopNetwork; w += 500)
                Sleep(500);
            continue;
        }

        VddLog("Network: listening for host connections (transport=%s)",
               asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

        /* Accept loop — 1s timeout so we re-check bStopNetwork between accepts. */
        while (!proc->bStopNetwork) {
            AsbConn* nc = asb_accept(l, 1000);
            if (!nc)
                continue;

            VddLog("Network: host connected");

            /* If a previous pending connection wasn't picked up yet, close it. */
            {
                AsbConn* old = (AsbConn*)InterlockedExchangePointer(
                    (PVOID volatile*)&proc->hPendingConn, nc);
                if (old) {
                    VddCloseConn(&old);
                    VddLog("Network: replaced unclaimed pending connection");
                }
            }
        } /* end accept loop */

        asb_close_listener(l);
        l = NULL;
    } /* end retry loop */

    /* Close any pending connection. */
    {
        AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
            (PVOID volatile*)&proc->hPendingConn, NULL);
        if (pending) VddCloseConn(&pending);
    }

    if (l) asb_close_listener(l);

    VddLog("Network: stopped");
    return 0;
}

/* ========================================================================= */
/*  D3D11 device — cached in VDD_DEVICE_CONTEXT across swap chain transitions
 *  to avoid destroying the D3D device during IddCx's WdfObjectDelete
 *  teardown (which crashes WUDFHost.exe).                                    */
/* ========================================================================= */

static HRESULT VddGetOrCreateD3DDevice(VDD_DEVICE_CONTEXT* ctx, LUID adapterLuid,
                                        ID3D11Device** ppDevice, ID3D11DeviceContext** ppCtx)
{
    /* Check if cached device matches the requested LUID and is still healthy */
    if (ctx->pCachedDevice &&
        ctx->cachedDeviceLuid.HighPart == adapterLuid.HighPart &&
        ctx->cachedDeviceLuid.LowPart == adapterLuid.LowPart)
    {
        HRESULT reason = ctx->pCachedDevice->GetDeviceRemovedReason();
        if (reason == S_OK)
        {
            VddLog("GetOrCreateD3DDevice: reusing cached device (LUID=%08X:%08X)",
                   adapterLuid.HighPart, adapterLuid.LowPart);
            *ppDevice = ctx->pCachedDevice;
            *ppCtx = ctx->pCachedCtx;
            return S_OK;
        }
        VddLog("GetOrCreateD3DDevice: cached device removed (reason=0x%08X), recreating",
               reason);
        ctx->pCachedCtx->Release();
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
        ctx->pCachedCtx = nullptr;
    }

    /* Release any stale cached device with a different LUID */
    if (ctx->pCachedDevice)
    {
        VddLog("GetOrCreateD3DDevice: releasing stale cached device (LUID mismatch)");
        ctx->pCachedCtx->Release();
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
        ctx->pCachedCtx = nullptr;
    }

    VddLog("GetOrCreateD3DDevice: creating new device (LUID=%08X:%08X)",
           adapterLuid.HighPart, adapterLuid.LowPart);

    IDXGIFactory5* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory5), (void**)&pFactory);
    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: CreateDXGIFactory2 FAILED hr=0x%08X", hr);
        return hr;
    }

    IDXGIAdapter1* pAdapter = nullptr;
    hr = pFactory->EnumAdapterByLuid(adapterLuid, __uuidof(IDXGIAdapter1), (void**)&pAdapter);
    pFactory->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: EnumAdapterByLuid FAILED hr=0x%08X", hr);
        return hr;
    }

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pDevCtx = nullptr;

    hr = D3D11CreateDevice(
        pAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &pDevice,
        nullptr,
        &pDevCtx);

    pAdapter->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: D3D11CreateDevice FAILED hr=0x%08X", hr);
        return hr;
    }

    VddLog("GetOrCreateD3DDevice: D3D11 device created successfully");

    /* Cache in device context */
    ctx->pCachedDevice = pDevice;
    ctx->pCachedCtx = pDevCtx;
    ctx->cachedDeviceLuid = adapterLuid;

    *ppDevice = pDevice;
    *ppCtx = pDevCtx;
    return S_OK;
}

static HRESULT VddCreateStagingTexture(VDD_SWAP_PROC* proc, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    return proc->pDevice->CreateTexture2D(&desc, nullptr, &proc->pStagingTex);
}

/* ========================================================================= */
/*  Swap chain processor thread                                              */
/* ========================================================================= */

static void VddSwapChainRunCore(VDD_SWAP_PROC* proc);

static DWORD WINAPI VddSwapChainThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    DWORD taskIndex = 0;
    HANDLE hAvrt;
    IDXGIDevice* pDxgiDevice = NULL;
    HRESULT hr;

    hAvrt = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);

    VddLog("SwapChain thread started");

    /* Raise GPU priority (IddCx 1.9+) */
#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority) && proc->pDevice) {
        hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&pDxgiDevice));
        if (SUCCEEDED(hr)) {
            IDARG_IN_SETREALTIMEGPUPRIORITY priorityArgs = { 0 };
            priorityArgs.pDevice = pDxgiDevice;
            IddCxSetRealtimeGPUPriority(proc->hSwapChain, &priorityArgs);
            pDxgiDevice->Release();
            pDxgiDevice = NULL;
        }
    }

    VddSwapChainRunCore(proc);

    /* Always delete the swap chain when the processing loop terminates, to
       kick the system to provide a new swap chain if necessary.  The D3D
       device is NOT released here — it's cached in VDD_DEVICE_CONTEXT to
       survive IddCx teardown. */
    if (proc->hSwapChain) {
        VddLog("SwapChain thread: deleting swap chain (WdfObjectDelete)");
        WdfObjectDelete((WDFOBJECT)proc->hSwapChain);
        proc->hSwapChain = NULL;
    }

    if (hAvrt)
        AvRevertMmThreadCharacteristics(hAvrt);

    VddLog("SwapChain thread exiting");
    return 0;
}

/* Query hardware cursor and send update to host if connected */
static void VddSendCursorUpdate(VDD_SWAP_PROC* proc)
{
    IDARG_IN_QUERY_HWCURSOR inArgs = {};
    IDARG_OUT_QUERY_HWCURSOR3 outArgs = {};
    VDD_WIRE_CURSOR_HEADER chdr;
    AsbConn* s;

    if (!proc->hCursorEvent || !proc->pCursorShapeBuffer || !proc->hMonitor)
        return;

    s = proc->hClientConn;
    if (s == NULL)
        return;

    inArgs.LastShapeId = proc->lastShapeId;
    inArgs.ShapeBufferSizeInBytes = proc->cursorBufSize;
    inArgs.pShapeBuffer = proc->pCursorShapeBuffer;

    HRESULT hr = IddCxMonitorQueryHardwareCursor3(proc->hMonitor, &inArgs, &outArgs);
    if (FAILED(hr)) {
        VddLog("Cursor: QueryHardwareCursor3 FAILED 0x%08X", hr);
        return;
    }

    chdr.magic          = VDD_CURSOR_MAGIC;
    chdr.x              = outArgs.X;
    chdr.y              = outArgs.Y;
    chdr.visible        = outArgs.IsCursorVisible ? 1 : 0;
    chdr.shape_updated  = outArgs.IsCursorShapeUpdated ? 1 : 0;
    chdr.shape_id       = outArgs.CursorShapeInfo.ShapeId;
    chdr.width          = outArgs.CursorShapeInfo.Width;
    chdr.height         = outArgs.CursorShapeInfo.Height;
    chdr.pitch          = outArgs.CursorShapeInfo.Pitch;
    chdr.xhot           = outArgs.CursorShapeInfo.XHot;
    chdr.yhot           = outArgs.CursorShapeInfo.YHot;
    chdr.cursor_type    = (UINT32)outArgs.CursorShapeInfo.CursorType;

    if (outArgs.IsCursorShapeUpdated) {
        UINT32 data_size = outArgs.CursorShapeInfo.Pitch * outArgs.CursorShapeInfo.Height;
        if (data_size > proc->cursorBufSize) {
            VddLog("Cursor: shape data %u exceeds buffer %u, skipping",
                   data_size, proc->cursorBufSize);
            chdr.shape_updated = 0;
            chdr.shape_data_size = 0;
        } else {
            chdr.shape_data_size = data_size;
            proc->lastShapeId = outArgs.CursorShapeInfo.ShapeId;
        }
    } else {
        chdr.shape_data_size = 0;
    }

    /* Header and shape go together, never between a frame's bytes. */
    {
        BOOL ok;
        EnterCriticalSection(&proc->sendLock);
        ok = VddSendAll(s, (const char*)&chdr, sizeof(chdr)) == 0 &&
             (chdr.shape_data_size == 0 ||
              VddSendAll(s, (const char*)proc->pCursorShapeBuffer, (int)chdr.shape_data_size) == 0);
        LeaveCriticalSection(&proc->sendLock);
        if (!ok) {
            VddLog("Cursor: send failed, disconnecting");
            VddDropClient(proc);
        }
    }
}


/* Pack one frame (header, dirty rects, size, pixels) into proc->pSendBuf so it
   goes to the host in a single send. Sending a 1080p frame row by row took
   ~1000 socket calls per frame, which held the frame rate well under 60.
   rectCount 0 = full frame. Returns the packed length, or 0 when there is
   nothing to send (every rect empty) or no buffer. */
/* BGRA -> NV12, full-range BT.601, one 2x2 block at a time (chroma is the
   block average). The host's idd_nv12_to_bgra is the exact inverse. */
static void VddPackNv12(const D3D11_MAPPED_SUBRESOURCE* mapped, PBYTE out, UINT w, UINT h)
{
    const UINT aw = VDD_ALIGN2(w), ah = VDD_ALIGN2(h);
    PBYTE yPlane = out, uvPlane = out + aw * ah;
    for (UINT y = 0; y < ah; y += 2) {
        const BYTE* r0 = (const BYTE*)mapped->pData + y * mapped->RowPitch;
        const BYTE* r1 = y + 1 < h ? r0 + mapped->RowPitch : r0;   /* odd height: repeat the last row */
        PBYTE y0 = yPlane + y * aw, y1 = y0 + aw;
        PBYTE uv = uvPlane + (y / 2) * aw;
        const BYTE* a = r0;
        const BYTE* c = r1;
#define VDD_LUM(px) (BYTE)((77 * (px)[2] + 150 * (px)[1] + 29 * (px)[0] + 128) >> 8)
        for (UINT x = 0; x < aw; x += 2, a += 8, c += 8) {
            const UINT n = x + 1 < w ? 4 : 0;   /* odd width: repeat the last column */
            y0[x] = VDD_LUM(a); y0[x + 1] = VDD_LUM(a + n);
            y1[x] = VDD_LUM(c); y1[x + 1] = VDD_LUM(c + n);
            int sb = (a[0] + a[n + 0] + c[0] + c[n + 0]) >> 2;
            int sg = (a[1] + a[n + 1] + c[1] + c[n + 1]) >> 2;
            int sr = (a[2] + a[n + 2] + c[2] + c[n + 2]) >> 2;
            int u = ((-43 * sr - 85 * sg + 128 * sb + 128) >> 8) + 128;
            int v = ((128 * sr - 107 * sg - 21 * sb + 128) >> 8) + 128;
            uv[x]     = (BYTE)(u < 0 ? 0 : u > 255 ? 255 : u);
            uv[x + 1] = (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
#undef VDD_LUM
}

static int VddPackFrame(VDD_SWAP_PROC* proc, const D3D11_MAPPED_SUBRESOURCE* mapped,
                        const RECT* rects, UINT32 rectCount, BOOL nv12)
{
    const UINT w = proc->width, h = proc->height, stride = w * VDD_BPP;
    const SIZE_T cap = sizeof(VDD_WIRE_FRAME_HEADER) + VDD_MAX_DIRTY_RECTS * sizeof(RECT) +
                       sizeof(UINT32) + (SIZE_T)stride * h;
    VDD_WIRE_FRAME_HEADER whdr;
    PBYTE p, sizePtr;
    UINT32 dataSize = 0;
    if (rectCount > VDD_MAX_DIRTY_RECTS) rectCount = 0;   /* too many: send it all */
    if (!proc->pSendBuf) {
        proc->pSendBuf = (PBYTE)malloc(cap);
        if (!proc->pSendBuf) return 0;
    }
    p = proc->pSendBuf + sizeof(whdr);
    if (rectCount) {
        memcpy(p, rects, rectCount * sizeof(RECT));
        p += rectCount * sizeof(RECT);
    }
    sizePtr = p;
    p += sizeof(UINT32);
    if (nv12) {
        rectCount = 0;
        p = proc->pSendBuf + sizeof(whdr);
        sizePtr = p;
        p += sizeof(UINT32);
        VddPackNv12(mapped, p, w, h);
        p += VDD_NV12_BYTES(w, h);
        dataSize = VDD_NV12_BYTES(w, h);
    } else if (!rectCount) {
        for (UINT row = 0; row < h; row++) {
            memcpy(p, (const BYTE*)mapped->pData + row * mapped->RowPitch, stride);
            p += stride;
        }
        dataSize = stride * h;
    } else {
        for (UINT32 i = 0; i < rectCount; i++) {
            const RECT* r = &rects[i];
            if (r->left >= r->right || r->top >= r->bottom) continue;
            UINT rw = (UINT)(r->right - r->left), rh = (UINT)(r->bottom - r->top);
            for (UINT row = 0; row < rh; row++) {
                memcpy(p, (const BYTE*)mapped->pData + ((UINT)r->top + row) * mapped->RowPitch +
                          (UINT)r->left * 4, rw * 4);
                p += rw * 4;
            }
            dataSize += rw * 4 * rh;
        }
        if (dataSize == 0) return 0;
    }
    proc->frameSeq++;
    whdr.magic            = nv12 ? VDD_FRAME_NV12_MAGIC : VDD_FRAME_MAGIC;
    whdr.width            = w;
    whdr.height           = h;
    whdr.stride           = nv12 ? VDD_ALIGN2(w) : stride;
    whdr.frame_seq        = proc->frameSeq;
    whdr.dirty_rect_count = rectCount;
    memcpy(proc->pSendBuf, &whdr, sizeof(whdr));
    memcpy(sizePtr, &dataSize, sizeof(dataSize));
    return (int)(p - proc->pSendBuf);
}

/* Make the staging texture and send buffers fit a surface of another size.
   Each mode change normally brings a new swap chain of the right size anyway;
   this keeps a surface that differs from turning into a failed copy. */
static BOOL VddResizeCapture(VDD_SWAP_PROC* proc, UINT width, UINT height)
{
    HRESULT hr;
    VddLog("Frame: surface is %ux%u, capture was %ux%u", width, height, proc->width, proc->height);
    if (!VddWaitSendIdle(proc)) return FALSE;
    free(proc->pSendBuf);
    free(proc->pSendBufOut);
    proc->pSendBuf = proc->pSendBufOut = NULL;
    if (proc->pStagingTex) {
        proc->pStagingTex->Release();
        proc->pStagingTex = NULL;
    }
    hr = VddCreateStagingTexture(proc, width, height);
    if (FAILED(hr)) {
        VddLog("Frame: staging texture for %ux%u FAILED hr=0x%08X", width, height, hr);
        return FALSE;
    }
    proc->width = width;
    proc->height = height;
    return TRUE;
}

static void VddRequestMode(VDD_DEVICE_CONTEXT* ctx, UINT width, UINT height);

/* Read a mode request from the host, without waiting. A request can arrive in
   pieces, so what came so far stays in resizeMsg. Returns FALSE when the
   connection is gone or the host sent something else, so the caller drops it. */
static BOOL VddPollResizeRequest(VDD_SWAP_PROC* proc)
{
    const unsigned long long sock = asb_conn_socket_u64(proc->hClientConn);
    SOCKET s = (SOCKET)sock;
    fd_set rd;
    const struct timeval now = { 0, 0 };
    int n;
    if (sock == ~0ull) return TRUE;   /* ivshmem: the Mac host never resizes */
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    if (select(0, &rd, NULL, NULL, &now) <= 0) return TRUE;
    n = recv(s, (char*)&proc->resizeMsg + proc->resizeMsgLen,
             (int)(sizeof(proc->resizeMsg) - proc->resizeMsgLen), 0);
    if (n <= 0) return FALSE;
    proc->resizeMsgLen += (UINT)n;
    if (proc->resizeMsgLen < sizeof(proc->resizeMsg)) return TRUE;
    proc->resizeMsgLen = 0;
    if (proc->resizeMsg.magic != VDD_RESIZE_MAGIC) {
        VddLog("Frame: bad request from host (magic 0x%08X)", proc->resizeMsg.magic);
        return FALSE;
    }
    VddRequestMode(proc->pCtx, proc->resizeMsg.width, proc->resizeMsg.height);
    return TRUE;
}

static void VddSwapChainRunCore(VDD_SWAP_PROC* proc)
{
    IDXGIDevice* pDxgiDevice = NULL;
    IDARG_IN_SWAPCHAINSETDEVICE setDevice;
    HRESULT hr;
    HANDLE waitHandles[3];  /* swap chain, terminate, cursor */
    DWORD waitCount;

    /* Retry limits — on persistent failure, exit RunCore so the caller can
       WdfObjectDelete the swap chain, which tells IddCx to reassign a fresh one. */
    const int MAX_RETRIES = 5;
    const DWORD RETRY_DELAY_INIT = 1;
    const DWORD RETRY_DELAY_MAX = 100;
    DWORD retryDelay = RETRY_DELAY_INIT;
    int retryCount = 0;

    VddLog("SwapChainRunCore: entered");

    hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&pDxgiDevice));
    if (FAILED(hr)) {
        VddLog("SwapChainRunCore: QueryInterface for IDXGIDevice FAILED hr=0x%08X", hr);
        return;
    }

    memset(&setDevice, 0, sizeof(setDevice));
    setDevice.pDevice = pDxgiDevice;

    hr = IddCxSwapChainSetDevice(proc->hSwapChain, &setDevice);
    pDxgiDevice->Release();
    if (FAILED(hr)) {
        VddLog("SwapChainRunCore: IddCxSwapChainSetDevice FAILED hr=0x%08X, exiting for reassignment", hr);
        return;
    }
    VddLog("SwapChainRunCore: swap chain device set, entering frame loop");

    waitHandles[0] = proc->hAvailableEvent;
    waitHandles[1] = proc->hTerminateEvent;
    waitCount = 2;
    if (proc->hCursorEvent) {
        waitHandles[2] = proc->hCursorEvent;
        waitCount = 3;
    }

    /* Track staging texture row pitch (set on first capture) and whether
       we have a valid frame in the staging texture for resend on idle. */
    BOOL bHasFrame = FALSE;
    BOOL bSentFullFrame = FALSE;   /* TRUE after first full frame sent to current client */
    UINT cachedRowPitch = proc->width * VDD_BPP;
    /* Per-frame timing, logged every 2 s: readback (copy + map), packing and
       sending. With the frame rate this shows which stage limits it. */
    LARGE_INTEGER perfFreq, perfWindow, perfA, perfB;
    double perfReadMs = 0.0, perfPackMs = 0.0, perfSendMs = 0.0;
    UINT perfFrames = 0, perfFull = 0;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfWindow);


    InitializeCriticalSection(&proc->sendLock);
    proc->hSendWork = CreateEventW(NULL, FALSE, FALSE, NULL);
    proc->hSendIdle = CreateEventW(NULL, TRUE, TRUE, NULL);
    proc->hSendStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    proc->sendFailed = FALSE;
    proc->perfSendMs = 0.0;
    proc->hSendThread = (proc->hSendWork && proc->hSendIdle && proc->hSendStop)
        ? CreateThread(NULL, 0, VddSenderThread, proc, 0, NULL) : NULL;
    if (!proc->hSendThread) {
        VddLog("SwapChainRunCore: sender thread could not start (%lu), exiting for reassignment",
               GetLastError());
        goto stop_sender;
    }

    /* Main frame acquisition loop */
    for (;;)
    {
        IDXGIResource* pSurface = NULL;

        /* Check for pending new connection from network thread */
        {
            AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
                (PVOID volatile*)&proc->hPendingConn, NULL);
            if (pending) {
                /* Drop the previous client. On PC the old socket is distinct -> close it. On ivshmem
                   the pending connection is the SAME single slot the host just re-armed, so closing
                   the old wrapper would mark the shared slot CLOSING and kill the new one — abandon
                   the stale wrapper instead. */
                if (proc->hClientConn) {
                    VddWaitSendIdle(proc);
                    if (asb_conn_socket_u64(proc->hClientConn) != ~0ull)
                        VddCloseConn(&proc->hClientConn);          /* PC: real close */
                    else { asb_abandon(proc->hClientConn); proc->hClientConn = NULL; }  /* ivshmem */
                    VddLog("Frame: replaced previous client connection");
                }
                asb_stream_reset(pending);   /* ivshmem: align the fresh full frame at ring offset 0 (no-op on PC) */
                proc->hClientConn = pending;
                proc->frameSeq = 0;
                proc->resizeMsgLen = 0;
                bSentFullFrame = FALSE;   /* new connection -> first frame is full (the reconnect trigger) */
                VddLog("Frame: new client connection active");
            }
        }

        if (proc->hClientConn && !VddPollResizeRequest(proc)) {
            VddLog("Frame: host connection closed");
            VddDropClient(proc);
            bSentFullFrame = FALSE;
        }


        /* Use v2 acquire when available */
#pragma warning(suppress: 4127)
        if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
            IDARG_IN_RELEASEANDACQUIREBUFFER2 bufferInArgs = { 0 };
            IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = { 0 };
            bufferInArgs.Size = sizeof(bufferInArgs);
            hr = IddCxSwapChainReleaseAndAcquireBuffer2(proc->hSwapChain, &bufferInArgs, &buffer);
            pSurface = buffer.MetaData.pSurface;
        } else {
            IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = { 0 };
            hr = IddCxSwapChainReleaseAndAcquireBuffer(proc->hSwapChain, &buffer);
            pSurface = buffer.MetaData.pSurface;
        }

        if (hr == E_PENDING) {
            DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, 100);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                break;  /* Terminate */
            }
            if (waitResult == WAIT_OBJECT_0 + 2) {
                /* Cursor event — query and send cursor update over the transport */
                VddSendCursorUpdate(proc);
                continue;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
                VddLog("SwapChainRunCore: unexpected wait result 0x%lX, exiting for reassignment", waitResult);
                break;
            }

            /* On timeout with a connected client and a cached frame, resend
               the staging texture contents to keep the connection alive. */
            if (waitResult == WAIT_TIMEOUT && bHasFrame &&
                proc->hClientConn != NULL && proc->pStagingTex)
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    /* Lossless, so a still screen sharpens after NV12 frames. */
                    int len = VddPackFrame(proc, &mapped, NULL, 0, FALSE);
                    BOOL send_ok = len > 0 && VddQueueSend(proc, len);

                    proc->pDeviceContext->Unmap(proc->pStagingTex, 0);

                    if (!send_ok) {
                        VddLog("Frame: resend failed, disconnecting (seq=%llu)", proc->frameSeq);
                        VddDropClient(proc);
                        bSentFullFrame = FALSE;
                    }
                }
            }

            continue;
        } else if (SUCCEEDED(hr)) {
            /* Reset retry state on successful acquisition */
            retryDelay = RETRY_DELAY_INIT;
            retryCount = 0;

            /* New frame from IddCx — capture and send to host if connected */
            if (pSurface && proc->pStagingTex)
            {
                ID3D11Texture2D* pTexture = nullptr;
                hr = pSurface->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture);
                if (SUCCEEDED(hr) && pTexture)
                {
                    D3D11_TEXTURE2D_DESC surfaceDesc = {};
                    pTexture->GetDesc(&surfaceDesc);
                    if (surfaceDesc.Width != proc->width || surfaceDesc.Height != proc->height) {
                        bHasFrame = FALSE;
                        bSentFullFrame = FALSE;
                        if (!VddResizeCapture(proc, surfaceDesc.Width, surfaceDesc.Height)) {
                            pTexture->Release();
                            pSurface->Release();
                            break;   /* exit for reassignment */
                        }
                    }

                    QueryPerformanceCounter(&perfA);
                    proc->pDeviceContext->CopyResource(proc->pStagingTex, pTexture);

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                    QueryPerformanceCounter(&perfB);
                    perfReadMs += (double)(perfB.QuadPart - perfA.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
                    if (SUCCEEDED(hr))
                    {
                        bHasFrame = TRUE;
                        cachedRowPitch = mapped.RowPitch;

                        if (proc->hClientConn != NULL)
                        {
                            RECT dirtyRects[VDD_MAX_DIRTY_RECTS];
                            UINT32 rectCount = 0;
                            BOOL sendFull = !bSentFullFrame;  /* first frame must be full */

                            /* Query dirty rects from IddCx.  If GetDirtyRects returns
                               0 rects OR fails, fall back to sending a full frame.
                               0 rects does NOT reliably mean "identical" — during early
                               boot the presenter may not supply dirty rect metadata,
                               and skipping would stall updates until the E_PENDING
                               timeout resend path fires ~100ms later. */
                            if (!sendFull) {
                                IDARG_IN_GETDIRTYRECTS drIn = {};
                                IDARG_OUT_GETDIRTYRECTS drOut = {};
                                drIn.DirtyRectInCount = VDD_MAX_DIRTY_RECTS;
                                drIn.pDirtyRects = dirtyRects;
                                HRESULT drHr = IddCxSwapChainGetDirtyRects(proc->hSwapChain, &drIn, &drOut);
                                if (SUCCEEDED(drHr) && drOut.DirtyRectOutCount > 0) {
                                    rectCount = drOut.DirtyRectOutCount;
                                } else {
                                    sendFull = TRUE;
                                }
                            }

                            BOOL send_ok = TRUE;
                            int len;

                            if (!sendFull) {
                                /* Clamp rects to the frame; empty ones carry no pixels. */
                                for (UINT32 i = 0; i < rectCount; i++) {
                                    if (dirtyRects[i].left < 0) dirtyRects[i].left = 0;
                                    if (dirtyRects[i].top  < 0) dirtyRects[i].top  = 0;
                                    if (dirtyRects[i].right  > (LONG)proc->width)  dirtyRects[i].right  = (LONG)proc->width;
                                    if (dirtyRects[i].bottom > (LONG)proc->height) dirtyRects[i].bottom = (LONG)proc->height;
                                }
                            }
                            /* Most of the screen changed (a game, video): NV12. The
                               first frame after connecting stays lossless BGRA. */
                            BOOL nv12 = FALSE;
                            if (bSentFullFrame) {
                                if (sendFull) {
                                    nv12 = TRUE;
                                } else {
                                    UINT64 area = 0;
                                    for (UINT32 i = 0; i < rectCount; i++)
                                        if (dirtyRects[i].right > dirtyRects[i].left &&
                                            dirtyRects[i].bottom > dirtyRects[i].top)
                                            area += (UINT64)(dirtyRects[i].right - dirtyRects[i].left) *
                                                    (UINT64)(dirtyRects[i].bottom - dirtyRects[i].top);
                                    nv12 = area * 100 >= (UINT64)proc->width * proc->height * VDD_NV12_AREA_PCT;
                                }
                            }
                            QueryPerformanceCounter(&perfA);
                            len = VddPackFrame(proc, &mapped, dirtyRects, sendFull ? 0 : rectCount, nv12);
                            QueryPerformanceCounter(&perfB);
                            perfPackMs += (double)(perfB.QuadPart - perfA.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
                            /* All rects empty after clamping: nothing to send. */
                            if (len == 0 && !sendFull) goto skip_send;
                            perfA = perfB;
                            /* Waits only while the previous frame is still sending. */
                            send_ok = len > 0 && VddQueueSend(proc, len);
                            QueryPerformanceCounter(&perfB);
                            perfSendMs += (double)(perfB.QuadPart - perfA.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
                            perfFrames++;
                            if (sendFull || rectCount == 0 || nv12) perfFull++;
                            if (send_ok && sendFull) bSentFullFrame = TRUE;
                            {
                                double windowMs = (double)(perfB.QuadPart - perfWindow.QuadPart) * 1000.0 /
                                                  (double)perfFreq.QuadPart;
                                if (windowMs >= 2000.0 && perfFrames) {
                                    VddLog("Perf: %.1f fps sent (%u full), readback %.1f ms, pack %.1f ms, "
                                           "wait for sender %.1f ms, send %.1f ms per frame",
                                           perfFrames * 1000.0 / windowMs, perfFull,
                                           perfReadMs / perfFrames, perfPackMs / perfFrames,
                                           perfSendMs / perfFrames, proc->perfSendMs / perfFrames);
                                    perfWindow = perfB;
                                    perfReadMs = perfPackMs = perfSendMs = 0.0;
                                    proc->perfSendMs = 0.0;
                                    perfFrames = perfFull = 0;
                                }
                            }

                            if (!send_ok) {
                                VddLog("Frame: send failed, disconnecting (seq=%llu)", proc->frameSeq);
                                VddDropClient(proc);
                                bSentFullFrame = FALSE;
                            } else if (proc->frameSeq == 1) {
                                VddLog("Frame: first frame sent (%ux%u, %u bytes)",
                                       proc->width, proc->height, proc->width * VDD_BPP * proc->height);
                            }
                        skip_send:;
                        }

                        proc->pDeviceContext->Unmap(proc->pStagingTex, 0);
                    }

                    pTexture->Release();
                }
            }

            if (pSurface) {
                pSurface->Release();
            }

            /* Check for cursor update alongside frame */
            if (proc->hCursorEvent &&
                WaitForSingleObject(proc->hCursorEvent, 0) == WAIT_OBJECT_0) {
                VddSendCursorUpdate(proc);
            }


            hr = IddCxSwapChainFinishedProcessingFrame(proc->hSwapChain);
            if (FAILED(hr)) {
                VddLog("SwapChainRunCore: FinishedProcessingFrame FAILED hr=0x%08X, exiting for reassignment", hr);
                break;
            }
        } else {
            /* ACCESS_LOST can be transient (display mode change) — retry with
               exponential backoff up to MAX_RETRIES.  All other errors: exit
               immediately so WdfObjectDelete triggers IddCx reassignment. */
            if (hr == (HRESULT)0x887A0026 /* DXGI_ERROR_ACCESS_LOST */ && retryCount < MAX_RETRIES) {
                retryCount++;
                VddLog("SwapChainRunCore: DXGI_ERROR_ACCESS_LOST, retry %d/%d after %lums",
                       retryCount, MAX_RETRIES, retryDelay);
                if (WaitForSingleObject(proc->hTerminateEvent, retryDelay) == WAIT_OBJECT_0)
                    break;
                retryDelay = min(retryDelay * 2, RETRY_DELAY_MAX);
                continue;
            }
            VddLog("SwapChainRunCore: AcquireBuffer FAILED hr=0x%08X (retries=%d), exiting for reassignment",
                   hr, retryCount);
            break;
        }
    }

stop_sender:
    if (proc->hSendThread) {
        VddWaitSendIdle(proc);
        SetEvent(proc->hSendStop);
        WaitForSingleObject(proc->hSendThread, 5000);
        CloseHandle(proc->hSendThread);
        proc->hSendThread = NULL;
    }
    if (proc->hSendWork) { CloseHandle(proc->hSendWork); proc->hSendWork = NULL; }
    if (proc->hSendIdle) { CloseHandle(proc->hSendIdle); proc->hSendIdle = NULL; }
    if (proc->hSendStop) { CloseHandle(proc->hSendStop); proc->hSendStop = NULL; }
    DeleteCriticalSection(&proc->sendLock);
}

/* ========================================================================= */
/*  Swap chain cleanup helper                                                */
/* ========================================================================= */

static void VddDestroySwapProc(VDD_SWAP_PROC* proc)
{
    if (!proc) return;

    /* Stop network first */
    proc->bStopNetwork = TRUE;

    VddLog("DestroySwapProc: signaling terminate event");
    if (proc->hTerminateEvent)
        SetEvent(proc->hTerminateEvent);

    BOOL threadStopped = TRUE;
    if (proc->hThread)
    {
        VddLog("DestroySwapProc: waiting for swap chain thread...");
        threadStopped = WaitForSingleObject(proc->hThread, 5000) == WAIT_OBJECT_0;
        VddLog("DestroySwapProc: swap chain thread exited");
        CloseHandle(proc->hThread);
    }

    /* Wait for network thread to exit */
    if (proc->hNetworkThread)
    {
        VddLog("DestroySwapProc: waiting for network thread...");
        WaitForSingleObject(proc->hNetworkThread, 5000);
        VddLog("DestroySwapProc: network thread exited");
        CloseHandle(proc->hNetworkThread);
    }

    /* A mode change keeps the host connected; nothing is mid-frame now that the
       thread and its sender have stopped. */
    if (threadStopped && proc->pCtx && proc->pCtx->bReplugging && proc->hClientConn &&
        !proc->pCtx->hCarriedConn) {
        VddLog("DestroySwapProc: keeping client connection across the mode change");
        proc->pCtx->hCarriedConn = proc->hClientConn;
        proc->hClientConn = NULL;
    }

    /* Close any remaining client connection */
    VddLog("DestroySwapProc: closing client connection (%p)", (void*)proc->hClientConn);
    VddCloseConn(&proc->hClientConn);
    VddLog("DestroySwapProc: closing pending connection");
    {
        AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
            (PVOID volatile*)&proc->hPendingConn, NULL);
        if (pending) VddCloseConn(&pending);
    }

    VddLog("DestroySwapProc: closing terminate event");
    if (proc->hTerminateEvent)
        CloseHandle(proc->hTerminateEvent);

    VddLog("DestroySwapProc: closing cursor resources");
    if (proc->hCursorEvent)
        CloseHandle(proc->hCursorEvent);
    if (proc->pCursorShapeBuffer)
        free(proc->pCursorShapeBuffer);
    if (proc->pSendBuf)
        free(proc->pSendBuf);
    if (proc->pSendBufOut)
        free(proc->pSendBufOut);

    /* Only release the per-swap-chain staging texture.  The D3D device and
       context are cached in VDD_DEVICE_CONTEXT and intentionally NOT released
       here — this is the fix for the pDevice->Release() crash after session
       logoff.  Destroying the D3D device after WdfObjectDelete triggers IddCx
       teardown corrupts DXGI internal state. */
    VddLog("DestroySwapProc: releasing staging texture (staging=%p, device=%p cached)",
           (void*)proc->pStagingTex, (void*)proc->pDevice);
    if (proc->pStagingTex)
        proc->pStagingTex->Release();
    VddLog("DestroySwapProc: staging texture released");

    free(proc);
    VddLog("DestroySwapProc: COMPLETE");
}

/* ========================================================================= */
/*  Device context init/cleanup                                              */
/* ========================================================================= */

static void VddContextInit(VDD_DEVICE_CONTEXT* ctx, WDFDEVICE device)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->wdfDevice = device;

    /* The monitor starts at the default size; the host's window sets it from there. */
    g_ModeWidth = ctx->goodWidth = VDD_DEFAULT_WIDTH;
    g_ModeHeight = ctx->goodHeight = VDD_DEFAULT_HEIGHT;
    ctx->wantedMode = ((LONG64)VDD_DEFAULT_WIDTH << 32) | VDD_DEFAULT_HEIGHT;
}

static void VddContextCleanup(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("Context cleanup starting");

    /* Stop recovery timer (wait for any in-progress callback to finish) */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, TRUE);
    }
    if (ctx->hModeTimer) {
        WdfTimerStop(ctx->hModeTimer, TRUE);
    }

    if (ctx->pSwapProc)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }
    VddCloseConn(&ctx->hCarriedConn);

    /* Release cached D3D device (safe here — no IddCx teardown in progress) */
    if (ctx->pCachedCtx) {
        ctx->pCachedCtx->Release();
        ctx->pCachedCtx = nullptr;
    }
    if (ctx->pCachedDevice) {
        VddLog("Context cleanup: releasing cached D3D device");
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
    }

    VddLog("Context cleanup done");
}

/* ========================================================================= */
/*  InitAdapter (called from D0Entry)                                        */
/* ========================================================================= */

static void VddInitAdapter(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("InitAdapter: entered (ctx=%p, wdfDevice=%p)", (void*)ctx, (void*)ctx->wdfDevice);

    IDDCX_ADAPTER_CAPS adapterCaps = {};
    adapterCaps.Size = sizeof(adapterCaps);
    VddLog("InitAdapter: IDDCX_ADAPTER_CAPS.Size=%u", (unsigned)adapterCaps.Size);

    /* Do NOT set IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16.  This driver is SDR-only
       (BGRA8 staging texture, no-op HDR metadata).  Advertising FP16 makes DWM
       compose this display's desktop in R16G16B16A16_FLOAT, which breaks
       third-party Desktop Duplication capture (Parsec etc.): DuplicateOutput1
       fails with DXGI_ERROR_UNSUPPORTED and the fallback path double-applies
       sRGB gamma, giving a washed-out image. */
    adapterCaps.Flags = IDDCX_ADAPTER_FLAGS_NONE;
    VddLog("InitAdapter: SDR only (FP16 not advertised)");

    adapterCaps.MaxMonitorsSupported = 1;

    adapterCaps.EndPointDiagnostics.Size = sizeof(adapterCaps.EndPointDiagnostics);
    adapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    adapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    adapterCaps.EndPointDiagnostics.pEndPointFriendlyName      = L"AppSandbox Virtual Display";
    adapterCaps.EndPointDiagnostics.pEndPointManufacturerName   = L"AppSandbox";
    adapterCaps.EndPointDiagnostics.pEndPointModelName          = L"VDD";

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    adapterCaps.EndPointDiagnostics.pFirmwareVersion = &version;
    adapterCaps.EndPointDiagnostics.pHardwareVersion = &version;

    VddLog("InitAdapter: adapter caps configured, calling IddCxAdapterInitAsync...");

    /* Put IndirectDeviceContextWrapper on the adapter object so we can
       retrieve our context in AdapterInitFinished */
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = ctx->wdfDevice;
    adapterInit.pCaps     = &adapterCaps;
    adapterInit.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT adapterInitOut = {};
    NTSTATUS st = IddCxAdapterInitAsync(&adapterInit, &adapterInitOut);

    if (NT_SUCCESS(st))
    {
        ctx->hAdapter = adapterInitOut.AdapterObject;

        /* Link adapter wrapper back to our context */
        IndirectDeviceContextWrapper* pWrapper =
            WdfObjectGet_IndirectDeviceContextWrapper(adapterInitOut.AdapterObject);
        pWrapper->pContext = ctx;

        VddLog("InitAdapter: IddCxAdapterInitAsync succeeded (adapter=%p)", (void*)adapterInitOut.AdapterObject);

        /* Render-adapter selection is left to IddCx / the OS default (WARP); the VDD is
           display-only and pins no render adapter. */
    }
    else
    {
        VddLog("InitAdapter: IddCxAdapterInitAsync FAILED st=0x%08X", st);
    }
}

/* ========================================================================= */
/*  FinishInit (called from AdapterInitFinished)                             */
/* ========================================================================= */

static void VddFinishInit(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("FinishInit: adapter ready (ctx=%p)", (void*)ctx);
    UNREFERENCED_PARAMETER(ctx);
}

/* ========================================================================= */
/*  CreateMonitor (called after adapter init finished)                       */
/* ========================================================================= */

static NTSTATUS VddCreateMonitor(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("CreateMonitor: entered (ctx=%p, adapter=%p)", (void*)ctx, (void*)ctx->hAdapter);

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = 0;

    monitorInfo.MonitorDescription.Size     = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = 128;
    monitorInfo.MonitorDescription.pData    = (void*)VddGetEdid();

    VddLog("CreateMonitor: EDID checksum byte=0x%02X, IDDCX_MONITOR_INFO.Size=%u",
           ((const BYTE*)monitorInfo.MonitorDescription.pData)[127],
           (unsigned)monitorInfo.Size);

    /* A container GUID, the same every time this device's monitor arrives */
    if (!ctx->hasContainerId) {
        HRESULT hrGuid = CoCreateGuid(&ctx->containerId);
        VddLog("CreateMonitor: CoCreateGuid hr=0x%08X", hrGuid);
        ctx->hasContainerId = SUCCEEDED(hrGuid);
    }
    monitorInfo.MonitorContainerId = ctx->containerId;

    WDF_OBJECT_ATTRIBUTES monAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttrs, MonitorContextWrapper);

    IDARG_IN_MONITORCREATE createIn = {};
    createIn.ObjectAttributes = &monAttrs;
    createIn.pMonitorInfo     = &monitorInfo;

    IDARG_OUT_MONITORCREATE createOut = {};

    VddLog("CreateMonitor: calling IddCxMonitorCreate...");
    NTSTATUS st = IddCxMonitorCreate(ctx->hAdapter, &createIn, &createOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorCreate FAILED st=0x%08X", st);
        return st;
    }
    VddLog("CreateMonitor: IddCxMonitorCreate succeeded (monitor=%p)", (void*)createOut.MonitorObject);

    ctx->hMonitor = createOut.MonitorObject;

    /* Store context on the monitor object */
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject);
    pMonCtx->pContext = ctx;

    VddLog("CreateMonitor: calling IddCxMonitorArrival...");
    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    st = IddCxMonitorArrival(ctx->hMonitor, &arrivalOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorArrival FAILED st=0x%08X", st);
        return st;
    }

    VddLog("CreateMonitor: monitor created and arrived successfully");

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  Monitor recovery: depart + re-arrive if no AssignSwapChain after Unassign*/
/*  Uses WDF timer so callback runs on a proper WDF thread — IddCx APIs     */
/*  crash when called from raw CreateThread threads in UMDF.                 */
/* ========================================================================= */

static VOID VddRecoveryTimerCallback(WDFTIMER Timer)
{
    RecoveryTimerContext* timerCtx = WdfObjectGet_RecoveryTimerContext(Timer);
    VDD_DEVICE_CONTEXT* ctx = timerCtx->pContext;

    /* If a swap chain was assigned in the meantime, nothing to do */
    if (ctx->pSwapProc) {
        VddLog("Recovery: swap chain already reassigned, skipping");
        return;
    }

    /* No swap chain arrived yet.  This is normal during session transitions
       (logoff → login screen) — DWM may take longer than 5s to start
       compositing.  Do NOT call WdfDeviceSetFailed here; that permanently
       bricks the device and requires a full PnP restart.  The monitor stays
       arrived and IddCx will call AssignSwapChain when DWM is ready. */
    VddLog("Recovery: no AssignSwapChain after timeout — waiting (device remains available)");
}

/* ========================================================================= */
/*  Mode changes: depart, then arrive again offering only the requested size */
/*  (see VDD_DEVICE_CONTEXT). A timer step for each, as with recovery above. */
/* ========================================================================= */

#define VDD_MODE_POLL_MS        20
#define VDD_UNASSIGN_WAIT_MS    3000    /* for the old swap chain to be unassigned */
#define VDD_ASSIGN_WAIT_MS      10000   /* for a swap chain at the new size */

static void VddModeTimerStart(VDD_DEVICE_CONTEXT* ctx, ULONG ms)
{
    if (ctx->hModeTimer)
        WdfTimerStart(ctx->hModeTimer, WDF_REL_TIMEOUT_IN_MS(ms));
}

static void VddSetWantedMode(VDD_DEVICE_CONTEXT* ctx, UINT width, UINT height)
{
    InterlockedExchange64(&ctx->wantedMode, ((LONG64)width << 32) | height);
}

/* The size the host's window asks for, from the swap chain thread. Only limits
   are applied: any size inside them is taken exactly, odd and portrait included. */
static void VddRequestMode(VDD_DEVICE_CONTEXT* ctx, UINT width, UINT height)
{
    if (width < VDD_MIN_WIDTH) width = VDD_MIN_WIDTH;
    if (height < VDD_MIN_HEIGHT) height = VDD_MIN_HEIGHT;
    if (width > VDD_MAX_SIDE) width = VDD_MAX_SIDE;
    if (height > VDD_MAX_SIDE) height = VDD_MAX_SIDE;
    if ((UINT64)width * height > VDD_MAX_PIXELS) height = VDD_MAX_PIXELS / width;
    VddLog("Mode: host asks for %ux%u", width, height);
    VddSetWantedMode(ctx, width, height);
    VddModeTimerStart(ctx, 1);
}

static VOID VddModeTimerCallback(WDFTIMER Timer)
{
    VDD_DEVICE_CONTEXT* ctx = WdfObjectGet_RecoveryTimerContext(Timer)->pContext;
    const LONG64 wanted = InterlockedCompareExchange64(&ctx->wantedMode, 0, 0);
    const UINT w = (UINT)(wanted >> 32), h = (UINT)(wanted & 0xFFFFFFFF);
    const ULONGLONG now = GetTickCount64();
    NTSTATUS st;

    switch (ctx->modeState)
    {
    case VDD_MODE_IDLE:
        if (w == g_ModeWidth && h == g_ModeHeight) return;
        if (!ctx->hMonitor) {
            /* The adapter hasn't reported its monitor yet; it arrives at this size. */
            g_ModeWidth = w;
            g_ModeHeight = h;
            return;
        }
        VddLog("Mode: %ux%u -> %ux%u, monitor departing", g_ModeWidth, g_ModeHeight, w, h);
        ctx->bReplugging = TRUE;
        st = IddCxMonitorDeparture(ctx->hMonitor);
        if (!NT_SUCCESS(st)) {
            VddLog("Mode: IddCxMonitorDeparture FAILED st=0x%08X, staying at %ux%u",
                   st, g_ModeWidth, g_ModeHeight);
            ctx->bReplugging = FALSE;
            VddSetWantedMode(ctx, g_ModeWidth, g_ModeHeight);
            return;
        }
        ctx->hMonitor = NULL;
        ctx->modeState = VDD_MODE_DEPARTED;
        ctx->modeDeadline = now + VDD_UNASSIGN_WAIT_MS;
        VddModeTimerStart(ctx, VDD_MODE_POLL_MS);
        return;

    case VDD_MODE_DEPARTED:
        if (ctx->pSwapProc && now < ctx->modeDeadline) {
            VddModeTimerStart(ctx, VDD_MODE_POLL_MS);
            return;
        }
        g_ModeWidth = w;
        g_ModeHeight = h;
        st = VddCreateMonitor(ctx);
        if (!NT_SUCCESS(st) && (w != ctx->goodWidth || h != ctx->goodHeight)) {
            VddLog("Mode: no monitor at %ux%u (0x%08X), back to %ux%u", w, h, st,
                   ctx->goodWidth, ctx->goodHeight);
            g_ModeWidth = ctx->goodWidth;
            g_ModeHeight = ctx->goodHeight;
            VddSetWantedMode(ctx, ctx->goodWidth, ctx->goodHeight);
            ctx->hMonitor = NULL;
            VddCreateMonitor(ctx);
        }
        ctx->modeState = VDD_MODE_WAIT_ASSIGN;
        ctx->modeDeadline = now + VDD_ASSIGN_WAIT_MS;
        VddModeTimerStart(ctx, VDD_MODE_POLL_MS);
        return;

    case VDD_MODE_WAIT_ASSIGN:
        if (ctx->pSwapProc) {
            VddLog("Mode: swap chain assigned at %ux%u", g_ModeWidth, g_ModeHeight);
            ctx->goodWidth = g_ModeWidth;
            ctx->goodHeight = g_ModeHeight;
        } else if (now < ctx->modeDeadline) {
            VddModeTimerStart(ctx, VDD_MODE_POLL_MS);
            return;
        } else if (g_ModeWidth != ctx->goodWidth || g_ModeHeight != ctx->goodHeight) {
            /* A size Windows won't use must not leave the VM without a display,
               whether or not the host is still there to notice. */
            VddLog("Mode: no swap chain at %ux%u, back to %ux%u", g_ModeWidth, g_ModeHeight,
                   ctx->goodWidth, ctx->goodHeight);
            VddSetWantedMode(ctx, ctx->goodWidth, ctx->goodHeight);
        } else {
            VddLog("Mode: no swap chain yet at %ux%u (normal between sessions)",
                   g_ModeWidth, g_ModeHeight);
        }
        ctx->modeState = VDD_MODE_IDLE;
        ctx->bReplugging = FALSE;
        if (!ctx->pSwapProc)
            VddCloseConn(&ctx->hCarriedConn);   /* the host reconnects when one arrives */
        VddModeTimerStart(ctx, 1);              /* a newer request may be waiting */
        return;
    }
}

/* ========================================================================= */
/*  IddCx: Assign / Unassign Swap Chain                                     */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorAssignSwapChain(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    VddLog("AssignSwapChain: entered (MonitorObject=%p)", (void*)MonitorObject);

    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    /* Cancel any pending recovery timer — a new swap chain is being assigned */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, FALSE);
        VddLog("AssignSwapChain: recovery timer cancelled");
    }

    /* Destroy existing swap proc if any */
    if (ctx->pSwapProc)
    {
        VddLog("AssignSwapChain: destroying existing swap proc");
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
        VddLog("AssignSwapChain: existing swap proc destroyed");
    }

    VddLog("AssignSwapChain: allocating new swap proc (%u bytes)", (unsigned)sizeof(VDD_SWAP_PROC));
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)malloc(sizeof(VDD_SWAP_PROC));
    if (!proc)
    {
        VddLog("AssignSwapChain: malloc FAILED");
        return STATUS_NO_MEMORY;
    }
    memset(proc, 0, sizeof(*proc));
    VddLog("AssignSwapChain: swap proc allocated at %p", (void*)proc);

    proc->pCtx            = ctx;
    proc->hSwapChain      = pInArgs->hSwapChain;
    proc->hAvailableEvent = pInArgs->hNextSurfaceAvailable;
    proc->hTerminateEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    VddLog("AssignSwapChain: hSwapChain=%p hAvailableEvent=%p hTerminateEvent=%p",
           (void*)proc->hSwapChain, (void*)proc->hAvailableEvent, (void*)proc->hTerminateEvent);

    VddLog("AssignSwapChain: getting D3D11 device (LUID=%08X:%08X)...",
           pInArgs->RenderAdapterLuid.HighPart, pInArgs->RenderAdapterLuid.LowPart);

    /* Get or reuse cached D3D11 device (cached in ctx to survive swap chain
       transitions) */
    HRESULT hr = VddGetOrCreateD3DDevice(ctx, pInArgs->RenderAdapterLuid,
                                          &proc->pDevice, &proc->pDeviceContext);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: D3D11 device creation FAILED hr=0x%08X", hr);
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: D3D11 device ready (cached=%p)", (void*)proc->pDevice);

    /* Initialize network state */
    VddLog("AssignSwapChain: initializing network state");
    proc->hClientConn    = NULL;
    proc->hPendingConn   = NULL;
    proc->bStopNetwork   = FALSE;
    proc->frameSeq       = 0;

    /* Create staging texture for CPU readback */
    proc->width  = g_ModeWidth;
    proc->height = g_ModeHeight;
    VddLog("AssignSwapChain: creating staging texture (%ux%u)...", proc->width, proc->height);
    hr = VddCreateStagingTexture(proc, proc->width, proc->height);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: staging texture creation FAILED hr=0x%08X", hr);
        /* Don't release pDevice/pDeviceContext — they're cached in ctx */
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: staging texture created");

    /* Setup hardware cursor BEFORE starting swap chain thread — the thread
       reads proc->hCursorEvent at startup to build its wait handle array */
    VddLog("AssignSwapChain: setting up hardware cursor...");
    proc->hMonitor = MonitorObject;
    proc->hCursorEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    proc->cursorBufSize = 256 * 256 * 4 * 2;  /* max 256x256, 2x for MASKED_COLOR double-height */
    proc->pCursorShapeBuffer = (PBYTE)malloc(proc->cursorBufSize);
    proc->lastShapeId = 0;

    if (proc->hCursorEvent && proc->pCursorShapeBuffer) {
        IDDCX_CURSOR_CAPS cursorCaps = {};
        IDARG_IN_SETUP_HWCURSOR hwCursorArgs = {};

        cursorCaps.Size = sizeof(cursorCaps);
        cursorCaps.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
        cursorCaps.AlphaCursorSupport = TRUE;
        cursorCaps.MaxX = 256;
        cursorCaps.MaxY = 256;

        hwCursorArgs.CursorInfo = cursorCaps;
        hwCursorArgs.hNewCursorDataAvailable = proc->hCursorEvent;

        VddLog("AssignSwapChain: calling IddCxMonitorSetupHardwareCursor...");
        NTSTATUS status = IddCxMonitorSetupHardwareCursor(MonitorObject, &hwCursorArgs);
        if (NT_SUCCESS(status)) {
            VddLog("AssignSwapChain: hardware cursor enabled (max 256x256)");
        } else {
            VddLog("AssignSwapChain: hardware cursor setup FAILED 0x%08X", status);
            CloseHandle(proc->hCursorEvent);
            proc->hCursorEvent = NULL;
        }
    } else {
        VddLog("AssignSwapChain: failed to allocate cursor resources (event=%p buf=%p)",
               (void*)proc->hCursorEvent, (void*)proc->pCursorShapeBuffer);
    }

    /* The host stays connected across a mode change; its first frame here is
       a full one, as for any new client. */
    proc->hClientConn = (AsbConn*)InterlockedExchangePointer(
        (PVOID volatile*)&ctx->hCarriedConn, NULL);

    /* Start swap chain processing thread (cursor must be set up first) */
    VddLog("AssignSwapChain: creating swap chain thread...");
    proc->hThread = CreateThread(NULL, 0, VddSwapChainThread, proc, 0, NULL);
    if (!proc->hThread)
    {
        VddLog("AssignSwapChain: CreateThread FAILED err=%u", GetLastError());
        VddCloseConn(&proc->hClientConn);
        proc->pStagingTex->Release();
        /* Don't release pDevice/pDeviceContext — they're cached in ctx */
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        if (proc->hCursorEvent)
            CloseHandle(proc->hCursorEvent);
        free(proc->pCursorShapeBuffer);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: swap chain thread created (handle=%p)", (void*)proc->hThread);

    /* Start network listener thread for direct host connection */
    VddLog("AssignSwapChain: creating network thread...");
    proc->hNetworkThread = CreateThread(NULL, 0, VddNetworkThread, proc, 0, NULL);
    if (!proc->hNetworkThread)
    {
        VddLog("AssignSwapChain: network thread CreateThread FAILED err=%u", GetLastError());
        /* Non-fatal: swap chain still works, just no remote display */
    }
    else
    {
        VddLog("AssignSwapChain: network thread created (handle=%p)", (void*)proc->hNetworkThread);
    }

    ctx->pSwapProc = proc;

    VddLog("AssignSwapChain: COMPLETE — swap chain assigned, all threads started");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VddMonitorUnassignSwapChain(
    IDDCX_MONITOR MonitorObject)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    /* After a slow mode change this can be the old monitor's, with the new
       monitor's swap chain already running: leave that one alone. */
    if (ctx->pSwapProc && ctx->pSwapProc->hMonitor == MonitorObject)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }

    VddLog("SwapChain unassigned — starting recovery timer (5s)");

    /* Start recovery timer: if IddCx doesn't call AssignSwapChain within
       5 seconds, the timer callback departs and re-arrives the monitor. */
    if (ctx->hRecoveryTimer) {
        WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
    } else {
        VddLog("Recovery: no timer available (not created during DeviceAdd)");
    }

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Parse Monitor Description (EDID)                                 */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    VddLog("ParseMonitorDescription: InputCount=%u", pInArgs->MonitorModeBufferInputCount);

    pOutArgs->MonitorModeBufferOutputCount = 1;

    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->MonitorModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_MONITOR_MODE* pMode = pInArgs->pMonitorModes;
    pMode->Size = sizeof(IDDCX_MONITOR_MODE);
    pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, 0); /* vSyncDivider=0 for monitor modes */

    pOutArgs->PreferredMonitorModeIdx = 0;
    VddLog("ParseMonitorDescription: returning 1 mode (%ux%u@60)", g_ModeWidth, g_ModeHeight);
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Get Default Modes                                                */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorGetDefaultModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    VddLog("GetDefaultModes: InputCount=%u", pInArgs->DefaultMonitorModeBufferInputCount);

    pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
    pOutArgs->PreferredMonitorModeIdx = 0;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->DefaultMonitorModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_MONITOR_MODE* pMode = pInArgs->pDefaultMonitorModes;
    pMode->Size = sizeof(IDDCX_MONITOR_MODE);
    pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, 0); /* vSyncDivider=0 for monitor modes */

    VddLog("GetDefaultModes: returning 1 default mode");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Query Target Modes (WAS MISSING - required by IddCx)             */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorQueryTargetModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    VddLog("QueryTargetModes: InputCount=%u", pInArgs->TargetModeBufferInputCount);

    pOutArgs->TargetModeBufferOutputCount = 1;

    if (pInArgs->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->TargetModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_TARGET_MODE* pMode = pInArgs->pTargetModes;
    pMode->Size = sizeof(IDDCX_TARGET_MODE);
    VddCreateMonitorMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, 1); /* vSyncDivider=1 for TARGET modes */

    VddLog("QueryTargetModes: returning 1 target mode");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Commit Modes (v1 fallback)                                       */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterCommitModes(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("CommitModes (v1): called");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Commit Modes 2                                                */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterCommitModes2(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_COMMITMODES2* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);

    VddLog("CommitModes2 (v2): called, PathCount=%u", pInArgs->PathCount);
    for (UINT i = 0; i < pInArgs->PathCount; i++) {
        UINT flags = (UINT)pInArgs->pPaths[i].Flags;
        BOOL active = (flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        BOOL changed = (flags & IDDCX_PATH_FLAGS_CHANGED) != 0;
        VddLog("CommitModes2: path[%u] monitor=%p flags=0x%X (active=%d changed=%d)",
               i, (void*)pInArgs->pPaths[i].MonitorObject, flags, active, changed);
    }

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Parse Monitor Description 2                                   */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    VddLog("ParseMonitorDescription2: InputCount=%u", pInArgs->MonitorModeBufferInputCount);

    pOutArgs->MonitorModeBufferOutputCount = 1;

    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->MonitorModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_MONITOR_MODE2* pMode = pInArgs->pMonitorModes;
    pMode->Size = sizeof(IDDCX_MONITOR_MODE2);
    pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, 0);
    pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;

    pOutArgs->PreferredMonitorModeIdx = 0;
    VddLog("ParseMonitorDescription2: returning 1 mode (%ux%u@60, 8bpc)", g_ModeWidth, g_ModeHeight);
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Query Target Modes 2                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorQueryTargetModes2(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES2* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    VddLog("QueryTargetModes2: InputCount=%u", pInArgs->TargetModeBufferInputCount);

    pOutArgs->TargetModeBufferOutputCount = 1;

    if (pInArgs->TargetModeBufferInputCount >= 1)
    {
        IDDCX_TARGET_MODE2* pMode = pInArgs->pTargetModes;
        pMode->Size = sizeof(IDDCX_TARGET_MODE2);
        pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
        VddCreateMonitorMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, 1);
    }

    VddLog("QueryTargetModes2: returning 1 target mode (%ux%u@60, 8bpc)", g_ModeWidth, g_ModeHeight);
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Adapter Query Target Info                                     */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterQueryTargetInfo(
    IDDCX_ADAPTER AdapterObject,
    IDARG_IN_QUERYTARGET_INFO* pInArgs,
    IDARG_OUT_QUERYTARGET_INFO* pOutArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    VddLog("QueryTargetInfo: called (ConnectorIndex=%u)", pInArgs->ConnectorIndex);

    pOutArgs->TargetCaps = (IDDCX_TARGET_CAPS)0;
    pOutArgs->DitheringSupport.Rgb = IDDCX_BITS_PER_COMPONENT_8;

    VddLog("QueryTargetInfo: returning caps=0, dithering=8bpc");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Set Default HDR Metadata (no-op, SDR only)                    */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorSetDefaultHdrMetadata(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("SetDefaultHdrMetadata: called (no-op, SDR only)");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Set Gamma Ramp (no-op)                                        */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorSetGammaRamp(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_SET_GAMMARAMP* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("SetGammaRamp: called (no-op)");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Adapter Init Finished                                            */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterInitFinished(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);

    VddLog("AdapterInitFinished: AdapterInitStatus=0x%08X", pInArgs->AdapterInitStatus);

    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        VddLog("AdapterInitFinished: adapter init succeeded, creating monitor...");
        VddFinishInit(pWrapper->pContext);
        NTSTATUS st = VddCreateMonitor(pWrapper->pContext);
        VddLog("AdapterInitFinished: VddCreateMonitor returned 0x%08X", st);
        return st;
    }

    VddLog("AdapterInitFinished: adapter init FAILED, not creating monitor");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: D0 Entry (device power-up)                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState)
{
    VddLog("D0Entry: entered (Device=%p, PreviousState=%d)", (void*)Device, (int)PreviousState);

    /* Set GPU scheduling priority to realtime for this process (WUDFHost.exe) */
    {
        NTSTATUS st = D3DKMTSetProcessSchedulingPriorityClass(
            GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME);
        VddLog("D0Entry: D3DKMTSetProcessSchedulingPriorityClass(REALTIME) = 0x%08X", st);
    }

    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(Device);

    if (pWrapper && pWrapper->pContext)
    {
        VddLog("D0Entry: have context, calling VddInitAdapter");
        VddInitAdapter(pWrapper->pContext);
    }
    else
    {
        VddLog("D0Entry: WARNING - no context (pWrapper=%p)", (void*)pWrapper);
    }

    VddLog("D0Entry: returning STATUS_SUCCESS");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: EvtCleanupCallback                                                  */
/* ========================================================================= */

static void EvtCleanupCallback(WDFOBJECT Object)
{
    VddLog("EvtCleanupCallback: entered (Object=%p)", (void*)Object);
    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(Object);
    if (pWrapper && pWrapper->pContext)
    {
        VddLog("EvtCleanupCallback: cleaning up context %p", (void*)pWrapper->pContext);
        VddContextCleanup(pWrapper->pContext);
        free(pWrapper->pContext);
        pWrapper->pContext = NULL;
    }
    else
    {
        VddLog("EvtCleanupCallback: no context to clean up");
    }
}

/* ========================================================================= */
/*  WDF: Device Add                                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT pDeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    IDD_CX_CLIENT_CONFIG iddConfig;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE hDevice = NULL;
    IndirectDeviceContextWrapper* pWrapper;
    VDD_DEVICE_CONTEXT* ctx;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);

    VddLog("DeviceAdd: entered");

    /* PnP power callbacks FIRST */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = VddDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpCallbacks);
    VddLog("DeviceAdd: PnP callbacks set");

    /* Configure IddCx callbacks */
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);

    iddConfig.EvtIddCxAdapterInitFinished               = VddAdapterInitFinished;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes  = VddMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain             = VddMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain           = VddMonitorUnassignSwapChain;

    /* Use v2 callbacks when compiled with IddCx 1.10+ */
    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)) {
        VddLog("DeviceAdd: registering v2 callbacks (IddCx 1.10+)");
        iddConfig.EvtIddCxAdapterQueryTargetInfo            = VddAdapterQueryTargetInfo;
        iddConfig.EvtIddCxMonitorSetDefaultHdrMetaData      = VddMonitorSetDefaultHdrMetadata;
        iddConfig.EvtIddCxParseMonitorDescription2          = VddParseMonitorDescription2;
        iddConfig.EvtIddCxMonitorQueryTargetModes2          = VddMonitorQueryTargetModes2;
        iddConfig.EvtIddCxAdapterCommitModes2               = VddAdapterCommitModes2;
        iddConfig.EvtIddCxMonitorSetGammaRamp               = VddMonitorSetGammaRamp;
    } else {
        VddLog("DeviceAdd: registering v1 callbacks");
        iddConfig.EvtIddCxParseMonitorDescription           = VddParseMonitorDescription;
        iddConfig.EvtIddCxMonitorQueryTargetModes            = VddMonitorQueryTargetModes;
        iddConfig.EvtIddCxAdapterCommitModes                = VddAdapterCommitModes;
    }

    st = IddCxDeviceInitConfig(pDeviceInit, &iddConfig);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: IddCxDeviceInitConfig FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: IddCxDeviceInitConfig succeeded");

    /* Create WDFDEVICE with IndirectDeviceContextWrapper + EvtCleanupCallback */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);
    attr.EvtCleanupCallback = EvtCleanupCallback;

    st = WdfDeviceCreate(&pDeviceInit, &attr, &hDevice);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: WdfDeviceCreate FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: WdfDeviceCreate succeeded (hDevice=%p)", (void*)hDevice);

    st = IddCxDeviceInitialize(hDevice);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: IddCxDeviceInitialize FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: IddCxDeviceInitialize succeeded");

    /* Heap-allocate device context */
    ctx = (VDD_DEVICE_CONTEXT*)malloc(sizeof(VDD_DEVICE_CONTEXT));
    if (!ctx)
    {
        VddLog("DeviceAdd: malloc for VDD_DEVICE_CONTEXT FAILED");
        return STATUS_NO_MEMORY;
    }

    VddContextInit(ctx, hDevice);

    pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(hDevice);
    pWrapper->pContext = ctx;

    /* Create WDF timer for monitor recovery — fires on a proper WDF thread
       so IddCx APIs (MonitorDeparture, MonitorCreate, MonitorArrival) are safe. */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;
        WDFTIMER hTimer = NULL;

        WDF_TIMER_CONFIG_INIT(&timerConfig, VddRecoveryTimerCallback);
        timerConfig.AutomaticSerialization = FALSE;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, RecoveryTimerContext);
        timerAttrs.ParentObject = hDevice;

        st = WdfTimerCreate(&timerConfig, &timerAttrs, &hTimer);
        if (NT_SUCCESS(st)) {
            RecoveryTimerContext* timerCtx = WdfObjectGet_RecoveryTimerContext(hTimer);
            timerCtx->pContext = ctx;
            ctx->hRecoveryTimer = hTimer;
            VddLog("DeviceAdd: recovery timer created");
        } else {
            VddLog("DeviceAdd: WdfTimerCreate FAILED st=0x%08X (recovery disabled)", st);
        }
    }

    /* And one for mode changes. Without it the display stays at the default size. */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;
        WDFTIMER hTimer = NULL;

        WDF_TIMER_CONFIG_INIT(&timerConfig, VddModeTimerCallback);
        timerConfig.AutomaticSerialization = FALSE;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, RecoveryTimerContext);
        timerAttrs.ParentObject = hDevice;

        st = WdfTimerCreate(&timerConfig, &timerAttrs, &hTimer);
        if (NT_SUCCESS(st)) {
            WdfObjectGet_RecoveryTimerContext(hTimer)->pContext = ctx;
            ctx->hModeTimer = hTimer;
        } else {
            VddLog("DeviceAdd: WdfTimerCreate FAILED st=0x%08X (mode changes disabled)", st);
        }
    }

    VddLog("DeviceAdd: complete, context=%p", (void*)ctx);
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: Driver Unload                                                       */
/* ========================================================================= */

_Use_decl_annotations_
VOID VddDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    VddLog("DriverUnload: called");
}

/* ========================================================================= */
/*  DllMain                                                                  */
/* ========================================================================= */

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    if (dwReason == DLL_PROCESS_ATTACH) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        VddLog("DllMain: DLL_PROCESS_ATTACH (WSAStartup done)");
    } else if (dwReason == DLL_PROCESS_DETACH) {
        VddLog("DllMain: DLL_PROCESS_DETACH");
        WSACleanup();
    }
    return TRUE;
}

/* ========================================================================= */
/*  DriverEntry                                                              */
/* ========================================================================= */

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT pDriverObject,
    PUNICODE_STRING pRegistryPath)
{
    VddLog("DriverEntry: entered (pDriverObject=%p)", pDriverObject);

    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, VddDeviceAdd);
    config.EvtDriverUnload = VddDriverUnload;

    NTSTATUS st = WdfDriverCreate(pDriverObject, pRegistryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(st))
    {
        VddLog("DriverEntry: WdfDriverCreate FAILED st=0x%08X", st);
    }
    else
    {
        VddLog("DriverEntry: WdfDriverCreate succeeded");
    }

    return st;
}

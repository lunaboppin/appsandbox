/*++

Module Name:

    vdd.cpp

Abstract:

    AppSandbox Virtual Display Driver - Two 1920x1080 monitors with
    direct HvSocket frame transport to the host (no agent middleman).

    IddCx lifecycle:
    - DllMain
    - DriverEntry with WDF_OBJECT_ATTRIBUTES_INIT + EvtDriverUnload
    - DeviceAdd with PnP callbacks, IddCx config, EvtCleanupCallback,
      heap-allocated context (IndirectDeviceContextWrapper)
    - D0Entry -> InitAdapter (context type on adapter object)
    - AdapterInitFinished -> FinishInit -> CreateMonitor (twice)
    - ParseMonitorDescription, GetDefaultModes, QueryTargetModes
    - AssignSwapChain with direct HvSocket frame transport to host
    - UnassignSwapChain with cleanup

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

static const GUID g_monitorContainerIds[VDD_MONITOR_COUNT] = {
    { 0xd54e2d6a, 0x9935, 0x4ca0, { 0x8b, 0x5d, 0x54, 0x63, 0x0e, 0x08, 0x31, 0x01 } },
    { 0xd54e2d6a, 0x9935, 0x4ca0, { 0x8b, 0x5d, 0x54, 0x63, 0x0e, 0x08, 0x31, 0x02 } }
};

static const BYTE* VddGetEdid(UINT displayIndex)
{
    static BYTE edid[VDD_MONITOR_COUNT][128];
    static BOOL computed[VDD_MONITOR_COUNT] = {};

    if (displayIndex >= VDD_MONITOR_COUNT) displayIndex = 0;
    if (!computed[displayIndex])
    {
        memcpy(edid[displayIndex], VDD_EDID, 127);
        /* Give Windows two stable EDID serials so display topology survives
           adapter restarts and the monitors cannot be de-duplicated. */
        edid[displayIndex][12] = (BYTE)(displayIndex + 1);
        edid[displayIndex][13] = 0xA5;
        edid[displayIndex][14] = 0xB0;
        edid[displayIndex][15] = 0x01;
        BYTE sum = 0;
        for (int i = 0; i < 127; i++)
            sum += edid[displayIndex][i];
        edid[displayIndex][127] = (BYTE)(256 - sum);
        computed[displayIndex] = TRUE;
    }
    return edid[displayIndex];
}

/* ========================================================================= */
/*  Monitor mode helper                                                      */
/* ========================================================================= */

/* Refresh rates offered at 1920x1080. 60 Hz stays preferred so a guest
   always starts in a safe mode; the guest input helper switches to the
   closest rate to the host monitor the viewer is on. */
static const UINT kVddRefreshRates[] = { 60, 120, 144, 165, 240 };
static void VddCreateMonitorMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, UINT vSyncDivider, UINT hz,
                                 UINT width = VDD_WIDTH, UINT height = VDD_HEIGHT);

#define VDD_RATE_COUNT ((UINT)(sizeof(kVddRefreshRates) / sizeof(kVddRefreshRates[0])))

/* Resolutions offered at every rate. 1920x1080 first, so mode 0 is the
   preferred 1080p60. The host picks between them by viewer window size
   (larger than 1080p -> 1440p) and scales the picture to the window. Both
   modes always exist, so switching is an ordinary mode change. */
static const UINT kVddBaseSizes[][2] = {
    { 1920, 1080 }, { 2560, 1440 },
};
#define VDD_BASE_SIZE_COUNT ((UINT)(sizeof(kVddBaseSizes) / sizeof(kVddBaseSizes[0])))
#define VDD_MAX_SIZES VDD_BASE_SIZE_COUNT
#define VDD_MAX_MODES (VDD_MAX_SIZES * VDD_RATE_COUNT)

static IDDCX_MONITOR g_vddMonitorObjects[VDD_MONITOR_COUNT];

static int VddMonitorIndex(IDDCX_MONITOR monitor)
{
    for (int i = 0; i < (int)VDD_MONITOR_COUNT; i++)
        if (g_vddMonitorObjects[i] == monitor) return i;
    return -1;
}

/* Sizes offered on a display (the same fixed list everywhere). */
static UINT VddSizes(int displayIndex, UINT out[VDD_MAX_SIZES][2])
{
    UNREFERENCED_PARAMETER(displayIndex);
    for (UINT i = 0; i < VDD_BASE_SIZE_COUNT; i++) { out[i][0] = kVddBaseSizes[i][0]; out[i][1] = kVddBaseSizes[i][1]; }
    return VDD_BASE_SIZE_COUNT;
}
static UINT VddModeTotal(int displayIndex)
{
    UINT sizes[VDD_MAX_SIZES][2];
    return VddSizes(displayIndex, sizes) * VDD_RATE_COUNT;
}

/* Mode i of a display: size i / rates, refresh i % rates. */
static void VddFillMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, UINT vSyncDivider, int displayIndex, UINT i)
{
    UINT sizes[VDD_MAX_SIZES][2];
    UINT n = VddSizes(displayIndex, sizes);
    UINT s = i / VDD_RATE_COUNT;
    if (s >= n) s = 0;
    VddCreateMonitorMode(sig, vSyncDivider, kVddRefreshRates[i % VDD_RATE_COUNT], sizes[s][0], sizes[s][1]);
}

static void VddCreateMonitorMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, UINT vSyncDivider, UINT hz,
                                 UINT width, UINT height)
{
    /* totalSize == activeSize, pixelRate = refresh * w * h.
       IddCx validates these relationships; using actual blanking values causes
       STATUS_INVALID_PARAMETER from IddCxMonitorArrival. */
    sig->totalSize.cx                           = width;
    sig->totalSize.cy                           = height;
    sig->activeSize.cx                          = width;
    sig->activeSize.cy                          = height;
    sig->AdditionalSignalInfo.vSyncFreqDivider  = vSyncDivider;
    sig->AdditionalSignalInfo.videoStandard     = 255;
    sig->vSyncFreq.Numerator                    = hz;
    sig->vSyncFreq.Denominator                  = 1;
    sig->hSyncFreq.Numerator                    = hz * height;
    sig->hSyncFreq.Denominator                  = 1;
    sig->scanLineOrdering                       = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    sig->pixelRate                               = (UINT64)hz * width * height;
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

    int channel = proc->displayIndex == 0 ? ASB_CH_DISPLAY : ASB_CH_DISPLAY2;
    VddLog("Network: display %u starting on channel %d", proc->displayIndex + 1, channel);
    VddEnsureTransportInit();

    while (!proc->bStopNetwork) {
        /* (Re)create the listener. PC: socket(AF_HYPERV)+bind(ch2 GUID, wildcard)+listen;
           Mac: bind the ivshmem ch2 slot. */
        l = asb_listen(channel);
        if (!l) {
            VddLog("Network: asb_listen(ch%d) failed, retrying in 3s (transport=%s)", channel,
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

            /* Never let a stalled viewer pin the IddCx capture thread
               indefinitely.  Local HvSocket transfers normally complete far
               below this bound; a non-draining peer is disconnected and can
               reconnect to the latest full frame. */
            asb_set_timeout(nc, 0, 1000);
            {
                unsigned long long socketValue = asb_conn_socket_u64(nc);
                if (socketValue != ~0ull) {
                    int sendBuffer = 16 * 1024 * 1024;
                    setsockopt((SOCKET)socketValue, SOL_SOCKET, SO_SNDBUF,
                               (const char*)&sendBuffer, sizeof(sendBuffer));
                }
            }
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
    EnterCriticalSection(&ctx->d3dCacheLock);
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
            /* The cache owns its references; each swap processor owns another
               pair so replacing a removed/mismatched cached device cannot
               invalidate the other monitor's live worker. */
            (*ppDevice)->AddRef();
            (*ppCtx)->AddRef();
            LeaveCriticalSection(&ctx->d3dCacheLock);
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
        LeaveCriticalSection(&ctx->d3dCacheLock);
        return hr;
    }

    IDXGIAdapter1* pAdapter = nullptr;
    hr = pFactory->EnumAdapterByLuid(adapterLuid, __uuidof(IDXGIAdapter1), (void**)&pAdapter);
    pFactory->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: EnumAdapterByLuid FAILED hr=0x%08X", hr);
        LeaveCriticalSection(&ctx->d3dCacheLock);
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
        LeaveCriticalSection(&ctx->d3dCacheLock);
        return hr;
    }

    VddLog("GetOrCreateD3DDevice: D3D11 device created successfully");

    /* Both monitors share this cached immediate context.  Ask D3D11 to
       serialize context access across their independent capture threads. */
    {
        ID3D11Multithread* multithread = nullptr;
        if (SUCCEEDED(pDevCtx->QueryInterface(__uuidof(ID3D11Multithread),
                                              (void**)&multithread))) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
    }

    /* Cache in device context */
    ctx->pCachedDevice = pDevice;
    ctx->pCachedCtx = pDevCtx;
    ctx->cachedDeviceLuid = adapterLuid;

    *ppDevice = pDevice;
    *ppCtx = pDevCtx;
    /* Preserve the creation references as the cache's ownership and give the
       caller a separate pair for this monitor's swap processor. */
    pDevice->AddRef();
    pDevCtx->AddRef();
    LeaveCriticalSection(&ctx->d3dCacheLock);
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

/* Largest wire frame for a w x h surface: header + every dirty rect + size
   word + a full frame. The buffer grows with the guest resolution. */
#define VDD_SEND_BUF_BYTES_FOR(w, h) (sizeof(VDD_WIRE_FRAME_HEADER) + VDD_MAX_DIRTY_RECTS * sizeof(RECT) + \
                                      sizeof(UINT32) + (size_t)(w) * (h) * 4)

static void VddEnsureSendBuf(VDD_SWAP_PROC* proc)
{
    size_t need = VDD_SEND_BUF_BYTES_FOR(proc->frameW, proc->frameH);
    if (proc->pSendBuf && proc->sendBufCap >= need) return;
    BYTE* grown = (BYTE*)realloc(proc->pSendBuf, need);
    if (grown) { proc->pSendBuf = grown; proc->sendBufCap = need; }
    else VddLog("SwapChain: send buffer growth to %zu bytes failed; using per-row sends", need);
}

/* Send one frame (full when rectCount == 0, else the clamped dirty rects).
   Assembles the whole wire frame and sends it in one call: per-row sends
   cost ~1080 transport calls per full frame and starved video playback.
   Returns 0 on success, -1 on send failure, 1 if the rects' pixel data would
   not fit the buffer (caller sends a full frame instead). */
static int VddSendFrame(VDD_SWAP_PROC* proc, AsbConn* s, const VDD_WIRE_FRAME_HEADER* whdr,
                        const RECT* rects, UINT32 rectCount, UINT32 dataSize,
                        const D3D11_MAPPED_SUBRESOURCE* mapped)
{
    BYTE* p = proc->pSendBuf;
    size_t total = sizeof(*whdr) + rectCount * sizeof(RECT) + sizeof(dataSize) + dataSize;
    UINT32 i;

    if (!p) {
        /* No buffer: fall back to row-by-row sends. */
        if (VddSendAll(s, (const char*)whdr, sizeof(*whdr)) != 0 ||
            (rectCount && VddSendAll(s, (const char*)rects, rectCount * sizeof(RECT)) != 0) ||
            VddSendAll(s, (const char*)&dataSize, sizeof(dataSize)) != 0)
            return -1;
        if (!rectCount) {
            for (UINT row = 0; row < proc->frameH; row++)
                if (VddSendAll(s, (const char*)((const BYTE*)mapped->pData + row * mapped->RowPitch), (int)(proc->frameW * 4)) != 0)
                    return -1;
            return 0;
        }
        for (i = 0; i < rectCount; i++) {
            UINT rw = (UINT)(rects[i].right - rects[i].left), rh = (UINT)(rects[i].bottom - rects[i].top);
            for (UINT row = 0; row < rh; row++) {
                const BYTE* src = (const BYTE*)mapped->pData + ((UINT)rects[i].top + row) * mapped->RowPitch +
                                  (UINT)rects[i].left * 4;
                if (rw && VddSendAll(s, (const char*)src, rw * 4) != 0) return -1;
            }
        }
        return 0;
    }

    if (total > proc->sendBufCap || total > INT_MAX) return 1;
    memcpy(p, whdr, sizeof(*whdr));                          p += sizeof(*whdr);
    if (rectCount) { memcpy(p, rects, rectCount * sizeof(RECT)); p += rectCount * sizeof(RECT); }
    memcpy(p, &dataSize, sizeof(dataSize));                  p += sizeof(dataSize);
    if (!rectCount) {
        for (UINT row = 0; row < proc->frameH; row++, p += proc->frameW * 4)
            memcpy(p, (const BYTE*)mapped->pData + row * mapped->RowPitch, proc->frameW * 4);
    } else {
        for (i = 0; i < rectCount; i++) {
            UINT rw = (UINT)(rects[i].right - rects[i].left), rh = (UINT)(rects[i].bottom - rects[i].top);
            if (!rw) continue;
            for (UINT row = 0; row < rh; row++, p += rw * 4)
                memcpy(p, (const BYTE*)mapped->pData + ((UINT)rects[i].top + row) * mapped->RowPitch +
                          (UINT)rects[i].left * 4, rw * 4);
        }
    }
    return VddSendAll(s, (const char*)proc->pSendBuf, (int)total) == 0 ? 0 : -1;
}

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

    VddEnsureSendBuf(proc);

    VddSwapChainRunCore(proc);

    free(proc->pSendBuf);
    proc->pSendBuf = NULL;
    proc->sendBufCap = 0;

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

    /* Send cursor header */
    if (VddSendAll(s, (const char*)&chdr, sizeof(chdr)) != 0) {
        VddLog("Cursor: send header failed, disconnecting");
        VddCloseConn(&proc->hClientConn);
        return;
    }

    /* Send shape bitmap if updated */
    if (chdr.shape_data_size > 0) {
        if (VddSendAll(s, (const char*)proc->pCursorShapeBuffer, (int)chdr.shape_data_size) != 0) {
            VddLog("Cursor: send shape data failed, disconnecting");
            VddCloseConn(&proc->hClientConn);
            return;
        }
    }
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
    UINT cachedRowPitch = proc->frameW * 4;


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
                    if (asb_conn_socket_u64(proc->hClientConn) != ~0ull)
                        VddCloseConn(&proc->hClientConn);          /* PC: real close */
                    else { asb_abandon(proc->hClientConn); proc->hClientConn = NULL; }  /* ivshmem */
                    VddLog("Frame: replaced previous client connection");
                }
                asb_stream_reset(pending);   /* ivshmem: align the fresh full frame at ring offset 0 (no-op on PC) */
                proc->hClientConn = pending;
                proc->frameSeq = 0;
                bSentFullFrame = FALSE;   /* new connection -> first frame is full (the reconnect trigger) */
                VddLog("Frame: new client connection active");
            }
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

            /* A client can connect while the desktop is completely static.
               Send the cached image once so it has an initial frame, but do
               not use full-frame traffic as a keepalive: at 1080p that was
               8 MiB every 100 ms and could starve fresh frames. */
            if (waitResult == WAIT_TIMEOUT && bHasFrame && !bSentFullFrame &&
                proc->hClientConn != NULL && proc->pStagingTex)
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    VDD_WIRE_FRAME_HEADER whdr;
                    BOOL send_ok;

                    proc->frameSeq++;
                    whdr.magic            = VDD_FRAME_MAGIC;
                    whdr.width            = proc->frameW;
                    whdr.height           = proc->frameH;
                    whdr.stride           = proc->frameW * 4;
                    whdr.frame_seq        = proc->frameSeq;
                    whdr.dirty_rect_count = 0;

                    send_ok = VddSendFrame(proc, proc->hClientConn, &whdr, NULL, 0,
                                           proc->frameW * proc->frameH * 4, &mapped) == 0;

                    proc->pDeviceContext->Unmap(proc->pStagingTex, 0);

                    if (!send_ok) {
                        VddLog("Frame: initial cached frame failed, disconnecting (seq=%llu)", proc->frameSeq);
                        VddCloseConn(&proc->hClientConn);
                        bSentFullFrame = FALSE;
                    } else {
                        bSentFullFrame = TRUE;
                    }
                }
            }

            continue;
        } else if (SUCCEEDED(hr)) {
            /* Reset retry state on successful acquisition */
            retryDelay = RETRY_DELAY_INIT;
            retryCount = 0;

            /* New frame from IddCx — capture and send to host if connected.
               Only the GPU copy and the dirty-rect query need the IddCx
               surface; release it as soon as both are queued (as the IddCx
               sample does) so the compositor can render the next frame while
               this one is read back and sent. */
            BOOL frameFinished = FALSE;
            HRESULT finishHr = S_OK;
            if (pSurface && proc->pStagingTex)
            {
                ID3D11Texture2D* pTexture = nullptr;
                hr = pSurface->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture);
                if (SUCCEEDED(hr) && pTexture)
                {
                    RECT dirtyRects[VDD_MAX_DIRTY_RECTS];
                    UINT32 rectCount = 0;
                    D3D11_TEXTURE2D_DESC surfaceDesc;

                    /* The guest resolution follows the host viewer window: when
                       the surface size changes, recreate the staging copy and
                       start the new size with a full frame. */
                    pTexture->GetDesc(&surfaceDesc);
                    if (surfaceDesc.Width != proc->frameW || surfaceDesc.Height != proc->frameH) {
                        ID3D11Texture2D* oldStaging = proc->pStagingTex;
                        proc->pStagingTex = nullptr;
                        if (SUCCEEDED(VddCreateStagingTexture(proc, surfaceDesc.Width, surfaceDesc.Height))) {
                            if (oldStaging) oldStaging->Release();
                            VddLog("SwapChain[%u]: surface %ux%u -> %ux%u", proc->displayIndex,
                                   proc->frameW, proc->frameH, surfaceDesc.Width, surfaceDesc.Height);
                            proc->frameW = surfaceDesc.Width;
                            proc->frameH = surfaceDesc.Height;
                            VddEnsureSendBuf(proc);
                            bSentFullFrame = FALSE;
                            bHasFrame = FALSE;
                        } else {
                            proc->pStagingTex = oldStaging;   /* keep the old size; copy below is skipped */
                        }
                    }
                    BOOL sendFull = !bSentFullFrame;  /* first frame (or new size) must be full */

                    if (surfaceDesc.Width == proc->frameW && surfaceDesc.Height == proc->frameH)
                        proc->pDeviceContext->CopyResource(proc->pStagingTex, pTexture);
                    proc->pDeviceContext->Flush();

                    /* Query dirty rects from IddCx.  If GetDirtyRects returns
                       0 rects OR fails, fall back to sending a full frame.
                       0 rects does NOT reliably mean "identical" — during early
                       boot the presenter may not supply dirty rect metadata,
                       and skipping would stall updates until the E_PENDING
                       timeout resend path fires ~100ms later. */
                    if (proc->hClientConn != NULL && !sendFull) {
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

                    pTexture->Release();
                    pSurface->Release();
                    pSurface = nullptr;
                    finishHr = IddCxSwapChainFinishedProcessingFrame(proc->hSwapChain);
                    frameFinished = TRUE;

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    hr = SUCCEEDED(finishHr)
                        ? proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped)
                        : finishHr;
                    if (SUCCEEDED(hr))
                    {
                        bHasFrame = TRUE;
                        cachedRowPitch = mapped.RowPitch;

                        if (proc->hClientConn != NULL)
                        {
                            VDD_WIRE_FRAME_HEADER whdr;
                            BOOL send_ok = TRUE;
                            AsbConn* s = proc->hClientConn;
                            UINT32 data_size = 0;
                            int sent;

                            if (!sendFull) {
                                /* Clamp rects to frame bounds; an inverted rect becomes empty. */
                                for (UINT32 i = 0; i < rectCount; i++) {
                                    if (dirtyRects[i].left < 0) dirtyRects[i].left = 0;
                                    if (dirtyRects[i].top  < 0) dirtyRects[i].top  = 0;
                                    if (dirtyRects[i].right  > (LONG)proc->frameW) dirtyRects[i].right  = (LONG)proc->frameW;
                                    if (dirtyRects[i].bottom > (LONG)proc->frameH) dirtyRects[i].bottom = (LONG)proc->frameH;
                                    if (dirtyRects[i].right < dirtyRects[i].left)  dirtyRects[i].right  = dirtyRects[i].left;
                                    if (dirtyRects[i].bottom < dirtyRects[i].top)  dirtyRects[i].bottom = dirtyRects[i].top;
                                    data_size += (UINT32)(dirtyRects[i].right - dirtyRects[i].left) * 4 *
                                                 (UINT32)(dirtyRects[i].bottom - dirtyRects[i].top);
                                }
                                /* All rects zero area after clamping — skip send */
                                if (data_size == 0) goto skip_send;
                            }

                            proc->frameSeq++;
                            whdr.magic            = VDD_FRAME_MAGIC;
                            whdr.width            = proc->frameW;
                            whdr.height           = proc->frameH;
                            whdr.stride           = proc->frameW * 4;
                            whdr.frame_seq        = proc->frameSeq;

                            sent = 1;
                            if (!sendFull) {
                                whdr.dirty_rect_count = rectCount;
                                sent = VddSendFrame(proc, s, &whdr, dirtyRects, rectCount, data_size, &mapped);
                            }
                            if (sent == 1) {
                                /* Full frame (first frame, no rect metadata, or rects
                                   overlapping beyond a full frame's worth of pixels). */
                                whdr.dirty_rect_count = 0;
                                sent = VddSendFrame(proc, s, &whdr, NULL, 0, proc->frameW * proc->frameH * 4, &mapped);
                                if (sent == 0) bSentFullFrame = TRUE;
                            }
                            send_ok = (sent == 0);

                            if (!send_ok) {
                                VddLog("Frame: send failed, disconnecting (seq=%llu)", proc->frameSeq);
                                VddCloseConn(&proc->hClientConn);
                                bSentFullFrame = FALSE;
                            } else if (proc->frameSeq == 1) {
                                VddLog("Frame: first frame sent (%ux%u, %u bytes)",
                                       proc->frameW, proc->frameH, proc->frameW * 4 * proc->frameH);
                            }
                        skip_send:;
                        }

                        proc->pDeviceContext->Unmap(proc->pStagingTex, 0);
                    }
                }
                else if (pTexture)
                {
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

            if (!frameFinished)
                finishHr = IddCxSwapChainFinishedProcessingFrame(proc->hSwapChain);
            if (FAILED(finishHr)) {
                VddLog("SwapChainRunCore: FinishedProcessingFrame FAILED hr=0x%08X, exiting for reassignment", finishHr);
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
}

/* ========================================================================= */
/*  Swap chain cleanup helper                                                */
/* ========================================================================= */

static BOOL VddDestroySwapProc(VDD_SWAP_PROC* proc)
{
    DWORD swapWait = WAIT_OBJECT_0;
    DWORD networkWait = WAIT_OBJECT_0;
    if (!proc) return TRUE;

    /* Stop network first */
    proc->bStopNetwork = TRUE;

    VddLog("DestroySwapProc: signaling terminate event");
    if (proc->hTerminateEvent)
        SetEvent(proc->hTerminateEvent);

    /* Connection wrappers belong to the workers, which can close/free them
       concurrently. Never dereference those slots from this teardown thread.
       Socket sends are bounded by SO_SNDTIMEO; accept polls the stop flag. */

    if (proc->hThread)
    {
        VddLog("DestroySwapProc: waiting for swap chain thread...");
        swapWait = WaitForSingleObject(proc->hThread, 15000);
        VddLog("DestroySwapProc: swap chain wait result=0x%lX", swapWait);
    }

    /* Wait for network thread to exit */
    if (proc->hNetworkThread)
    {
        VddLog("DestroySwapProc: waiting for network thread...");
        networkWait = WaitForSingleObject(proc->hNetworkThread, 15000);
        VddLog("DestroySwapProc: network wait result=0x%lX", networkWait);
    }

    /* A timed-out worker still owns proc and its D3D resources.  Leaking that
       rare failure is safer than the previous use-after-free in WUDFHost. */
    if (swapWait != WAIT_OBJECT_0 || networkWait != WAIT_OBJECT_0) {
        VddLog("DestroySwapProc: worker did not stop; preserving state to avoid use-after-free");
        return FALSE;
    }
    if (proc->hThread) CloseHandle(proc->hThread);
    if (proc->hNetworkThread) CloseHandle(proc->hNetworkThread);
    proc->hThread = proc->hNetworkThread = NULL;

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

    /* Release the processor's references. The cache retains a separate pair,
       so these calls cannot destroy the shared device during IddCx teardown. */
    VddLog("DestroySwapProc: releasing swap resources (staging=%p, device=%p cached)",
           (void*)proc->pStagingTex, (void*)proc->pDevice);
    if (proc->pStagingTex)
        proc->pStagingTex->Release();
    if (proc->pDeviceContext)
        proc->pDeviceContext->Release();
    if (proc->pDevice)
        proc->pDevice->Release();
    VddLog("DestroySwapProc: per-monitor D3D references released");

    free(proc);
    VddLog("DestroySwapProc: COMPLETE");
    return TRUE;
}

/* ========================================================================= */
/*  Device context init/cleanup                                              */
/* ========================================================================= */

static void VddContextInit(VDD_DEVICE_CONTEXT* ctx, WDFDEVICE device)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->wdfDevice = device;
    InitializeCriticalSection(&ctx->d3dCacheLock);

    /* Pre-populate the preferred 1920x1080@60Hz mode shared by both monitors. */
    VddCreateMonitorMode(&ctx->modes[0], 0, kVddRefreshRates[0]);  /* vSyncDivider=0 for monitor modes */
    ctx->modeCount = 1;
}

static void VddContextCleanup(VDD_DEVICE_CONTEXT* ctx)
{
    UINT i;
    BOOL canReleaseDevice = TRUE;
    VddLog("Context cleanup starting");

    /* Stop recovery timer (wait for any in-progress callback to finish) */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, TRUE);
    }

    for (i = 0; i < VDD_MONITOR_COUNT; ++i) {
        if (ctx->pSwapProcs[i]) {
            if (!VddDestroySwapProc(ctx->pSwapProcs[i]))
                canReleaseDevice = FALSE;
            ctx->pSwapProcs[i] = nullptr;
        }
    }

    /* Release cached D3D device (safe here — no IddCx teardown in progress) */
    if (canReleaseDevice && ctx->pCachedCtx) {
        ctx->pCachedCtx->Release();
        ctx->pCachedCtx = nullptr;
    }
    if (canReleaseDevice && ctx->pCachedDevice) {
        VddLog("Context cleanup: releasing cached D3D device");
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
    }
    if (!canReleaseDevice)
        VddLog("Context cleanup: preserving cached D3D device for a live worker");
    DeleteCriticalSection(&ctx->d3dCacheLock);

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

    adapterCaps.MaxMonitorsSupported = VDD_MONITOR_COUNT;

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

static NTSTATUS VddCreateMonitor(VDD_DEVICE_CONTEXT* ctx, UINT displayIndex)
{
    VddLog("CreateMonitor: display %u entered (ctx=%p, adapter=%p)",
           displayIndex + 1, (void*)ctx, (void*)ctx->hAdapter);

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = displayIndex;

    monitorInfo.MonitorDescription.Size     = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = 128;
    monitorInfo.MonitorDescription.pData    = (void*)VddGetEdid(displayIndex);

    VddLog("CreateMonitor: EDID checksum byte=0x%02X, IDDCX_MONITOR_INFO.Size=%u",
           ((const BYTE*)monitorInfo.MonitorDescription.pData)[127],
           (unsigned)monitorInfo.Size);

    monitorInfo.MonitorContainerId = g_monitorContainerIds[displayIndex];

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

    ctx->hMonitors[displayIndex] = createOut.MonitorObject;
    g_vddMonitorObjects[displayIndex] = createOut.MonitorObject;

    /* Store context on the monitor object */
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject);
    pMonCtx->pContext = ctx;
    pMonCtx->displayIndex = displayIndex;

    VddLog("CreateMonitor: calling IddCxMonitorArrival...");
    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    st = IddCxMonitorArrival(ctx->hMonitors[displayIndex], &arrivalOut);
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

    /* If either swap chain was assigned in the meantime, the adapter is
       healthy.  Each monitor will still be reassigned independently by DWM. */
    if (ctx->pSwapProcs[0] || ctx->pSwapProcs[1]) {
        VddLog("Recovery: at least one swap chain already reassigned, skipping");
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
    UINT displayIndex = pMonCtx->displayIndex;
    if (displayIndex >= VDD_MONITOR_COUNT) return STATUS_INVALID_PARAMETER;

    /* Cancel any pending recovery timer — a new swap chain is being assigned */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, FALSE);
        VddLog("AssignSwapChain: recovery timer cancelled");
    }

    /* Destroy existing swap proc if any */
    if (ctx->pSwapProcs[displayIndex])
    {
        VddLog("AssignSwapChain: destroying existing display %u swap proc", displayIndex + 1);
        if (!VddDestroySwapProc(ctx->pSwapProcs[displayIndex])) {
            VddLog("AssignSwapChain: old worker did not stop; abandoning reassignment");
            return (NTSTATUS)0xC01E0012L;
        }
        ctx->pSwapProcs[displayIndex] = nullptr;
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
    proc->displayIndex = displayIndex;
    VddLog("AssignSwapChain: swap proc allocated at %p", (void*)proc);

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
    /* Initial size; the swap-chain loop resizes to the real surface. */
    proc->frameW = VDD_WIDTH;
    proc->frameH = VDD_HEIGHT;
    VddLog("AssignSwapChain: creating staging texture (%ux%u)...", proc->frameW, proc->frameH);
    hr = VddCreateStagingTexture(proc, proc->frameW, proc->frameH);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: staging texture creation FAILED hr=0x%08X", hr);
        proc->pDeviceContext->Release();
        proc->pDevice->Release();
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

    /* Start swap chain processing thread (cursor must be set up first) */
    VddLog("AssignSwapChain: creating swap chain thread...");
    proc->hThread = CreateThread(NULL, 0, VddSwapChainThread, proc, 0, NULL);
    if (!proc->hThread)
    {
        VddLog("AssignSwapChain: CreateThread FAILED err=%u", GetLastError());
        proc->pStagingTex->Release();
        proc->pDeviceContext->Release();
        proc->pDevice->Release();
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

    ctx->pSwapProcs[displayIndex] = proc;

    VddLog("AssignSwapChain: COMPLETE — swap chain assigned, all threads started");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VddMonitorUnassignSwapChain(
    IDDCX_MONITOR MonitorObject)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;
    UINT displayIndex = pMonCtx->displayIndex;
    if (displayIndex >= VDD_MONITOR_COUNT) return STATUS_INVALID_PARAMETER;

    if (ctx->pSwapProcs[displayIndex])
    {
        if (VddDestroySwapProc(ctx->pSwapProcs[displayIndex]))
            ctx->pSwapProcs[displayIndex] = nullptr;
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

    UINT count = VddModeTotal(-1);
    pOutArgs->MonitorModeBufferOutputCount = count;

    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->MonitorModeBufferInputCount < count)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < count; i++) {
        IDDCX_MONITOR_MODE* pMode = &pInArgs->pMonitorModes[i];
        pMode->Size = sizeof(IDDCX_MONITOR_MODE);
        pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        VddFillMode(&pMode->MonitorVideoSignalInfo, 0, -1, i); /* vSyncDivider=0 for monitor modes */
    }

    pOutArgs->PreferredMonitorModeIdx = 0;
    VddLog("ParseMonitorDescription: returning %u modes", count);
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

    int displayIndex = VddMonitorIndex(MonitorObject);
    UINT count = VddModeTotal(displayIndex);
    pOutArgs->DefaultMonitorModeBufferOutputCount = count;
    pOutArgs->PreferredMonitorModeIdx = 0;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->DefaultMonitorModeBufferInputCount < count)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < count; i++) {
        IDDCX_MONITOR_MODE* pMode = &pInArgs->pDefaultMonitorModes[i];
        pMode->Size = sizeof(IDDCX_MONITOR_MODE);
        pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        VddFillMode(&pMode->MonitorVideoSignalInfo, 0, displayIndex, i); /* vSyncDivider=0 for monitor modes */
    }

    VddLog("GetDefaultModes: returning %u default modes", count);
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

    int displayIndex = VddMonitorIndex(MonitorObject);
    UINT count = VddModeTotal(displayIndex);
    pOutArgs->TargetModeBufferOutputCount = count;

    if (pInArgs->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->TargetModeBufferInputCount < count)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < count; i++) {
        IDDCX_TARGET_MODE* pMode = &pInArgs->pTargetModes[i];
        pMode->Size = sizeof(IDDCX_TARGET_MODE);
        VddFillMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, 1, displayIndex, i); /* vSyncDivider=1 for TARGET modes */
    }

    VddLog("QueryTargetModes: returning %u target modes", count);
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

    UINT count = VddModeTotal(-1);
    pOutArgs->MonitorModeBufferOutputCount = count;

    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->MonitorModeBufferInputCount < count)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < count; i++) {
        IDDCX_MONITOR_MODE2* pMode = &pInArgs->pMonitorModes[i];
        pMode->Size = sizeof(IDDCX_MONITOR_MODE2);
        pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        VddFillMode(&pMode->MonitorVideoSignalInfo, 0, -1, i);
        pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    }

    pOutArgs->PreferredMonitorModeIdx = 0;
    VddLog("ParseMonitorDescription2: returning %u modes (8bpc)", count);
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

    int displayIndex = VddMonitorIndex(MonitorObject);
    UINT count = VddModeTotal(displayIndex);
    pOutArgs->TargetModeBufferOutputCount = count;

    if (pInArgs->TargetModeBufferInputCount >= count)
    {
        for (UINT i = 0; i < count; i++) {
            IDDCX_TARGET_MODE2* pMode = &pInArgs->pTargetModes[i];
            pMode->Size = sizeof(IDDCX_TARGET_MODE2);
            pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
            VddFillMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, 1, displayIndex, i);
        }
    }

    VddLog("QueryTargetModes2: returning %u target modes (8bpc)", count);
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
        NTSTATUS st;
        UINT i;
        VddLog("AdapterInitFinished: adapter init succeeded, creating %u monitors...",
               VDD_MONITOR_COUNT);
        VddFinishInit(pWrapper->pContext);
        for (i = 0; i < VDD_MONITOR_COUNT; ++i) {
            st = VddCreateMonitor(pWrapper->pContext, i);
            VddLog("AdapterInitFinished: display %u VddCreateMonitor returned 0x%08X",
                   i + 1, st);
            if (!NT_SUCCESS(st)) return st;
        }
        return STATUS_SUCCESS;
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

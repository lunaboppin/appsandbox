/*
 * audio-mic.c — host microphone -> "App Sandbox Microphone" in the guest.
 * Part of appsandbox-audio.exe (SYSTEM, console session).
 *
 * The AppSandboxVAD driver exposes a capture endpoint whose topology filter
 * carries a private property set (KSPROPSETID_AsbMic, tools/vad/pinnodes.h):
 *   DATA  (SET) — PCM, 48 kHz 16-bit stereo, appended to the driver's ring
 *   STATE (GET) — ULONG, number of running capture streams
 *
 * Channel ASB_CH_MIC (Hyper-V socket only). The host connects; we send a
 * UINT32 state whenever it changes (and right after connecting):
 *   MIC_STATE_IDLE (0)      nothing in the guest is recording
 *   MIC_STATE_RECORDING (1) something is: send microphone audio
 *   MIC_STATE_NO_DEVICE     the guest's audio driver has no microphone
 * The host sends frames: UINT32 byte count, then that many PCM bytes.
 */

#include "../transport/asb_transport.h"
#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")

void audio_log(const char *fmt, ...);   /* appsandbox-audio.c */

#define MIC_STATE_IDLE      0u
#define MIC_STATE_RECORDING 1u
#define MIC_STATE_NO_DEVICE 0xFFFFFFFFu

#define MIC_FRAME_MAX       (64 * 1024)  /* driver's MICIN_MAX_FEED_BYTES */
#define MIC_BLOCK_ALIGN     4            /* 16-bit stereo */
#define MIC_STATE_POLL_MS   200

/* Minimal KS definitions (ks.h equivalents) so this file needs no INITGUID. */
typedef struct {
    GUID  Set;
    ULONG Id;
    ULONG Flags;
} ASB_KSPROPERTY;

#define ASB_KSPROPERTY_TYPE_GET 0x00000001
#define ASB_KSPROPERTY_TYPE_SET 0x00000002
#define ASB_IOCTL_KS_PROPERTY   CTL_CODE(0x0000002F /* FILE_DEVICE_KS */, 0x000, METHOD_NEITHER, FILE_ANY_ACCESS)

static const GUID ASB_KSCATEGORY_TOPOLOGY =
    { 0xDDA54A40, 0x1E4C, 0x11D1, { 0xA0, 0x50, 0x40, 0x57, 0x05, 0xC1, 0x00, 0x00 } };
static const GUID ASB_KSPROPSETID_AsbMic =
    { 0x6A1C3E52, 0x9F0D, 0x4B7E, { 0xA4, 0x31, 0x5C, 0x8E, 0x2D, 0x7F, 0x90, 0x16 } };

/* Open the microphone's topology filter (interface reference "TopologyMicIn"). */
static HANDLE open_mic_filter(void)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA did;
    DWORD i;
    HANDLE h = INVALID_HANDLE_VALUE;

    set = SetupDiGetClassDevsW(&ASB_KSCATEGORY_TOPOLOGY, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    ZeroMemory(&did, sizeof(did));
    did.cbSize = sizeof(did);
    for (i = 0; h == INVALID_HANDLE_VALUE &&
                SetupDiEnumDeviceInterfaces(set, NULL, &ASB_KSCATEGORY_TOPOLOGY, i, &did); i++) {
        BYTE buf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024 * sizeof(wchar_t)];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        wchar_t lower[1100];
        size_t len;

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, sizeof(buf), NULL, NULL))
            continue;
        wcsncpy_s(lower, _countof(lower), detail->DevicePath, _TRUNCATE);
        _wcslwr_s(lower, _countof(lower));
        len = wcslen(lower);
        if (len < 14 || wcscmp(lower + len - 14, L"\\topologymicin") != 0)
            continue;
        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE)
            audio_log("Mic: cannot open %ls (%lu).", detail->DevicePath, GetLastError());
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

static BOOL mic_get_running(HANDLE h, ULONG *running)
{
    ASB_KSPROPERTY p;
    DWORD got = 0;
    p.Set = ASB_KSPROPSETID_AsbMic;
    p.Id = 1;   /* KSPROPERTY_ASBMIC_STATE */
    p.Flags = ASB_KSPROPERTY_TYPE_GET;
    return DeviceIoControl(h, ASB_IOCTL_KS_PROPERTY, &p, sizeof(p),
                           running, sizeof(*running), &got, NULL) && got == sizeof(*running);
}

static BOOL mic_feed(HANDLE h, const BYTE *pcm, DWORD bytes)
{
    ASB_KSPROPERTY p;
    DWORD got = 0;
    p.Set = ASB_KSPROPSETID_AsbMic;
    p.Id = 0;   /* KSPROPERTY_ASBMIC_DATA */
    p.Flags = ASB_KSPROPERTY_TYPE_SET;
    /* KS passes a SET's value in the output buffer. */
    return DeviceIoControl(h, ASB_IOCTL_KS_PROPERTY, &p, sizeof(p),
                           (LPVOID)pcm, bytes, &got, NULL);
}

static BOOL recv_all(AsbConn *c, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = asb_recv(c, (char *)buf + got, len - got);
        if (n <= 0)
            return FALSE;
        got += n;
    }
    return TRUE;
}

static void handle_mic_conn(AsbConn *c)
{
    SOCKET s = (SOCKET)asb_conn_socket_u64(c);
    HANDLE h = open_mic_filter();
    BYTE *frame = NULL;
    UINT32 state, sent_state = 0xDEADBEEF;
    DWORD last_poll = 0;
    UINT64 fed = 0;

    if (h == INVALID_HANDLE_VALUE) {
        state = MIC_STATE_NO_DEVICE;
        asb_send(c, &state, sizeof(state));
        audio_log("Mic: no App Sandbox Microphone device (the audio driver needs updating).");
        return;
    }
    frame = (BYTE *)HeapAlloc(GetProcessHeap(), 0, MIC_FRAME_MAX);
    if (!frame)
        goto out;
    audio_log("Mic: host connected.");

    for (;;) {
        DWORD now = GetTickCount();
        fd_set rf;
        struct timeval tv;
        int sel;

        if (!last_poll || (DWORD)(now - last_poll) >= MIC_STATE_POLL_MS) {
            ULONG running = 0;
            last_poll = now;
            if (!mic_get_running(h, &running)) {
                /* Driver restarted: reopen once, else give up on this connection. */
                CloseHandle(h);
                h = open_mic_filter();
                if (h == INVALID_HANDLE_VALUE || !mic_get_running(h, &running)) {
                    audio_log("Mic: lost the microphone device.");
                    break;
                }
            }
            state = running ? MIC_STATE_RECORDING : MIC_STATE_IDLE;
            if (state != sent_state) {
                if (asb_send(c, &state, sizeof(state)) != (int)sizeof(state))
                    break;
                sent_state = state;
                audio_log("Mic: guest %s.", state ? "started recording" : "stopped recording");
            }
        }

        FD_ZERO(&rf);
        FD_SET(s, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        sel = select(0, &rf, NULL, NULL, &tv);
        if (sel < 0)
            break;
        if (sel == 0)
            continue;

        {
            UINT32 bytes;
            if (!recv_all(c, &bytes, sizeof(bytes)))
                break;
            if (bytes == 0 || bytes > MIC_FRAME_MAX || (bytes % MIC_BLOCK_ALIGN) != 0) {
                audio_log("Mic: bad frame size %u, dropping the connection.", bytes);
                break;
            }
            if (!recv_all(c, frame, (int)bytes))
                break;
            if (mic_feed(h, frame, bytes))
                fed += bytes;
        }
    }
    audio_log("Mic: host disconnected (%llu bytes fed).", fed);

out:
    if (frame)
        HeapFree(GetProcessHeap(), 0, frame);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static DWORD WINAPI mic_listen_thread(LPVOID param)
{
    AsbListener *l;
    (void)param;

    for (;;) {
        l = asb_listen(ASB_CH_MIC);
        if (l)
            break;
        Sleep(5000);   /* ivshmem hosts have no mic channel: stay idle */
    }
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        if (asb_conn_socket_u64(c) != ~0ull)
            handle_mic_conn(c);   /* one host at a time */
        asb_close(c);
    }
}

void mic_start(void)
{
    HANDLE t = CreateThread(NULL, 0, mic_listen_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

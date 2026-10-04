/*
 * appsandbox-input.exe — Console-session input injector for AppSandbox.
 *
 * Runs in the interactive console session (Session 1+) as SYSTEM, spawned by
 * the agent service via CreateProcessAsUser. Receives InputPacket messages
 * from the host over the AppSandbox transport (asb_transport, ASB_CH_INPUT:
 * AF_HYPERV on a Windows host, ivshmem shared memory on a macOS host) and
 * calls SendInput to inject mouse/keyboard events into the input desktop.
 *
 * Desktop following: a thread only injects into the desktop it is attached
 * to, and Windows does not tell it when the input desktop changes (UAC
 * secure desktop, lock screen, logon). Worse, injecting into the wrong
 * desktop frequently "succeeds" while doing nothing. So the injecting thread
 * re-checks the input desktop whenever WinSta0_DesktopSwitch fires, at least
 * every DESKTOP_RECHECK_MS while input is flowing, and after any failure.
 * Running as SYSTEM is what lets it attach to the Winlogon desktop that
 * hosts UAC prompts.
 *
 * Logs to C:\Windows\AppSandbox\input.log (beside agent.log).
 */

#include "../transport/asb_transport.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")

/* ---- Input protocol (must match the host sender) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0           /* p1/p2 = absolute x/y in guest pixels */
#define INPUT_MOUSE_BUTTON  1           /* p1 = button, p2 = down */
#define INPUT_MOUSE_WHEEL   2           /* p1 = INT32 delta */
#define INPUT_KEY           3           /* p1 = vk, p2 = scan, p3 = bit0 ext, bit1 up */
#define INPUT_MOUSE_MOVE_REL 4          /* p1/p2 = INT32 dx/dy */
#define INPUT_MOUSE_HWHEEL  5           /* p1 = INT32 delta */
#define INPUT_SET_REFRESH   0x20        /* p1 = desired refresh rate in Hz */
#define INPUT_SET_MODE      0x21        /* p1 = width, p2 = height, p3 = refresh Hz */
#define INPUT_GUEST_CAPS    0x80        /* guest -> host: p1 = caps, p2 = version */
#define INPUT_GUEST_CURSOR  0x81        /* guest -> host: p1 = 1 if the cursor is hidden */

#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2
#define INPUT_BTN_X1        3
#define INPUT_BTN_X2        4
#define INPUT_BTN_COUNT     5

#define INPUT_CAP_REL_MOUSE 0x01
#define INPUT_CAP_XBUTTONS  0x02
#define INPUT_CAP_HWHEEL    0x04
#define INPUT_CAP_REFRESH   0x08
#define INPUT_CAP_CURSOR_REPORT 0x10
#define INPUT_CAP_SET_MODE  0x20
#define INPUT_PROTO_VERSION 4

#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" little-endian */

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 type;
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;
#pragma pack(pop)

void filedrop_start(void);   /* input-filedrop.c */

/* ---- Logging (rate limited where it can be hot) ---- */

void input_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\input.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ==================================================================
 * Input desktop tracking
 * ================================================================== */

#define DESKTOP_RECHECK_MS 250

/* Bumped by the watcher thread on every desktop switch. */
static volatile LONG g_desktop_switch_seq = 0;

/* Per injecting thread: which desktop it is attached to. */
static __declspec(thread) HDESK   t_desk;
static __declspec(thread) wchar_t t_desk_name[64];
static __declspec(thread) LONG    t_seen_seq = -1;
static __declspec(thread) DWORD   t_last_check;

static DWORD WINAPI desktop_switch_watcher(LPVOID param)
{
    HANDLE ev;
    (void)param;

    /* Signalled (pulsed) by winlogon on every desktop switch in this session. */
    ev = OpenEventW(SYNCHRONIZE, FALSE, L"WinSta0_DesktopSwitch");
    if (!ev) {
        input_log("Desktop watcher: OpenEvent failed (%lu); relying on periodic checks.",
                  GetLastError());
        return 1;
    }
    for (;;) {
        if (WaitForSingleObject(ev, INFINITE) != WAIT_OBJECT_0)
            break;
        InterlockedIncrement(&g_desktop_switch_seq);
        /* The event is pulsed; avoid spinning if it is ever left set. */
        Sleep(10);
    }
    CloseHandle(ev);
    return 0;
}

/* Attach the calling thread to the current input desktop if it has moved.
   Cheap when nothing changed: one sequence compare and a tick compare. */
static void ensure_input_desktop(BOOL force)
{
    LONG seq = g_desktop_switch_seq;
    DWORD now = GetTickCount();
    HDESK desk;
    wchar_t name[64];
    DWORD needed = 0;

    if (!force && t_desk && seq == t_seen_seq &&
        (DWORD)(now - t_last_check) < DESKTOP_RECHECK_MS)
        return;
    t_seen_seq = seq;
    t_last_check = now;

    desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!desk)
        return;   /* transient during a switch; the next event retries */

    name[0] = L'\0';
    GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed);

    if (t_desk && _wcsicmp(name, t_desk_name) == 0) {
        CloseDesktop(desk);
        return;
    }
    if (!SetThreadDesktop(desk)) {
        input_log("SetThreadDesktop(%ls) failed: %lu", name, GetLastError());
        CloseDesktop(desk);
        return;
    }
    /* The previous handle can only be closed once we are off it. */
    if (t_desk)
        CloseDesktop(t_desk);
    t_desk = desk;
    wcscpy_s(t_desk_name, 64, name);
    input_log("Attached to input desktop '%ls'.", name);
}

static CRITICAL_SECTION g_inject_cs;

static void inject(INPUT *inp, const char *what)
{
    static DWORD last_fail_log = 0;

    EnterCriticalSection(&g_inject_cs);
    ensure_input_desktop(FALSE);
    if (SendInput(1, inp, sizeof(INPUT)) == 0) {
        DWORD err = GetLastError();
        /* Most likely the desktop changed between checks: re-attach, retry once. */
        ensure_input_desktop(TRUE);
        if (SendInput(1, inp, sizeof(INPUT)) == 0) {
            DWORD now = GetTickCount();
            if (!last_fail_log || (DWORD)(now - last_fail_log) >= 2000) {
                last_fail_log = now;
                input_log("SendInput(%s) failed: %lu (desktop '%ls')",
                          what, err, t_desk_name);
            }
        }
    }
    LeaveCriticalSection(&g_inject_cs);
}

/* Guest cursor visibility from the cursor watcher: -1 = not known yet. */
static volatile LONG g_cursor_hidden = -1;

/* Diagnostics: one summary line per second of mouse motion. Absolute moves
   while the cursor is hidden are what make games spin, so they are counted. */
static void mouse_diag(BOOL rel, LONG dx, LONG dy)
{
    static DWORD tick;
    static UINT rel_n, abs_hidden_n;
    static LONG sum_x, sum_y;
    DWORD now = GetTickCount();
    if (rel) { rel_n++; sum_x += dx; sum_y += dy; }
    else if (g_cursor_hidden == 1) abs_hidden_n++;
    if ((DWORD)(now - tick) < 1000)
        return;
    if (tick && (rel_n || abs_hidden_n))
        input_log("Mouse: %u relative (sum %ld, %ld), %u absolute while cursor hidden.",
                  rel_n, sum_x, sum_y, abs_hidden_n);
    tick = now;
    rel_n = abs_hidden_n = 0;
    sum_x = sum_y = 0;
}

/* ==================================================================
 * Per-connection state: what this host still holds down
 * ================================================================== */

typedef struct {
    BYTE key_down[256];
    BYTE key_scan[256];
    BYTE key_ext[256];
    BYTE btn_down[INPUT_BTN_COUNT];
} HeldState;

/* Pause and NumLock share scancode 0x45 and Windows reports them swapped in
   the extended bit, so their scancodes cannot be replayed faithfully. */
static BOOL use_vk_injection(UINT32 vk, UINT32 scan)
{
    return scan == 0 || vk == VK_PAUSE || vk == VK_NUMLOCK;
}

static void inject_key(UINT32 vk, UINT32 scan, BOOL ext, BOOL up)
{
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_KEYBOARD;
    if (use_vk_injection(vk, scan)) {
        inp.ki.wVk = (WORD)vk;
        inp.ki.wScan = (WORD)scan;
    } else {
        /* Scancode injection: games reading DirectInput / Raw Input see the
           physical make code, exactly like a real keyboard. */
        inp.ki.wScan = (WORD)scan;
        inp.ki.dwFlags |= KEYEVENTF_SCANCODE;
    }
    if (ext) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up)  inp.ki.dwFlags |= KEYEVENTF_KEYUP;
    inject(&inp, "KEY");
}

static void inject_button(UINT32 btn, BOOL down)
{
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    switch (btn) {
    case INPUT_BTN_LEFT:   inp.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
    case INPUT_BTN_RIGHT:  inp.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
    case INPUT_BTN_MIDDLE: inp.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case INPUT_BTN_X1:
    case INPUT_BTN_X2:
        inp.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        inp.mi.mouseData = (btn == INPUT_BTN_X1) ? XBUTTON1 : XBUTTON2;
        break;
    default:
        return;
    }
    inject(&inp, "MOUSE_BUTTON");
}

/* Release everything a vanished host left held, so nothing sticks down in
   the guest when a display is closed (or the link drops) mid-press. */
static void release_held(HeldState *h)
{
    int i;
    for (i = 0; i < INPUT_BTN_COUNT; i++) {
        if (h->btn_down[i]) {
            inject_button((UINT32)i, FALSE);
            h->btn_down[i] = 0;
        }
    }
    for (i = 0; i < 256; i++) {
        if (h->key_down[i]) {
            inject_key((UINT32)i, h->key_scan[i], h->key_ext[i], TRUE);
            h->key_down[i] = 0;
        }
    }
}

/* ==================================================================
 * Display mode (resolution + refresh rate)
 * ================================================================== */

/* Switch every active display to width x height (0 = keep the current
   resolution, or if the display does not offer it) at the highest refresh
   rate it offers there that does not exceed hz (within 1 Hz). The only
   display in a VM is the AppSandbox VDD (the GPU-PV adapter has no outputs). */
static void apply_mode(UINT32 width, UINT32 height, UINT32 hz)
{
    DISPLAY_DEVICEW dd;
    DWORD di;

    if (hz < 24 || hz > 1000)
        return;

    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (di = 0; EnumDisplayDevicesW(NULL, di, &dd, 0); di++, dd.cb = sizeof(dd)) {
        DEVMODEW cur, dm;
        DWORD mi, best = 0;
        LONG rc;

        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
            continue;

        ZeroMemory(&cur, sizeof(cur));
        cur.dmSize = sizeof(cur);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &cur))
            continue;

        /* Target resolution: the requested one if this display offers it at
           the current colour depth, otherwise the current one. */
        {
            DWORD tw = cur.dmPelsWidth, th = cur.dmPelsHeight;
            if (width && height) {
                for (mi = 0;; mi++) {
                    ZeroMemory(&dm, sizeof(dm));
                    dm.dmSize = sizeof(dm);
                    if (!EnumDisplaySettingsW(dd.DeviceName, mi, &dm))
                        break;
                    if (dm.dmPelsWidth == width && dm.dmPelsHeight == height &&
                        dm.dmBitsPerPel == cur.dmBitsPerPel) {
                        tw = width;
                        th = height;
                        break;
                    }
                }
                if (tw != width || th != height)
                    input_log("Mode: %ls does not offer %ux%u; keeping %lux%lu.",
                              dd.DeviceName, width, height, cur.dmPelsWidth, cur.dmPelsHeight);
            }

            for (mi = 0;; mi++) {
                ZeroMemory(&dm, sizeof(dm));
                dm.dmSize = sizeof(dm);
                if (!EnumDisplaySettingsW(dd.DeviceName, mi, &dm))
                    break;
                if (dm.dmPelsWidth != tw ||
                    dm.dmPelsHeight != th ||
                    dm.dmBitsPerPel != cur.dmBitsPerPel)
                    continue;
                /* +1: hosts report 59/143/239 Hz for 60/144/240 Hz panels. */
                if (dm.dmDisplayFrequency <= hz + 1 && dm.dmDisplayFrequency > best)
                    best = dm.dmDisplayFrequency;
            }

            if (best == 0 ||
                (best == cur.dmDisplayFrequency && tw == cur.dmPelsWidth && th == cur.dmPelsHeight)) {
                input_log("Mode: %ls stays at %lux%lu @ %lu Hz (requested %ux%u @ %u).",
                          dd.DeviceName, cur.dmPelsWidth, cur.dmPelsHeight,
                          cur.dmDisplayFrequency, width, height, hz);
                continue;
            }

            cur.dmPelsWidth = tw;
            cur.dmPelsHeight = th;
            cur.dmDisplayFrequency = best;
            cur.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
            rc = ChangeDisplaySettingsExW(dd.DeviceName, &cur, NULL, CDS_UPDATEREGISTRY, NULL);
            input_log("Mode: %ls -> %lux%lu @ %lu Hz (requested %ux%u @ %u): %s (%ld).",
                      dd.DeviceName, tw, th, best, width, height, hz,
                      rc == DISP_CHANGE_SUCCESSFUL ? "ok" : "failed", rc);
        }
    }
}

/* ==================================================================
 * Packet handling
 * ================================================================== */

static void handle_packet(const InputPacket *pkt, HeldState *h)
{
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));

    switch (pkt->type) {
    case INPUT_MOUSE_MOVE: {
        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 1) screen_w = 1920;
        if (screen_h <= 1) screen_h = 1080;
        inp.type = INPUT_MOUSE;
        inp.mi.dx = (LONG)((UINT64)pkt->param1 * 65535 / (UINT32)(screen_w - 1));
        inp.mi.dy = (LONG)((UINT64)pkt->param2 * 65535 / (UINT32)(screen_h - 1));
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inject(&inp, "MOUSE_MOVE");
        mouse_diag(FALSE, 0, 0);
        break;
    }
    case INPUT_MOUSE_MOVE_REL:
        /* No MOUSEEVENTF_ABSOLUTE: Raw Input then reports MOUSE_MOVE_RELATIVE
           with the real delta, which is what game engines read. NOCOALESCE
           keeps every delta instead of letting the system merge them. */
        inp.type = INPUT_MOUSE;
        inp.mi.dx = (LONG)(INT32)pkt->param1;
        inp.mi.dy = (LONG)(INT32)pkt->param2;
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
        inject(&inp, "MOUSE_MOVE_REL");
        mouse_diag(TRUE, inp.mi.dx, inp.mi.dy);
        break;
    case INPUT_MOUSE_BUTTON:
        if (pkt->param1 < INPUT_BTN_COUNT) {
            inject_button(pkt->param1, pkt->param2 != 0);
            h->btn_down[pkt->param1] = pkt->param2 ? 1 : 0;
        }
        break;
    case INPUT_MOUSE_WHEEL:
    case INPUT_MOUSE_HWHEEL:
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = pkt->type == INPUT_MOUSE_WHEEL ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
        inp.mi.mouseData = (DWORD)(INT32)pkt->param1;
        inject(&inp, "MOUSE_WHEEL");
        break;
    case INPUT_KEY: {
        UINT32 vk = pkt->param1 & 0xFF;
        BOOL ext = (pkt->param3 & 1) != 0;
        BOOL up  = (pkt->param3 & 2) != 0;
        inject_key(vk, pkt->param2 & 0xFF, ext, up);
        h->key_down[vk] = up ? 0 : 1;
        h->key_scan[vk] = (BYTE)pkt->param2;
        h->key_ext[vk]  = (BYTE)(ext ? 1 : 0);
        break;
    }
    case INPUT_SET_REFRESH:
        ensure_input_desktop(FALSE);
        apply_mode(0, 0, pkt->param1);
        break;
    case INPUT_SET_MODE:
        ensure_input_desktop(FALSE);
        apply_mode(pkt->param1, pkt->param2, pkt->param3);
        break;
    default:
        break;   /* unknown types from newer hosts are ignored */
    }
}

/* ==================================================================
 * Guest cursor visibility reporting
 *
 * The host switches to relative mouse mode when the guest hides its cursor.
 * The display driver's hardware-cursor packet is one signal; this is a second
 * one that does not depend on the driver: poll GetCursorInfo on the input
 * desktop and report changes to the host.
 * ================================================================== */

#define CURSOR_POLL_MS 16

/* Guards g_report_conn and every send on it (the recv loop never sends). */
static CRITICAL_SECTION g_send_cs;
static AsbConn *g_report_conn = NULL;

static void send_cursor_locked(AsbConn *c, BOOL hidden)
{
    InputPacket pkt;
    ZeroMemory(&pkt, sizeof(pkt));
    pkt.magic  = INPUT_MAGIC;
    pkt.type   = INPUT_GUEST_CURSOR;
    pkt.param1 = hidden ? 1 : 0;
    if (asb_send(c, &pkt, sizeof(pkt)) != (int)sizeof(pkt))
        input_log("Failed to send cursor state.");
}

static DWORD WINAPI cursor_watch_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        CURSORINFO ci;
        LONG hidden;

        Sleep(CURSOR_POLL_MS);
        /* Per-thread attachment, like the injector: GetCursorInfo reports the
           cursor of the desktop this thread is on. */
        ensure_input_desktop(FALSE);
        ZeroMemory(&ci, sizeof(ci));
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci))
            continue;
        /* hCursor == NULL: SetCursor(NULL), which games also use to hide it. */
        hidden = (!(ci.flags & CURSOR_SHOWING) ||
                  (ci.flags & CURSOR_SUPPRESSED) ||
                  ci.hCursor == NULL) ? 1 : 0;
        if (hidden == g_cursor_hidden)
            continue;

        EnterCriticalSection(&g_send_cs);
        InterlockedExchange(&g_cursor_hidden, hidden);
        if (g_report_conn)
            send_cursor_locked(g_report_conn, hidden != 0);
        LeaveCriticalSection(&g_send_cs);
        {
            static DWORD last_log = 0;
            DWORD now = GetTickCount();
            if (!last_log || (DWORD)(now - last_log) >= 1000) {
                last_log = now;
                input_log("Guest cursor %s (flags 0x%lX).", hidden ? "hidden" : "shown", ci.flags);
            }
        }
    }
}

/* Receive exactly len bytes (transport may deliver partial reads). */
static int recv_full(AsbConn *c, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = asb_recv(c, (char *)buf + got, len - got);
        if (n <= 0)
            return n;   /* 0 = peer closed, <0 = error */
        got += n;
    }
    return got;
}

static void handle_conn(AsbConn *c)
{
    InputPacket pkt;
    HeldState held;
    UINT pkt_count = 0;
    UINT32 ready = INPUT_READY_MAGIC;
    BOOL report = asb_conn_socket_u64(c) != ~0ull;
    BOOL sent;

    ZeroMemory(&held, sizeof(held));

    /* Ready first (all hosts wait for exactly this), then our capabilities.
       Hosts that predate the caps packet never read past the ready magic,
       so the extra bytes are harmless to them. Cursor reports are only sent
       on socket connections (ivshmem hosts never read past the ready magic). */
    ZeroMemory(&pkt, sizeof(pkt));
    pkt.magic  = INPUT_MAGIC;
    pkt.type   = INPUT_GUEST_CAPS;
    pkt.param1 = INPUT_CAP_REL_MOUSE | INPUT_CAP_XBUTTONS |
                 INPUT_CAP_HWHEEL | INPUT_CAP_REFRESH | INPUT_CAP_SET_MODE |
                 (report ? INPUT_CAP_CURSOR_REPORT : 0);
    pkt.param2 = INPUT_PROTO_VERSION;
    EnterCriticalSection(&g_send_cs);
    sent = asb_send(c, &ready, sizeof(ready)) == (int)sizeof(ready) &&
           asb_send(c, &pkt, sizeof(pkt)) == (int)sizeof(pkt);
    if (sent && report) {
        /* From here on the cursor thread may report on this connection.
           Send the current state at once so the host starts in sync. */
        g_report_conn = c;
        if (g_cursor_hidden >= 0)
            send_cursor_locked(c, g_cursor_hidden != 0);
    }
    LeaveCriticalSection(&g_send_cs);
    if (!sent) {
        input_log("Failed to send ready/caps.");
        return;
    }
    ensure_input_desktop(TRUE);
    input_log("Sent ready + caps (0x%02X). Entering recv loop.", pkt.param1);

    for (;;) {
        int n = recv_full(c, &pkt, (int)sizeof(pkt));
        if (n <= 0) {
            input_log("%s after %u packets.",
                      n == 0 ? "Host disconnected" : "recv error", pkt_count);
            break;
        }
        if (pkt.magic != INPUT_MAGIC) {
            input_log("Bad magic 0x%08X, dropping connection.", pkt.magic);
            break;   /* misaligned stream: let the host reconnect cleanly */
        }
        pkt_count++;
        handle_packet(&pkt, &held);
    }

    /* Stop cursor reports before the caller closes the connection. */
    EnterCriticalSection(&g_send_cs);
    if (g_report_conn == c)
        g_report_conn = NULL;
    LeaveCriticalSection(&g_send_cs);

    release_held(&held);
}

/* ==================================================================
 * Connections: newest wins
 *
 * The host reconnects whenever it decides the link is stale (display
 * reopened, send error, helper restart). If we served connections strictly
 * one at a time, a half-dead old connection would keep the new one waiting
 * in the backlog and the user would see no input until they reopened the
 * display. Instead each connection gets a thread and a new arrival shuts
 * the previous one down.
 * ================================================================== */

static CRITICAL_SECTION g_conn_cs;
static AsbConn *g_active_conn = NULL;

static DWORD WINAPI conn_thread(LPVOID param)
{
    AsbConn *c = (AsbConn *)param;
    handle_conn(c);

    EnterCriticalSection(&g_conn_cs);
    if (g_active_conn == c)
        g_active_conn = NULL;
    LeaveCriticalSection(&g_conn_cs);

    asb_close(c);
    return 0;
}

static void kick_active_conn(void)
{
    EnterCriticalSection(&g_conn_cs);
    if (g_active_conn) {
        unsigned long long s = asb_conn_socket_u64(g_active_conn);
        if (s != ~0ull) {
            /* Unblocks the old thread's recv; it releases its held input
               and closes the connection itself. */
            shutdown((SOCKET)s, SD_BOTH);
            input_log("New host connection: shutting down the previous one.");
        }
    }
    LeaveCriticalSection(&g_conn_cs);
}

int main(void)
{
    AsbListener *l;

    InitializeCriticalSection(&g_inject_cs);
    InitializeCriticalSection(&g_conn_cs);
    InitializeCriticalSection(&g_send_cs);

    input_log("Starting (PID=%lu, session=%lu).",
              GetCurrentProcessId(),
              WTSGetActiveConsoleSessionId());

    {
        HANDLE w = CreateThread(NULL, 0, desktop_switch_watcher, NULL, 0, NULL);
        if (w) CloseHandle(w);
        w = CreateThread(NULL, 0, cursor_watch_thread, NULL, 0, NULL);
        if (w) CloseHandle(w);
    }

    if (asb_transport_init() != 0) {
        input_log("asb_transport_init failed.");
        return 1;
    }

    /* Files dragged onto the host display window (own channel and threads). */
    filedrop_start();

    l = asb_listen(ASB_CH_INPUT);
    if (!l) {
        input_log("asb_listen(ASB_CH_INPUT) failed.");
        return 1;
    }
    input_log("Listening on input channel (transport=%s).",
              asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        HANDLE t;

        if (!c) {
            Sleep(100);
            continue;
        }
        input_log("Host connected.");

        if (asb_conn_socket_u64(c) == ~0ull) {
            /* ivshmem: a single shared slot, so connections are inherently
               sequential; serve it inline as before. */
            handle_conn(c);
            asb_close(c);
            continue;
        }

        kick_active_conn();
        EnterCriticalSection(&g_conn_cs);
        g_active_conn = c;
        LeaveCriticalSection(&g_conn_cs);

        t = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
        if (t) {
            CloseHandle(t);
        } else {
            input_log("CreateThread failed (%lu); serving inline.", GetLastError());
            EnterCriticalSection(&g_conn_cs);
            g_active_conn = NULL;
            LeaveCriticalSection(&g_conn_cs);
            handle_conn(c);
            asb_close(c);
        }
    }
}

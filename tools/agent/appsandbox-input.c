/*
 * appsandbox-input.exe — Console-session input injector for AppSandbox.
 *
 * Runs in the interactive console session (Session 1+), spawned by the agent
 * service via CreateProcessAsUser. Receives InputPacket messages from the host
 * over the AppSandbox transport (asb_transport, ASB_CH_INPUT: AF_HYPERV on a
 * Windows host, ivshmem shared memory on a macOS host) and calls SendInput to
 * inject mouse/keyboard events into the active desktop.
 *
 * Logs to C:\Windows\AppSandbox\input.log (beside agent.log).
 */

#include "../transport/asb_transport.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../vhid/appsandbox_vhid_ioctl.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

/* ---- Input protocol (must match the host sender) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3
#define INPUT_MOUSE_RELATIVE 4
#define INPUT_DISPLAY_REFRESH 5   /* p1 = host monitor Hz, p3 = display index */
#define INPUT_DISPLAY_COUNT   6   /* p1 = number of guest monitors (1 or 2) */
#define INPUT_DISPLAY_RESIZE  7   /* p1 = width, p2 = height, p3 = display index */
#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2
#define INPUT_BTN_X1        3
#define INPUT_BTN_X2        4
#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" little-endian */
#define INPUT_READY_VHID    0x44494856  /* "VHID" little-endian */
/* Same, plus "absolute moves are normalized 0..65535" (newer hosts send that only to helpers announcing it). */
#define INPUT_READY_MAGIC2  0x32445249  /* "IRD2" */
#define INPUT_READY_VHID2   0x32444856  /* "VHD2" */

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 type;
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;
#pragma pack(pop)

/* ---- Logging ---- */

static void input_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    /* A persistent failure repeats per mouse move; keep one line per second. */
    if (strstr(fmt, "failed")) {
        static ULONGLONG last_failure_log;
        static UINT suppressed;
        ULONGLONG now = GetTickCount64();
        if (now - last_failure_log < 1000) { suppressed++; return; }
        last_failure_log = now;
        if (suppressed) {
            UINT count = suppressed;
            suppressed = 0;
            input_log("(%u similar failure lines suppressed)", count);
        }
    }
    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\input.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- Desktop switching ---- */

static void switch_to_input_desktop(void)
{
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (desk) {
        SetThreadDesktop(desk);
        CloseDesktop(desk);
    }
}

/* SendInput only reaches the desktop this thread is attached to. The input
   desktop changes at lock, unlock, logon and UAC prompts; a helper attached
   to the old one gets ERROR_ACCESS_DENIED for every event, while VHID buttons
   (a kernel device) still click wherever the cursor was left. Re-attach to
   the current input desktop and retry once. Preserves GetLastError. */
static UINT send_input_on_input_desktop(INPUT *inp)
{
    UINT result = SendInput(1, inp, sizeof(INPUT));
    if (result == 0 && GetLastError() == ERROR_ACCESS_DENIED) {
        switch_to_input_desktop();
        result = SendInput(1, inp, sizeof(INPUT));
    }
    return result;
}

/* ---- Input injection ---- */

static HANDLE g_vhid = INVALID_HANDLE_VALUE;
static UINT32 g_vhid_buttons;
static INT32 g_vhid_wheel_remainder;
static BOOL g_vhid_failed;
static UINT32 g_legacy_buttons;
static BOOL g_held_keys[256];
static WORD g_held_scans[256];
static DWORD g_held_flags[256];

typedef struct DisplayList {
    RECT rects[8];
    BOOL primary[8];
    UINT count;
} DisplayList;

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC dc, LPRECT rect,
                                     LPARAM param)
{
    DisplayList *list = (DisplayList *)param;
    MONITORINFO info;
    (void)dc; (void)rect;
    if (list->count >= _countof(list->rects)) return FALSE;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        list->rects[list->count] = info.rcMonitor;
        list->primary[list->count] =
            (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->count++;
    }
    return TRUE;
}

/* Resolve the stable EDID serial published by VDD. Primary/left-to-right
   ordering changes when Windows rearranges displays and is not a channel ID. */
static BOOL is_vdd_monitor(const wchar_t *device_path, UINT index)
{
    wchar_t instance[256], key_path[512], *end, *p;
    HKEY key;
    BYTE edid[128];
    DWORD bytes = sizeof(edid), type = 0, serial;
    LONG result;
    if (wcsncmp(device_path, L"\\\\?\\DISPLAY#", 12) != 0) return FALSE;
    wcscpy_s(instance, _countof(instance), device_path + 4);
    end = wcsrchr(instance, L'#');
    if (!end) return FALSE;
    *end = L'\0';
    for (p = instance; *p; ++p) if (*p == L'#') *p = L'\\';
    swprintf_s(key_path, _countof(key_path),
               L"SYSTEM\\CurrentControlSet\\Enum\\%s\\Device Parameters", instance);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
    result = RegQueryValueExW(key, L"EDID", NULL, &type, edid, &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_BINARY || bytes < 16 ||
        edid[8] != 0x06 || edid[9] != 0x62 || edid[10] != 0x01 || edid[11] != 0) return FALSE;
    serial = (DWORD)edid[12] | ((DWORD)edid[13] << 8) | ((DWORD)edid[14] << 16) | ((DWORD)edid[15] << 24);
    /* VDD serial: low byte = display index + 1, upper bytes 01 B0 A5 (vdd.cpp VddGetEdid). */
    return (serial & 0xFFu) == index + 1 && (serial >> 8) == 0x01B0A5u;
}

static BOOL vdd_display_rect(UINT index, RECT *rect)
{
    DISPLAY_DEVICEW adapter, monitor;
    DWORD a, m;
    for (a = 0; ; ++a) {
        ZeroMemory(&adapter, sizeof(adapter)); adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, a, &adapter, 0)) break;
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        for (m = 0; ; ++m) {
            DEVMODEW mode;
            ZeroMemory(&monitor, sizeof(monitor)); monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(adapter.DeviceName, m, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            if (!is_vdd_monitor(monitor.DeviceID, index)) continue;
            ZeroMemory(&mode, sizeof(mode)); mode.dmSize = sizeof(mode);
            if (!EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) continue;
            rect->left = mode.dmPosition.x; rect->top = mode.dmPosition.y;
            rect->right = rect->left + mode.dmPelsWidth; rect->bottom = rect->top + mode.dmPelsHeight;
            return TRUE;
        }
    }
    return FALSE;
}

/* GDI device name (\\.\DISPLAYn) of the VDD monitor for a display index. */
static BOOL vdd_display_device(UINT index, wchar_t *name, size_t name_chars)
{
    DISPLAY_DEVICEW adapter, monitor;
    DWORD a, m;
    for (a = 0; ; ++a) {
        ZeroMemory(&adapter, sizeof(adapter)); adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, a, &adapter, 0)) return FALSE;
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        for (m = 0; ; ++m) {
            ZeroMemory(&monitor, sizeof(monitor)); monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(adapter.DeviceName, m, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            if (is_vdd_monitor(monitor.DeviceID, index)) {
                wcscpy_s(name, name_chars, adapter.DeviceName);
                return TRUE;
            }
        }
    }
}

/* Refresh rate the host asked for, per display (0 = none yet). The display
   guard thread re-applies it every few seconds, because Windows refuses mode
   changes for a while after a topology change; resolution switches use it. */
static volatile LONG g_host_hz[2];

/* Match the host monitor's refresh rate: pick the VDD mode at the current
   resolution whose rate is closest to the host's (e.g. 239 Hz -> 240). */
static void apply_host_refresh(UINT index, UINT host_hz)
{
    wchar_t device[32];
    DEVMODEW current, mode, best;
    DWORD i, best_diff = MAXDWORD;
    LONG rc;
    if (!host_hz || !vdd_display_device(index, device, _countof(device))) return;
    ZeroMemory(&current, sizeof(current)); current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &current)) return;
    ZeroMemory(&best, sizeof(best));
    for (i = 0; ; ++i) {
        DWORD diff;
        ZeroMemory(&mode, sizeof(mode)); mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(device, i, &mode)) break;
        if (mode.dmPelsWidth != current.dmPelsWidth || mode.dmPelsHeight != current.dmPelsHeight ||
            mode.dmBitsPerPel != current.dmBitsPerPel) continue;
        diff = mode.dmDisplayFrequency > host_hz ? mode.dmDisplayFrequency - host_hz
                                                 : host_hz - mode.dmDisplayFrequency;
        if (diff < best_diff) { best_diff = diff; best = mode; }
    }
    if (best_diff == MAXDWORD || best.dmDisplayFrequency == current.dmDisplayFrequency) return;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    rc = ChangeDisplaySettingsExW(device, &best, NULL, CDS_UPDATEREGISTRY, NULL);
    {
        /* Retried every few seconds while Windows refuses; log changes only. */
        static LONG last_rc[2] = { 1, 1 };
        if (rc == DISP_CHANGE_SUCCESSFUL || rc != last_rc[index & 1])
            input_log("Display %u (%ls): host %u Hz -> guest %lu Hz (was %lu Hz) -> %ld",
                      index + 1, device, host_hz, best.dmDisplayFrequency, current.dmDisplayFrequency, rc);
        last_rc[index & 1] = rc;
    }
}

/* 1 or 2 guest monitors (viewer badge menu). The second VDD monitor is
   detached/re-attached; re-attaching places it right of display 1 at the
   same size and refresh. A detached monitor no longer enumerates under its
   adapter, so remember its device name, falling back to any detached
   AppSandbox display adapter. */
static wchar_t g_second_display[32];
static void promote_to_primary(const wchar_t *device);

static void set_display_count(UINT count)
{
    wchar_t device[32], first[32];
    DEVMODEW dm;
    LONG rc;
    if (count < 2) {
        if (!vdd_display_device(1, device, _countof(device))) return;   /* already detached */
        wcscpy_s(g_second_display, _countof(g_second_display), device);
        /* The remaining display must be the one the main viewer shows. */
        if (vdd_display_device(0, first, _countof(first)))
            promote_to_primary(first);
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;   /* all zero = detach */
        rc = ChangeDisplaySettingsExW(device, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
        input_log("Displays: 1 (detached %ls) -> %ld", device, rc);
        return;
    }
    if (vdd_display_device(1, device, _countof(device))) return;       /* already attached */
    if (!g_second_display[0]) {
        DISPLAY_DEVICEW adapter;
        DWORD a;
        for (a = 0; ; ++a) {
            ZeroMemory(&adapter, sizeof(adapter)); adapter.cb = sizeof(adapter);
            if (!EnumDisplayDevicesW(NULL, a, &adapter, 0)) break;
            if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
                wcsstr(adapter.DeviceString, L"App Sandbox")) {
                wcscpy_s(g_second_display, _countof(g_second_display), adapter.DeviceName);
                break;
            }
        }
    }
    if (!g_second_display[0]) { input_log("Displays: 2 requested, no detached VDD monitor found"); return; }
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (vdd_display_device(0, device, _countof(device)) &&
        EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm)) {
        dm.dmPosition.x += (LONG)dm.dmPelsWidth;   /* right of display 1 */
    } else {
        dm.dmPosition.x = 1920; dm.dmPosition.y = 0;
        dm.dmPelsWidth = 1920; dm.dmPelsHeight = 1080; dm.dmDisplayFrequency = 60;
    }
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    rc = ChangeDisplaySettingsExW(g_second_display, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    input_log("Displays: 2 (attached %ls at %ld,%ld %lux%lu@%lu) -> %ld", g_second_display,
              dm.dmPosition.x, dm.dmPosition.y, dm.dmPelsWidth, dm.dmPelsHeight,
              dm.dmDisplayFrequency, rc);
}

/* ---- Guest resolution ----
   The display driver offers 1920x1080 and 2560x1440 (each at 60-240 Hz);
   the host picks one from the viewer window size and scales the picture.
   A worker thread applies the latest request per display so input never
   waits on a mode change. */
static volatile LONG g_resize_req[2];      /* (w << 16) | h, 0 = none */
static HANDLE g_resize_event;

static BOOL find_mode(const wchar_t *device, DWORD w, DWORD h, DWORD want_hz, DEVMODEW *out)
{
    DEVMODEW mode;
    DWORD i, best_diff = MAXDWORD;
    for (i = 0; ; ++i) {
        DWORD diff;
        ZeroMemory(&mode, sizeof(mode)); mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(device, i, &mode)) break;
        if (mode.dmPelsWidth != w || mode.dmPelsHeight != h) continue;
        diff = mode.dmDisplayFrequency > want_hz ? mode.dmDisplayFrequency - want_hz
                                                 : want_hz - mode.dmDisplayFrequency;
        if (diff < best_diff) { best_diff = diff; *out = mode; }
    }
    return best_diff != MAXDWORD;
}

static void apply_resize(UINT index, DWORD w, DWORD h)
{
    wchar_t device[32];
    DEVMODEW current, mode;
    LONG rc;
    if (!vdd_display_device(index, device, _countof(device))) return;
    ZeroMemory(&current, sizeof(current)); current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &current)) return;
    if (current.dmPelsWidth == w && current.dmPelsHeight == h) return;
    if (!find_mode(device, w, h, g_host_hz[index] ? (DWORD)g_host_hz[index] : current.dmDisplayFrequency, &mode)) {
        input_log("Display %u: %lux%lu is not offered by the display driver", index + 1, w, h);
        return;
    }
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    rc = ChangeDisplaySettingsExW(device, &mode, NULL, CDS_UPDATEREGISTRY, NULL);
    input_log("Display %u (%ls): %lux%lu -> %lux%lu@%lu -> %ld", index + 1, device,
              current.dmPelsWidth, current.dmPelsHeight, w, h, mode.dmDisplayFrequency, rc);
}
static DWORD WINAPI resize_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        UINT i;
        WaitForSingleObject(g_resize_event, INFINITE);
        switch_to_input_desktop();
        for (i = 0; i < 2; i++) {
            LONG req = InterlockedExchange(&g_resize_req[i], 0);
            if (req) apply_resize(i, (DWORD)req >> 16, (DWORD)req & 0xFFFF);
        }
    }
}

static void request_resize(UINT index, UINT w, UINT h)
{
    if (index > 1 || w < 640 || h < 480 || w > 7680 || h > 4320) return;
    if (!g_resize_event) {
        HANDLE t;
        g_resize_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        t = g_resize_event ? CreateThread(NULL, 0, resize_thread, NULL, 0, NULL) : NULL;
        if (t) CloseHandle(t);
    }
    InterlockedExchange(&g_resize_req[index], (LONG)((w << 16) | h));
    if (g_resize_event) SetEvent(g_resize_event);
}

static RECT display_rect(UINT index)
{
    DisplayList list;
    RECT fallback = { 0, 0, 1920, 1080 };
    UINT i, j;
    static RECT cached[2];
    static ULONGLONG updated[2];
    ULONGLONG now = GetTickCount64();
    if (index < 2) {
        if (updated[index] && now - updated[index] < 250) return cached[index];
        if (vdd_display_rect(index, &cached[index])) {
            updated[index] = now;
            return cached[index];
        }
    }
    ZeroMemory(&list, sizeof(list));
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&list);
    for (i = 0; i < list.count; ++i) {
        for (j = i + 1; j < list.count; ++j) {
            BOOL swap = (!list.primary[i] && list.primary[j]) ||
                (list.primary[i] == list.primary[j] &&
                 (list.rects[j].left < list.rects[i].left ||
                  (list.rects[j].left == list.rects[i].left &&
                   list.rects[j].top < list.rects[i].top)));
            if (swap) {
                RECT r = list.rects[i]; BOOL p = list.primary[i];
                list.rects[i] = list.rects[j]; list.primary[i] = list.primary[j];
                list.rects[j] = r; list.primary[j] = p;
            }
        }
    }
    return index < list.count ? list.rects[index] : fallback;
}

static BOOL vhid_submit(INT32 dx, INT32 dy, INT32 wheel)
{
    AsbVhidMouseInput report;
    DWORD returned = 0;
    if (g_vhid == INVALID_HANDLE_VALUE) return FALSE;
    do {
    report.dx = dx < -32767 ? -32767 : dx > 32767 ? 32767 : dx;
    report.dy = dy < -32767 ? -32767 : dy > 32767 ? 32767 : dy;
    report.wheel = wheel < -127 ? -127 : wheel > 127 ? 127 : wheel;
    report.buttons = g_vhid_buttons;
    if (!DeviceIoControl(g_vhid, IOCTL_ASB_VHID_MOUSE,
                         &report, sizeof(report), NULL, 0,
                         &returned, NULL)) {
        input_log("Virtual HID submit failed: %lu", GetLastError());
        g_vhid_failed = TRUE;
        return FALSE;
    }
    dx -= report.dx;
    dy -= report.dy;
    wheel -= report.wheel;
    } while (dx || dy || wheel);
    return TRUE;
}

static void inject_input(const InputPacket *pkt)
{
    INPUT inp;
    UINT result;
    ZeroMemory(&inp, sizeof(inp));

    switch (pkt->type) {
    case INPUT_MOUSE_MOVE: {
        RECT target = display_rect(pkt->param3 < 2 ? pkt->param3 : 0);
        int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int virtual_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtual_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        /* p1/p2 are normalized 0..65535 across the display (any resolution). */
        LONG x = target.left + (LONG)((UINT64)pkt->param1 *
                 (UINT)(target.right - target.left - 1) / 65535u);
        LONG y = target.top + (LONG)((UINT64)pkt->param2 *
                 (UINT)(target.bottom - target.top - 1) / 65535u);
        if (virtual_w <= 1) virtual_w = 1920;
        if (virtual_h <= 1) virtual_h = 1080;
        inp.type = INPUT_MOUSE;
        inp.mi.dx = (LONG)(((INT64)x - virtual_x) * 65535 / (virtual_w - 1));
        inp.mi.dy = (LONG)(((INT64)y - virtual_y) * 65535 / (virtual_h - 1));
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                         MOUSEEVENTF_VIRTUALDESK;
        result = send_input_on_input_desktop(&inp);
        if (result == 0)
            input_log("SendInput(MOUSE_MOVE) failed: %lu", GetLastError());
        break;
    }
    case INPUT_MOUSE_BUTTON: {
        if (g_vhid != INVALID_HANDLE_VALUE) {
            UINT32 bit;
            switch (pkt->param1) {
            case INPUT_BTN_LEFT: bit = 1u; break;
            case INPUT_BTN_RIGHT: bit = 2u; break;
            case INPUT_BTN_MIDDLE: bit = 4u; break;
            case INPUT_BTN_X1: bit = 8u; break;
            case INPUT_BTN_X2: bit = 16u; break;
            default: return;
            }
            if (pkt->param2) g_vhid_buttons |= bit;
            else g_vhid_buttons &= ~bit;
            vhid_submit(0, 0, 0);
            break;
        }
        inp.type = INPUT_MOUSE;
        switch (pkt->param1) {
        case INPUT_BTN_LEFT:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case INPUT_BTN_RIGHT:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case INPUT_BTN_MIDDLE:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        case INPUT_BTN_X1:
        case INPUT_BTN_X2:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            inp.mi.mouseData = pkt->param1 == INPUT_BTN_X1 ? XBUTTON1 : XBUTTON2;
            break;
        default:
            return;
        }
        result = send_input_on_input_desktop(&inp);
        if (result) {
            UINT32 bit = 1u << pkt->param1;
            if (pkt->param2) g_legacy_buttons |= bit;
            else g_legacy_buttons &= ~bit;
        }
        if (result == 0)
            input_log("SendInput(MOUSE_BUTTON btn=%u down=%u) failed: %lu",
                       pkt->param1, pkt->param2, GetLastError());
        break;
    }
    case INPUT_MOUSE_WHEEL: {
        if (g_vhid != INVALID_HANDLE_VALUE) {
            /* Windows wheel deltas use 120 units per detent; a HID mouse
               report uses one unit. Preserve high-resolution partial turns. */
            INT64 total = (INT64)g_vhid_wheel_remainder + (INT32)pkt->param1;
            g_vhid_wheel_remainder = (INT32)(total % WHEEL_DELTA);
            vhid_submit(0, 0, (INT32)(total / WHEEL_DELTA));
            break;
        }
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = (DWORD)(INT32)pkt->param1;
        result = send_input_on_input_desktop(&inp);
        if (result == 0)
            input_log("SendInput(MOUSE_WHEEL delta=%d) failed: %lu",
                       (INT32)pkt->param1, GetLastError());
        break;
    }
    case INPUT_MOUSE_RELATIVE: {
        if (g_vhid != INVALID_HANDLE_VALUE) {
            vhid_submit((INT32)pkt->param1, (INT32)pkt->param2, 0);
            break;
        }
        /* Compatibility fallback for a driver disappearing mid-session.  A
           fresh connection advertises legacy mode until the VHID is back. */
        inp.type = INPUT_MOUSE;
        inp.mi.dx = (LONG)(INT32)pkt->param1;
        inp.mi.dy = (LONG)(INT32)pkt->param2;
        inp.mi.dwFlags = MOUSEEVENTF_MOVE;
        result = send_input_on_input_desktop(&inp);
        if (result == 0)
            input_log("SendInput(MOUSE_RELATIVE) failed: %lu", GetLastError());
        break;
    }
    case INPUT_DISPLAY_REFRESH:
        InterlockedExchange(&g_host_hz[pkt->param3 < 2 ? pkt->param3 : 0], (LONG)pkt->param1);
        apply_host_refresh(pkt->param3 < 2 ? pkt->param3 : 0, pkt->param1);
        break;
    case INPUT_DISPLAY_COUNT:
        set_display_count(pkt->param1);
        break;
    case INPUT_DISPLAY_RESIZE:
        request_resize(pkt->param3, pkt->param1, pkt->param2);
        break;
    case INPUT_KEY: {
        if (pkt->param1 >= _countof(g_held_keys)) break;
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = (WORD)pkt->param1;
        inp.ki.wScan = (WORD)pkt->param2;
        inp.ki.dwFlags = 0;
        if (pkt->param3 & 1) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (pkt->param3 & 2) inp.ki.dwFlags |= KEYEVENTF_KEYUP;
        result = send_input_on_input_desktop(&inp);
        if (result) {
            g_held_keys[pkt->param1] = !(pkt->param3 & 2);
            g_held_scans[pkt->param1] = inp.ki.wScan;
            g_held_flags[pkt->param1] = inp.ki.dwFlags & KEYEVENTF_EXTENDEDKEY;
        }
        if (result == 0)
            input_log("SendInput(KEY vk=0x%X scan=0x%X flags=0x%X) failed: %lu",
                       pkt->param1, pkt->param2, pkt->param3, GetLastError());
        break;
    }
    }
}

/* ---- Receive exactly len bytes (transport may deliver partial reads) ---- */

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

/* ---- Handle one host connection ---- */

static void handle_conn(AsbConn *c)
{
    InputPacket pkt;
    UINT pkt_count = 0;
    UINT32 ready;

    g_vhid_buttons = 0;
    g_vhid_wheel_remainder = 0;
    g_vhid_failed = FALSE;
    g_legacy_buttons = 0;
    g_vhid = CreateFileW(L"\\\\.\\AppSandboxVHID",
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_vhid != INVALID_HANDLE_VALUE && !vhid_submit(0, 0, 0)) {
        CloseHandle(g_vhid);
        g_vhid = INVALID_HANDLE_VALUE;
        g_vhid_failed = FALSE;
    }
    ready = g_vhid != INVALID_HANDLE_VALUE
        ? INPUT_READY_VHID2 : INPUT_READY_MAGIC2;

    /* Tell the host we're ready to receive input. */
    if (asb_send(c, &ready, sizeof(ready)) != (int)sizeof(ready)) {
        input_log("Failed to send ready signal.");
        if (g_vhid != INVALID_HANDLE_VALUE) {
            CloseHandle(g_vhid);
            g_vhid = INVALID_HANDLE_VALUE;
        }
        return;
    }
    switch_to_input_desktop();
    input_log("Sent %s ready signal to host. Entering recv loop.",
              ready == INPUT_READY_VHID2 ? "virtual-HID" : "legacy");

    for (;;) {
        int n = recv_full(c, &pkt, (int)sizeof(pkt));
        if (n <= 0) {
            input_log("%s after %u packets.",
                       n == 0 ? "Host disconnected" : "recv error", pkt_count);
            break;
        }
        if (pkt.magic != INPUT_MAGIC) {
            input_log("Bad magic 0x%08X, skipping.", pkt.magic);
            continue;
        }
        pkt_count++;
        if (pkt_count == 1)
            input_log("First packet: type=%u p1=%u p2=%u p3=%u",
                       pkt.type, pkt.param1, pkt.param2, pkt.param3);
        inject_input(&pkt);
        if (g_vhid_failed) break; /* reconnect renegotiates capability */
    }
    if (g_vhid != INVALID_HANDLE_VALUE) {
        g_vhid_buttons = 0;
        vhid_submit(0, 0, 0);
        CloseHandle(g_vhid);
        g_vhid = INVALID_HANDLE_VALUE;
    }
    /* Legacy guests need the same disconnect cleanup as VHID guests. A lost
       button-up packet must not leave a drag or a held game action behind. */
    {
        UINT button;
        static const DWORD flags[] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP,
            MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP, MOUSEEVENTF_XUP };
        for (button = 0; button < _countof(flags); ++button) {
            INPUT release;
            if (!(g_legacy_buttons & (1u << button))) continue;
            ZeroMemory(&release, sizeof(release));
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = flags[button];
            if (button >= INPUT_BTN_X1)
                release.mi.mouseData = button == INPUT_BTN_X1 ? XBUTTON1 : XBUTTON2;
            SendInput(1, &release, sizeof(release));
        }
        g_legacy_buttons = 0;
    }
    {
        UINT vk;
        for (vk = 0; vk < _countof(g_held_keys); ++vk) {
            INPUT release;
            if (!g_held_keys[vk]) continue;
            ZeroMemory(&release, sizeof(release));
            release.type = INPUT_KEYBOARD;
            release.ki.wVk = (WORD)vk;
            release.ki.wScan = g_held_scans[vk];
            release.ki.dwFlags = KEYEVENTF_KEYUP | g_held_flags[vk];
            SendInput(1, &release, sizeof(release));
            g_held_keys[vk] = FALSE;
        }
    }
}

/* ---- Main: listen on the input channel, accept connections ---- */

/* ---- Display guard ----
   Client VMs keep a 1024x768 Hyper-V framebuffer (the boot console). Once
   the agent disables the Hyper-V video driver, Windows' basic display driver
   still drives that framebuffer as an extra monitor that no viewer shows, so
   windows can open on a screen the user cannot see. While at least one
   AppSandbox VDD monitor (EDID ASB0001) is attached, detach every other
   non-primary display. Without a VDD monitor nothing is touched, so a guest
   whose display driver failed keeps its only screen. */

static BOOL adapter_has_vdd_monitor(const wchar_t *adapter)
{
    DISPLAY_DEVICEW mon;
    DWORD m;
    for (m = 0; ; ++m) {
        ZeroMemory(&mon, sizeof(mon)); mon.cb = sizeof(mon);
        if (!EnumDisplayDevicesW(adapter, m, &mon, 0)) return FALSE;
        if (wcsstr(mon.DeviceID, L"\\ASB0001\\")) return TRUE;
    }
}

/* Make `device` the primary display at (0,0) and line every other attached
   AppSandbox display up to its right (NORESET: caller applies). */
static void promote_to_primary(const wchar_t *device)
{
    DISPLAY_DEVICEW adapter;
    DEVMODEW dm;
    LONG x;
    DWORD a;
    ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm)) return;
    x = (LONG)dm.dmPelsWidth;
    dm.dmPosition.x = 0;
    dm.dmPosition.y = 0;
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    input_log("Displays: making %ls primary -> %ld", device,
              ChangeDisplaySettingsExW(device, &dm, NULL, CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET, NULL));
    for (a = 0; ; ++a) {
        ZeroMemory(&adapter, sizeof(adapter)); adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, a, &adapter, 0)) break;
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            wcscmp(adapter.DeviceName, device) == 0 || !adapter_has_vdd_monitor(adapter.DeviceName)) continue;
        ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) continue;
        dm.dmPosition.x = x;
        dm.dmPosition.y = 0;
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        ChangeDisplaySettingsExW(adapter.DeviceName, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        x += (LONG)dm.dmPelsWidth;
    }
}

static void detach_stray_displays(void)
{
    DISPLAY_DEVICEW adapter;
    wchar_t stray[8][32], first_vdd[32] = {0}, vdd1[32];
    UINT stray_count = 0, i;
    BOOL stray_primary = FALSE;
    DWORD a;
    for (a = 0; ; ++a) {
        ZeroMemory(&adapter, sizeof(adapter)); adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, a, &adapter, 0)) break;
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        if (adapter_has_vdd_monitor(adapter.DeviceName)) {
            if (!first_vdd[0]) wcscpy_s(first_vdd, _countof(first_vdd), adapter.DeviceName);
        } else if (stray_count < _countof(stray)) {
            if (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) stray_primary = TRUE;
            wcscpy_s(stray[stray_count++], 32, adapter.DeviceName);
        }
    }
    if (!first_vdd[0] || !stray_count) return;
    /* A stray display can end up primary (e.g. after a monitor re-plug);
       hand primary to AppSandbox Display 1 (the main viewer) first. */
    if (stray_primary)
        promote_to_primary(vdd_display_device(0, vdd1, _countof(vdd1)) ? vdd1 : first_vdd);
    for (i = 0; i < stray_count; ++i) {
        DEVMODEW dm;
        LONG rc;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;   /* all zero = detach */
        rc = ChangeDisplaySettingsExW(stray[i], &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        input_log("Display guard: detaching %ls (non-AppSandbox display) -> %ld", stray[i], rc);
    }
    ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
}
static DWORD WINAPI display_guard_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        UINT i;
        switch_to_input_desktop();
        detach_stray_displays();
        for (i = 0; i < 2; i++)
            if (g_host_hz[i]) apply_host_refresh(i, (UINT)g_host_hz[i]);   /* no-op when already matched */
        Sleep(3000);
    }
}

int main(void)
{
    AsbListener *l;
    HANDLE guard;

    /* Work in physical pixels on every monitor. Without this, Windows scales
       some metrics (virtual desktop size) but not others (display modes) once
       a guest monitor is not at 100%, and absolute clicks land elsewhere. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    input_log("Starting (PID=%lu, session=%lu).",
              GetCurrentProcessId(),
              WTSGetActiveConsoleSessionId());

    guard = CreateThread(NULL, 0, display_guard_thread, NULL, 0, NULL);
    if (guard) CloseHandle(guard);

    if (asb_transport_init() != 0) {
        input_log("asb_transport_init failed.");
        return 1;
    }

    l = asb_listen(ASB_CH_INPUT);
    if (!l) {
        input_log("asb_listen(ASB_CH_INPUT) failed.");
        return 1;
    }
    input_log("Listening on input channel (transport=%s).",
              asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    /* Accept loop — one host connection at a time. */
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        input_log("Host connected.");
        handle_conn(c);
        asb_close(c);
    }
}

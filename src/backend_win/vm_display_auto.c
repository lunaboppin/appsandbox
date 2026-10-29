/*
 * vm_display_auto.c -- VM display that follows the boot.
 *
 * A booting VM has no guest agent and no IDD driver yet, so the frame channel
 * (vm_display_idd.c) has nothing to show. HCS does give every VM a synthetic
 * video device (the VideoMonitor in hcs_vm.c), which vmwp.exe serves as an
 * RDP stream on \\.\pipe\<vm>.BasicSession -- the same "basic session" that
 * Hyper-V Manager's VMConnect uses. vm_display.c renders that pipe with the
 * MsRdpClient ActiveX control. This controller shows that console while the
 * VM boots and replaces it with the IDD window as soon as asb_vm_idd_ready
 * turns TRUE (agent online + idd_status:ok), at the same screen position.
 *
 * Threading: every window create/destroy after the first call runs on the
 * watcher thread, so the GUI thread and the headless request loop never
 * block on a swap. The public calls only read/write a few fields under cs.
 */

#include <windows.h>
#include <stdio.h>

#include "vm_display_auto.h"
#include "vm_display.h"
#include "vm_display_idd.h"
#include "asb_core.h"
#include "ui.h"

#define VDA_POLL_MS        250
#define VDA_PIPE_WAIT_MS   5000
#define VDA_OPEN_GRACE_MS  15000   /* window thread still starting up */

struct VmDisplayAuto {
    VmInstance      *vm;
    HINSTANCE        hinst;
    HWND             main_hwnd;

    CRITICAL_SECTION cs;
    VdaView          view;      /* VDA_VIEW_CONSOLE or VDA_VIEW_IDD */
    BOOL             follow;    /* console: swap to IDD once it is ready */
    VmDisplay       *con;
    VmDisplayIdd    *idd;
    BOOL             seen_open;    /* current window has been observed open */
    ULONGLONG        opened_tick;  /* when the current window was created */

    volatile LONG    pending;   /* requested VdaView + 1; 0 = none */
    volatile LONG    closed;    /* window gone (user closed it, or open failed) */
    HANDLE           stop_evt;
    HANDLE           thread;
};

const char *vm_display_auto_view_name(VdaView view)
{
    switch (view) {
    case VDA_VIEW_CONSOLE: return "console";
    case VDA_VIEW_IDD:     return "idd";
    default:               return "auto";
    }
}

static BOOL idd_ready(VmDisplayAuto *d)
{
    return asb_vm_idd_ready((AsbVm)d->vm);
}

/* vmwp.exe creates the BasicSession pipe a couple of seconds after the VM
   starts; wait for it so a click right after Start doesn't fail outright. */
static void wait_for_console_pipe(VmDisplayAuto *d)
{
    wchar_t pipe[300];
    ULONGLONG deadline = GetTickCount64() + VDA_PIPE_WAIT_MS;
    swprintf_s(pipe, 300, L"\\\\.\\pipe\\%s.BasicSession", d->vm->name);
    while (GetTickCount64() < deadline) {
        DWORD err;
        if (WaitNamedPipeW(pipe, 0)) return;
        err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_SEM_TIMEOUT) return;
        if (WaitForSingleObject(d->stop_evt, 100) == WAIT_OBJECT_0) return;
    }
}

/* The window threads create their HWND asynchronously, so "not open" right
   after a create means "not up yet", not "closed by the user". */
static void mark_new_window(VmDisplayAuto *d)
{
    d->seen_open = FALSE;
    d->opened_tick = GetTickCount64();
}

static BOOL window_gone(VmDisplayAuto *d, BOOL open_now)
{
    if (open_now) { d->seen_open = TRUE; return FALSE; }
    return d->seen_open || GetTickCount64() - d->opened_tick > VDA_OPEN_GRACE_MS;
}

static BOOL swap_to_idd(VmDisplayAuto *d)
{
    POINT pos;
    BOOL has_pos = d->con && vm_display_get_pos(d->con, &pos);
    VmDisplay *old;
    VmDisplayIdd *idd;

    ui_log(L"\"%s\": guest display is up - switching to the IDD display.", d->vm->name);
    /* Close the Hyper-V screen first. Destroying its RDP control after the
       IDD window had come up moved activation away from the IDD window, so
       it never saw the WM_ACTIVATE that enables keyboard forwarding and the
       user couldn't type until switching views by hand. */
    EnterCriticalSection(&d->cs);
    old = d->con;
    d->con = NULL;
    LeaveCriticalSection(&d->cs);
    if (old) {
        vm_display_disconnect(old);
        vm_display_destroy(old);
    }
    idd = vm_display_idd_create_switchable(d->vm, d->hinst, d->main_hwnd,
                                           has_pos ? &pos : NULL);
    if (!idd) {
        ui_log(L"Error: Failed to create IDD display window for \"%s\".", d->vm->name);
        return FALSE;
    }
    EnterCriticalSection(&d->cs);
    d->idd = idd;
    d->view = VDA_VIEW_IDD;
    d->follow = FALSE;
    mark_new_window(d);
    LeaveCriticalSection(&d->cs);
    return TRUE;
}

static BOOL swap_to_console(VmDisplayAuto *d)
{
    POINT pos;
    BOOL has_pos = d->idd && vm_display_idd_get_pos(d->idd, &pos);
    VmDisplayIdd *old;
    VmDisplay *con;

    ui_log(L"Opening Hyper-V screen for \"%s\"...", d->vm->name);
    wait_for_console_pipe(d);
    con = vm_display_create_switchable(d->vm, d->hinst, d->main_hwnd,
                                       has_pos ? &pos : NULL);
    if (!con) {
        ui_log(L"Error: Failed to create Hyper-V screen window for \"%s\".", d->vm->name);
        return FALSE;
    }
    EnterCriticalSection(&d->cs);
    old = d->idd;
    d->idd = NULL;
    d->con = con;
    d->view = VDA_VIEW_CONSOLE;
    mark_new_window(d);
    LeaveCriticalSection(&d->cs);
    if (old) vm_display_idd_destroy(old);
    return TRUE;
}

/* One watcher tick: notice a user close, apply switch requests (API or the
   windows' own button / menu item), and follow the boot to IDD. */
static void watch_step(VmDisplayAuto *d)
{
    LONG req = InterlockedExchange(&d->pending, 0);

    if (d->view == VDA_VIEW_CONSOLE) {
        if (window_gone(d, vm_display_is_open(d->con))) { InterlockedExchange(&d->closed, 1); return; }
        if (vm_display_take_switch_request(d->con)) req = VDA_VIEW_IDD + 1;
        if (req == VDA_VIEW_CONSOLE + 1) {
            d->follow = FALSE;
        } else if (req == VDA_VIEW_IDD + 1 || req == VDA_VIEW_AUTO + 1) {
            if (idd_ready(d)) {
                swap_to_idd(d);
            } else {
                ui_log(L"\"%s\": guest display is not up yet - the window will switch "
                       L"to it when the agent reports the display driver running.",
                       d->vm->name);
                d->follow = TRUE;
            }
        } else if (d->follow && idd_ready(d)) {
            swap_to_idd(d);
        }
    } else {
        if (window_gone(d, vm_display_idd_is_open(d->idd))) { InterlockedExchange(&d->closed, 1); return; }
        if (vm_display_idd_take_switch_request(d->idd)) req = VDA_VIEW_CONSOLE + 1;
        if (req == VDA_VIEW_CONSOLE + 1) {
            d->follow = FALSE;
            swap_to_console(d);
        }
    }
}

static DWORD WINAPI watch_thread(LPVOID param)
{
    VmDisplayAuto *d = (VmDisplayAuto *)param;
    BOOL ok;

    if (d->view == VDA_VIEW_IDD) {
        VmDisplayIdd *idd;
        ui_log(L"Opening IDD display for \"%s\"...", d->vm->name);
        idd = vm_display_idd_create_switchable(d->vm, d->hinst, d->main_hwnd, NULL);
        EnterCriticalSection(&d->cs);
        d->idd = idd;
        mark_new_window(d);
        LeaveCriticalSection(&d->cs);
        ok = idd != NULL;
        if (!ok) ui_log(L"Error: Failed to create IDD display window.");
    } else {
        ok = swap_to_console(d);
    }
    if (!ok) { InterlockedExchange(&d->closed, 1); return 1; }

    while (WaitForSingleObject(d->stop_evt, VDA_POLL_MS) == WAIT_TIMEOUT) {
        watch_step(d);
        if (d->closed) break;
    }
    return 0;
}

VmDisplayAuto *vm_display_auto_create(VmInstance *vm, HINSTANCE hInstance,
                                      HWND main_hwnd, VdaView view)
{
    VmDisplayAuto *d;

    if (!vm) return NULL;
    d = (VmDisplayAuto *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return NULL;

    d->vm        = vm;
    d->hinst     = hInstance;
    d->main_hwnd = main_hwnd;
    InitializeCriticalSection(&d->cs);

    if (view == VDA_VIEW_AUTO) {
        BOOL ready = asb_vm_idd_ready((AsbVm)vm);
        d->view   = ready ? VDA_VIEW_IDD : VDA_VIEW_CONSOLE;
        d->follow = !ready;
    } else {
        d->view   = view;
        d->follow = FALSE;
    }

    d->stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->thread = d->stop_evt ? CreateThread(NULL, 0, watch_thread, d, 0, NULL) : NULL;
    if (!d->thread) {
        ui_log(L"Error: Failed to start the display watcher for \"%s\".", vm->name);
        if (d->stop_evt) CloseHandle(d->stop_evt);
        DeleteCriticalSection(&d->cs);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }
    return d;
}

void vm_display_auto_destroy(VmDisplayAuto *d)
{
    if (!d) return;
    SetEvent(d->stop_evt);

    /* Pump while waiting: the watcher may be mid-swap, and the window
       threads it waits on log through ui_log. */
    {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(1, &d->thread, FALSE, 10000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
    }
    CloseHandle(d->thread);
    CloseHandle(d->stop_evt);

    if (d->con) {
        vm_display_disconnect(d->con);
        vm_display_destroy(d->con);
    }
    if (d->idd) vm_display_idd_destroy(d->idd);
    DeleteCriticalSection(&d->cs);
    HeapFree(GetProcessHeap(), 0, d);
}

BOOL vm_display_auto_is_open(VmDisplayAuto *d)
{
    return d && !d->closed;
}

VdaView vm_display_auto_view(VmDisplayAuto *d)
{
    VdaView v;
    if (!d) return VDA_VIEW_AUTO;
    EnterCriticalSection(&d->cs);
    v = d->view;
    LeaveCriticalSection(&d->cs);
    return v;
}

BOOL vm_display_auto_following(VmDisplayAuto *d)
{
    BOOL f;
    if (!d) return FALSE;
    EnterCriticalSection(&d->cs);
    f = d->view == VDA_VIEW_CONSOLE && d->follow;
    LeaveCriticalSection(&d->cs);
    return f;
}

void vm_display_auto_set_view(VmDisplayAuto *d, VdaView view)
{
    if (!d) return;
    InterlockedExchange(&d->pending, (LONG)view + 1);
}

void vm_display_auto_focus(VmDisplayAuto *d)
{
    if (!d) return;
    EnterCriticalSection(&d->cs);
    if (d->idd) vm_display_idd_focus(d->idd);
    else if (d->con) vm_display_focus(d->con);
    LeaveCriticalSection(&d->cs);
}

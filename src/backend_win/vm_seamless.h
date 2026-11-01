#ifndef VM_SEAMLESS_H
#define VM_SEAMLESS_H

#include <windows.h>
#include "hcs_vm.h"

/*
 * vm_seamless -- seamless mode for a Windows VM.
 *
 * Each top-level window in the guest appears as a host window of its own, with
 * its own taskbar entry, showing only that window (tools/agent/appsandbox-seamless.cpp
 * captures them one by one and sends what changed on channel :0009). A border in
 * the VM's colour and the VM's name in the title mark them as the VM's. A small
 * launcher window lists the guest's apps and open windows; closing it leaves
 * seamless mode and the guest's programs keep running.
 *
 * While the sign-in screen, a lock or a UAC prompt has the guest's input, none of
 * that can be captured, so the normal display window opens until it's over.
 *
 * Everything runs on the session's own threads, so these return at once and are
 * safe to call from the GUI thread or the headless request loop.
 */

/* Why this VM can't use seamless mode, or NULL if it can. */
const wchar_t *vm_seamless_unavailable(VmInstance *vm);

/* Start seamless mode, or bring its launcher to the front. */
void vm_seamless_open(VmInstance *vm, HINSTANCE hinst);

/* Leave seamless mode. The host windows close; the guest's programs stay. */
void vm_seamless_close(UINT64 vm_id);

BOOL vm_seamless_is_open(UINT64 vm_id);

/* Open an app in the guest: a launch target from its app list, or a full path. */
BOOL vm_seamless_run(UINT64 vm_id, const wchar_t *target);

/* Drive a seamless window as the host user would, for scripts and tests: the event
   goes to the host window (guest: its guest id from the status) and takes the same
   path as a real one. event is move, down, up (a, b: inside the window, c: button),
   wheel (a: delta), keydown, keyup (a: virtual key), activate, close, minimize,
   maximize, restore, position (a, b: host screen) or size (a, b: content).
   FALSE if there's no such window or event. */
BOOL vm_seamless_input(UINT64 vm_id, UINT32 guest, const wchar_t *event, int a, int b, int c);

/* {"width":..,"height":..,"bgra":"<base64>"}: the picture the host holds for one
   window, top row first. malloc'd, or NULL if there's no such window. */
char *vm_seamless_picture_json(UINT64 vm_id, UINT32 guest);

/* {"open":..,"connected":..,"secure":..,"status":"..","framesReceived":..,
   "bytesReceived":..,"windows":[{"guest":..,"host":..,"title":"..",...}]}.
   Returns FALSE if buf is too small. */
BOOL vm_seamless_status_json(UINT64 vm_id, char *buf, size_t cap);

#endif

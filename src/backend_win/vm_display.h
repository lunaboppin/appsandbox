#ifndef VM_DISPLAY_H
#define VM_DISPLAY_H

#include <windows.h>
#include "hcs_vm.h"

/* Display context for a VM viewer (RDP ActiveX over named pipe). */
typedef struct VmDisplay VmDisplay;

/* Create and show a display window for the given VM.
   Connects to the VM's RDP pipe (\\.\pipe\<name>.BasicSession).
   main_hwnd receives WM_VM_DISPLAY_CLOSED when the user closes the window.
   Returns a display context, or NULL on failure. */
VmDisplay *vm_display_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

/* Like vm_display_create, for a window owned by vm_display_auto: opens at
   *pos when non-NULL and adds a "Switch to VM Display" toolbar button whose
   clicks are collected with vm_display_take_switch_request. */
VmDisplay *vm_display_create_switchable(VmInstance *vm, HINSTANCE hInstance,
                                        HWND main_hwnd, const POINT *pos);

/* TRUE (once per click) if the user pressed the switch button. */
BOOL vm_display_take_switch_request(VmDisplay *display);

/* Top-left corner of the window in screen coordinates. */
BOOL vm_display_get_pos(VmDisplay *display, POINT *pos);

/* Restore (if minimized) and bring the window to the foreground. */
void vm_display_focus(VmDisplay *display);

/* Close the display window and free resources. */
void vm_display_destroy(VmDisplay *display);

/* Check if the display window is still open. */
BOOL vm_display_is_open(VmDisplay *display);

/* Disconnect the RDP session and terminate the pipe connector.
   Call when the VM is exiting so the RDP session doesn't block shutdown. */
void vm_display_disconnect(VmDisplay *display);

/* Switch to enhanced session mode and reconnect.
   No-op if already in enhanced mode. */
void vm_display_set_enhanced(VmDisplay *display);

#endif /* VM_DISPLAY_H */

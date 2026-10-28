#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

/* Create IDD display window for VM. Connects to VM's AF_HYPERV channels
   (:0002 frames, :0003 input, :0005 clipboard writer, :0006 clipboard reader)
   and renders received frames via D3D11.
   main_hwnd receives WM_VM_DISPLAY_CLOSED when closed. */
VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

/* Like vm_display_idd_create, for a window owned by vm_display_auto: opens at
   *pos when non-NULL and adds a "Show Hyper-V Screen" system-menu item whose
   clicks are collected with vm_display_idd_take_switch_request. */
VmDisplayIdd *vm_display_idd_create_switchable(VmInstance *vm, HINSTANCE hInstance,
                                               HWND main_hwnd, const POINT *pos);

/* TRUE (once per click) if the user picked "Show Hyper-V Screen". */
BOOL vm_display_idd_take_switch_request(VmDisplayIdd *display);

/* Top-left corner of the window in screen coordinates. */
BOOL vm_display_idd_get_pos(VmDisplayIdd *display, POINT *pos);

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);

/* Bring an already-open display window to the foreground/focus.
   Safe to call from any thread; the work is marshaled to the window thread. */
void vm_display_idd_focus(VmDisplayIdd *display);

#endif /* VM_DISPLAY_IDD_H */

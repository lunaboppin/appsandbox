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

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);

/* Bring an already-open display window to the foreground/focus.
   Safe to call from any thread; the work is marshaled to the window thread. */
void vm_display_idd_focus(VmDisplayIdd *display);

/* Enumerate active host capture endpoints (id + friendly name) for the
   microphone passthrough device picker. */
void idd_enum_capture_devices(void (*cb)(const wchar_t *id, const wchar_t *name, void *ud), void *ud);

#endif /* VM_DISPLAY_IDD_H */

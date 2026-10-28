#ifndef VM_DISPLAY_AUTO_H
#define VM_DISPLAY_AUTO_H

#include <windows.h>
#include "hcs_vm.h"

/* One VM display "window" that shows whichever source is usable:
     - console: the Hyper-V synthetic video framebuffer over vmwp.exe's
       BasicSession pipe (vm_display.c). Works from firmware / boot loader
       onward with no guest cooperation, so it is what a booting VM shows.
     - idd: the guest's IddCx / DRM virtual monitor over the frame channel
       (vm_display_idd.c). Needs the guest agent + display driver up.
   In AUTO mode the controller opens the console while asb_vm_idd_ready is
   FALSE and swaps to the IDD stream by itself once it turns TRUE, placing
   the IDD window where the console was. Each window also offers a manual
   switch (console toolbar button / IDD system-menu item). All swaps run on
   the controller's watcher thread, so callers never block on them. */
typedef struct VmDisplayAuto VmDisplayAuto;

typedef enum {
    VDA_VIEW_AUTO    = 0,   /* request: IDD if ready, else console then follow IDD */
    VDA_VIEW_CONSOLE = 1,   /* request / state: Hyper-V console (stays there)       */
    VDA_VIEW_IDD     = 2,   /* request / state: IDD stream                          */
} VdaView;

/* Open a display for a running VM. view = AUTO picks as described above;
   CONSOLE pins the boot screen; IDD requires asb_vm_idd_ready (caller gates).
   main_hwnd (may be NULL) receives WM_VM_DISPLAY_CLOSED when the user closes
   the window. Returns NULL on failure. */
VmDisplayAuto *vm_display_auto_create(VmInstance *vm, HINSTANCE hInstance,
                                      HWND main_hwnd, VdaView view);

void vm_display_auto_destroy(VmDisplayAuto *d);

/* TRUE while a window is showing (FALSE once the user closed it). */
BOOL vm_display_auto_is_open(VmDisplayAuto *d);

/* Currently shown source: VDA_VIEW_CONSOLE or VDA_VIEW_IDD. */
VdaView vm_display_auto_view(VmDisplayAuto *d);

/* TRUE while showing the console and waiting to swap to IDD automatically. */
BOOL vm_display_auto_following(VmDisplayAuto *d);

/* Switch source. AUTO = go to IDD now if ready, otherwise stay on (or go to)
   the console and follow IDD. Asynchronous: applied by the watcher thread. */
void vm_display_auto_set_view(VmDisplayAuto *d, VdaView view);

/* Bring the window to the foreground. */
void vm_display_auto_focus(VmDisplayAuto *d);

/* Stable lowercase name for a view ("console" / "idd" / "auto"). */
const char *vm_display_auto_view_name(VdaView view);

#endif /* VM_DISPLAY_AUTO_H */

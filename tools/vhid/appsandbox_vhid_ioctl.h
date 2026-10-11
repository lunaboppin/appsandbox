#ifndef APPSANDBOX_VHID_IOCTL_H
#define APPSANDBOX_VHID_IOCTL_H

/* Buffered control request accepted by the AppSandbox virtual HID mouse.
   The caller supplies relative motion, wheel motion, and the complete current
   button mask (bits 0..4). */
#define IOCTL_ASB_VHID_MOUSE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct AsbVhidMouseInput {
    LONG  dx;
    LONG  dy;
    LONG  wheel;
    ULONG buttons;
} AsbVhidMouseInput;

#endif

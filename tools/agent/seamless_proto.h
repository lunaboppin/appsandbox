/*
 * seamless_proto.h -- wire protocol of seamless mode, shared by the guest helper
 * (appsandbox-seamless.cpp) and the host (src/backend_win/vm_seamless.c).
 *
 * One stream on ASB_CH_SEAMLESS. Every message in both directions is an
 * SlHeader followed by header.len bytes. Everything from the guest is
 * untrusted: the host checks every size against the limits below.
 */
#ifndef SEAMLESS_PROTO_H
#define SEAMLESS_PROTO_H

#include <stdint.h>

#define SL_MAGIC          0x4C535341u   /* "ASSL" little-endian */
#define SL_VERSION        1u
#define SL_MAX_PAYLOAD    (256u * 1024u * 1024u)
#define SL_MAX_SIDE       8192u
#define SL_MAX_WINDOWS    256
#define SL_MAX_TITLE      512           /* characters */
#define SL_MAX_ICON_SIDE  64u
#define SL_MAX_CURSOR_SIDE 128u
#define SL_MAX_APPS_BYTES (4u * 1024u * 1024u)

/* Guest to host */
#define SL_G_HELLO        1    /* SlHello */
#define SL_G_WINDOW       2    /* SlWindow + UTF-16 title (no terminator): a new or changed window */
#define SL_G_CLOSED       3    /* header.window is gone */
#define SL_G_ICON         4    /* SlImage + BGRA */
#define SL_G_FRAME        5    /* SlFrame + BGRA rows of the rectangle, tightly packed */
#define SL_G_CURSOR       6    /* SlImage + BGRA; width 0: hot_x is a standard cursor (IDC_*), or 0 = hidden */
#define SL_G_FOREGROUND   7    /* header.window took the guest's foreground */
#define SL_G_MOVE_START   8    /* header.window entered a title-bar drag in the guest */
#define SL_G_SECURE       9    /* SlValue: a=1 the sign-in screen, a lock or UAC has the input; a=0 back */
#define SL_G_APPS         10   /* UTF-16 name NUL launch-target NUL ... */

/* Host to guest */
#define SL_H_HELLO        20   /* SlHostHello */
#define SL_H_MOUSE_MOVE   21   /* SlValue: a=x b=y, guest screen pixels */
#define SL_H_BUTTON       22   /* SlValue: a=SL_BTN_* b=1 down, 0 up */
#define SL_H_WHEEL        23   /* SlValue: a=delta b=1 horizontal */
#define SL_H_KEY          24   /* SlValue: a=vk b=scan c=SL_KEY_* */
#define SL_H_ACTIVATE     25   /* header.window: bring it to the guest's foreground */
#define SL_H_CLOSE        26   /* header.window: ask it to close */
#define SL_H_SHOW         27   /* header.window, SlValue: a=SL_STATE_* */
#define SL_H_RESIZE       28   /* header.window, SlValue: a=width b=height of the visible frame */
#define SL_H_MOVE_CANCEL  29   /* header.window: the host took over the drag, end the guest's */
#define SL_H_BLUR         30   /* no VM window is active on the host: close open menus */
#define SL_H_LIST_APPS    31
#define SL_H_RUN          32   /* UTF-16 launch target (no terminator) */

#define SL_BTN_LEFT       0
#define SL_BTN_RIGHT      1
#define SL_BTN_MIDDLE     2
#define SL_BTN_X1         3
#define SL_BTN_X2         4

#define SL_KEY_EXTENDED   1
#define SL_KEY_UP         2

#define SL_STATE_NORMAL   0
#define SL_STATE_MINIMIZED 1
#define SL_STATE_MAXIMIZED 2

/* SlWindow.flags */
#define SL_WF_APP         0x01  /* a window of its own: taskbar entry, Alt+Tab */
#define SL_WF_NOACTIVATE  0x02  /* menus, tooltips, drop-downs: never takes focus on the host */
#define SL_WF_SELF_MOVED  0x04  /* the helper moved it to keep it on the guest's screen; the host window stays */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t window;    /* guest HWND, or 0 */
    uint32_t len;
} SlHeader;

typedef struct {
    uint32_t version;
    uint32_t screen_w, screen_h;    /* the guest's largest monitor */
    uint32_t capture_flags;         /* SL_CAP_* */
} SlHello;

#define SL_CAP_WGC 1                /* Windows.Graphics.Capture works here */

typedef struct {
    uint32_t version;
} SlHostHello;

typedef struct {
    uint32_t owner;     /* guest HWND it floats above, or 0 */
    uint32_t flags;     /* SL_WF_* */
    uint32_t state;     /* SL_STATE_* */
    int32_t  x, y;      /* visible frame, guest screen pixels */
    int32_t  w, h;
    /* then UTF-16 title */
} SlWindow;

typedef struct {
    uint32_t width, height;
    int32_t  hot_x, hot_y;
    /* then width * height * 4 bytes of BGRA */
} SlImage;

typedef struct {
    uint32_t width, height;   /* whole captured frame; its top-left pixel is the window's x,y */
    uint32_t x, y, w, h;      /* the rectangle that follows */
} SlFrame;

typedef struct {
    int32_t a, b, c;
} SlValue;
#pragma pack(pop)

#endif /* SEAMLESS_PROTO_H */

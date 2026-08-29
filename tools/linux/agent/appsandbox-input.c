/*
 * appsandbox-input.c — Linux input injection for AppSandbox.
 *
 * Listens on AF_VSOCK port 3, sends IRDY on accept, and translates
 * incoming ASIN InputPackets into /dev/uinput events. The host stack
 * already speaks this protocol (see src/backend_win/vm_display_idd.c).
 *
 * Single virtual device exposing absolute pointer, wheel, buttons, and
 * full keyboard. GNOME / Mutter (Wayland) and Xorg both pick it up via
 * libinput automatically because uinput presents a real evdev node.
 *
 * Wire protocol (host → guest):
 *   #pragma pack(1)
 *   { uint32 magic=0x4E495341 ('ASIN'), uint32 type,
 *     uint32 p1, uint32 p2, uint32 p3 }
 *
 *   type=0 MOUSE_MOVE   : p1=x, p2=y in pixels (frame coords)
 *   type=1 MOUSE_BUTTON : p1=btn (0=L,1=R,2=M), p2=down(1)/up(0)
 *   type=2 MOUSE_WHEEL  : p1=delta (signed int32, ~120/notch)
 *   type=3 KEY          : p1=Windows VK, p2=scancode,
 *                          p3 bit0=extended, bit1=keyup (clear=keydown)
 *   type=4 MOUSE_MOVE_REL : p1=dx, p2=dy (signed int32) — sent instead of
 *                          MOUSE_MOVE while the host display has the mouse
 *                          captured, so games get true relative motion
 *
 * Build:  gcc -O2 -Wall -o appsandbox-input appsandbox-input.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/uinput.h>
#include <linux/vm_sockets.h>

#define VSOCK_PORT          3
#define INPUT_MAGIC         0x4E495341u  /* 'ASIN' */
#define INPUT_READY_MAGIC   0x59445249u  /* 'IRDY' */

#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3
#define INPUT_MOUSE_MOVE_REL 4          /* p1/p2 = int32 dx/dy */

#define BTN_ID_LEFT         0
#define BTN_ID_RIGHT        1
#define BTN_ID_MIDDLE       2

/* Absolute pointer range. We don't know the guest screen size before
 * accept, and it can change mid-session. We pick a coordinate space
 * large enough for any plausible resolution and trust libinput to map
 * it to the active screen. 32767 is the de-facto absolute-tablet range. */
#define ABS_RANGE   32767

#pragma pack(push, 1)
struct input_packet {
    uint32_t magic;
    uint32_t type;
    uint32_t p1, p2, p3;
};
#pragma pack(pop)

static volatile sig_atomic_t g_stop = 0;
static int g_frame_w = 1920;  /* updated by hint command if we add one later */
static int g_frame_h = 1080;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void in_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[input] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- VK → Linux keycode table ----
 * Windows VK values are 8-bit; keys we don't map fall through to 0
 * (which uinput silently drops). Reference KEY_* values come from
 * <linux/input-event-codes.h>. */

#include <linux/input-event-codes.h>

static const uint16_t g_vk_to_key[256] = {
    [0x08] = KEY_BACKSPACE,
    [0x09] = KEY_TAB,
    [0x0D] = KEY_ENTER,
    [0x10] = KEY_LEFTSHIFT,
    [0x11] = KEY_LEFTCTRL,
    [0x12] = KEY_LEFTALT,
    [0x13] = KEY_PAUSE,
    [0x14] = KEY_CAPSLOCK,
    [0x1B] = KEY_ESC,
    [0x20] = KEY_SPACE,
    [0x21] = KEY_PAGEUP,
    [0x22] = KEY_PAGEDOWN,
    [0x23] = KEY_END,
    [0x24] = KEY_HOME,
    [0x25] = KEY_LEFT,
    [0x26] = KEY_UP,
    [0x27] = KEY_RIGHT,
    [0x28] = KEY_DOWN,
    [0x2C] = KEY_SYSRQ,    /* PrintScreen */
    [0x2D] = KEY_INSERT,
    [0x2E] = KEY_DELETE,
    [0x30] = KEY_0, [0x31] = KEY_1, [0x32] = KEY_2, [0x33] = KEY_3,
    [0x34] = KEY_4, [0x35] = KEY_5, [0x36] = KEY_6, [0x37] = KEY_7,
    [0x38] = KEY_8, [0x39] = KEY_9,
    [0x41] = KEY_A, [0x42] = KEY_B, [0x43] = KEY_C, [0x44] = KEY_D,
    [0x45] = KEY_E, [0x46] = KEY_F, [0x47] = KEY_G, [0x48] = KEY_H,
    [0x49] = KEY_I, [0x4A] = KEY_J, [0x4B] = KEY_K, [0x4C] = KEY_L,
    [0x4D] = KEY_M, [0x4E] = KEY_N, [0x4F] = KEY_O, [0x50] = KEY_P,
    [0x51] = KEY_Q, [0x52] = KEY_R, [0x53] = KEY_S, [0x54] = KEY_T,
    [0x55] = KEY_U, [0x56] = KEY_V, [0x57] = KEY_W, [0x58] = KEY_X,
    [0x59] = KEY_Y, [0x5A] = KEY_Z,
    [0x5B] = KEY_LEFTMETA,  /* Left Win */
    [0x5C] = KEY_RIGHTMETA, /* Right Win */
    [0x5D] = KEY_COMPOSE,   /* App / Menu */
    [0x60] = KEY_KP0, [0x61] = KEY_KP1, [0x62] = KEY_KP2, [0x63] = KEY_KP3,
    [0x64] = KEY_KP4, [0x65] = KEY_KP5, [0x66] = KEY_KP6, [0x67] = KEY_KP7,
    [0x68] = KEY_KP8, [0x69] = KEY_KP9,
    [0x6A] = KEY_KPASTERISK,
    [0x6B] = KEY_KPPLUS,
    [0x6D] = KEY_KPMINUS,
    [0x6E] = KEY_KPDOT,
    [0x6F] = KEY_KPSLASH,
    [0x70] = KEY_F1,  [0x71] = KEY_F2,  [0x72] = KEY_F3,  [0x73] = KEY_F4,
    [0x74] = KEY_F5,  [0x75] = KEY_F6,  [0x76] = KEY_F7,  [0x77] = KEY_F8,
    [0x78] = KEY_F9,  [0x79] = KEY_F10, [0x7A] = KEY_F11, [0x7B] = KEY_F12,
    [0x90] = KEY_NUMLOCK,
    [0x91] = KEY_SCROLLLOCK,
    [0xA0] = KEY_LEFTSHIFT,  [0xA1] = KEY_RIGHTSHIFT,
    [0xA2] = KEY_LEFTCTRL,   [0xA3] = KEY_RIGHTCTRL,
    [0xA4] = KEY_LEFTALT,    [0xA5] = KEY_RIGHTALT,
    [0xBA] = KEY_SEMICOLON,
    [0xBB] = KEY_EQUAL,
    [0xBC] = KEY_COMMA,
    [0xBD] = KEY_MINUS,
    [0xBE] = KEY_DOT,
    [0xBF] = KEY_SLASH,
    [0xC0] = KEY_GRAVE,
    [0xDB] = KEY_LEFTBRACE,
    [0xDC] = KEY_BACKSLASH,
    [0xDD] = KEY_RIGHTBRACE,
    [0xDE] = KEY_APOSTROPHE,
};

/* ---- uinput device setup ---- */

static int uinput_open(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        in_log("open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    /* Enable absolute pointer */
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    /* Pointer buttons + wheel */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);   /* hint to libinput: absolute pointer */
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    /* Keyboard — enable everything in the table that's nonzero */
    for (int i = 0; i < 256; i++) {
        if (g_vk_to_key[i])
            ioctl(fd, UI_SET_KEYBIT, g_vk_to_key[i]);
    }

    /* Modern uinput setup (UI_DEV_SETUP) gives us absinfo per-axis */
    struct uinput_setup usetup = {0};
    strncpy(usetup.name, "AppSandbox Virtual Input", UINPUT_MAX_NAME_SIZE - 1);
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0xA53B;
    usetup.id.product = 0x0001;
    usetup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        in_log("UI_DEV_SETUP: %s", strerror(errno));
        close(fd); return -1;
    }

    struct uinput_abs_setup abs = {0};
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = ABS_RANGE;
    abs.code = ABS_X; ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_Y; ioctl(fd, UI_ABS_SETUP, &abs);

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        in_log("UI_DEV_CREATE: %s", strerror(errno));
        close(fd); return -1;
    }
    /* Let udev notice the new device before we start posting events */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
    nanosleep(&ts, NULL);
    return fd;
}

/* Relative motion lives on its own uinput device, created on demand the first
 * time the host sends INPUT_MOUSE_MOVE_REL (i.e. when the display window has
 * the mouse captured). It is deliberately NOT folded into the device above:
 * that one advertises ABS_X/ABS_Y plus BTN_TOUCH so libinput classifies it as
 * an absolute pointer, and adding REL_X/REL_Y would muddy that classification
 * and risk the desktop path that already works. */
static int uinput_open_relative(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        in_log("open /dev/uinput (relative): %s", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    /* libinput only treats a device as a mouse if it has at least one button. */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);

    struct uinput_setup usetup = {0};
    strncpy(usetup.name, "AppSandbox Virtual Mouse (relative)",
            UINPUT_MAX_NAME_SIZE - 1);
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0xA53B;
    usetup.id.product = 0x0002;
    usetup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        in_log("UI_DEV_SETUP (relative): %s", strerror(errno));
        close(fd); return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        in_log("UI_DEV_CREATE (relative): %s", strerror(errno));
        close(fd); return -1;
    }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
    nanosleep(&ts, NULL);
    in_log("relative pointer device created");
    return fd;
}

static void uinput_close(int fd)
{
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
    }
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev = {0};
    ev.type = type; ev.code = code; ev.value = value;
    /* Don't fail loudly on partial writes — keep streaming. */
    (void)!write(fd, &ev, sizeof(ev));
}

static void emit_syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

/* ---- Event translation ---- */

static void do_mouse_move(int ui_fd, uint32_t x, uint32_t y)
{
    /* Host gives us pixel coordinates in the current frame. Map to
     * 0..ABS_RANGE so the compositor scales correctly regardless of
     * the actual screen size. */
    int32_t ax = (int32_t)((uint64_t)x * ABS_RANGE / (g_frame_w ? g_frame_w : 1));
    int32_t ay = (int32_t)((uint64_t)y * ABS_RANGE / (g_frame_h ? g_frame_h : 1));
    if (ax < 0) ax = 0; if (ax > ABS_RANGE) ax = ABS_RANGE;
    if (ay < 0) ay = 0; if (ay > ABS_RANGE) ay = ABS_RANGE;
    emit(ui_fd, EV_ABS, ABS_X, ax);
    emit(ui_fd, EV_ABS, ABS_Y, ay);
    emit_syn(ui_fd);
}

/* Relative motion from a captured host pointer. The device is opened lazily so
 * guests that never enter game mode never grow a second input device; if it
 * cannot be created we drop the motion rather than fail the connection. */
static int g_rel_fd = -1;

static void do_mouse_move_rel(int32_t dx, int32_t dy)
{
    if (g_rel_fd < 0) {
        g_rel_fd = uinput_open_relative();
        if (g_rel_fd < 0) return;
    }
    if (dx) emit(g_rel_fd, EV_REL, REL_X, dx);
    if (dy) emit(g_rel_fd, EV_REL, REL_Y, dy);
    emit_syn(g_rel_fd);
}

static void do_mouse_button(int ui_fd, uint32_t btn_id, uint32_t down)
{
    uint16_t code = 0;
    switch (btn_id) {
    case BTN_ID_LEFT:   code = BTN_LEFT;   break;
    case BTN_ID_RIGHT:  code = BTN_RIGHT;  break;
    case BTN_ID_MIDDLE: code = BTN_MIDDLE; break;
    default: return;
    }
    emit(ui_fd, EV_KEY, code, down ? 1 : 0);
    emit_syn(ui_fd);
}

static void do_mouse_wheel(int ui_fd, int32_t delta)
{
    /* Windows wheel delta is ~120 per notch; REL_WHEEL is one tick per
     * notch. Round toward zero. */
    int32_t notches = delta / 120;
    if (notches == 0) notches = (delta > 0) - (delta < 0);
    emit(ui_fd, EV_REL, REL_WHEEL, notches);
    emit_syn(ui_fd);
}

static void do_key(int ui_fd, uint32_t vk, uint32_t scan, uint32_t flags)
{
    (void)scan;
    uint16_t code = g_vk_to_key[vk & 0xFF];
    if (!code) return;
    int is_up = (flags & 2) != 0;
    emit(ui_fd, EV_KEY, code, is_up ? 0 : 1);
    emit_syn(ui_fd);
}

/* ---- vsock listener ---- */

static int vsock_listen(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { in_log("socket: %s", strerror(errno)); return -1; }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        in_log("bind :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        in_log("listen: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static int recv_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = recv(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; left -= (size_t)n;
    }
    return 0;
}

static void serve(int client_fd, int ui_fd)
{
    /* Tell the host the guest is ready. */
    uint32_t ready = INPUT_READY_MAGIC;
    if (send(client_fd, &ready, sizeof(ready), MSG_NOSIGNAL) != sizeof(ready)) {
        in_log("send IRDY: %s", strerror(errno));
        return;
    }

    while (!g_stop) {
        struct input_packet pkt;
        if (recv_exact(client_fd, &pkt, sizeof(pkt)) < 0) break;
        if (pkt.magic != INPUT_MAGIC) {
            in_log("bad magic 0x%08x — desync, closing", pkt.magic);
            break;
        }
        switch (pkt.type) {
        case INPUT_MOUSE_MOVE:   do_mouse_move(ui_fd, pkt.p1, pkt.p2); break;
        case INPUT_MOUSE_BUTTON: do_mouse_button(ui_fd, pkt.p1, pkt.p2); break;
        case INPUT_MOUSE_WHEEL:  do_mouse_wheel(ui_fd, (int32_t)pkt.p1); break;
        case INPUT_KEY:          do_key(ui_fd, pkt.p1, pkt.p2, pkt.p3); break;
        case INPUT_MOUSE_MOVE_REL:
            do_mouse_move_rel((int32_t)pkt.p1, (int32_t)pkt.p2); break;
        default: break;
        }
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    /* Install SIGINT/SIGTERM WITHOUT SA_RESTART so a blocking accept()
     * returns EINTR on signal (the loop below checks for it) instead of
     * auto-restarting. glibc's signal() defaults to BSD SA_RESTART
     * semantics, which would leave accept() blocked through shutdown and
     * make systemd wait the full stop timeout. Mirrors the agent. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    int ui = uinput_open();
    if (ui < 0) return 1;
    in_log("uinput device created");

    int srv = vsock_listen(VSOCK_PORT);
    if (srv < 0) { uinput_close(ui); return 1; }
    in_log("listening on vsock :%u", VSOCK_PORT);

    while (!g_stop) {
        struct sockaddr_vm peer;
        socklen_t plen = sizeof(peer);
        int c = accept(srv, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR) continue;
            in_log("accept: %s", strerror(errno));
            break;
        }
        in_log("client connected (cid=%u)", peer.svm_cid);
        serve(c, ui);
        close(c);
        in_log("client disconnected");
    }

    close(srv);
    uinput_close(ui);
    uinput_close(g_rel_fd);
    return 0;
}

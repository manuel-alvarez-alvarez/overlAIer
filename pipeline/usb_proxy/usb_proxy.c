#include "usb_proxy.h"
#include "usb_caps.h"
#include "gadget_configfs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "../common/log.h"
#include "../processor/processor_mgr.h"

#define MAX_PROXY_DEVICES 16
#define MAX_REPORT_SIZE   8

// Standard USB HID boot keyboard report descriptor.
// Report format: [modifier(1), reserved(1), keys(6)] = 8 bytes.
static const uint8_t BOOT_KBD_DESC[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};
#define BOOT_KBD_REPORT_LEN 8

// Standard USB HID boot mouse report descriptor (HID 1.11 Appendix B.2).
// Report format: [buttons(1), dx(1), dy(1)] = 3 bytes.
static const uint8_t BOOT_MOUSE_DESC[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15,
    0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};
#define BOOT_MOUSE_REPORT_LEN 3

// Linux KEY_* code -> HID usage code (Keyboard/Keypad page 0x07).
// Modifier keys (Ctrl/Shift/Alt/Meta L+R) map to HID 0xE0..0xE7 and end up in
// the modifier byte rather than the 6-key array. Only the keys we actually
// see on a typical keyboard / multimedia layout are populated; everything
// else is 0 (ignored).
static const uint8_t LINUX_KEY_TO_HID[256] = {
    [KEY_ESC] = 0x29,        [KEY_1] = 0x1E,         [KEY_2] = 0x1F,
    [KEY_3] = 0x20,          [KEY_4] = 0x21,         [KEY_5] = 0x22,
    [KEY_6] = 0x23,          [KEY_7] = 0x24,         [KEY_8] = 0x25,
    [KEY_9] = 0x26,          [KEY_0] = 0x27,         [KEY_MINUS] = 0x2D,
    [KEY_EQUAL] = 0x2E,      [KEY_BACKSPACE] = 0x2A, [KEY_TAB] = 0x2B,
    [KEY_Q] = 0x14,          [KEY_W] = 0x1A,         [KEY_E] = 0x08,
    [KEY_R] = 0x15,          [KEY_T] = 0x17,         [KEY_Y] = 0x1C,
    [KEY_U] = 0x18,          [KEY_I] = 0x0C,         [KEY_O] = 0x12,
    [KEY_P] = 0x13,          [KEY_LEFTBRACE] = 0x2F, [KEY_RIGHTBRACE] = 0x30,
    [KEY_ENTER] = 0x28,      [KEY_LEFTCTRL] = 0xE0,  [KEY_A] = 0x04,
    [KEY_S] = 0x16,          [KEY_D] = 0x07,         [KEY_F] = 0x09,
    [KEY_G] = 0x0A,          [KEY_H] = 0x0B,         [KEY_J] = 0x0D,
    [KEY_K] = 0x0E,          [KEY_L] = 0x0F,         [KEY_SEMICOLON] = 0x33,
    [KEY_APOSTROPHE] = 0x34, [KEY_GRAVE] = 0x35,     [KEY_LEFTSHIFT] = 0xE1,
    [KEY_BACKSLASH] = 0x31,  [KEY_Z] = 0x1D,         [KEY_X] = 0x1B,
    [KEY_C] = 0x06,          [KEY_V] = 0x19,         [KEY_B] = 0x05,
    [KEY_N] = 0x11,          [KEY_M] = 0x10,         [KEY_COMMA] = 0x36,
    [KEY_DOT] = 0x37,        [KEY_SLASH] = 0x38,     [KEY_RIGHTSHIFT] = 0xE5,
    [KEY_KPASTERISK] = 0x55, [KEY_LEFTALT] = 0xE2,   [KEY_SPACE] = 0x2C,
    [KEY_CAPSLOCK] = 0x39,   [KEY_F1] = 0x3A,        [KEY_F2] = 0x3B,
    [KEY_F3] = 0x3C,         [KEY_F4] = 0x3D,        [KEY_F5] = 0x3E,
    [KEY_F6] = 0x3F,         [KEY_F7] = 0x40,        [KEY_F8] = 0x41,
    [KEY_F9] = 0x42,         [KEY_F10] = 0x43,       [KEY_F11] = 0x44,
    [KEY_F12] = 0x45,        [KEY_NUMLOCK] = 0x53,   [KEY_SCROLLLOCK] = 0x47,
    [KEY_KP7] = 0x5F,        [KEY_KP8] = 0x60,       [KEY_KP9] = 0x61,
    [KEY_KPMINUS] = 0x56,    [KEY_KP4] = 0x5C,       [KEY_KP5] = 0x5D,
    [KEY_KP6] = 0x5E,        [KEY_KPPLUS] = 0x57,    [KEY_KP1] = 0x59,
    [KEY_KP2] = 0x5A,        [KEY_KP3] = 0x5B,       [KEY_KP0] = 0x62,
    [KEY_KPDOT] = 0x63,      [KEY_KPENTER] = 0x58,   [KEY_RIGHTCTRL] = 0xE4,
    [KEY_KPSLASH] = 0x54,    [KEY_SYSRQ] = 0x46,     [KEY_RIGHTALT] = 0xE6,
    [KEY_HOME] = 0x4A,       [KEY_UP] = 0x52,        [KEY_PAGEUP] = 0x4B,
    [KEY_LEFT] = 0x50,       [KEY_RIGHT] = 0x4F,     [KEY_END] = 0x4D,
    [KEY_DOWN] = 0x51,       [KEY_PAGEDOWN] = 0x4E,  [KEY_INSERT] = 0x49,
    [KEY_DELETE] = 0x4C,     [KEY_LEFTMETA] = 0xE3,  [KEY_RIGHTMETA] = 0xE7,
    [KEY_PAUSE] = 0x48,
};

// State for assembling boot-format reports from evdev events. The dirty flag
// limits flushes to SYN_REPORTs that actually changed this role — without
// it, a busy mouse stream on a combo device would also re-send the kbd
// report on every SYN.
struct kbd_state {
    uint8_t mods;    // bitmap of pressed modifier keys (HID 0xE0..0xE7)
    uint8_t keys[6]; // currently pressed regular keys (HID usage codes)
    int dirty;
};
struct mouse_state {
    uint8_t buttons;    // bitmap: bit0=left, bit1=right, bit2=middle, bit3=side, bit4=extra
    int dx_acc, dy_acc; // accumulated relative motion since last SYN_REPORT
    int dirty;
    int abs_mode;
    int abs_x_code, abs_y_code;
    int abs_x, abs_y;
    int last_abs_x, last_abs_y;
    int abs_seen;
    int abs_scale_x, abs_scale_y;
    int abs_rem_x, abs_rem_y;
};

// One proxy_device per source evdev. A single evdev under hid-logitech-dj can
// expose both keyboard and mouse capabilities at once (the K400 does this),
// so each device may drive up to two gadget functions in parallel — one for
// the keyboard role, one for the mouse role.
struct proxy_device {
    struct ovl_usb_hid_info info;
    int evdev_fd;

    // Keyboard role (-1/0 if not active).
    int kbd_index;   // gadget function index, -1 if no kbd role
    int kbd_hidg_fd; // /dev/hidgN write fd, -1 if not opened
    struct kbd_state kbd;

    // Mouse role (-1/0 if not active).
    int mouse_index;
    int mouse_hidg_fd;
    struct mouse_state mouse;

    pthread_t thread;
    int thread_started;
    volatile int running;
    struct ovl_processor_mgr *proc_mgr;
};

struct ovl_usb_proxy {
    struct proxy_device devices[MAX_PROXY_DEVICES];
    int num_devices;
    volatile int active;
};

static int kbd_press(struct kbd_state *s, uint8_t hid) {
    for (int i = 0; i < 6; i++)
        if (s->keys[i] == hid)
            return 0;
    for (int i = 0; i < 6; i++) {
        if (s->keys[i] == 0) {
            s->keys[i] = hid;
            return 0;
        }
    }
    return -1; // 6-key roll-over full
}

static void kbd_release(struct kbd_state *s, uint8_t hid) {
    for (int i = 0; i < 6; i++) {
        if (s->keys[i] == hid) {
            s->keys[i] = 0;
            return;
        }
    }
}

static void encode_kbd_report(const struct kbd_state *s, uint8_t out[8]) {
    out[0] = s->mods;
    out[1] = 0;
    for (int i = 0; i < 6; i++)
        out[2 + i] = s->keys[i];
}

static int8_t clamp_axis(int v) {
    if (v > 127)
        return 127;
    if (v < -127)
        return -127;
    return (int8_t)v;
}

static void encode_mouse_report(const struct mouse_state *s, uint8_t out[3]) {
    out[0] = s->buttons & 0x07; // boot mouse only carries 3 buttons
    out[1] = (uint8_t)clamp_axis(s->dx_acc);
    out[2] = (uint8_t)clamp_axis(s->dy_acc);
}

static int setup_abs_pointer_state(struct proxy_device *dev) {
    if (!(dev->info.usage_kind & OVL_USB_KIND_MOUSE))
        return 0;

    uint8_t abs_bits[(ABS_MAX / 8) + 1];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(dev->evdev_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return 0;

    int x_code = -1, y_code = -1;
    if (((abs_bits[ABS_X / 8] >> (ABS_X % 8)) & 1) != 0 &&
        ((abs_bits[ABS_Y / 8] >> (ABS_Y % 8)) & 1) != 0) {
        x_code = ABS_X;
        y_code = ABS_Y;
    } else if (((abs_bits[ABS_MT_POSITION_X / 8] >> (ABS_MT_POSITION_X % 8)) & 1) != 0 &&
               ((abs_bits[ABS_MT_POSITION_Y / 8] >> (ABS_MT_POSITION_Y % 8)) & 1) != 0) {
        x_code = ABS_MT_POSITION_X;
        y_code = ABS_MT_POSITION_Y;
    }
    if (x_code < 0 || y_code < 0)
        return 0;

    struct input_absinfo abs_x, abs_y;
    if (ioctl(dev->evdev_fd, EVIOCGABS(x_code), &abs_x) < 0 ||
        ioctl(dev->evdev_fd, EVIOCGABS(y_code), &abs_y) < 0)
        return 0;

    int range_x = abs_x.maximum - abs_x.minimum;
    int range_y = abs_y.maximum - abs_y.minimum;
    dev->mouse.abs_mode = 1;
    dev->mouse.abs_x_code = x_code;
    dev->mouse.abs_y_code = y_code;
    dev->mouse.abs_x = abs_x.value;
    dev->mouse.abs_y = abs_y.value;
    dev->mouse.last_abs_x = abs_x.value;
    dev->mouse.last_abs_y = abs_y.value;
    dev->mouse.abs_seen = 0;
    dev->mouse.abs_scale_x = range_x > 0 ? (range_x / 256) : 1;
    dev->mouse.abs_scale_y = range_y > 0 ? (range_y / 256) : 1;
    if (dev->mouse.abs_scale_x < 1)
        dev->mouse.abs_scale_x = 1;
    if (dev->mouse.abs_scale_y < 1)
        dev->mouse.abs_scale_y = 1;

    ZF_LOGD("usb_proxy: '%s' using ABS pointer mode codes=%d,%d x=%d..%d y=%d..%d scale=%d,%d",
            dev->info.evdev_path, x_code, y_code, abs_x.minimum, abs_x.maximum, abs_y.minimum,
            abs_y.maximum, dev->mouse.abs_scale_x, dev->mouse.abs_scale_y);
    return 0;
}

static void send_report(struct proxy_device *dev, int hidg_fd, int gadget_index, uint8_t *report,
                        int len) {
    if (hidg_fd < 0)
        return;
    if (dev->proc_mgr) {
        if (ovl_processor_mgr_process_hid(dev->proc_mgr, report, &len, dev->info.name,
                                          dev->info.vid, dev->info.pid) < 0)
            return;
    }
    ssize_t w = write(hidg_fd, report, (size_t)len);
    if (w < 0 && errno != EINTR && errno != ESHUTDOWN)
        ZF_LOGD("usb_proxy: write error on hidg(%d): %s", gadget_index, strerror(errno));
}

static void handle_kbd_event(struct proxy_device *dev, const struct input_event *ev) {
    if (ev->type == EV_KEY && ev->value != 2) {
        if (ev->code >= 256)
            return; // out of range for our table
        uint8_t hid = LINUX_KEY_TO_HID[ev->code];
        if (!hid)
            return;
        if (hid >= 0xE0 && hid <= 0xE7) {
            uint8_t bit = 1u << (hid - 0xE0);
            if (ev->value)
                dev->kbd.mods |= bit;
            else
                dev->kbd.mods &= (uint8_t)~bit;
        } else if (ev->value) {
            kbd_press(&dev->kbd, hid);
        } else {
            kbd_release(&dev->kbd, hid);
        }
        dev->kbd.dirty = 1;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT && dev->kbd.dirty) {
        uint8_t report[8];
        encode_kbd_report(&dev->kbd, report);
        send_report(dev, dev->kbd_hidg_fd, dev->kbd_index, report, 8);
        dev->kbd.dirty = 0;
    }
}

static void handle_mouse_event(struct proxy_device *dev, const struct input_event *ev) {
    if (ev->type == EV_REL) {
        if (ev->code == REL_X) {
            dev->mouse.dx_acc += ev->value;
            dev->mouse.dirty = 1;
        } else if (ev->code == REL_Y) {
            dev->mouse.dy_acc += ev->value;
            dev->mouse.dirty = 1;
        }
        // REL_WHEEL ignored — boot mouse has no wheel.
    } else if (ev->type == EV_ABS && dev->mouse.abs_mode) {
        if (ev->code == dev->mouse.abs_x_code) {
            dev->mouse.abs_x = ev->value;
        } else if (ev->code == dev->mouse.abs_y_code) {
            dev->mouse.abs_y = ev->value;
        } else {
            return;
        }
        dev->mouse.dirty = 1;
    } else if (ev->type == EV_KEY) {
        uint8_t bit = 0;
        switch (ev->code) {
        case BTN_LEFT:
            bit = 1u << 0;
            break;
        case BTN_RIGHT:
            bit = 1u << 1;
            break;
        case BTN_MIDDLE:
            bit = 1u << 2;
            break;
        case BTN_SIDE:
            bit = 1u << 3;
            break;
        case BTN_EXTRA:
            bit = 1u << 4;
            break;
        default:
            return;
        }
        if (ev->value)
            dev->mouse.buttons |= bit;
        else
            dev->mouse.buttons &= (uint8_t)~bit;
        dev->mouse.dirty = 1;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT && dev->mouse.dirty) {
        if (dev->mouse.abs_mode) {
            if (!dev->mouse.abs_seen) {
                dev->mouse.abs_seen = 1;
            } else {
                int raw_dx = (dev->mouse.abs_x - dev->mouse.last_abs_x) + dev->mouse.abs_rem_x;
                int raw_dy = (dev->mouse.abs_y - dev->mouse.last_abs_y) + dev->mouse.abs_rem_y;
                dev->mouse.dx_acc = raw_dx / dev->mouse.abs_scale_x;
                dev->mouse.dy_acc = raw_dy / dev->mouse.abs_scale_y;
                dev->mouse.abs_rem_x = raw_dx % dev->mouse.abs_scale_x;
                dev->mouse.abs_rem_y = raw_dy % dev->mouse.abs_scale_y;
            }
            dev->mouse.last_abs_x = dev->mouse.abs_x;
            dev->mouse.last_abs_y = dev->mouse.abs_y;
        }
        uint8_t report[3];
        encode_mouse_report(&dev->mouse, report);
        send_report(dev, dev->mouse_hidg_fd, dev->mouse_index, report, 3);
        dev->mouse.dx_acc = 0;
        dev->mouse.dy_acc = 0;
        dev->mouse.dirty = 0;
    }
}

static void *proxy_thread_fn(void *arg) {
    struct proxy_device *dev = arg;

    ZF_LOGI("usb_proxy: proxying '%s' (%04x:%04x) %s [kbd=%d mouse=%d]", dev->info.name,
            dev->info.vid, dev->info.pid, dev->info.evdev_path, dev->kbd_index, dev->mouse_index);

    struct pollfd pfd = {.fd = dev->evdev_fd, .events = POLLIN};
    while (dev->running) {
        int pr = poll(&pfd, 1, 100); // 100ms — bounded latency on shutdown
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            ZF_LOGE("usb_proxy: poll error on '%s': %s", dev->info.evdev_path, strerror(errno));
            break;
        }
        if (pr == 0)
            continue;

        struct input_event ev;
        ssize_t n = read(dev->evdev_fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            ZF_LOGE("usb_proxy: read error on '%s': %s", dev->info.evdev_path, strerror(errno));
            break;
        }
        if (n != sizeof(ev))
            continue;

        // Each handler is a no-op for events outside its role, and
        // send_report bails when the corresponding hidg fd is unset, so it's
        // safe (and necessary, for combo devices) to call both.
        if (dev->kbd_hidg_fd >= 0)
            handle_kbd_event(dev, &ev);
        if (dev->mouse_hidg_fd >= 0)
            handle_mouse_event(dev, &ev);
    }

    dev->running = 0;
    return NULL;
}

int ovl_usb_proxy_init(struct ovl_usb_proxy **out, const char *udc, const char **vid_pids,
                       int num_vid_pids, struct ovl_processor_mgr *proc_mgr) {
    if (num_vid_pids == 0)
        return 0;

    struct ovl_usb_hid_info matched[MAX_PROXY_DEVICES];
    int num_matched = ovl_usb_find_devices(vid_pids, num_vid_pids, matched, MAX_PROXY_DEVICES);
    if (num_matched == 0) {
        ZF_LOGW("usb_proxy: no matching HID input devices found");
        return -1;
    }

    if (ovl_gadget_create(udc) < 0)
        return -1;

    struct ovl_usb_proxy *proxy = calloc(1, sizeof(*proxy));
    if (!proxy) {
        ovl_gadget_destroy();
        return -1;
    }

    int next_gadget_index = 0;
    for (int i = 0; i < num_matched; i++) {
        if ((matched[i].usage_kind & (OVL_USB_KIND_KBD | OVL_USB_KIND_MOUSE)) == 0) {
            ZF_LOGD("usb_proxy: skipping '%s' (usage_kind=0x%x)", matched[i].evdev_path,
                    matched[i].usage_kind);
            continue;
        }

        struct proxy_device *dev = &proxy->devices[proxy->num_devices];
        memset(dev, 0, sizeof(*dev));
        dev->info = matched[i];
        dev->proc_mgr = proc_mgr;
        dev->evdev_fd = -1;
        dev->kbd_index = -1;
        dev->kbd_hidg_fd = -1;
        dev->mouse_index = -1;
        dev->mouse_hidg_fd = -1;

        // Allocate one gadget function per active role on this evdev. Using
        // bInterfaceSubClass=1 + bInterfaceProtocol=1|2 makes the host bind
        // its boot HID drivers, exactly as a real boot keyboard / boot mouse
        // would on a USB connection.
        if (dev->info.usage_kind & OVL_USB_KIND_KBD) {
            dev->kbd_index = next_gadget_index++;
            if (ovl_gadget_add_hid(dev->kbd_index, BOOT_KBD_DESC, (int)sizeof(BOOT_KBD_DESC),
                                   BOOT_KBD_REPORT_LEN, 1, 1) < 0) {
                ZF_LOGE("usb_proxy: failed to add keyboard gadget for '%s'", dev->info.evdev_path);
                dev->kbd_index = -1;
            }
        }
        if (dev->info.usage_kind & OVL_USB_KIND_MOUSE) {
            dev->mouse_index = next_gadget_index++;
            if (ovl_gadget_add_hid(dev->mouse_index, BOOT_MOUSE_DESC, (int)sizeof(BOOT_MOUSE_DESC),
                                   BOOT_MOUSE_REPORT_LEN, 1, 2) < 0) {
                ZF_LOGE("usb_proxy: failed to add mouse gadget for '%s'", dev->info.evdev_path);
                dev->mouse_index = -1;
            }
        }
        if (dev->kbd_index < 0 && dev->mouse_index < 0)
            continue;

        dev->evdev_fd = open(dev->info.evdev_path, O_RDONLY);
        if (dev->evdev_fd < 0) {
            ZF_LOGE("usb_proxy: cannot open '%s': %s", dev->info.evdev_path, strerror(errno));
            continue;
        }
        // Take exclusive ownership so the kernel doesn't also deliver these
        // events to the local console / X session.
        if (ioctl(dev->evdev_fd, EVIOCGRAB, 1) < 0) {
            ZF_LOGW("usb_proxy: EVIOCGRAB failed on '%s': %s", dev->info.evdev_path,
                    strerror(errno));
        }
        setup_abs_pointer_state(dev);

        proxy->num_devices++;
        ZF_LOGI("usb_proxy: configured '%s' (%04x:%04x) bus %d addr %d %s [kbd=%d mouse=%d]",
                dev->info.name, dev->info.vid, dev->info.pid, dev->info.bus, dev->info.address,
                dev->info.evdev_path, dev->kbd_index, dev->mouse_index);
    }

    if (proxy->num_devices == 0) {
        ZF_LOGE("usb_proxy: no devices could be set up");
        free(proxy);
        ovl_gadget_destroy();
        return -1;
    }

    if (ovl_gadget_enable() < 0) {
        for (int i = 0; i < proxy->num_devices; i++) {
            struct proxy_device *d = &proxy->devices[i];
            if (d->evdev_fd >= 0) {
                ioctl(d->evdev_fd, EVIOCGRAB, 0);
                close(d->evdev_fd);
            }
        }
        free(proxy);
        ovl_gadget_destroy();
        return -1;
    }

    usleep(100000); // wait for /dev/hidgN to appear

    // Open the gadget output for each role we allocated. The kernel assigns
    // /dev/hidgN minors at bind time and not necessarily in configfs label
    // order, so look up each one through the function's `dev` attribute.
    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->kbd_index >= 0) {
            int minor = ovl_gadget_get_hid_minor(dev->kbd_index);
            char path[64];
            snprintf(path, sizeof(path), "/dev/hidg%d", minor);
            dev->kbd_hidg_fd = (minor >= 0) ? open(path, O_WRONLY) : -1;
            if (dev->kbd_hidg_fd < 0)
                ZF_LOGE("usb_proxy: cannot open %s for kbd: %s", path, strerror(errno));
            else
                ZF_LOGI("usb_proxy: '%s' kbd (hid.usb%d) -> %s", dev->info.name, dev->kbd_index,
                        path);
        }
        if (dev->mouse_index >= 0) {
            int minor = ovl_gadget_get_hid_minor(dev->mouse_index);
            char path[64];
            snprintf(path, sizeof(path), "/dev/hidg%d", minor);
            dev->mouse_hidg_fd = (minor >= 0) ? open(path, O_WRONLY) : -1;
            if (dev->mouse_hidg_fd < 0)
                ZF_LOGE("usb_proxy: cannot open %s for mouse: %s", path, strerror(errno));
            else
                ZF_LOGI("usb_proxy: '%s' mouse (hid.usb%d) -> %s", dev->info.name, dev->mouse_index,
                        path);
        }
    }

    proxy->active = 1;
    *out = proxy;
    return 0;
}

int ovl_usb_proxy_start(struct ovl_usb_proxy *proxy) {
    if (!proxy)
        return 0;

    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->evdev_fd < 0)
            continue;
        if (dev->kbd_hidg_fd < 0 && dev->mouse_hidg_fd < 0)
            continue;
        dev->running = 1;
        if (pthread_create(&dev->thread, NULL, proxy_thread_fn, dev) == 0)
            dev->thread_started = 1;
    }
    return 0;
}

void ovl_usb_proxy_stop(struct ovl_usb_proxy *proxy) {
    if (!proxy)
        return;
    proxy->active = 0;

    for (int i = 0; i < proxy->num_devices; i++)
        proxy->devices[i].running = 0;

    // Threads exit on the next poll() timeout (≤100ms).
    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->thread_started)
            pthread_join(dev->thread, NULL);
    }
}

void ovl_usb_proxy_destroy(struct ovl_usb_proxy *proxy) {
    if (!proxy)
        return;

    ovl_usb_proxy_stop(proxy);

    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->evdev_fd >= 0) {
            ioctl(dev->evdev_fd, EVIOCGRAB, 0);
            close(dev->evdev_fd);
        }
        if (dev->kbd_hidg_fd >= 0)
            close(dev->kbd_hidg_fd);
        if (dev->mouse_hidg_fd >= 0)
            close(dev->mouse_hidg_fd);
    }

    ovl_gadget_destroy();
    free(proxy);
}

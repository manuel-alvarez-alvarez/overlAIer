#ifndef OVERLAIER_USB_CAPS_H
#define OVERLAIER_USB_CAPS_H

#include <stdint.h>

#define OVL_USB_MAX_DEVICES 32

// Description of a single source input that can be proxied. We work at the
// evdev layer (/dev/input/eventN) rather than libusb so that HID++ devices
// like the Logitech K400 — whose keyboard data the receiver only emits via
// the Logitech-proprietary interface — work via the kernel's hid-logitech-dj
// translation. The kernel exposes one event device per logical role
// (keyboard, mouse, consumer, ...).
// usage_kind is a bitmask of roles the same evdev node can play. A single
// evdev under hid-logitech-dj often combines keyboard+mouse, so we treat it
// as both rather than forcing a single classification.
#define OVL_USB_KIND_KBD     0x1
#define OVL_USB_KIND_MOUSE   0x2
#define OVL_USB_KIND_GAMEPAD 0x4

struct ovl_usb_hid_info {
    char name[128]; // EVIOCGNAME / device product string
    uint16_t vid, pid;
    uint8_t bus, address; // sysfs busnum / devnum of the parent USB device
    int input_index;      // N from /dev/input/eventN
    int usage_kind;       // bitmask of OVL_USB_KIND_*
    char evdev_path[64];  // "/dev/input/eventN"
};

// Lifecycle hooks. No-ops in the evdev-based implementation, kept for
// source-compatibility with main.c.
int ovl_usb_init(void);
void ovl_usb_exit(void);

// Enumerate input devices that come from a USB HID source.
// Returns count, fills entries up to max_entries.
int ovl_usb_enum_hid_devices(struct ovl_usb_hid_info *entries, int max_entries);

// Find devices matching a list of "VVVV:PPPP" VID:PID strings.
// Returns count of matched devices, fills matched[] up to max_matched.
int ovl_usb_find_devices(const char **vid_pids, int num_vid_pids, struct ovl_usb_hid_info *matched,
                         int max_matched);

#endif

#ifndef OVERLAIER_USB_CAPS_H
#define OVERLAIER_USB_CAPS_H

#include <stdint.h>

#define OVL_USB_MAX_DEVICES  32
#define OVL_USB_MAX_DESC_LEN 4096

struct ovl_usb_hid_info {
    char name[128];
    uint16_t vid, pid;
    uint8_t bus, address;        // for unique device identification
    int interface_number;
    uint8_t ep_in;               // interrupt IN endpoint address
    int protocol;                // 1=keyboard, 2=mouse, 0=other
    uint8_t report_desc[OVL_USB_MAX_DESC_LEN];
    int report_desc_len;
};

// Initialize the USB subsystem. Call once at startup.
int ovl_usb_init(void);

// Shut down the USB subsystem. Call once at exit.
void ovl_usb_exit(void);

// Enumerate connected USB HID devices.
// Returns count, fills entries up to max_entries.
int ovl_usb_enum_hid_devices(struct ovl_usb_hid_info *entries, int max_entries);

// Find devices matching a list of VID:PID strings.
// Returns count of matched devices, fills matched[] up to max_matched.
int ovl_usb_find_devices(const char **vid_pids, int num_vid_pids,
                         struct ovl_usb_hid_info *matched, int max_matched);

#endif

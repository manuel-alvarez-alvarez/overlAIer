#include "usb_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libusb-1.0/libusb.h>

#include "../common/log.h"

static libusb_context *usb_ctx;

int ovl_usb_init(void) {
    return libusb_init(&usb_ctx);
}

void ovl_usb_exit(void) {
    if (usb_ctx) {
        libusb_exit(usb_ctx);
        usb_ctx = NULL;
    }
}

// Get HID report descriptor via USB control transfer
static int get_hid_report_descriptor(libusb_device_handle *handle, int iface,
                                     uint8_t *buf, int max_len) {
    // GET_DESCRIPTOR request for HID Report Descriptor (0x22)
    int rc = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_INTERFACE,
        LIBUSB_REQUEST_GET_DESCRIPTOR,
        (0x22 << 8),  // HID Report Descriptor type
        (uint16_t)iface,
        buf, (uint16_t)max_len,
        1000);
    return rc > 0 ? rc : -1;
}

// Get product string from device
static void get_product_string(libusb_device_handle *handle,
                               struct libusb_device_descriptor *desc,
                               char *buf, int len) {
    buf[0] = '\0';
    if (desc->iProduct > 0) {
        libusb_get_string_descriptor_ascii(handle, desc->iProduct,
                                           (unsigned char *)buf, len);
    }
}

// Detect protocol from HID descriptor
// Returns: 1=keyboard, 2=mouse, 3=gamepad, 0=other, -1=vendor-specific (skip)
static int detect_protocol(const uint8_t *desc, int desc_len) {
    int has_standard = 0;
    int has_vendor = 0;
    int protocol = 0;

    for (int i = 0; i + 1 < desc_len;) {
        uint8_t item = desc[i];
        int size = item & 0x03;
        if (size == 3) size = 4;
        if (i + size >= desc_len) break;

        int tag = item & 0xFC;

        // Usage Page (1 or 2 byte value)
        if (tag == 0x04 || tag == 0x06) { // short or long usage page
            int page = 0;
            if (size >= 1) page = desc[i + 1];
            if (size >= 2) page |= desc[i + 2] << 8;

            if (page >= 0xFF00)
                has_vendor = 1;
            else if (page == 0x01) // Generic Desktop
                has_standard = 1;
        }

        // Usage (after Generic Desktop usage page)
        if (tag == 0x08 && has_standard && size >= 1) {
            uint8_t usage = desc[i + 1];
            if (usage == 0x06 && protocol == 0) protocol = 1; // Keyboard
            if (usage == 0x02 && protocol == 0) protocol = 2; // Mouse
            if ((usage == 0x04 || usage == 0x05) && protocol == 0) protocol = 3; // Joystick/Gamepad
        }

        i += 1 + size;
    }

    // If descriptor only has vendor-specific usage pages, skip it
    if (has_vendor && !has_standard)
        return -1;

    return protocol;
}

int ovl_usb_enum_hid_devices(struct ovl_usb_hid_info *entries, int max_entries) {
    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(usb_ctx, &devs);
    if (cnt < 0)
        return 0;

    int count = 0;
    for (ssize_t i = 0; i < cnt && count < max_entries; i++) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) < 0)
            continue;

        struct libusb_config_descriptor *config;
        if (libusb_get_active_config_descriptor(dev, &config) < 0)
            continue;

        for (int j = 0; j < config->bNumInterfaces && count < max_entries; j++) {
            const struct libusb_interface *iface = &config->interface[j];
            for (int k = 0; k < iface->num_altsetting; k++) {
                const struct libusb_interface_descriptor *setting = &iface->altsetting[k];

                // Only HID class interfaces
                if (setting->bInterfaceClass != 3) // USB_CLASS_HID
                    continue;

                // Find interrupt IN endpoint
                uint8_t ep_in = 0;
                for (int e = 0; e < setting->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &setting->endpoint[e];
                    if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN &&
                        (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                        ep_in = ep->bEndpointAddress;
                        break;
                    }
                }
                if (!ep_in)
                    continue;

                struct ovl_usb_hid_info *info = &entries[count];
                memset(info, 0, sizeof(*info));
                info->vid = desc.idVendor;
                info->pid = desc.idProduct;
                info->bus = libusb_get_bus_number(dev);
                info->address = libusb_get_device_address(dev);
                info->interface_number = setting->bInterfaceNumber;
                info->ep_in = ep_in;

                // Try to get product string and report descriptor
                libusb_device_handle *handle;
                if (libusb_open(dev, &handle) == 0) {
                    get_product_string(handle, &desc, info->name, sizeof(info->name));

                    // Detach kernel driver temporarily to get the report descriptor
                    int was_attached = libusb_kernel_driver_active(handle, info->interface_number);
                    if (was_attached == 1)
                        libusb_detach_kernel_driver(handle, info->interface_number);

                    libusb_claim_interface(handle, info->interface_number);

                    info->report_desc_len = get_hid_report_descriptor(
                        handle, info->interface_number,
                        info->report_desc, OVL_USB_MAX_DESC_LEN);

                    libusb_release_interface(handle, info->interface_number);

                    // Reattach kernel driver so the device works normally
                    if (was_attached == 1)
                        libusb_attach_kernel_driver(handle, info->interface_number);

                    libusb_close(handle);
                }

                if (!info->name[0])
                    snprintf(info->name, sizeof(info->name), "%04x:%04x",
                             info->vid, info->pid);

                if (info->report_desc_len > 0) {
                    info->protocol = detect_protocol(info->report_desc,
                                                      info->report_desc_len);
                    // Skip vendor-specific interfaces
                    if (info->protocol < 0) {
                        ZF_LOGD("usb: skipping vendor-specific interface %d on %04x:%04x",
                                info->interface_number, info->vid, info->pid);
                        continue;
                    }
                }

                count++;
            }
        }

        libusb_free_config_descriptor(config);
    }

    libusb_free_device_list(devs, 1);
    return count;
}

int ovl_usb_find_devices(const char **vid_pids, int num_vid_pids,
                         struct ovl_usb_hid_info *matched, int max_matched) {
    struct ovl_usb_hid_info all[OVL_USB_MAX_DEVICES];
    int total = ovl_usb_enum_hid_devices(all, OVL_USB_MAX_DEVICES);

    int count = 0;
    for (int i = 0; i < total && count < max_matched; i++) {
        char vid_pid[16];
        snprintf(vid_pid, sizeof(vid_pid), "%04x:%04x", all[i].vid, all[i].pid);

        for (int j = 0; j < num_vid_pids; j++) {
            if (strcasecmp(vid_pid, vid_pids[j]) == 0) {
                matched[count++] = all[i];
                break;
            }
        }
    }
    return count;
}

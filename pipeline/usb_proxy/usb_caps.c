#include "usb_caps.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <hidapi/hidapi.h>

#include "../common/log.h"

static int read_sysfs_binary(const char *path, void *buf, int max_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, (size_t)max_len);
    close(fd);
    return (n > 0) ? (int)n : -1;
}

// Get max report length from sysfs (the kernel already parsed the descriptor)
static int get_report_len_from_sysfs(const char *hidraw_name) {
    // The kernel exposes the max report size via HIDIOCGRDESCSIZE or we can
    // read it from the hidraw device. For simplicity and correctness, use a
    // generous fixed maximum — the gadget driver uses this as a buffer size.
    // 64 bytes covers all standard HID reports (keyboard=8, mouse=4-8,
    // Logitech long reports=20, gamepad=up to 64).
    (void)hidraw_name;
    return 64;
}

// Detect protocol from HID descriptor (keyboard=1, mouse=2, other=0)
static int detect_protocol(const uint8_t *desc, int desc_len) {
    for (int i = 0; i + 3 < desc_len; i++) {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 &&
            i + 3 < desc_len && desc[i + 2] == 0x09) {
            uint8_t usage = desc[i + 3];
            if (usage == 0x06) return 1; // Keyboard
            if (usage == 0x02) return 2; // Mouse
        }
    }
    return 0;
}

// Extract hidraw name from hidapi path (e.g. "/dev/hidraw3" -> "hidraw3")
static const char *hidraw_from_path(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

int ovl_usb_enum_hid_devices(struct ovl_usb_hid_info *entries, int max_entries) {
    struct hid_device_info *devs = hid_enumerate(0, 0);
    if (!devs)
        return 0;

    int count = 0;
    for (struct hid_device_info *d = devs; d && count < max_entries; d = d->next) {
        struct ovl_usb_hid_info *info = &entries[count];
        memset(info, 0, sizeof(*info));

        info->vid = d->vendor_id;
        info->pid = d->product_id;
        info->interface_number = d->interface_number;

        if (d->path)
            snprintf(info->path, sizeof(info->path), "%s", d->path);

        // Convert wide string name to UTF-8
        if (d->product_string) {
            for (int i = 0; i < (int)sizeof(info->name) - 1 && d->product_string[i]; i++)
                info->name[i] = (char)d->product_string[i]; // ASCII subset
        }
        if (!info->name[0])
            snprintf(info->name, sizeof(info->name), "%04x:%04x", info->vid, info->pid);

        // Skip devices with no VID/PID
        if (info->vid == 0 && info->pid == 0)
            continue;

        // Read report descriptor from sysfs (hidapi doesn't expose it)
        char desc_path[512];
        snprintf(desc_path, sizeof(desc_path),
                 "/sys/class/hidraw/%s/device/report_descriptor",
                 hidraw_from_path(info->path));
        info->report_desc_len = read_sysfs_binary(desc_path, info->report_desc,
                                                   OVL_USB_MAX_DESC_LEN);
        if (info->report_desc_len <= 0)
            continue;

        info->report_len = get_report_len_from_sysfs(hidraw_from_path(info->path));
        info->protocol = detect_protocol(info->report_desc, info->report_desc_len);

        count++;
    }

    hid_free_enumeration(devs);
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

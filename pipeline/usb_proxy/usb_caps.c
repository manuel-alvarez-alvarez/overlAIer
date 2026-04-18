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

// Estimate max report length from HID descriptor
static int estimate_report_len(const uint8_t *desc, int desc_len) {
    int max_len = 0;
    int report_size = 0;
    int report_count = 0;

    for (int i = 0; i < desc_len;) {
        uint8_t item = desc[i];
        int size = item & 0x03;
        if (size == 3)
            size = 4;
        if (i + size >= desc_len)
            break;

        int tag = item & 0xFC;
        int value = 0;
        if (size >= 1)
            value = desc[i + 1];
        if (size >= 2)
            value |= desc[i + 2] << 8;

        if (tag == 0x74)
            report_size = value;
        else if (tag == 0x94)
            report_count = value;
        else if (tag == 0x80 || tag == 0x90 || tag == 0xB0) {
            int bits = report_size * report_count;
            int bytes = (bits + 7) / 8;
            if (bytes > max_len)
                max_len = bytes;
        }

        i += 1 + size;
    }

    return max_len > 0 ? max_len + 1 : 8;
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

        info->report_len = estimate_report_len(info->report_desc, info->report_desc_len);
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

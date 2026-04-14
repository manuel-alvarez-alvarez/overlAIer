#include "usb_caps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/log.h"

static int read_sysfs_string(const char *path, char *buf, int len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, (size_t)(len - 1));
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    // Strip trailing newline
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return (int)n;
}

static int read_sysfs_hex16(const char *path, uint16_t *out) {
    char buf[16];
    if (read_sysfs_string(path, buf, sizeof(buf)) < 0)
        return -1;
    *out = (uint16_t)strtoul(buf, NULL, 16);
    return 0;
}

static int read_sysfs_binary(const char *path, void *buf, int max_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, (size_t)max_len);
    close(fd);
    return (n > 0) ? (int)n : -1;
}

// Estimate max report length from HID descriptor.
// This is a simplified parser — looks for report size and count fields.
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

        if (tag == 0x74) // Report Size
            report_size = value;
        else if (tag == 0x94) // Report Count
            report_count = value;
        else if (tag == 0x80 || tag == 0x90 || tag == 0xB0) { // Input/Output/Feature
            int bits = report_size * report_count;
            int bytes = (bits + 7) / 8;
            if (bytes > max_len)
                max_len = bytes;
        }

        i += 1 + size;
    }

    // Add 1 for report ID if descriptor uses multiple reports
    return max_len > 0 ? max_len + 1 : 8;
}

int ovl_usb_enum_hid_devices(struct ovl_usb_hid_info *entries, int max_entries) {
    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        if (ent->d_name[0] == '.')
            continue;

        struct ovl_usb_hid_info *info = &entries[count];
        memset(info, 0, sizeof(*info));
        snprintf(info->hidraw, sizeof(info->hidraw), "%s", ent->d_name);

        char path[512];

        // Read device name
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", ent->d_name);
        char uevent[1024];
        if (read_sysfs_string(path, uevent, sizeof(uevent)) > 0) {
            // Parse HID_NAME from uevent
            char *name_line = strstr(uevent, "HID_NAME=");
            if (name_line) {
                name_line += 9;
                char *end = strchr(name_line, '\n');
                if (end)
                    *end = '\0';
                snprintf(info->name, sizeof(info->name), "%s", name_line);
            }
            // Parse HID_ID for VID/PID (format: "0003:VVVV:PPPP.NNNN")
            char *id_line = strstr(uevent, "HID_ID=");
            if (id_line) {
                unsigned int bus, vid, pid;
                if (sscanf(id_line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
                    info->vid = (uint16_t)vid;
                    info->pid = (uint16_t)pid;
                }
            }
        }

        // Skip devices with no VID/PID (virtual/internal)
        if (info->vid == 0 && info->pid == 0)
            continue;

        // Read HID report descriptor
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/report_descriptor",
                 ent->d_name);
        info->report_desc_len = read_sysfs_binary(path, info->report_desc,
                                                   OVL_USB_MAX_DESC_LEN);
        if (info->report_desc_len <= 0)
            continue;

        info->report_len = estimate_report_len(info->report_desc, info->report_desc_len);

        // Determine protocol from descriptor heuristics
        // Check uevent for HID_UNIQ or use descriptor parsing
        // Simple: check for boot protocol usage page
        info->protocol = 0; // default: other (gamepad)
        // Boot keyboard uses protocol 1, boot mouse uses protocol 2
        // We can check the HID descriptor for usage page + usage
        for (int i = 0; i + 3 < info->report_desc_len; i++) {
            if (info->report_desc[i] == 0x05 && info->report_desc[i + 1] == 0x01) {
                // Usage Page: Generic Desktop
                if (i + 3 < info->report_desc_len &&
                    info->report_desc[i + 2] == 0x09) {
                    uint8_t usage = info->report_desc[i + 3];
                    if (usage == 0x06) // Keyboard
                        info->protocol = 1;
                    else if (usage == 0x02) // Mouse
                        info->protocol = 2;
                    // 0x04 = Joystick, 0x05 = Game Pad → stays 0
                }
            }
        }

        count++;
    }

    closedir(d);
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

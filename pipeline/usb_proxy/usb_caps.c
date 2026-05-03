#include "usb_caps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input.h>

#include "../common/log.h"

int ovl_usb_init(void) {
    return 0;
}
void ovl_usb_exit(void) {}

// Bit-test on an EVIOCGBIT result.
#define BIT(buf, n) (((buf)[(n) / 8] >> ((n) % 8)) & 1)

// Read /sys/class/input/eventN's device chain to find the parent USB device,
// then read its idVendor/idProduct/busnum/devnum. Returns 0 on success.
static int read_parent_usb(int input_index, uint16_t *vid, uint16_t *pid, uint8_t *bus,
                           uint8_t *addr) {
    char link[256];
    snprintf(link, sizeof(link), "/sys/class/input/event%d/device", input_index);

    char target[PATH_MAX];
    if (!realpath(link, target))
        return -1;

    // Walk up looking for a directory that has idVendor/idProduct/busnum/devnum.
    char path[PATH_MAX];
    while (1) {
        snprintf(path, sizeof(path), "%s/idVendor", target);
        if (access(path, R_OK) == 0)
            break;
        // Strip the last component
        char *slash = strrchr(target, '/');
        if (!slash || slash == target)
            return -1;
        *slash = '\0';
    }

    char buf[16];
    int v, p, b, d;
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/idVendor", target);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    if (sscanf(buf, "%x", &v) != 1)
        return -1;

    snprintf(path, sizeof(path), "%s/idProduct", target);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    if (sscanf(buf, "%x", &p) != 1)
        return -1;

    snprintf(path, sizeof(path), "%s/busnum", target);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    if (sscanf(buf, "%d", &b) != 1)
        return -1;

    snprintf(path, sizeof(path), "%s/devnum", target);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    if (sscanf(buf, "%d", &d) != 1)
        return -1;

    *vid = (uint16_t)v;
    *pid = (uint16_t)p;
    *bus = (uint8_t)b;
    *addr = (uint8_t)d;
    return 0;
}

// Open the evdev, classify it (keyboard / mouse / gamepad / other), and read
// its name. Returns 0 on success.
static int classify_evdev(const char *path, char *name, int name_len, int *usage_kind) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        ZF_LOGD("usb_caps: cannot open '%s': %s", path, strerror(errno));
        return -1;
    }

    if (ioctl(fd, EVIOCGNAME(name_len), name) < 0)
        name[0] = '\0';

    uint8_t key_bits[(KEY_MAX / 8) + 1] = {0};
    uint8_t rel_bits[(REL_MAX / 8) + 1] = {0};
    uint8_t abs_bits[(ABS_MAX / 8) + 1] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);

    int has_alpha = BIT(key_bits, KEY_A);
    int has_btn_left = BIT(key_bits, BTN_LEFT);
    int has_rel_pointer = BIT(rel_bits, REL_X) && BIT(rel_bits, REL_Y);
    int has_abs_pointer = (BIT(abs_bits, ABS_X) && BIT(abs_bits, ABS_Y)) ||
                          (BIT(abs_bits, ABS_MT_POSITION_X) && BIT(abs_bits, ABS_MT_POSITION_Y));
    int has_touch_hint = BIT(key_bits, BTN_TOUCH) || BIT(key_bits, BTN_TOOL_FINGER) ||
                         BIT(key_bits, BTN_TOOL_DOUBLETAP) || BIT(key_bits, BTN_TOOL_TRIPLETAP);
    int has_btn_joy = BIT(key_bits, BTN_JOYSTICK) || BIT(key_bits, BTN_GAMEPAD);

    int kind = 0;
    if (has_alpha)
        kind |= OVL_USB_KIND_KBD;
    if ((has_rel_pointer && has_btn_left) || (has_abs_pointer && (has_touch_hint || has_btn_left)))
        kind |= OVL_USB_KIND_MOUSE;
    // Gamepads are mutually exclusive with kbd/mouse — only set if neither
    // matched, to avoid mislabelling devices that happen to expose BTN_*.
    if (kind == 0 && has_btn_joy)
        kind = OVL_USB_KIND_GAMEPAD;
    *usage_kind = kind;

    close(fd);
    return 0;
}

int ovl_usb_enum_hid_devices(struct ovl_usb_hid_info *entries, int max_entries) {
    DIR *d = opendir("/sys/class/input");
    if (!d)
        return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        int idx;
        if (sscanf(ent->d_name, "event%d", &idx) != 1)
            continue;

        struct ovl_usb_hid_info *info = &entries[count];
        memset(info, 0, sizeof(*info));

        if (read_parent_usb(idx, &info->vid, &info->pid, &info->bus, &info->address) < 0)
            continue; // not a USB-backed input device

        info->input_index = idx;
        snprintf(info->evdev_path, sizeof(info->evdev_path), "/dev/input/event%d", idx);

        if (classify_evdev(info->evdev_path, info->name, sizeof(info->name), &info->usage_kind) < 0)
            continue;

        if (!info->name[0])
            snprintf(info->name, sizeof(info->name), "%04x:%04x", info->vid, info->pid);

        count++;
    }
    closedir(d);
    return count;
}

int ovl_usb_find_devices(const char **vid_pids, int num_vid_pids, struct ovl_usb_hid_info *matched,
                         int max_matched) {
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

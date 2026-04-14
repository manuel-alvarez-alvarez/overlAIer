#include "usb_proxy.h"
#include "usb_caps.h"
#include "gadget_configfs.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/log.h"
#include "../processor/processor_mgr.h"

#define MAX_PROXY_DEVICES 16
#define MAX_REPORT_SIZE   4096

struct proxy_device {
    struct ovl_usb_hid_info info;
    int hidraw_fd;      // /dev/hidrawN
    int hidg_fd;        // /dev/hidgN
    int evdev_fd;       // evdev for EVIOCGRAB
    int index;          // gadget function index
    pthread_t thread;
    volatile int running;
    struct ovl_processor_mgr *proc_mgr;
};

struct ovl_usb_proxy {
    struct proxy_device devices[MAX_PROXY_DEVICES];
    int num_devices;
    volatile int active;
};

// Find the evdev device corresponding to a hidraw device for grab
static int find_evdev_fd(const char *hidraw_name) {
    char input_dir[256];
    snprintf(input_dir, sizeof(input_dir),
             "/sys/class/hidraw/%s/device/input", hidraw_name);

    DIR *d = opendir(input_dir);
    if (!d)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0)
            continue;

        // Look for eventN inside inputN
        char event_dir[512];
        snprintf(event_dir, sizeof(event_dir), "%s/%s", input_dir, ent->d_name);
        DIR *ed = opendir(event_dir);
        if (!ed)
            continue;

        struct dirent *ev;
        while ((ev = readdir(ed)) != NULL) {
            if (strncmp(ev->d_name, "event", 5) != 0)
                continue;
            char dev_path[256];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", ev->d_name);
            closedir(ed);
            closedir(d);
            int fd = open(dev_path, O_RDONLY);
            if (fd >= 0) {
                ZF_LOGD("usb_proxy: found evdev %s for %s", dev_path, hidraw_name);
            }
            return fd;
        }
        closedir(ed);
    }

    closedir(d);
    return -1;
}

static void *proxy_thread_fn(void *arg) {
    struct proxy_device *dev = arg;
    uint8_t report[MAX_REPORT_SIZE];

    ZF_LOGI("usb_proxy: proxying '%s' (%04x:%04x) hidraw%d -> hidg%d",
            dev->info.name, dev->info.vid, dev->info.pid,
            dev->index, dev->index);

    while (dev->running) {
        ssize_t n = read(dev->hidraw_fd, report, sizeof(report));
        if (n <= 0) {
            if (n == 0 || errno == ENODEV) {
                ZF_LOGW("usb_proxy: device '%s' disconnected", dev->info.name);
                break;
            }
            if (errno == EINTR)
                continue;
            ZF_LOGE("usb_proxy: read error on '%s': %s", dev->info.name, strerror(errno));
            break;
        }

        int len = (int)n;

        // Run through processor chain
        if (dev->proc_mgr) {
            if (ovl_processor_mgr_process_hid(dev->proc_mgr, report, &len,
                                               dev->info.name, dev->info.vid,
                                               dev->info.pid) < 0)
                continue; // dropped by processor
        }

        // Forward to gadget
        ssize_t w = write(dev->hidg_fd, report, (size_t)len);
        if (w < 0 && errno != EINTR) {
            ZF_LOGE("usb_proxy: write error on hidg%d: %s", dev->index, strerror(errno));
            break;
        }
    }

    dev->running = 0;
    return NULL;
}

int ovl_usb_proxy_init(struct ovl_usb_proxy **out,
                       const char *udc,
                       const char **vid_pids, int num_vid_pids,
                       struct ovl_processor_mgr *proc_mgr) {
    if (num_vid_pids == 0)
        return 0; // nothing to proxy

    // Find matching devices
    struct ovl_usb_hid_info matched[MAX_PROXY_DEVICES];
    int num_matched = ovl_usb_find_devices(vid_pids, num_vid_pids, matched, MAX_PROXY_DEVICES);
    if (num_matched == 0) {
        ZF_LOGW("usb_proxy: no matching HID devices found");
        return -1;
    }

    // Create ConfigFS gadget
    if (ovl_gadget_create(udc) < 0)
        return -1;

    struct ovl_usb_proxy *proxy = calloc(1, sizeof(*proxy));
    if (!proxy) {
        ovl_gadget_destroy();
        return -1;
    }

    // Set up each device
    for (int i = 0; i < num_matched; i++) {
        struct proxy_device *dev = &proxy->devices[proxy->num_devices];
        dev->info = matched[i];
        dev->index = i;
        dev->proc_mgr = proc_mgr;
        dev->hidraw_fd = -1;
        dev->hidg_fd = -1;
        dev->evdev_fd = -1;

        // Add HID function to gadget
        if (ovl_gadget_add_hid(i, dev->info.report_desc, dev->info.report_desc_len,
                               dev->info.report_len, dev->info.protocol) < 0) {
            ZF_LOGE("usb_proxy: failed to add gadget function for '%s'", dev->info.name);
            continue;
        }

        // Open hidraw for reading
        char hidraw_path[64];
        snprintf(hidraw_path, sizeof(hidraw_path), "/dev/%s", dev->info.hidraw);
        dev->hidraw_fd = open(hidraw_path, O_RDONLY);
        if (dev->hidraw_fd < 0) {
            ZF_LOGE("usb_proxy: cannot open %s: %s", hidraw_path, strerror(errno));
            continue;
        }

        // Grab evdev exclusively
        dev->evdev_fd = find_evdev_fd(dev->info.hidraw);
        if (dev->evdev_fd >= 0) {
            if (ioctl(dev->evdev_fd, EVIOCGRAB, 1) < 0) {
                ZF_LOGW("usb_proxy: EVIOCGRAB failed for '%s': %s",
                        dev->info.name, strerror(errno));
            }
        }

        proxy->num_devices++;
        ZF_LOGI("usb_proxy: configured '%s' (%04x:%04x) via %s",
                dev->info.name, dev->info.vid, dev->info.pid, dev->info.hidraw);
    }

    if (proxy->num_devices == 0) {
        ZF_LOGE("usb_proxy: no devices could be set up");
        free(proxy);
        ovl_gadget_destroy();
        return -1;
    }

    // Enable the gadget
    if (ovl_gadget_enable() < 0) {
        // Clean up opened fds
        for (int i = 0; i < proxy->num_devices; i++) {
            if (proxy->devices[i].hidraw_fd >= 0)
                close(proxy->devices[i].hidraw_fd);
            if (proxy->devices[i].evdev_fd >= 0)
                close(proxy->devices[i].evdev_fd);
        }
        free(proxy);
        ovl_gadget_destroy();
        return -1;
    }

    // Small delay for /dev/hidgN to appear
    usleep(100000);

    // Open gadget HID devices for writing
    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        char hidg_path[64];
        snprintf(hidg_path, sizeof(hidg_path), "/dev/hidg%d", dev->index);
        dev->hidg_fd = open(hidg_path, O_WRONLY);
        if (dev->hidg_fd < 0) {
            ZF_LOGE("usb_proxy: cannot open %s: %s", hidg_path, strerror(errno));
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
        if (dev->hidraw_fd < 0 || dev->hidg_fd < 0)
            continue;
        dev->running = 1;
        pthread_create(&dev->thread, NULL, proxy_thread_fn, dev);
    }
    return 0;
}

void ovl_usb_proxy_stop(struct ovl_usb_proxy *proxy) {
    if (!proxy)
        return;
    proxy->active = 0;

    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        dev->running = 0;
        // Close hidraw to unblock the read() in the proxy thread
        if (dev->hidraw_fd >= 0) {
            close(dev->hidraw_fd);
            dev->hidraw_fd = -1;
        }
    }

    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->running == 0 && dev->thread)
            pthread_join(dev->thread, NULL);
    }
}

void ovl_usb_proxy_destroy(struct ovl_usb_proxy *proxy) {
    if (!proxy)
        return;

    ovl_usb_proxy_stop(proxy);

    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        // Release evdev grab
        if (dev->evdev_fd >= 0) {
            ioctl(dev->evdev_fd, EVIOCGRAB, 0);
            close(dev->evdev_fd);
        }
        if (dev->hidg_fd >= 0)
            close(dev->hidg_fd);
        // hidraw_fd already closed in stop()
    }

    ovl_gadget_destroy();
    free(proxy);
}

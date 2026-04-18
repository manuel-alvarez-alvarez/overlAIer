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
#include <libusb-1.0/libusb.h>

#include "../common/log.h"
#include "../processor/processor_mgr.h"

#define MAX_PROXY_DEVICES 16
#define MAX_REPORT_SIZE   64

struct proxy_device {
    struct ovl_usb_hid_info info;
    libusb_device_handle *handle;
    int hidg_fd;            // /dev/hidgN (gadget output)
    int index;              // gadget function index
    pthread_t thread;
    volatile int running;
    struct ovl_processor_mgr *proc_mgr;
};

struct ovl_usb_proxy {
    struct proxy_device devices[MAX_PROXY_DEVICES];
    int num_devices;
    volatile int active;
};

static void *proxy_thread_fn(void *arg) {
    struct proxy_device *dev = arg;
    uint8_t report[MAX_REPORT_SIZE];

    ZF_LOGI("usb_proxy: proxying '%s' (%04x:%04x) iface %d -> hidg%d",
            dev->info.name, dev->info.vid, dev->info.pid,
            dev->info.interface_number, dev->index);

    while (dev->running) {
        int transferred = 0;
        int rc = libusb_interrupt_transfer(dev->handle, dev->info.ep_in,
                                           report, sizeof(report),
                                           &transferred, 100); // 100ms timeout
        if (rc == LIBUSB_ERROR_TIMEOUT)
            continue;
        if (rc < 0) {
            if (rc == LIBUSB_ERROR_NO_DEVICE) {
                ZF_LOGW("usb_proxy: device '%s' disconnected", dev->info.name);
                break;
            }
            if (rc == LIBUSB_ERROR_INTERRUPTED)
                continue;
            ZF_LOGE("usb_proxy: read error on '%s': %s",
                    dev->info.name, libusb_error_name(rc));
            break;
        }
        if (transferred == 0)
            continue;

        int len = transferred;

        // Run through processor chain
        if (dev->proc_mgr) {
            if (ovl_processor_mgr_process_hid(dev->proc_mgr, report, &len,
                                               dev->info.name, dev->info.vid,
                                               dev->info.pid) < 0)
                continue; // dropped by processor
        }

        // Forward to gadget (ignore ESHUTDOWN when OTG host not connected)
        ssize_t w = write(dev->hidg_fd, report, (size_t)len);
        if (w < 0 && errno != EINTR && errno != ESHUTDOWN)
            ZF_LOGD("usb_proxy: write error on hidg%d: %s", dev->index, strerror(errno));
    }

    dev->running = 0;
    return NULL;
}

// Find a libusb device matching bus:address
static libusb_device *find_usb_device(uint8_t bus, uint8_t address) {
    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(NULL, &devs);
    if (cnt < 0)
        return NULL;

    libusb_device *found = NULL;
    for (ssize_t i = 0; i < cnt; i++) {
        if (libusb_get_bus_number(devs[i]) == bus &&
            libusb_get_device_address(devs[i]) == address) {
            found = libusb_ref_device(devs[i]);
            break;
        }
    }
    libusb_free_device_list(devs, 1);
    return found;
}

int ovl_usb_proxy_init(struct ovl_usb_proxy **out,
                       const char *udc,
                       const char **vid_pids, int num_vid_pids,
                       struct ovl_processor_mgr *proc_mgr) {
    if (num_vid_pids == 0)
        return 0;

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
        dev->handle = NULL;
        dev->hidg_fd = -1;

        // Add HID function to gadget
        if (dev->info.report_desc_len <= 0) {
            ZF_LOGE("usb_proxy: no report descriptor for '%s' iface %d",
                    dev->info.name, dev->info.interface_number);
            continue;
        }
        if (ovl_gadget_add_hid(i, dev->info.report_desc, dev->info.report_desc_len,
                               MAX_REPORT_SIZE, dev->info.protocol) < 0) {
            ZF_LOGE("usb_proxy: failed to add gadget function for '%s'", dev->info.name);
            continue;
        }

        // Open the USB device and claim the interface
        libusb_device *usb_dev = find_usb_device(dev->info.bus, dev->info.address);
        if (!usb_dev) {
            ZF_LOGE("usb_proxy: cannot find USB device %d:%d",
                    dev->info.bus, dev->info.address);
            continue;
        }

        int rc = libusb_open(usb_dev, &dev->handle);
        libusb_unref_device(usb_dev);
        if (rc < 0) {
            ZF_LOGE("usb_proxy: cannot open '%s': %s",
                    dev->info.name, libusb_error_name(rc));
            continue;
        }

        // Detach kernel driver (replaces EVIOCGRAB — exclusive access)
        if (libusb_kernel_driver_active(dev->handle, dev->info.interface_number) == 1) {
            libusb_detach_kernel_driver(dev->handle, dev->info.interface_number);
        }

        rc = libusb_claim_interface(dev->handle, dev->info.interface_number);
        if (rc < 0) {
            ZF_LOGE("usb_proxy: cannot claim interface %d on '%s': %s",
                    dev->info.interface_number, dev->info.name, libusb_error_name(rc));
            libusb_close(dev->handle);
            dev->handle = NULL;
            continue;
        }

        proxy->num_devices++;
        ZF_LOGI("usb_proxy: configured '%s' (%04x:%04x) bus %d addr %d iface %d",
                dev->info.name, dev->info.vid, dev->info.pid,
                dev->info.bus, dev->info.address, dev->info.interface_number);
    }

    if (proxy->num_devices == 0) {
        ZF_LOGE("usb_proxy: no devices could be set up");
        free(proxy);
        ovl_gadget_destroy();
        return -1;
    }

    // Enable the gadget
    if (ovl_gadget_enable() < 0) {
        for (int i = 0; i < proxy->num_devices; i++) {
            if (proxy->devices[i].handle) {
                libusb_release_interface(proxy->devices[i].handle,
                                         proxy->devices[i].info.interface_number);
                libusb_close(proxy->devices[i].handle);
            }
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
        if (dev->hidg_fd < 0)
            ZF_LOGE("usb_proxy: cannot open %s: %s", hidg_path, strerror(errno));
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
        if (!dev->handle || dev->hidg_fd < 0)
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

    for (int i = 0; i < proxy->num_devices; i++)
        proxy->devices[i].running = 0;

    // Threads will exit on next libusb_interrupt_transfer timeout
    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->thread)
            pthread_join(dev->thread, NULL);
    }
}

void ovl_usb_proxy_destroy(struct ovl_usb_proxy *proxy) {
    if (!proxy)
        return;

    ovl_usb_proxy_stop(proxy);

    for (int i = 0; i < proxy->num_devices; i++) {
        struct proxy_device *dev = &proxy->devices[i];
        if (dev->handle) {
            libusb_release_interface(dev->handle, dev->info.interface_number);
            libusb_attach_kernel_driver(dev->handle, dev->info.interface_number);
            libusb_close(dev->handle);
        }
        if (dev->hidg_fd >= 0)
            close(dev->hidg_fd);
    }

    ovl_gadget_destroy();
    free(proxy);
}

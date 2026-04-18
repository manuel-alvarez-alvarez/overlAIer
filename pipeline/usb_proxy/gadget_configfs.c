#include "gadget_configfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/log.h"

#define GADGET_BASE "/sys/kernel/config/usb_gadget"
#define GADGET_NAME "overlAIer"
#define GADGET_PATH GADGET_BASE "/" GADGET_NAME

static char saved_udc[128];
static int num_hid_functions;

static int write_file(const char *path, const void *data, int len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        ZF_LOGE("gadget: cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, data, (size_t)len);
    close(fd);
    if (w < 0) {
        ZF_LOGE("gadget: write to '%s' failed: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_string(const char *path, const char *str) {
    return write_file(path, str, (int)strlen(str));
}

static int detect_udc(char *buf, int len) {
    DIR *d = opendir("/sys/class/udc");
    if (!d)
        return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        snprintf(buf, (size_t)len, "%s", ent->d_name);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

int ovl_gadget_create(const char *udc) {
    if (udc && udc[0]) {
        snprintf(saved_udc, sizeof(saved_udc), "%s", udc);
    } else {
        if (detect_udc(saved_udc, sizeof(saved_udc)) < 0) {
            ZF_LOGE("gadget: no UDC controller found");
            return -1;
        }
    }

    ZF_LOGD("gadget: using UDC %s", saved_udc);

    // Clean up stale gadget from a previous run
    struct stat st;
    if (stat(GADGET_PATH, &st) == 0) {
        ZF_LOGW("gadget: cleaning up stale gadget at %s", GADGET_PATH);
        ovl_gadget_destroy();
    }

    if (mkdir(GADGET_PATH, 0755) < 0 && errno != EEXIST) {
        ZF_LOGE("gadget: mkdir '%s': %s", GADGET_PATH, strerror(errno));
        return -1;
    }

    // Set IDs (Linux Foundation, composite device)
    write_string(GADGET_PATH "/idVendor", "0x1d6b");
    write_string(GADGET_PATH "/idProduct", "0x0104");
    write_string(GADGET_PATH "/bcdUSB", "0x0200");

    // Strings
    if (mkdir(GADGET_PATH "/strings/0x409", 0755) < 0 && errno != EEXIST)
        return -1;
    write_string(GADGET_PATH "/strings/0x409/manufacturer", "overlAIer");
    write_string(GADGET_PATH "/strings/0x409/product", "HID Proxy");

    // Configuration
    if (mkdir(GADGET_PATH "/configs/c.1", 0755) < 0 && errno != EEXIST)
        return -1;
    if (mkdir(GADGET_PATH "/configs/c.1/strings/0x409", 0755) < 0 && errno != EEXIST)
        return -1;
    write_string(GADGET_PATH "/configs/c.1/strings/0x409/configuration", "HID");
    write_string(GADGET_PATH "/configs/c.1/MaxPower", "100");

    num_hid_functions = 0;
    return 0;
}

int ovl_gadget_add_hid(int index, const void *report_desc, int desc_len,
                       int report_len, int protocol) {
    char func_path[256];
    snprintf(func_path, sizeof(func_path), GADGET_PATH "/functions/hid.usb%d", index);

    if (mkdir(func_path, 0755) < 0 && errno != EEXIST) {
        ZF_LOGE("gadget: mkdir '%s': %s", func_path, strerror(errno));
        return -1;
    }

    char path[512];

    snprintf(path, sizeof(path), "%s/protocol", func_path);
    char proto_str[8];
    snprintf(proto_str, sizeof(proto_str), "%d", protocol);
    write_string(path, proto_str);

    snprintf(path, sizeof(path), "%s/subclass", func_path);
    write_string(path, protocol > 0 ? "1" : "0"); // boot interface for kb/mouse

    snprintf(path, sizeof(path), "%s/report_length", func_path);
    char rlen_str[16];
    snprintf(rlen_str, sizeof(rlen_str), "%d", report_len);
    write_string(path, rlen_str);

    snprintf(path, sizeof(path), "%s/report_desc", func_path);
    if (write_file(path, report_desc, desc_len) < 0)
        return -1;

    // Link function to configuration
    char link_path[512];
    snprintf(link_path, sizeof(link_path), GADGET_PATH "/configs/c.1/hid.usb%d", index);
    // Remove stale symlink if present
    unlink(link_path);
    if (symlink(func_path, link_path) < 0) {
        ZF_LOGE("gadget: symlink '%s' -> '%s': %s", link_path, func_path, strerror(errno));
        return -1;
    }

    num_hid_functions++;
    ZF_LOGD("gadget: added hid.usb%d (protocol=%d, desc=%d bytes, report=%d bytes)",
            index, protocol, desc_len, report_len);
    return 0;
}

int ovl_gadget_enable(void) {
    if (write_string(GADGET_PATH "/UDC", saved_udc) < 0) {
        ZF_LOGE("gadget: failed to enable UDC %s", saved_udc);
        return -1;
    }
    ZF_LOGI("gadget: enabled on UDC %s with %d HID function(s)", saved_udc, num_hid_functions);
    return 0;
}

void ovl_gadget_destroy(void) {
    // Disable
    write_string(GADGET_PATH "/UDC", "");

    // Unlink and remove all hid.usb* functions (scan instead of relying on counter)
    for (int i = 0; i < 32; i++) {
        char link[256], func[256];
        snprintf(link, sizeof(link), GADGET_PATH "/configs/c.1/hid.usb%d", i);
        snprintf(func, sizeof(func), GADGET_PATH "/functions/hid.usb%d", i);
        unlink(link);
        rmdir(func);
    }

    rmdir(GADGET_PATH "/configs/c.1/strings/0x409");
    rmdir(GADGET_PATH "/configs/c.1");
    rmdir(GADGET_PATH "/strings/0x409");
    rmdir(GADGET_PATH);
    num_hid_functions = 0;
    ZF_LOGD("gadget: destroyed");
}

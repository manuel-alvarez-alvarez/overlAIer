#ifndef OVERLAIER_GADGET_CONFIGFS_H
#define OVERLAIER_GADGET_CONFIGFS_H

#include <stdint.h>

// Create a USB gadget via ConfigFS. udc may be NULL to auto-detect.
int ovl_gadget_create(const char *udc);

// Add a HID function to the gadget.
// index: function index (0, 1, 2, ...)
// report_desc: raw HID report descriptor bytes
// desc_len: length of report_desc
// report_len: max report length (used for the IN endpoint MaxPacketSize)
// subclass, protocol: copied verbatim from the source device's
//   bInterfaceSubClass / bInterfaceProtocol so the host sees the same
//   interface class triple as the original device.
// Returns 0 on success, -1 on error.
int ovl_gadget_add_hid(int index, const void *report_desc, int desc_len, int report_len,
                       int subclass, int protocol);

// Enable the gadget (write UDC name).
int ovl_gadget_enable(void);

// Read the /dev/hidgN minor allocated by the kernel for the given function
// index. Only valid after ovl_gadget_enable() succeeds. Returns -1 on error.
int ovl_gadget_get_hid_minor(int index);

// Tear down the gadget (disable, remove functions, remove gadget).
void ovl_gadget_destroy(void);

#endif

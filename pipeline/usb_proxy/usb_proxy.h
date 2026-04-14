#ifndef OVERLAIER_USB_PROXY_H
#define OVERLAIER_USB_PROXY_H

struct ovl_usb_proxy;
struct ovl_processor_mgr;

// Initialize USB HID proxy.
// udc: UDC controller name (NULL = auto-detect)
// vid_pids: array of "VVVV:PPPP" strings identifying devices to proxy
// num_devices: number of entries in vid_pids
// proc_mgr: processor manager for HID report chain (may be NULL)
int ovl_usb_proxy_init(struct ovl_usb_proxy **out,
                       const char *udc,
                       const char **vid_pids, int num_devices,
                       struct ovl_processor_mgr *proc_mgr);

// Start proxy threads for all matched devices.
int ovl_usb_proxy_start(struct ovl_usb_proxy *proxy);

// Stop all proxy threads.
void ovl_usb_proxy_stop(struct ovl_usb_proxy *proxy);

// Destroy proxy and release all resources (ConfigFS gadget, grabs, etc).
void ovl_usb_proxy_destroy(struct ovl_usb_proxy *proxy);

#endif

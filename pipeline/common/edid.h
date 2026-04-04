#ifndef OVERLAIER_EDID_H
#define OVERLAIER_EDID_H

#include <stddef.h>
#include <stdint.h>

#define OVL_EDID_BLOCK_SIZE 128
#define OVL_EDID_MAX_BLOCKS 4
#define OVL_EDID_MAX_SIZE   (OVL_EDID_BLOCK_SIZE * OVL_EDID_MAX_BLOCKS)
#define OVL_EDID_MAX_NAME   13 // max chars in EDID monitor name descriptor

// Read EDID from a DRM connector.
// Returns number of bytes read, or -1 on error.
int ovl_edid_read_drm(const char *drm_device, const char *connector_name, uint8_t *edid,
                      size_t max_len);

// Extract monitor name from EDID (descriptor tag 0xFC).
int ovl_edid_get_name(const uint8_t *edid, size_t len, char *name, size_t name_len);

// Build a passthrough EDID: copies manufacturer/serial from src_edid,
// sets monitor name, and advertises a single mode.
// Uses CVT-RBv2 (via libdisplay-info) for timing computation.
// Returns total EDID size (base + CEA extension = 256 bytes), or -1 on error.
int ovl_edid_build_passthrough(const uint8_t *src_edid, size_t src_len, const char *monitor_name,
                               uint32_t width, uint32_t height, uint32_t fps, uint8_t *dst_edid,
                               size_t dst_max);

// Write EDID to a V4L2 capture device (VIDIOC_S_EDID).
int ovl_edid_write_v4l2(const char *v4l2_device, const uint8_t *edid, size_t len);

#endif

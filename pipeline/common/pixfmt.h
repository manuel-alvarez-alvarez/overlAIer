#ifndef OVERLAIER_PIXFMT_H
#define OVERLAIER_PIXFMT_H

#include <stdint.h>
#include <stdio.h>

// Internal pixel format enum. All modules use this; conversion to/from
// V4L2 and DRM fourccs happens only at system boundaries.
//
// Names describe memory byte order (like DRM, not V4L2):
//   RGB888 = bytes R,G,B in memory
//   BGR888 = bytes B,G,R in memory

enum ovl_pixfmt {
    OVL_PIXFMT_UNKNOWN = 0,

    // RGB packed
    OVL_PIXFMT_RGB888,      // 3 bytes: R, G, B
    OVL_PIXFMT_BGR888,      // 3 bytes: B, G, R
    OVL_PIXFMT_XRGB8888,    // 4 bytes: B, G, R, X  (little-endian: 0x00RRGGBB)
    OVL_PIXFMT_ARGB8888,    // 4 bytes: B, G, R, A
    OVL_PIXFMT_XBGR8888,    // 4 bytes: R, G, B, X
    OVL_PIXFMT_ABGR8888,    // 4 bytes: R, G, B, A
    OVL_PIXFMT_RGB565,      // 2 bytes: 5-6-5
    OVL_PIXFMT_XRGB2101010, // 10-bit per channel
    OVL_PIXFMT_XBGR2101010,

    // YUV semi-planar (NV family)
    OVL_PIXFMT_NV12, // 4:2:0, Y + interleaved CbCr
    OVL_PIXFMT_NV21, // 4:2:0, Y + interleaved CrCb
    OVL_PIXFMT_NV16, // 4:2:2, Y + interleaved CbCr
    OVL_PIXFMT_NV61, // 4:2:2, Y + interleaved CrCb
    OVL_PIXFMT_NV24, // 4:4:4, Y + interleaved CbCr
    OVL_PIXFMT_NV42, // 4:4:4, Y + interleaved CrCb

    // YUV packed
    OVL_PIXFMT_YUYV, // 4:2:2 packed: Y0 Cb Y1 Cr
    OVL_PIXFMT_UYVY, // 4:2:2 packed: Cb Y0 Cr Y1
    OVL_PIXFMT_YVYU,
    OVL_PIXFMT_VYUY,

    OVL_PIXFMT_COUNT,
};

// Convert between external fourcc codes and internal format.
enum ovl_pixfmt ovl_pixfmt_from_v4l2(uint32_t v4l2_fourcc);
enum ovl_pixfmt ovl_pixfmt_from_drm(uint32_t drm_fourcc);
uint32_t ovl_pixfmt_to_v4l2(enum ovl_pixfmt fmt);
uint32_t ovl_pixfmt_to_drm(enum ovl_pixfmt fmt);

// Parse from a user-provided string (accepts both V4L2 and DRM fourccs).
// Returns OVL_PIXFMT_UNKNOWN if not recognized.
enum ovl_pixfmt ovl_pixfmt_from_str(const char *s);

// Short name for display (e.g. "NV24", "RGB888", "XRGB8888").
const char *ovl_pixfmt_name(enum ovl_pixfmt fmt);

// Print the short name to a file.
void ovl_pixfmt_fprint(FILE *f, enum ovl_pixfmt fmt);

// Bytes per pixel (for packed formats). Returns 0 for planar.
int ovl_pixfmt_bpp(enum ovl_pixfmt fmt);

// Number of planes for DRM framebuffer creation.
int ovl_pixfmt_num_planes(enum ovl_pixfmt fmt);

// Keep old helpers available during transition
#define OVL_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

uint32_t ovl_v4l2_to_drm(uint32_t v4l2_fmt);
uint32_t ovl_drm_to_v4l2(uint32_t drm_fmt);
void ovl_fprint_fourcc(FILE *f, uint32_t fourcc);

#endif

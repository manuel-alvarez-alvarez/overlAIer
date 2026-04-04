#include "pixfmt.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Master format table: maps internal enum ↔ V4L2 fourcc ↔ DRM fourcc ↔ name.
//
// IMPORTANT: V4L2 and DRM use *inverted naming* for 24-bit RGB:
//   V4L2 "BGR24" ('BGR3') = memory R,G,B = DRM "RGB888" ('RG24')
//   V4L2 "RGB24" ('RGB3') = memory B,G,R = DRM "BGR888" ('BG24')
//
// Our internal names describe memory byte order (like DRM).

static const struct {
    enum ovl_pixfmt id;
    uint32_t v4l2;
    uint32_t drm;
    const char *name;
    int bpp;    // bytes per pixel (0 = planar)
    int planes; // DRM plane count
} fmt_table[] = {
    // RGB packed
    // V4L2 and DRM use INVERTED naming for 24-bit RGB:
    //   V4L2 "BGR24" ('BGR3') = DRM "RGB888" ('RG24') = memory R,G,B
    //   V4L2 "RGB24" ('RGB3') = DRM "BGR888" ('BG24') = memory B,G,R
    // Confirmed by direct path testing on rk_hdmirx + Rockchip VOP.
    {OVL_PIXFMT_RGB888, OVL_FOURCC('B', 'G', 'R', '3'), OVL_FOURCC('R', 'G', '2', '4'), "RGB888", 3,
     1},
    {OVL_PIXFMT_BGR888, OVL_FOURCC('R', 'G', 'B', '3'), OVL_FOURCC('B', 'G', '2', '4'), "BGR888", 3,
     1},
    {OVL_PIXFMT_XRGB8888, OVL_FOURCC('X', 'R', '2', '4'), OVL_FOURCC('X', 'R', '2', '4'),
     "XRGB8888", 4, 1},
    {OVL_PIXFMT_ARGB8888, OVL_FOURCC('A', 'R', '2', '4'), OVL_FOURCC('A', 'R', '2', '4'),
     "ARGB8888", 4, 1},
    {OVL_PIXFMT_XBGR8888, OVL_FOURCC('X', 'B', '2', '4'), OVL_FOURCC('X', 'B', '2', '4'),
     "XBGR8888", 4, 1},
    {OVL_PIXFMT_ABGR8888, OVL_FOURCC('A', 'B', '2', '4'), OVL_FOURCC('A', 'B', '2', '4'),
     "ABGR8888", 4, 1},
    {OVL_PIXFMT_RGB565, OVL_FOURCC('R', 'G', 'B', 'P'), OVL_FOURCC('R', 'G', '1', '6'), "RGB565", 2,
     1},
    {OVL_PIXFMT_XRGB2101010, OVL_FOURCC('X', 'R', '3', '0'), OVL_FOURCC('X', 'R', '3', '0'),
     "XRGB2101010", 4, 1},
    {OVL_PIXFMT_XBGR2101010, OVL_FOURCC('X', 'B', '3', '0'), OVL_FOURCC('X', 'B', '3', '0'),
     "XBGR2101010", 4, 1},

    // YUV semi-planar
    {OVL_PIXFMT_NV12, OVL_FOURCC('N', 'V', '1', '2'), OVL_FOURCC('N', 'V', '1', '2'), "NV12", 0, 2},
    {OVL_PIXFMT_NV21, OVL_FOURCC('N', 'V', '2', '1'), OVL_FOURCC('N', 'V', '2', '1'), "NV21", 0, 2},
    {OVL_PIXFMT_NV16, OVL_FOURCC('N', 'V', '1', '6'), OVL_FOURCC('N', 'V', '1', '6'), "NV16", 0, 2},
    {OVL_PIXFMT_NV61, OVL_FOURCC('N', 'V', '6', '1'), OVL_FOURCC('N', 'V', '6', '1'), "NV61", 0, 2},
    {OVL_PIXFMT_NV24, OVL_FOURCC('N', 'V', '2', '4'), OVL_FOURCC('N', 'V', '2', '4'), "NV24", 0, 2},
    {OVL_PIXFMT_NV42, OVL_FOURCC('N', 'V', '4', '2'), OVL_FOURCC('N', 'V', '4', '2'), "NV42", 0, 2},

    // YUV packed
    {OVL_PIXFMT_YUYV, OVL_FOURCC('Y', 'U', 'Y', 'V'), OVL_FOURCC('Y', 'U', 'Y', 'V'), "YUYV", 2, 1},
    {OVL_PIXFMT_UYVY, OVL_FOURCC('U', 'Y', 'V', 'Y'), OVL_FOURCC('U', 'Y', 'V', 'Y'), "UYVY", 2, 1},
    {OVL_PIXFMT_YVYU, OVL_FOURCC('Y', 'V', 'Y', 'U'), OVL_FOURCC('Y', 'V', 'Y', 'U'), "YVYU", 2, 1},
    {OVL_PIXFMT_VYUY, OVL_FOURCC('V', 'Y', 'U', 'Y'), OVL_FOURCC('V', 'Y', 'U', 'Y'), "VYUY", 2, 1},
};

#define TABLE_SIZE (sizeof(fmt_table) / sizeof(fmt_table[0]))

// --- Lookup functions ---

enum ovl_pixfmt ovl_pixfmt_from_v4l2(uint32_t v4l2_fourcc) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].v4l2 == v4l2_fourcc)
            return fmt_table[i].id;
    return OVL_PIXFMT_UNKNOWN;
}

enum ovl_pixfmt ovl_pixfmt_from_drm(uint32_t drm_fourcc) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].drm == drm_fourcc)
            return fmt_table[i].id;
    return OVL_PIXFMT_UNKNOWN;
}

uint32_t ovl_pixfmt_to_v4l2(enum ovl_pixfmt fmt) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].id == fmt)
            return fmt_table[i].v4l2;
    return 0;
}

uint32_t ovl_pixfmt_to_drm(enum ovl_pixfmt fmt) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].id == fmt)
            return fmt_table[i].drm;
    return 0;
}

enum ovl_pixfmt ovl_pixfmt_from_str(const char *s) {
    if (!s)
        return OVL_PIXFMT_UNKNOWN;

    // Try internal name first (e.g. "NV24", "RGB888")
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (strcasecmp(fmt_table[i].name, s) == 0)
            return fmt_table[i].id;

    // Try as a 4-char fourcc (V4L2 or DRM)
    if (strlen(s) == 4) {
        uint32_t fourcc = OVL_FOURCC(s[0], s[1], s[2], s[3]);
        enum ovl_pixfmt f = ovl_pixfmt_from_v4l2(fourcc);
        if (f != OVL_PIXFMT_UNKNOWN)
            return f;
        f = ovl_pixfmt_from_drm(fourcc);
        if (f != OVL_PIXFMT_UNKNOWN)
            return f;
    }

    return OVL_PIXFMT_UNKNOWN;
}

const char *ovl_pixfmt_name(enum ovl_pixfmt fmt) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].id == fmt)
            return fmt_table[i].name;
    return "UNKNOWN";
}

void ovl_pixfmt_fprint(FILE *f, enum ovl_pixfmt fmt) {
    fprintf(f, "%s", ovl_pixfmt_name(fmt));
}

int ovl_pixfmt_bpp(enum ovl_pixfmt fmt) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].id == fmt)
            return fmt_table[i].bpp;
    return 0;
}

int ovl_pixfmt_num_planes(enum ovl_pixfmt fmt) {
    for (size_t i = 0; i < TABLE_SIZE; i++)
        if (fmt_table[i].id == fmt)
            return fmt_table[i].planes;
    return 1;
}

// --- Legacy helpers (kept for files not yet migrated) ---

uint32_t ovl_v4l2_to_drm(uint32_t v4l2_fmt) {
    enum ovl_pixfmt f = ovl_pixfmt_from_v4l2(v4l2_fmt);
    if (f != OVL_PIXFMT_UNKNOWN)
        return ovl_pixfmt_to_drm(f);
    return v4l2_fmt;
}

uint32_t ovl_drm_to_v4l2(uint32_t drm_fmt) {
    enum ovl_pixfmt f = ovl_pixfmt_from_drm(drm_fmt);
    if (f != OVL_PIXFMT_UNKNOWN)
        return ovl_pixfmt_to_v4l2(f);
    return drm_fmt;
}

void ovl_fprint_fourcc(FILE *f, uint32_t fourcc) {
    // Try to resolve to a known name
    enum ovl_pixfmt pf = ovl_pixfmt_from_v4l2(fourcc);
    if (pf == OVL_PIXFMT_UNKNOWN)
        pf = ovl_pixfmt_from_drm(fourcc);
    if (pf != OVL_PIXFMT_UNKNOWN) {
        fprintf(f, "%s", ovl_pixfmt_name(pf));
        return;
    }
    // Unknown: print raw fourcc
    fprintf(f, "%c%c%c%c", (char)(fourcc & 0xFF), (char)((fourcc >> 8) & 0xFF),
            (char)((fourcc >> 16) & 0xFF), (char)((fourcc >> 24) & 0xFF));
}

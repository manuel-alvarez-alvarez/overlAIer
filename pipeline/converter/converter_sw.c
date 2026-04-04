#include "converter_sw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libyuv.h>

#include "../common/log.h"

static const enum ovl_pixfmt sw_inputs[] = {
    OVL_PIXFMT_NV24,     OVL_PIXFMT_NV12,     OVL_PIXFMT_NV21,
    OVL_PIXFMT_RGB888,   OVL_PIXFMT_BGR888,   OVL_PIXFMT_XRGB8888,
    OVL_PIXFMT_ARGB8888, OVL_PIXFMT_XBGR8888, OVL_PIXFMT_ABGR8888,
};

static const enum ovl_pixfmt sw_outputs[] = {
    OVL_PIXFMT_RGB888,   OVL_PIXFMT_BGR888,   OVL_PIXFMT_XRGB8888,
    OVL_PIXFMT_ARGB8888, OVL_PIXFMT_XBGR8888, OVL_PIXFMT_ABGR8888,
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

void ovl_converter_sw_info(struct ovl_converter_backend_info *info) {
    memset(info, 0, sizeof(*info));
    info->name = "software";
    info->type = "software";
    info->supports_csc = 1;

    for (size_t i = 0; i < ARRAY_LEN(sw_inputs) && info->num_input_fmts < OVL_CONV_MAX_FORMATS; i++)
        info->input_fmts[info->num_input_fmts++] = sw_inputs[i];
    for (size_t i = 0; i < ARRAY_LEN(sw_outputs) && info->num_output_fmts < OVL_CONV_MAX_FORMATS;
         i++)
        info->output_fmts[info->num_output_fmts++] = sw_outputs[i];
}

int ovl_converter_sw_supports(enum ovl_pixfmt src, enum ovl_pixfmt dst) {
    int src_ok = 0, dst_ok = 0;
    for (size_t i = 0; i < ARRAY_LEN(sw_inputs); i++)
        if (sw_inputs[i] == src) {
            src_ok = 1;
            break;
        }
    for (size_t i = 0; i < ARRAY_LEN(sw_outputs); i++)
        if (sw_outputs[i] == dst) {
            dst_ok = 1;
            break;
        }
    return (src_ok && dst_ok) ? 0 : -1;
}

// NV24 → ARGB via libyuv: split CrCb → V,U planes (I444), then I444ToARGB.
// rk_hdmirx NV24 stores CrCb (not CbCr), so plane split goes to V, U.
static int convert_nv24_to_argb(const uint8_t *src, uint32_t width, uint32_t height,
                                uint32_t src_pitch, uint8_t *dst_argb, uint32_t dst_pitch) {
    const uint8_t *y_plane = src;
    const uint8_t *vu_plane = src + src_pitch * height; // CrCb interleaved

    // Allocate temporary U and V planes
    size_t plane_size = (size_t)width * height;
    uint8_t *u_plane = malloc(plane_size);
    uint8_t *v_plane = malloc(plane_size);
    if (!u_plane || !v_plane) {
        free(u_plane);
        free(v_plane);
        return -1;
    }

    // Split interleaved UV plane into separate U (Cb) and V (Cr) planes.
    // SplitUVPlane puts first byte → dst_u, second byte → dst_v.
    // Try standard CbCr order first (first byte = Cb = U).
    SplitUVPlane(vu_plane, (int)(src_pitch * 2), u_plane, (int)width, // first byte → U (Cb)
                 v_plane, (int)width,                                 // second byte → V (Cr)
                 (int)width, (int)height);

    // I444ToARGB: Y,U,V separate planes → ARGB (BGRA in memory on LE)
    int ret = I444ToARGB(y_plane, (int)src_pitch, u_plane, (int)width, v_plane, (int)width,
                         dst_argb, (int)dst_pitch, (int)width, (int)height);

    free(u_plane);
    free(v_plane);
    return ret;
}

// Convert ARGB (libyuv's native output) to the target format using libyuv
static int argb_to_target(const uint8_t *argb, uint32_t argb_pitch, uint8_t *dst,
                          uint32_t dst_pitch, uint32_t width, uint32_t height,
                          enum ovl_pixfmt dst_fmt) {
    int w = (int)width, h = (int)height;
    int sp = (int)argb_pitch, dp = (int)dst_pitch;

    switch (dst_fmt) {
    case OVL_PIXFMT_ARGB8888:
    case OVL_PIXFMT_XRGB8888:
        // libyuv I444ToARGB already outputs ARGB (= BGRA in memory on LE = XR24/AR24)
        if (argb != dst)
            memcpy(dst, argb, dst_pitch * height);
        return 0;

    case OVL_PIXFMT_ABGR8888:
    case OVL_PIXFMT_XBGR8888:
        return ARGBToABGR(argb, sp, dst, dp, w, h);

    case OVL_PIXFMT_RGB888:
        // RGB888 = memory R,G,B = DRM RG24
        return ARGBToRGB24(argb, sp, dst, dp, w, h);

    case OVL_PIXFMT_BGR888:
        // BGR888 = memory B,G,R = DRM BG24
        return ARGBToRAW(argb, sp, dst, dp, w, h);

    default:
        return -1;
    }
}

int ovl_converter_sw_process_mem(enum ovl_pixfmt src_fmt, enum ovl_pixfmt dst_fmt, uint32_t width,
                                 uint32_t height, const void *src, uint32_t src_pitch, void *dst,
                                 uint32_t dst_pitch) {
    if (src_fmt == OVL_PIXFMT_NV24) {
        // For ARGB/XRGB output we can convert directly to dst
        if (dst_fmt == OVL_PIXFMT_ARGB8888 || dst_fmt == OVL_PIXFMT_XRGB8888) {
            return convert_nv24_to_argb(src, width, height, src_pitch, dst, dst_pitch);
        }

        // For other formats: NV24 → ARGB (temp) → target
        uint32_t argb_pitch = width * 4;
        uint8_t *argb_tmp = malloc(argb_pitch * height);
        if (!argb_tmp)
            return -1;

        int ret = convert_nv24_to_argb(src, width, height, src_pitch, argb_tmp, argb_pitch);
        if (ret == 0)
            ret = argb_to_target(argb_tmp, argb_pitch, dst, dst_pitch, width, height, dst_fmt);

        free(argb_tmp);
        return ret;
    }

    if (src_fmt == OVL_PIXFMT_NV12) {
        if (dst_fmt == OVL_PIXFMT_ARGB8888 || dst_fmt == OVL_PIXFMT_XRGB8888) {
            return NV12ToARGB(src, (int)src_pitch, (const uint8_t *)src + src_pitch * height,
                              (int)src_pitch, dst, (int)dst_pitch, (int)width, (int)height);
        }
        // NV12 → ARGB → target
        uint32_t argb_pitch = width * 4;
        uint8_t *argb_tmp = malloc(argb_pitch * height);
        if (!argb_tmp)
            return -1;

        int ret = NV12ToARGB(src, (int)src_pitch, (const uint8_t *)src + src_pitch * height,
                             (int)src_pitch, argb_tmp, (int)argb_pitch, (int)width, (int)height);
        if (ret == 0)
            ret = argb_to_target(argb_tmp, argb_pitch, dst, dst_pitch, width, height, dst_fmt);
        free(argb_tmp);
        return ret;
    }

    if (src_fmt == OVL_PIXFMT_NV21) {
        if (dst_fmt == OVL_PIXFMT_ARGB8888 || dst_fmt == OVL_PIXFMT_XRGB8888) {
            return NV21ToARGB(src, (int)src_pitch, (const uint8_t *)src + src_pitch * height,
                              (int)src_pitch, dst, (int)dst_pitch, (int)width, (int)height);
        }
        uint32_t argb_pitch = width * 4;
        uint8_t *argb_tmp = malloc(argb_pitch * height);
        if (!argb_tmp)
            return -1;

        int ret = NV21ToARGB(src, (int)src_pitch, (const uint8_t *)src + src_pitch * height,
                             (int)src_pitch, argb_tmp, (int)argb_pitch, (int)width, (int)height);
        if (ret == 0)
            ret = argb_to_target(argb_tmp, argb_pitch, dst, dst_pitch, width, height, dst_fmt);
        free(argb_tmp);
        return ret;
    }

    // RGB → RGB direct single-pass conversions via libyuv
    int w = (int)width, h = (int)height;
    int sp = (int)src_pitch, dp = (int)dst_pitch;

    // src ARGB/XRGB → any target
    if (src_fmt == OVL_PIXFMT_XRGB8888 || src_fmt == OVL_PIXFMT_ARGB8888)
        return argb_to_target(src, src_pitch, dst, dst_pitch, width, height, dst_fmt);

    // On Rockchip, both V4L2 "BGR3" and DRM "RG24" mean B,G,R bytes in memory.
    // Our OVL_PIXFMT_RGB888 maps to both. For libyuv, B,G,R = "RGB24".
    //
    // OVL_PIXFMT_RGB888 = libyuv "RGB24" (B,G,R bytes)
    // OVL_PIXFMT_BGR888 = libyuv "RAW"   (R,G,B bytes)

    if (src_fmt == OVL_PIXFMT_RGB888) {
        if (dst_fmt == OVL_PIXFMT_XRGB8888 || dst_fmt == OVL_PIXFMT_ARGB8888)
            return RGB24ToARGB(src, sp, dst, dp, w, h);
        if (dst_fmt == OVL_PIXFMT_XBGR8888 || dst_fmt == OVL_PIXFMT_ABGR8888)
            return RGB24ToARGB(src, sp, dst, dp, w, h); // then swap via ABGRToARGB? TODO
    }

    if (src_fmt == OVL_PIXFMT_BGR888) {
        if (dst_fmt == OVL_PIXFMT_XRGB8888 || dst_fmt == OVL_PIXFMT_ARGB8888)
            return RAWToARGB(src, sp, dst, dp, w, h);
    }

    // src ABGR/XBGR
    if (src_fmt == OVL_PIXFMT_XBGR8888 || src_fmt == OVL_PIXFMT_ABGR8888) {
        if (dst_fmt == OVL_PIXFMT_XRGB8888 || dst_fmt == OVL_PIXFMT_ARGB8888)
            return ABGRToARGB(src, sp, dst, dp, w, h);
    }

    ZF_LOGE("unsupported %s -> %s", ovl_pixfmt_name(src_fmt), ovl_pixfmt_name(dst_fmt));
    return -1;
}

#include "converter_rga.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../common/log.h"

// RGA headers — only available on the target
#if __has_include(<im2d.h>)
#include <im2d.h>
#include <rga.h>
#define HAS_RGA 1
#elif __has_include(<rga/im2d.h>)
#include <rga/im2d.h>
#include <rga/rga.h>
#define HAS_RGA 1
#else
#define HAS_RGA 0
#endif

static const enum ovl_pixfmt rga_fmts[] = {
    OVL_PIXFMT_NV12,     OVL_PIXFMT_NV21,     OVL_PIXFMT_NV16,     OVL_PIXFMT_NV61,
    OVL_PIXFMT_YUYV,     OVL_PIXFMT_UYVY,     OVL_PIXFMT_YVYU,     OVL_PIXFMT_VYUY,
    OVL_PIXFMT_RGB888,   OVL_PIXFMT_BGR888,   OVL_PIXFMT_XRGB8888, OVL_PIXFMT_ARGB8888,
    OVL_PIXFMT_XBGR8888, OVL_PIXFMT_ABGR8888, OVL_PIXFMT_RGB565,
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_BUFS     8

// --- Format mapping ---

// Determine if a format is YUV (for CSC mode selection)
static int is_yuv(enum ovl_pixfmt fmt) {
    return fmt == OVL_PIXFMT_NV12 || fmt == OVL_PIXFMT_NV21 || fmt == OVL_PIXFMT_NV16 ||
           fmt == OVL_PIXFMT_NV61 || fmt == OVL_PIXFMT_YUYV || fmt == OVL_PIXFMT_UYVY ||
           fmt == OVL_PIXFMT_YVYU || fmt == OVL_PIXFMT_VYUY;
}

#if HAS_RGA
static int pixfmt_to_rga(enum ovl_pixfmt fmt) {
    switch (fmt) {
    case OVL_PIXFMT_NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case OVL_PIXFMT_NV21:
        return RK_FORMAT_YCrCb_420_SP;
    case OVL_PIXFMT_NV16:
        return RK_FORMAT_YCbCr_422_SP;
    case OVL_PIXFMT_NV61:
        return RK_FORMAT_YCrCb_422_SP;
    case OVL_PIXFMT_YUYV:
        return RK_FORMAT_YUYV_422;
    case OVL_PIXFMT_UYVY:
        return RK_FORMAT_UYVY_422;
    case OVL_PIXFMT_YVYU:
        return RK_FORMAT_YUYV_422; // closest
    case OVL_PIXFMT_VYUY:
        return RK_FORMAT_UYVY_422; // closest
    // On Rockchip, V4L2 "BGR3" / DRM "RG24" = our RGB888 = memory B,G,R.
    // RGA's "BGR_888" means memory B,G,R. So our RGB888 → RK_FORMAT_BGR_888.
    case OVL_PIXFMT_RGB888:
        return RK_FORMAT_BGR_888;
    case OVL_PIXFMT_BGR888:
        return RK_FORMAT_RGB_888;
    case OVL_PIXFMT_XRGB8888:
        return RK_FORMAT_XRGB_8888;
    case OVL_PIXFMT_ARGB8888:
        return RK_FORMAT_ARGB_8888;
    case OVL_PIXFMT_XBGR8888:
        return RK_FORMAT_XBGR_8888;
    case OVL_PIXFMT_ABGR8888:
        return RK_FORMAT_ABGR_8888;
    case OVL_PIXFMT_RGB565:
        return RK_FORMAT_RGB_565;
    default:
        return -1;
    }
}

// --- State ---

static struct {
    int initialized;
    enum ovl_pixfmt src_fmt, dst_fmt;
    int rga_src_fmt, rga_dst_fmt;
    uint32_t width, height;
    int csc_mode;

    rga_buffer_handle_t src_handles[MAX_BUFS];
    rga_buffer_handle_t dst_handles[MAX_BUFS];
    rga_buffer_t src_bufs[MAX_BUFS];
    rga_buffer_t dst_bufs[MAX_BUFS];
    int num_src, num_dst;
} rga_state;

#endif // HAS_RGA

// --- Public API ---

static int rga_available(void) {
    int fd = open("/dev/rga", O_RDWR);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int fmt_supported(enum ovl_pixfmt fmt) {
    for (size_t i = 0; i < ARRAY_LEN(rga_fmts); i++)
        if (rga_fmts[i] == fmt)
            return 1;
    return 0;
}

int ovl_converter_rga_info(struct ovl_converter_backend_info *info) {
    if (!rga_available())
        return -1;

    memset(info, 0, sizeof(*info));
    info->name = "rga";
    info->type = "hardware";
    info->device = "/dev/rga";
    info->supports_scale = 1;
    info->supports_rotate = 1;
    info->supports_csc = 1;
    info->max_input_w = 8192;
    info->max_input_h = 8192;
    info->max_output_w = 8128;
    info->max_output_h = 8128;

    for (size_t i = 0; i < ARRAY_LEN(rga_fmts) && info->num_input_fmts < OVL_CONV_MAX_FORMATS; i++)
        info->input_fmts[info->num_input_fmts++] = rga_fmts[i];
    for (size_t i = 0; i < ARRAY_LEN(rga_fmts) && info->num_output_fmts < OVL_CONV_MAX_FORMATS; i++)
        info->output_fmts[info->num_output_fmts++] = rga_fmts[i];

    return 0;
}

int ovl_converter_rga_supports(enum ovl_pixfmt src, enum ovl_pixfmt dst) {
    if (!rga_available())
        return -1;
    if (!fmt_supported(src) || !fmt_supported(dst))
        return -1;
    // RGA only handles conversions that involve colorspace change (YUV↔RGB).
    // Same-colorspace format changes (e.g. RGB888→XRGB8888) cause "no core match".
    if (is_yuv(src) == is_yuv(dst))
        return -1;
    return 0;
}

int ovl_converter_rga_init(enum ovl_pixfmt src_fmt, enum ovl_pixfmt dst_fmt, uint32_t width,
                           uint32_t height, int *src_fds, int num_src, int *dst_fds, int num_dst) {
#if !HAS_RGA
    (void)src_fmt;
    (void)dst_fmt;
    (void)width;
    (void)height;
    (void)src_fds;
    (void)num_src;
    (void)dst_fds;
    (void)num_dst;
    ZF_LOGE("not compiled with RGA support");
    return -1;
#else
    memset(&rga_state, 0, sizeof(rga_state));

    rga_state.src_fmt = src_fmt;
    rga_state.dst_fmt = dst_fmt;
    rga_state.rga_src_fmt = pixfmt_to_rga(src_fmt);
    rga_state.rga_dst_fmt = pixfmt_to_rga(dst_fmt);
    rga_state.width = width;
    rga_state.height = height;

    if (rga_state.rga_src_fmt < 0 || rga_state.rga_dst_fmt < 0) {
        ZF_LOGE("unsupported format");
        return -1;
    }

    // Determine CSC mode
    if (is_yuv(src_fmt) && !is_yuv(dst_fmt))
        rga_state.csc_mode = IM_YUV_TO_RGB_BT601_LIMIT;
    else if (!is_yuv(src_fmt) && is_yuv(dst_fmt))
        rga_state.csc_mode = IM_RGB_TO_YUV_BT601_LIMIT;
    else
        rga_state.csc_mode = IM_COLOR_SPACE_DEFAULT;

    rga_state.num_src = num_src;
    rga_state.num_dst = num_dst;

    // Import source DMABUFs
    for (int i = 0; i < num_src; i++) {
        im_handle_param_t param = {
            .width = width,
            .height = height,
            .format = (uint32_t)rga_state.rga_src_fmt,
        };
        rga_state.src_handles[i] = importbuffer_fd(src_fds[i], &param);
        if (rga_state.src_handles[i] == 0) {
            ZF_LOGE("importbuffer_fd src[%d] failed", i);
            ovl_converter_rga_cleanup();
            return -1;
        }
        // wrapbuffer_handle_t(handle, width, height, wstride, hstride, format)
        // wstride/hstride are in pixels for RGA, not bytes
        rga_state.src_bufs[i] =
            wrapbuffer_handle_t(rga_state.src_handles[i], (int)width, (int)height, (int)width,
                                (int)height, rga_state.rga_src_fmt);
        rga_state.src_bufs[i].width = (int)width;
        rga_state.src_bufs[i].height = (int)height;
        ZF_LOGD("rga src[%d] handle=%d", i, rga_state.src_handles[i]);
    }

    // Import destination DMABUFs
    for (int i = 0; i < num_dst; i++) {
        im_handle_param_t param = {
            .width = width,
            .height = height,
            .format = (uint32_t)rga_state.rga_dst_fmt,
        };
        rga_state.dst_handles[i] = importbuffer_fd(dst_fds[i], &param);
        if (rga_state.dst_handles[i] == 0) {
            ZF_LOGE("importbuffer_fd dst[%d] failed", i);
            ovl_converter_rga_cleanup();
            return -1;
        }
        rga_state.dst_bufs[i] =
            wrapbuffer_handle_t(rga_state.dst_handles[i], (int)width, (int)height, (int)width,
                                (int)height, rga_state.rga_dst_fmt);
        rga_state.dst_bufs[i].width = (int)width;
        rga_state.dst_bufs[i].height = (int)height;
        ZF_LOGD("rga dst[%d] handle=%d", i, rga_state.dst_handles[i]);
    }

    rga_state.initialized = 1;
    ZF_LOGD("rga initialized %s -> %s", ovl_pixfmt_name(src_fmt), ovl_pixfmt_name(dst_fmt));
    return 0;
#endif
}

int ovl_converter_rga_process(int src_idx, int dst_idx) {
#if !HAS_RGA
    (void)src_idx;
    (void)dst_idx;
    return -1;
#else
    if (!rga_state.initialized)
        return -1;

    IM_STATUS ret =
        imcvtcolor_t(rga_state.src_bufs[src_idx], rga_state.dst_bufs[dst_idx],
                     rga_state.rga_src_fmt, rga_state.rga_dst_fmt, rga_state.csc_mode, 1);

    if (ret != IM_STATUS_SUCCESS) {
        ZF_LOGE("RGA failed: %s", imStrError_t(ret));
        return -1;
    }

    return 0;
#endif
}

void ovl_converter_rga_cleanup(void) {
#if HAS_RGA
    for (int i = 0; i < rga_state.num_src; i++)
        if (rga_state.src_handles[i])
            releasebuffer_handle(rga_state.src_handles[i]);
    for (int i = 0; i < rga_state.num_dst; i++)
        if (rga_state.dst_handles[i])
            releasebuffer_handle(rga_state.dst_handles[i]);
    memset(&rga_state, 0, sizeof(rga_state));
#endif
}

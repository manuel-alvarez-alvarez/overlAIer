#include "v4l2_caps.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/videodev2.h>

#include "../common/log.h"

static int add_mode(struct ovl_video_caps *caps, uint32_t pixfmt, uint32_t w, uint32_t h,
                    uint32_t fps_num, uint32_t fps_den) {
    struct ovl_video_mode *new = realloc(caps->modes, (caps->num_modes + 1) * sizeof(*new));
    if (!new)
        return -1;
    caps->modes = new;
    caps->modes[caps->num_modes++] = (struct ovl_video_mode){
        .pixelformat = pixfmt,
        .width = w,
        .height = h,
        .fps_numerator = fps_num,
        .fps_denominator = fps_den,
    };
    return 0;
}

static void enumerate_frameintervals(int fd, struct ovl_video_caps *caps, uint32_t pixfmt,
                                     uint32_t w, uint32_t h) {
    struct v4l2_frmivalenum fival = {
        .index = 0,
        .pixel_format = pixfmt,
        .width = w,
        .height = h,
    };

    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == 0) {
        if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            add_mode(caps, pixfmt, w, h, fival.discrete.numerator, fival.discrete.denominator);
        } else {
            // For stepwise/continuous, report min and max intervals
            add_mode(caps, pixfmt, w, h, fival.stepwise.min.numerator,
                     fival.stepwise.min.denominator);
            add_mode(caps, pixfmt, w, h, fival.stepwise.max.numerator,
                     fival.stepwise.max.denominator);
            break;
        }
        fival.index++;
    }

    // If no intervals reported, add mode with 0/0 fps (unknown)
    if (fival.index == 0)
        add_mode(caps, pixfmt, w, h, 0, 0);
}

static void enumerate_framesizes(int fd, struct ovl_video_caps *caps, uint32_t pixfmt) {
    struct v4l2_frmsizeenum fsize = {
        .index = 0,
        .pixel_format = pixfmt,
    };

    while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0) {
        if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            enumerate_frameintervals(fd, caps, pixfmt, fsize.discrete.width, fsize.discrete.height);
        } else {
            // For stepwise/continuous, report min and max sizes
            enumerate_frameintervals(fd, caps, pixfmt, fsize.stepwise.min_width,
                                     fsize.stepwise.min_height);
            enumerate_frameintervals(fd, caps, pixfmt, fsize.stepwise.max_width,
                                     fsize.stepwise.max_height);
            break;
        }
        fsize.index++;
    }
}

int ovl_v4l2_query_caps(const char *device, struct ovl_video_caps *caps) {
    memset(caps, 0, sizeof(*caps));

    int fd = open(device, O_RDWR);
    if (fd < 0) {
        ZF_LOGE("open: %s", strerror(errno));
        return -1;
    }

    struct v4l2_capability vcap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &vcap) < 0) {
        ZF_LOGE("VIDIOC_QUERYCAP: %s", strerror(errno));
        close(fd);
        return -1;
    }

    uint32_t caps_flags =
        vcap.capabilities & V4L2_CAP_DEVICE_CAPS ? vcap.device_caps : vcap.capabilities;

    enum v4l2_buf_type buf_type;
    if (caps_flags & V4L2_CAP_VIDEO_CAPTURE) {
        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else if (caps_flags & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else {
        ZF_LOGE("%s is not a capture device (caps=0x%08x)", device, caps_flags);
        close(fd);
        return -1;
    }

    snprintf(caps->card, sizeof(caps->card), "%s", (char *)vcap.card);
    snprintf(caps->driver, sizeof(caps->driver), "%s", (char *)vcap.driver);
    snprintf(caps->bus_info, sizeof(caps->bus_info), "%s", (char *)vcap.bus_info);

    struct v4l2_fmtdesc fmt = {
        .index = 0,
        .type = buf_type,
    };

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        enumerate_framesizes(fd, caps, fmt.pixelformat);
        fmt.index++;
    }

    // Fallback: if no modes were enumerated (common for HDMI RX devices that
    // don't support ENUM_FRAMESIZES), query the current active format instead.
    if (caps->num_modes == 0) {
        struct v4l2_format gfmt = {.type = buf_type};
        if (ioctl(fd, VIDIOC_G_FMT, &gfmt) == 0) {
            uint32_t pixfmt, w, h;
            if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                pixfmt = gfmt.fmt.pix_mp.pixelformat;
                w = gfmt.fmt.pix_mp.width;
                h = gfmt.fmt.pix_mp.height;
            } else {
                pixfmt = gfmt.fmt.pix.pixelformat;
                w = gfmt.fmt.pix.width;
                h = gfmt.fmt.pix.height;
            }
            if (w > 0 && h > 0)
                add_mode(caps, pixfmt, w, h, 0, 0);
        }
    }

    // Fill in framerate for modes that don't have one.
    // Try DV timings first (HDMI RX), then fall back to stream params.
    uint32_t fps_num = 0, fps_den = 0;

    struct v4l2_dv_timings dvt;
    if (ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &dvt) == 0 && dvt.type == V4L2_DV_BT_656_1120) {
        struct v4l2_bt_timings *bt = &dvt.bt;
        uint64_t htotal = bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch;
        uint64_t vtotal = bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch;
        if (htotal > 0 && vtotal > 0 && bt->pixelclock > 0) {
            fps_num = (uint32_t)(htotal * vtotal);
            fps_den = (uint32_t)(bt->pixelclock);
        }
    }

    if (fps_den == 0) {
        struct v4l2_streamparm parm = {.type = buf_type};
        if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
            fps_num = parm.parm.capture.timeperframe.numerator;
            fps_den = parm.parm.capture.timeperframe.denominator;
        }
    }

    if (fps_den > 0) {
        for (size_t i = 0; i < caps->num_modes; i++) {
            if (caps->modes[i].fps_denominator == 0) {
                caps->modes[i].fps_numerator = fps_num;
                caps->modes[i].fps_denominator = fps_den;
            }
        }
    }

    close(fd);
    return 0;
}

static const char *fourcc_str(uint32_t fourcc, char buf[5]) {
    buf[0] = (char)(fourcc & 0xFF);
    buf[1] = (char)((fourcc >> 8) & 0xFF);
    buf[2] = (char)((fourcc >> 16) & 0xFF);
    buf[3] = (char)((fourcc >> 24) & 0xFF);
    buf[4] = '\0';
    return buf;
}

void ovl_v4l2_caps_print(const struct ovl_video_caps *caps) {
    printf("Video device: %s (%s, %s)\n", caps->card, caps->driver, caps->bus_info);
    printf("  Supported modes: %zu\n", caps->num_modes);

    char fcc[5];
    for (size_t i = 0; i < caps->num_modes; i++) {
        const struct ovl_video_mode *m = &caps->modes[i];
        if (m->fps_denominator > 0) {
            printf("    [%s] %ux%u @ %.2f fps\n", fourcc_str(m->pixelformat, fcc), m->width,
                   m->height, (double)m->fps_denominator / m->fps_numerator);
        } else {
            printf("    [%s] %ux%u\n", fourcc_str(m->pixelformat, fcc), m->width, m->height);
        }
    }
}

void ovl_v4l2_caps_free(struct ovl_video_caps *caps) {
    free(caps->modes);
    caps->modes = NULL;
    caps->num_modes = 0;
}

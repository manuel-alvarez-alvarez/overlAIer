#include "v4l2_capture.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <linux/videodev2.h>

#include "../common/log.h"

static int detect_buf_type(int fd, uint32_t *buf_type) {
    struct v4l2_capability vcap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &vcap) < 0)
        return -1;

    uint32_t caps = vcap.capabilities & V4L2_CAP_DEVICE_CAPS ? vcap.device_caps : vcap.capabilities;

    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        *buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        return 0;
    }
    if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        *buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        return 0;
    }
    return -1;
}

int ovl_v4l2_enum_formats(const char *device, uint32_t *fmts, int max_fmts) {
    int fd = open(device, O_RDWR);
    if (fd < 0)
        return -1;

    uint32_t buf_type;
    if (detect_buf_type(fd, &buf_type) < 0) {
        close(fd);
        return -1;
    }

    struct v4l2_fmtdesc desc = {.type = buf_type};
    int count = 0;
    while (count < max_fmts && ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0) {
        fmts[count++] = desc.pixelformat;
        desc.index++;
    }

    close(fd);
    return count;
}

// Apply configuration. Returns 0 if the driver accepted the requested settings.
static int apply_config(struct ovl_v4l2_capture *cap, const struct ovl_v4l2_capture_config *cfg) {
    struct v4l2_format fmt = {.type = cap->buf_type};
    if (ioctl(cap->fd, VIDIOC_G_FMT, &fmt) < 0)
        return -1;

    if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        if (cfg->pixelformat)
            fmt.fmt.pix_mp.pixelformat = cfg->pixelformat;
        if (cfg->width)
            fmt.fmt.pix_mp.width = cfg->width;
        if (cfg->height)
            fmt.fmt.pix_mp.height = cfg->height;
    } else {
        if (cfg->pixelformat)
            fmt.fmt.pix.pixelformat = cfg->pixelformat;
        if (cfg->width)
            fmt.fmt.pix.width = cfg->width;
        if (cfg->height)
            fmt.fmt.pix.height = cfg->height;
    }

    if (ioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0)
        return -1;

    // Set framerate via stream params
    if (cfg->framerate) {
        struct v4l2_streamparm parm = {.type = cap->buf_type};
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = cfg->framerate;
        ioctl(cap->fd, VIDIOC_S_PARM, &parm);
    }

    return 0;
}

static int query_current_format(struct ovl_v4l2_capture *cap) {
    struct v4l2_format fmt = {.type = cap->buf_type};
    if (ioctl(cap->fd, VIDIOC_G_FMT, &fmt) < 0) {
        ZF_LOGE("VIDIOC_G_FMT: %s", strerror(errno));
        return -1;
    }

    if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        cap->pixelformat = fmt.fmt.pix_mp.pixelformat;
        cap->width = fmt.fmt.pix_mp.width;
        cap->height = fmt.fmt.pix_mp.height;
        cap->num_planes = (int)fmt.fmt.pix_mp.num_planes;
    } else {
        cap->pixelformat = fmt.fmt.pix.pixelformat;
        cap->width = fmt.fmt.pix.width;
        cap->height = fmt.fmt.pix.height;
        cap->num_planes = 1;
    }

    ZF_LOGD("v4l2 capture %ux%u %d plane(s)", cap->width, cap->height, cap->num_planes);
    return 0;
}

static int setup_buffers(struct ovl_v4l2_capture *cap) {
    struct v4l2_requestbuffers reqbufs = {
        .count = OVL_V4L2_NUM_BUFFERS,
        .type = cap->buf_type,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(cap->fd, VIDIOC_REQBUFS, &reqbufs) < 0) {
        int saved_errno = errno;
        ZF_LOGE("VIDIOC_REQBUFS: %s", strerror(saved_errno));
        errno = saved_errno; // preserve for caller
        return -1;
    }

    cap->num_buffers = reqbufs.count;
    if (cap->num_buffers > OVL_V4L2_NUM_BUFFERS) {
        ZF_LOGW("v4l2 driver allocated %u buffers, clamping to %d", cap->num_buffers,
                OVL_V4L2_NUM_BUFFERS);
        cap->num_buffers = OVL_V4L2_NUM_BUFFERS;
    }
    ZF_LOGD("v4l2 %u buffers", cap->num_buffers);

    for (uint32_t i = 0; i < cap->num_buffers; i++) {
        struct ovl_v4l2_buffer *buf = &cap->buffers[i];
        buf->index = (int)i;

        // Query buffer to find number of planes and their sizes
        struct v4l2_buffer v4l2_buf = {
            .index = i,
            .type = cap->buf_type,
            .memory = V4L2_MEMORY_MMAP,
        };

        struct v4l2_plane planes[OVL_V4L2_MAX_PLANES] = {0};
        if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            v4l2_buf.m.planes = planes;
            v4l2_buf.length = (uint32_t)cap->num_planes;
        }

        if (ioctl(cap->fd, VIDIOC_QUERYBUF, &v4l2_buf) < 0) {
            ZF_LOGE("VIDIOC_QUERYBUF: %s", strerror(errno));
            return -1;
        }

        buf->num_planes = cap->num_planes;
        if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            for (int p = 0; p < buf->num_planes; p++)
                buf->offsets[p] = planes[p].data_offset;
        }

        if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            char plane_info[256] = "";
            int off = 0;
            for (int p = 0; p < buf->num_planes; p++)
                off += snprintf(plane_info + off, sizeof(plane_info) - (size_t)off, " p%d:%uB", p,
                                planes[p].length);
            ZF_LOGD("v4l2 buf[%u] %d plane(s)%s", i, buf->num_planes, plane_info);
        } else {
            ZF_LOGD("v4l2 buf[%u] %d plane(s)", i, buf->num_planes);
        }

        // Export each plane as DMABUF
        for (int p = 0; p < buf->num_planes; p++) {
            struct v4l2_exportbuffer expbuf = {
                .type = cap->buf_type,
                .index = i,
                .plane = (uint32_t)p,
            };
            if (ioctl(cap->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                ZF_LOGE("VIDIOC_EXPBUF: %s", strerror(errno));
                return -1;
            }
            buf->dmabuf_fds[p] = expbuf.fd;
        }
    }

    // Get pitches from the format
    struct v4l2_format fmt = {.type = cap->buf_type};
    if (ioctl(cap->fd, VIDIOC_G_FMT, &fmt) == 0) {
        for (uint32_t i = 0; i < cap->num_buffers; i++) {
            struct ovl_v4l2_buffer *buf = &cap->buffers[i];
            if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                for (int p = 0; p < buf->num_planes; p++)
                    buf->pitches[p] = fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
            } else {
                buf->pitches[0] = fmt.fmt.pix.bytesperline;
            }
        }
    }

    {
        char pitch_info[256] = "";
        int off = 0;
        for (int p = 0; p < cap->buffers[0].num_planes; p++)
            off += snprintf(pitch_info + off, sizeof(pitch_info) - (size_t)off, " %u",
                            cap->buffers[0].pitches[p]);
        ZF_LOGD("v4l2 pitch:%s", pitch_info);
    }

    return 0;
}

int ovl_v4l2_capture_init(struct ovl_v4l2_capture *cap, const char *device,
                          const struct ovl_v4l2_capture_config *cfg) {
    memset(cap, 0, sizeof(*cap));
    for (int i = 0; i < OVL_V4L2_NUM_BUFFERS; i++)
        for (int p = 0; p < OVL_V4L2_MAX_PLANES; p++)
            cap->buffers[i].dmabuf_fds[p] = -1;

    cap->fd = open(device, O_RDWR);
    if (cap->fd < 0) {
        ZF_LOGE("open: %s", strerror(errno));
        return -1;
    }

    if (detect_buf_type(cap->fd, &cap->buf_type) < 0) {
        ZF_LOGE("%s is not a capture device", device);
        close(cap->fd);
        return -1;
    }

    // Set DV timings from detected signal and extract fps
    struct v4l2_dv_timings dvt;
    if (ioctl(cap->fd, VIDIOC_QUERY_DV_TIMINGS, &dvt) == 0) {
        ioctl(cap->fd, VIDIOC_S_DV_TIMINGS, &dvt);
        if (dvt.type == V4L2_DV_BT_656_1120) {
            struct v4l2_bt_timings *bt = &dvt.bt;
            uint64_t htotal = bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch;
            uint64_t vtotal = bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch;
            if (htotal && vtotal && bt->pixelclock)
                cap->fps = (uint32_t)(bt->pixelclock / (htotal * vtotal));
            ZF_LOGI("v4l2 signal detected: %llux%llu@%u", (unsigned long long)bt->width,
                    (unsigned long long)bt->height, cap->fps);
        }
    } else {
        ZF_LOGD("v4l2 no signal detected (QUERY_DV_TIMINGS failed)");
    }

    // Apply requested configuration
    if (cfg && (cfg->pixelformat || cfg->width || cfg->height || cfg->framerate))
        apply_config(cap, cfg);

    if (query_current_format(cap) < 0) {
        close(cap->fd);
        return -1;
    }

    if (setup_buffers(cap) < 0) {
        // Clean up any partially-exported DMABUFs
        for (int i = 0; i < OVL_V4L2_NUM_BUFFERS; i++)
            for (int p = 0; p < OVL_V4L2_MAX_PLANES; p++)
                if (cap->buffers[i].dmabuf_fds[p] >= 0)
                    close(cap->buffers[i].dmabuf_fds[p]);
        close(cap->fd);
        cap->fd = -1;
        return -1;
    }

    return 0;
}

int ovl_v4l2_capture_start(struct ovl_v4l2_capture *cap) {
    // Queue all buffers
    for (uint32_t i = 0; i < cap->num_buffers; i++) {
        if (ovl_v4l2_capture_queue(cap, (int)i) < 0)
            return -1;
    }

    int type = (int)cap->buf_type;
    if (ioctl(cap->fd, VIDIOC_STREAMON, &type) < 0) {
        ZF_LOGE("VIDIOC_STREAMON: %s", strerror(errno));
        return -1;
    }

    ZF_LOGD("v4l2 streaming started");
    return 0;
}

static void fill_dequeue_info(struct ovl_v4l2_dequeue_info *dq, const struct v4l2_buffer *buf) {
    if (!dq)
        return;
    dq->sequence = buf->sequence;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    dq->timestamp_us = (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
}

int ovl_v4l2_capture_dequeue(struct ovl_v4l2_capture *cap, struct ovl_v4l2_dequeue_info *dq_info) {
    // Wait up to 500ms for a frame — detect signal loss if no frame arrives
    struct pollfd pfd = {.fd = cap->fd, .events = POLLIN};
    int pr = poll(&pfd, 1, 500);
    if (pr == 0) {
        // Timeout — no frame in 500ms, signal likely lost
        ZF_LOGW("VIDIOC_DQBUF: timeout (no frame in 500ms)");
        return -1;
    }
    if (pr < 0) {
        ZF_LOGE("poll: %s", strerror(errno));
        return -1;
    }

    struct v4l2_buffer buf = {
        .type = cap->buf_type,
        .memory = V4L2_MEMORY_MMAP,
    };

    struct v4l2_plane planes[OVL_V4L2_MAX_PLANES] = {0};
    if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = (uint32_t)cap->num_planes;
    }

    if (ioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) {
        ZF_LOGE("VIDIOC_DQBUF: %s", strerror(errno));
        return -1;
    }

    fill_dequeue_info(dq_info, &buf);
    return (int)buf.index;
}

int ovl_v4l2_capture_dequeue_nb(struct ovl_v4l2_capture *cap, struct ovl_v4l2_dequeue_info *dq_info) {
    struct v4l2_buffer buf = {
        .type = cap->buf_type,
        .memory = V4L2_MEMORY_MMAP,
    };

    struct v4l2_plane planes[OVL_V4L2_MAX_PLANES] = {0};
    if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = (uint32_t)cap->num_planes;
    }

    // Poll with zero timeout to check if a frame is ready
    struct pollfd pfd = {.fd = cap->fd, .events = POLLIN};
    if (poll(&pfd, 1, 0) <= 0)
        return -1;

    if (ioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0)
        return -1;

    fill_dequeue_info(dq_info, &buf);
    return (int)buf.index;
}

int ovl_v4l2_capture_queue(struct ovl_v4l2_capture *cap, int index) {
    struct v4l2_buffer buf = {
        .index = (uint32_t)index,
        .type = cap->buf_type,
        .memory = V4L2_MEMORY_MMAP,
    };

    struct v4l2_plane planes[OVL_V4L2_MAX_PLANES] = {0};
    if (cap->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = (uint32_t)cap->num_planes;
    }

    if (ioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
        ZF_LOGE("VIDIOC_QBUF: %s", strerror(errno));
        return -1;
    }

    return 0;
}

void ovl_v4l2_capture_stop(struct ovl_v4l2_capture *cap) {
    int type = (int)cap->buf_type;
    ioctl(cap->fd, VIDIOC_STREAMOFF, &type);
}

void ovl_v4l2_capture_free(struct ovl_v4l2_capture *cap) {
    if (cap->fd >= 0) {
        ovl_v4l2_capture_stop(cap);

        for (uint32_t i = 0; i < cap->num_buffers; i++)
            for (int p = 0; p < cap->buffers[i].num_planes; p++)
                if (cap->buffers[i].dmabuf_fds[p] >= 0)
                    close(cap->buffers[i].dmabuf_fds[p]);

        close(cap->fd);
    }

    memset(cap, 0, sizeof(*cap));
    cap->fd = -1;
}

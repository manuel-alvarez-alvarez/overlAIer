#include "converter.h"
#include "converter_sw.h"
#include "converter_rga.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <errno.h>
#include <linux/dma-heap.h>

#include "../common/log.h"

enum converter_backend { BACKEND_SW, BACKEND_RGA };

struct ovl_converter {
    struct ovl_converter_config cfg;
    enum converter_backend backend;
    int num_buffers;
    int next_buf;
    struct ovl_converter_buffer out_bufs[OVL_CONV_MAX_BUFFERS];
    void *out_maps[OVL_CONV_MAX_BUFFERS]; // SW only: persistent mmap
    uint32_t out_buf_size;
    uint32_t src_buf_size;
    uint32_t src_pitch;
    uint32_t dst_pitch;

    // SW: cached source mmaps
    void *src_maps[OVL_CONV_MAX_BUFFERS];
    int src_fds[OVL_CONV_MAX_BUFFERS];
};

// --- DMA heap ---

static int dma_heap_fd = -1;

static int dma_heap_open(void) {
    if (dma_heap_fd >= 0)
        return 0;
    dma_heap_fd = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);
    if (dma_heap_fd < 0)
        dma_heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (dma_heap_fd < 0) {
        ZF_LOGE("dma_heap open: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int dma_heap_alloc(uint32_t size) {
    if (dma_heap_open() < 0)
        return -1;
    struct dma_heap_allocation_data data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    if (ioctl(dma_heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        ZF_LOGE("dma_heap alloc: %s", strerror(errno));
        return -1;
    }
    return (int)data.fd;
}

// --- Buffer size computation ---

static uint32_t compute_pitch(enum ovl_pixfmt fmt, uint32_t width) {
    int bpp = ovl_pixfmt_bpp(fmt);
    if (bpp > 0)
        return width * (uint32_t)bpp;
    return width;
}

static uint32_t compute_buf_size(enum ovl_pixfmt fmt, uint32_t width, uint32_t height) {
    int bpp = ovl_pixfmt_bpp(fmt);
    if (bpp > 0)
        return width * height * (uint32_t)bpp;
    uint32_t y_size = width * height;
    switch (fmt) {
    case OVL_PIXFMT_NV24:
    case OVL_PIXFMT_NV42:
        return y_size * 3;
    case OVL_PIXFMT_NV16:
    case OVL_PIXFMT_NV61:
        return y_size * 2;
    case OVL_PIXFMT_NV12:
    case OVL_PIXFMT_NV21:
        return y_size * 3 / 2;
    default:
        return y_size;
    }
}

// --- DMA buffer sync (for SW backend) ---

static void dmabuf_sync_start(int fd, int write) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
    };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static void dmabuf_sync_end(int fd, int write) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
    };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

// --- Lazy mmap for SW source buffers ---

static void *get_src_map(struct ovl_converter *conv, int src_fd) {
    for (int i = 0; i < OVL_CONV_MAX_BUFFERS; i++)
        if (conv->src_fds[i] == src_fd)
            return conv->src_maps[i];
    for (int i = 0; i < OVL_CONV_MAX_BUFFERS; i++) {
        if (conv->src_fds[i] == -1) {
            conv->src_maps[i] = mmap(NULL, conv->src_buf_size, PROT_READ, MAP_SHARED, src_fd, 0);
            if (conv->src_maps[i] == MAP_FAILED) {
                ZF_LOGE("mmap src: %s", strerror(errno));
                return NULL;
            }
            conv->src_fds[i] = src_fd;
            return conv->src_maps[i];
        }
    }
    return NULL;
}

// --- Query ---

int ovl_converter_query_backends(struct ovl_converter_backend_info *infos, int max_backends) {
    int count = 0;
    if (count < max_backends && ovl_converter_rga_info(&infos[count]) == 0)
        count++;
    if (count < max_backends) {
        ovl_converter_sw_info(&infos[count]);
        count++;
    }
    return count;
}

// --- Create / Destroy ---

int ovl_converter_create(struct ovl_converter **out, const struct ovl_converter_config *cfg) {
    struct ovl_converter *conv = calloc(1, sizeof(*conv));
    if (!conv)
        return -1;

    conv->cfg = *cfg;
    conv->num_buffers = cfg->num_buffers;
    for (int i = 0; i < OVL_CONV_MAX_BUFFERS; i++) {
        conv->src_fds[i] = -1;
        conv->src_maps[i] = MAP_FAILED;
        conv->out_maps[i] = MAP_FAILED;
    }

    if (ovl_converter_rga_supports(cfg->src_fmt, cfg->dst_fmt) == 0) {
        conv->backend = BACKEND_RGA;
        // RGA backend
    } else if (ovl_converter_sw_supports(cfg->src_fmt, cfg->dst_fmt) == 0) {
        conv->backend = BACKEND_SW;
    } else {
        ZF_LOGE("no backend for %s -> %s", ovl_pixfmt_name(cfg->src_fmt),
                ovl_pixfmt_name(cfg->dst_fmt));
        free(conv);
        return -1;
    }

    conv->out_buf_size = compute_buf_size(cfg->dst_fmt, cfg->width, cfg->height);
    conv->src_buf_size = compute_buf_size(cfg->src_fmt, cfg->width, cfg->height);
    conv->src_pitch = compute_pitch(cfg->src_fmt, cfg->width);
    conv->dst_pitch = compute_pitch(cfg->dst_fmt, cfg->width);

    // Allocate output DMA buffers
    for (int i = 0; i < conv->num_buffers; i++) {
        int fd = dma_heap_alloc(conv->out_buf_size);
        if (fd < 0) {
            ovl_converter_destroy(conv);
            return -1;
        }
        conv->out_bufs[i].dmabuf_fds[0] = fd;
        conv->out_bufs[i].pitches[0] = conv->dst_pitch;
        conv->out_bufs[i].offsets[0] = 0;
        conv->out_bufs[i].num_planes = 1;

        // SW backend: persistent mmap of output buffers
        if (conv->backend == BACKEND_SW) {
            conv->out_maps[i] =
                mmap(NULL, conv->out_buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (conv->out_maps[i] == MAP_FAILED) {
                ZF_LOGE("mmap output: %s", strerror(errno));
                ovl_converter_destroy(conv);
                return -1;
            }
        }

        ZF_LOGD("converter buf[%d] fd=%d %uB", i, fd, conv->out_buf_size);
    }

    // RGA backend: pre-register all source and destination DMABUFs
    if (conv->backend == BACKEND_RGA) {
        int dst_fds[OVL_CONV_MAX_BUFFERS];
        for (int i = 0; i < conv->num_buffers; i++)
            dst_fds[i] = conv->out_bufs[i].dmabuf_fds[0];

        if (ovl_converter_rga_init(cfg->src_fmt, cfg->dst_fmt, cfg->width, cfg->height,
                                   (int *)cfg->src_dmabuf_fds, conv->num_buffers, dst_fds,
                                   conv->num_buffers) < 0) {
            ovl_converter_destroy(conv);
            return -1;
        }
    }

    *out = conv;
    return 0;
}

void ovl_converter_destroy(struct ovl_converter *conv) {
    if (!conv)
        return;
    if (conv->backend == BACKEND_RGA)
        ovl_converter_rga_cleanup();
    for (int i = 0; i < conv->num_buffers; i++) {
        if (conv->out_maps[i] != MAP_FAILED)
            munmap(conv->out_maps[i], conv->out_buf_size);
        if (conv->src_maps[i] != MAP_FAILED)
            munmap(conv->src_maps[i], conv->src_buf_size);
        for (int p = 0; p < conv->out_bufs[i].num_planes; p++)
            if (conv->out_bufs[i].dmabuf_fds[p] > 0)
                close(conv->out_bufs[i].dmabuf_fds[p]);
    }
    free(conv);
}

// --- Process ---

int ovl_converter_process(struct ovl_converter *conv, int *src_dmabuf_fds, uint32_t *src_pitches,
                          uint32_t *src_offsets, int src_num_planes) {
    (void)src_pitches;
    (void)src_offsets;
    (void)src_num_planes;

    int out_idx = conv->next_buf;
    conv->next_buf = (conv->next_buf + 1) % conv->num_buffers;
    int dst_fd = conv->out_bufs[out_idx].dmabuf_fds[0];

    switch (conv->backend) {
    case BACKEND_RGA: {
        // Find source buffer index by matching fd
        int src_idx = -1;
        for (int i = 0; i < conv->num_buffers; i++) {
            if (conv->cfg.src_dmabuf_fds[i] == src_dmabuf_fds[0]) {
                src_idx = i;
                break;
            }
        }
        if (src_idx < 0) {
            ZF_LOGE("unknown src fd %d", src_dmabuf_fds[0]);
            return -1;
        }

        if (ovl_converter_rga_process(src_idx, out_idx) < 0)
            return -1;
        break;
    }
    case BACKEND_SW: {
        void *src_ptr = get_src_map(conv, src_dmabuf_fds[0]);
        if (!src_ptr)
            return -1;
        void *dst_ptr = conv->out_maps[out_idx];

        dmabuf_sync_start(src_dmabuf_fds[0], 0);
        dmabuf_sync_start(dst_fd, 1);

        int ret = ovl_converter_sw_process_mem(conv->cfg.src_fmt, conv->cfg.dst_fmt,
                                               conv->cfg.width, conv->cfg.height, src_ptr,
                                               conv->src_pitch, dst_ptr, conv->dst_pitch);

        dmabuf_sync_end(src_dmabuf_fds[0], 0);
        dmabuf_sync_end(dst_fd, 1);

        if (ret < 0)
            return -1;
        break;
    }
    }

    return out_idx;
}

const struct ovl_converter_buffer *ovl_converter_get_output(struct ovl_converter *conv, int index) {
    if (index < 0 || index >= conv->num_buffers)
        return NULL;
    return &conv->out_bufs[index];
}

const char *ovl_converter_backend_name(struct ovl_converter *conv) {
    return conv->backend == BACKEND_RGA ? "rga" : "software";
}

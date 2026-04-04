#ifndef OVERLAIER_CONVERTER_H
#define OVERLAIER_CONVERTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "../common/pixfmt.h"

#define OVL_CONV_MAX_PLANES   3
#define OVL_CONV_MAX_BUFFERS  8
#define OVL_CONV_MAX_FORMATS  32
#define OVL_CONV_MAX_BACKENDS 4

// --- Backend info (for query) ---

struct ovl_converter_backend_info {
    const char *name;   // "rga", "software"
    const char *type;   // "hardware", "software"
    const char *device; // "/dev/rga" or NULL
    enum ovl_pixfmt input_fmts[OVL_CONV_MAX_FORMATS];
    int num_input_fmts;
    enum ovl_pixfmt output_fmts[OVL_CONV_MAX_FORMATS];
    int num_output_fmts;
    uint32_t max_input_w, max_input_h;
    uint32_t max_output_w, max_output_h;
    int supports_scale;
    int supports_rotate;
    int supports_csc;
};

int ovl_converter_query_backends(struct ovl_converter_backend_info *infos, int max_backends);

// --- Converter instance ---

struct ovl_converter;

struct ovl_converter_config {
    enum ovl_pixfmt src_fmt;
    enum ovl_pixfmt dst_fmt;
    uint32_t width, height;
    int num_buffers;
    // Source DMABUF fds (from V4L2 capture buffers), one per buffer
    int src_dmabuf_fds[OVL_CONV_MAX_BUFFERS];
};

struct ovl_converter_buffer {
    int dmabuf_fds[OVL_CONV_MAX_PLANES];
    uint32_t pitches[OVL_CONV_MAX_PLANES];
    uint32_t offsets[OVL_CONV_MAX_PLANES];
    int num_planes;
};

int ovl_converter_create(struct ovl_converter **conv, const struct ovl_converter_config *cfg);

int ovl_converter_process(struct ovl_converter *conv, int *src_dmabuf_fds, uint32_t *src_pitches,
                          uint32_t *src_offsets, int src_num_planes);

const struct ovl_converter_buffer *ovl_converter_get_output(struct ovl_converter *conv, int index);

const char *ovl_converter_backend_name(struct ovl_converter *conv);

void ovl_converter_destroy(struct ovl_converter *conv);

#endif

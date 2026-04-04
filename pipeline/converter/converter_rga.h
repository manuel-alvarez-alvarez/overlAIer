#ifndef OVERLAIER_CONVERTER_RGA_H
#define OVERLAIER_CONVERTER_RGA_H

#include "converter.h"
#include "../common/pixfmt.h"

int ovl_converter_rga_info(struct ovl_converter_backend_info *info);
int ovl_converter_rga_supports(enum ovl_pixfmt src, enum ovl_pixfmt dst);

// Initialize RGA for a given conversion. Returns 0 on success.
// Imports src/dst DMABUFs and caches the RGA handles.
int ovl_converter_rga_init(enum ovl_pixfmt src_fmt, enum ovl_pixfmt dst_fmt, uint32_t width,
                           uint32_t height, int *src_fds, int num_src, int *dst_fds, int num_dst);

// Convert one frame. src_idx/dst_idx select which buffer to use.
int ovl_converter_rga_process(int src_idx, int dst_idx);

// Release all RGA handles.
void ovl_converter_rga_cleanup(void);

#endif

#ifndef OVERLAIER_CONVERTER_SW_H
#define OVERLAIER_CONVERTER_SW_H

#include "converter.h"
#include "../common/pixfmt.h"

void ovl_converter_sw_info(struct ovl_converter_backend_info *info);
int ovl_converter_sw_supports(enum ovl_pixfmt src, enum ovl_pixfmt dst);

// Process using already-mapped memory pointers (no mmap per frame)
int ovl_converter_sw_process_mem(enum ovl_pixfmt src_fmt, enum ovl_pixfmt dst_fmt, uint32_t width,
                                 uint32_t height, const void *src, uint32_t src_pitch, void *dst,
                                 uint32_t dst_pitch);

#endif

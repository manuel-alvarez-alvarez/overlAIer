#ifndef OVERLAIER_OPTIONS_H
#define OVERLAIER_OPTIONS_H

#include <stdint.h>
#include "pixfmt.h"

enum output_format { FMT_PLAIN, FMT_JSON };

struct options {
    const char *command;     // "run", "query"
    const char *config_path; // --config override
    const char *video_in;
    const char *video_out;
    const char *audio_in;
    const char *audio_out;
    enum ovl_pixfmt fmt_in;
    enum ovl_pixfmt fmt_out;
    uint32_t res_in_w, res_in_h;
    uint32_t fps_in;
    uint32_t res_out_w, res_out_h;
    uint32_t fps_out;
    enum output_format out_fmt; // for query command
    int log_level;              // zf_log level, -1 = default
    int async_flip;             // 1 = async page flip (tearing, lower latency)
};

enum ovl_pixfmt parse_format(const char *s);
int parse_resolution(const char *s, uint32_t *w, uint32_t *h);
int parse_log_level(const char *s);

#endif

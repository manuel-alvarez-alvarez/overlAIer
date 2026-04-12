#include "fps_counter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_SIZE 300 // ~5 seconds at 60fps

struct fps_state {
    uint64_t frame_count;
    uint64_t last_reset_us;
    uint64_t last_frame_us;
    double smoothed_fps;

    // Frame time history (sliding window, ms)
    double frame_times[HISTORY_SIZE];
    int ft_idx;
    int ft_count;
    double last_ft;

    // Processing latency (dequeue -> submit, ms)
    double latencies[HISTORY_SIZE];
    int lat_idx;
    int lat_count;
    uint64_t last_flip_us;
    double last_latency;

    char text_fps[32];
    char text_frame[48];
    char text_proc[48];
    struct ovl_primitive prims[4]; // bg rect + 3 text lines
};

static double percentile(double *data, int count, double pct) {
    if (count == 0)
        return 0;
    double sorted[HISTORY_SIZE];
    memcpy(sorted, data, (size_t)count * sizeof(double));
    // insertion sort — small N, cache-friendly
    for (int i = 1; i < count; i++) {
        double key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return sorted[(int)(count * pct)];
}

static void *fps_init(const struct ovl_processor_def *def, uint32_t width, uint32_t height,
                      enum ovl_pixfmt format) {
    (void)def;
    (void)width;
    (void)height;
    (void)format;
    return calloc(1, sizeof(struct fps_state));
}

static void fps_on_frame(void *state, const void *frame_data, const struct ovl_frame_info *info,
                         struct ovl_primitive **prims_out, int *count_out) {
    (void)frame_data;
    struct fps_state *s = state;

    uint64_t now_us = info->timestamp_us;

    // Frame time in ms (using V4L2 capture timestamps)
    double ft = 0;
    if (s->last_frame_us > 0 && now_us > s->last_frame_us)
        ft = (double)(now_us - s->last_frame_us) / 1000.0;
    s->last_frame_us = now_us;

    if (s->last_reset_us == 0)
        s->last_reset_us = now_us;

    // Store frame time in circular buffer (skip first frame)
    if (s->frame_count > 0 && ft > 0.1 && ft < 1000.0) {
        s->frame_times[s->ft_idx] = ft;
        s->ft_idx = (s->ft_idx + 1) % HISTORY_SIZE;
        if (s->ft_count < HISTORY_SIZE)
            s->ft_count++;
        s->last_ft = ft;
    }

    s->frame_count++;

    // Smoothed FPS (rolling 1-second window)
    double elapsed = (double)(now_us - s->last_reset_us) / 1e6;
    if (elapsed > 0.0)
        s->smoothed_fps = (double)s->frame_count / elapsed;
    if (elapsed >= 1.0) {
        s->frame_count = 0;
        s->last_reset_us = now_us;
    }

    double ft_p99 = percentile(s->frame_times, s->ft_count, 0.99);
    double lat_p99 = percentile(s->latencies, s->lat_count, 0.99);

    snprintf(s->text_fps, sizeof(s->text_fps), "FPS: %.1f", s->smoothed_fps);
    snprintf(s->text_frame, sizeof(s->text_frame), "Frame: %.2fms  P99: %.2fms", s->last_ft, ft_p99);
    snprintf(s->text_proc, sizeof(s->text_proc), "Proc:  %.2fms  P99: %.2fms",
             s->last_latency, lat_p99);

    float line_h = 0.022f;
    float y0 = 0.008f;
    float x0 = 0.008f;

    s->prims[0] = (struct ovl_primitive){
        .type = OVL_PRIM_RECT,
        .fill = {0, 0, 0, 0.6f},
        .rect = {.x = 0, .y = 0, .w = 0.19f, .h = line_h * 3 + y0 * 2, .corner_radius = 0.008f},
    };
    s->prims[1] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {0, 1, 0, 1},
        .text = {.x = x0, .y = y0, .text = s->text_fps, .font = "Monospace Bold", .size = 0.018f},
    };
    s->prims[2] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {1, 1, 1, 1},
        .text = {.x = x0,
                 .y = y0 + line_h,
                 .text = s->text_frame,
                 .font = "Monospace",
                 .size = 0.016f},
    };
    s->prims[3] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {1, 0.7f, 0.2f, 1},
        .text = {.x = x0,
                 .y = y0 + line_h * 2,
                 .text = s->text_proc,
                 .font = "Monospace",
                 .size = 0.016f},
    };

    *prims_out = s->prims;
    *count_out = 4;
}

static void fps_on_flip(void *state, const struct ovl_frame_info *info) {
    struct fps_state *s = state;
    s->last_flip_us = info->timestamp_us;

    // Compute dequeue-to-submit latency
    if (s->last_frame_us > 0 && s->last_flip_us > s->last_frame_us) {
        double lat = (double)(s->last_flip_us - s->last_frame_us) / 1000.0;
        s->last_latency = lat;
        if (lat > 0.01 && lat < 1000.0) {
            s->latencies[s->lat_idx] = lat;
            s->lat_idx = (s->lat_idx + 1) % HISTORY_SIZE;
            if (s->lat_count < HISTORY_SIZE)
                s->lat_count++;
        }
    }
}

static void fps_destroy(void *state) {
    free(state);
}

const struct ovl_processor_def ovl_proc_fps_counter = {
    .name = "fps-counter",
    .init = fps_init,
    .on_frame = fps_on_frame,
    .on_flip = fps_on_flip,
    .destroy = fps_destroy,
};

const struct ovl_processor_def *ovl_processor_register(void) {
    return &ovl_proc_fps_counter;
}

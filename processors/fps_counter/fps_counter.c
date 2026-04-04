#include "fps_counter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HISTORY_SIZE 300 // ~5 seconds at 60fps

struct fps_state {
    uint64_t frame_count;
    struct timespec last_reset;
    struct timespec last_frame;
    double smoothed_fps;

    // Frame time history (sliding window, ms)
    double frame_times[HISTORY_SIZE];
    int history_idx;
    int history_count;

    // Last frame time for display
    double last_ft;

    char text_fps[32];
    char text_frame[32];
    char text_p1[32];
    char text_p50[32];
    char text_p99[32];
    struct ovl_primitive prims[6]; // bg rect + 5 text lines
};

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static void compute_percentiles(struct fps_state *s, double *out_p1, double *out_p50,
                                double *out_p99) {
    if (s->history_count == 0) {
        *out_p1 = *out_p50 = *out_p99 = 0;
        return;
    }

    double sorted[HISTORY_SIZE];
    int n = s->history_count;
    memcpy(sorted, s->frame_times, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), cmp_double);

    *out_p1 = sorted[(int)(n * 0.01)];
    *out_p50 = sorted[n / 2];
    *out_p99 = sorted[(int)(n * 0.99)];
}

static void *fps_init(const struct ovl_processor_def *def, uint32_t width, uint32_t height,
                      enum ovl_pixfmt format) {
    (void)def;
    (void)width;
    (void)height;
    (void)format;
    struct fps_state *s = calloc(1, sizeof(*s));
    clock_gettime(CLOCK_MONOTONIC, &s->last_reset);
    clock_gettime(CLOCK_MONOTONIC, &s->last_frame);
    return s;
}

static void fps_process(void *state, const void *frame_data, uint32_t width, uint32_t height,
                        uint32_t stride, struct ovl_primitive **prims_out, int *count_out) {
    (void)frame_data;
    (void)width;
    (void)height;
    (void)stride;
    struct fps_state *s = state;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Frame time in ms
    double ft = (double)(now.tv_sec - s->last_frame.tv_sec) * 1000.0 +
                (double)(now.tv_nsec - s->last_frame.tv_nsec) / 1e6;
    s->last_frame = now;

    // Store in circular buffer (skip first frame which has bogus timing)
    if (s->frame_count > 0 && ft > 0.1 && ft < 1000.0) {
        s->frame_times[s->history_idx] = ft;
        s->history_idx = (s->history_idx + 1) % HISTORY_SIZE;
        if (s->history_count < HISTORY_SIZE)
            s->history_count++;
        s->last_ft = ft;
    }

    s->frame_count++;

    // Smoothed FPS (rolling 1-second window)
    double elapsed = (double)(now.tv_sec - s->last_reset.tv_sec) +
                     (double)(now.tv_nsec - s->last_reset.tv_nsec) / 1e9;
    if (elapsed > 0.0)
        s->smoothed_fps = (double)s->frame_count / elapsed;
    if (elapsed >= 1.0) {
        s->frame_count = 0;
        s->last_reset = now;
    }

    // Compute percentiles from sliding window
    double p1, p50, p99;
    compute_percentiles(s, &p1, &p50, &p99);

    snprintf(s->text_fps, sizeof(s->text_fps), "FPS: %.1f", s->smoothed_fps);
    snprintf(s->text_frame, sizeof(s->text_frame), "Frame: %.2fms", s->last_ft);
    snprintf(s->text_p1, sizeof(s->text_p1), " P1:  %.2fms", p1);
    snprintf(s->text_p50, sizeof(s->text_p50), "P50:  %.2fms", p50);
    snprintf(s->text_p99, sizeof(s->text_p99), "P99:  %.2fms", p99);

    float line_h = 0.022f;
    float y0 = 0.008f;
    float x0 = 0.008f;

    // Background
    s->prims[0] = (struct ovl_primitive){
        .type = OVL_PRIM_RECT,
        .fill = {0, 0, 0, 0.6f},
        .rect = {.x = 0, .y = 0, .w = 0.12f, .h = line_h * 5 + y0 * 2, .corner_radius = 0.008f},
    };

    // FPS (green)
    s->prims[1] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {0, 1, 0, 1},
        .text = {.x = x0, .y = y0, .text = s->text_fps, .font = "Monospace Bold", .size = 0.018f},
    };
    // Frame time (white)
    s->prims[2] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {1, 1, 1, 1},
        .text =
            {.x = x0, .y = y0 + line_h, .text = s->text_frame, .font = "Monospace", .size = 0.016f},
    };
    // P1 (cyan)
    s->prims[3] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {0.4f, 1, 1, 1},
        .text = {.x = x0,
                 .y = y0 + line_h * 2,
                 .text = s->text_p1,
                 .font = "Monospace",
                 .size = 0.016f},
    };
    // P50 (yellow)
    s->prims[4] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {1, 1, 0.3f, 1},
        .text = {.x = x0,
                 .y = y0 + line_h * 3,
                 .text = s->text_p50,
                 .font = "Monospace",
                 .size = 0.016f},
    };
    // P99 (red)
    s->prims[5] = (struct ovl_primitive){
        .type = OVL_PRIM_TEXT,
        .fill = {1, 0.4f, 0.4f, 1},
        .text = {.x = x0,
                 .y = y0 + line_h * 4,
                 .text = s->text_p99,
                 .font = "Monospace",
                 .size = 0.016f},
    };

    *prims_out = s->prims;
    *count_out = 6;
}

static void fps_destroy(void *state) {
    free(state);
}

const struct ovl_processor_def ovl_proc_fps_counter = {
    .name = "fps-counter",
    .input =
        {
            .format = OVL_PIXFMT_UNKNOWN,
            .width = 0,
            .height = 0,
            .max_fps = 0,
        },
    .init = fps_init,
    .process = fps_process,
    .destroy = fps_destroy,
};

const struct ovl_processor_def *ovl_processor_register(void) {
    return &ovl_proc_fps_counter;
}
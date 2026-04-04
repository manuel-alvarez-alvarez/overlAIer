#ifndef OVERLAIER_V4L2_CAPS_H
#define OVERLAIER_V4L2_CAPS_H

#include <stddef.h>
#include <stdint.h>

struct ovl_video_mode {
    uint32_t pixelformat; // V4L2 fourcc
    uint32_t width;
    uint32_t height;
    uint32_t fps_numerator;
    uint32_t fps_denominator;
};

struct ovl_video_caps {
    char card[32];
    char driver[16];
    char bus_info[32];
    struct ovl_video_mode *modes;
    size_t num_modes;
};

int ovl_v4l2_query_caps(const char *device, struct ovl_video_caps *caps);
void ovl_v4l2_caps_print(const struct ovl_video_caps *caps);
void ovl_v4l2_caps_free(struct ovl_video_caps *caps);

#endif

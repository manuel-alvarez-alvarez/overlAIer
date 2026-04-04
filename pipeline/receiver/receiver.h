#ifndef OVERLAIER_RECEIVER_H
#define OVERLAIER_RECEIVER_H

#include "v4l2_caps.h"
#include "alsa_caps.h"

struct ovl_receiver_caps {
    struct ovl_video_caps video;
    struct ovl_audio_caps audio;
};

int ovl_receiver_query_caps(const char *video_dev, const char *audio_dev,
                            struct ovl_receiver_caps *caps);
void ovl_receiver_caps_print(const struct ovl_receiver_caps *caps);
void ovl_receiver_caps_free(struct ovl_receiver_caps *caps);

#endif

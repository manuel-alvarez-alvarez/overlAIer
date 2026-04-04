#include "receiver.h"

#include <stdio.h>
#include <string.h>

#include "../common/log.h"

int ovl_receiver_query_caps(const char *video_dev, const char *audio_dev,
                            struct ovl_receiver_caps *caps) {
    memset(caps, 0, sizeof(*caps));

    int ret = ovl_v4l2_query_caps(video_dev, &caps->video);
    if (ret < 0) {
        ZF_LOGW("failed to query video caps from %s", video_dev);
        return ret;
    }

    if (audio_dev) {
        ret = ovl_alsa_query_caps(audio_dev, OVL_ALSA_CAPTURE, &caps->audio);
        if (ret < 0)
            ZF_LOGW("failed to query audio caps from %s", audio_dev);
    }

    return 0;
}

void ovl_receiver_caps_print(const struct ovl_receiver_caps *caps) {
    ovl_v4l2_caps_print(&caps->video);
    if (caps->audio.name[0]) {
        printf("\n");
        ovl_alsa_caps_print(&caps->audio);
    }
}

void ovl_receiver_caps_free(struct ovl_receiver_caps *caps) {
    ovl_v4l2_caps_free(&caps->video);
}

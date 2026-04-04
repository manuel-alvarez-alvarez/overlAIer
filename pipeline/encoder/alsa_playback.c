#include "alsa_playback.h"

#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

#include "../common/log.h"

#define PERIOD_FRAMES 256
#define PERIODS       3

struct ovl_alsa_playback {
    snd_pcm_t *pcm;
    snd_pcm_uframes_t buffer_size;
};

int ovl_alsa_playback_init(struct ovl_alsa_playback **out, const char *device, int format,
                           unsigned int rate, unsigned int channels) {
    struct ovl_alsa_playback *play = calloc(1, sizeof(*play));
    if (!play)
        return -1;

    int err = snd_pcm_open(&play->pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        ZF_LOGE("alsa playback open %s: %s", device, snd_strerror(err));
        free(play);
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(play->pcm, params);

    if ((err = snd_pcm_hw_params_set_access(play->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) <
            0 ||
        (err = snd_pcm_hw_params_set_format(play->pcm, params, (snd_pcm_format_t)format)) < 0 ||
        (err = snd_pcm_hw_params_set_rate(play->pcm, params, rate, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(play->pcm, params, channels)) < 0) {
        ZF_LOGE("alsa playback config: %s", snd_strerror(err));
        goto fail;
    }

    snd_pcm_uframes_t ps = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(play->pcm, params, &ps, NULL);
    unsigned int periods = PERIODS;
    snd_pcm_hw_params_set_periods_near(play->pcm, params, &periods, NULL);

    if ((err = snd_pcm_hw_params(play->pcm, params)) < 0) {
        ZF_LOGE("alsa playback hw_params: %s", snd_strerror(err));
        goto fail;
    }

    // Software params: start when buffer is one period full (low latency)
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(play->pcm, sw);
    snd_pcm_sw_params_set_start_threshold(play->pcm, sw, ps);
    snd_pcm_sw_params(play->pcm, sw);

    snd_pcm_hw_params_get_buffer_size(params, &play->buffer_size);
    ZF_LOGI("alsa playback: %s %uHz %uch period=%lu buf=%lu (%.1fms)",
            snd_pcm_format_name((snd_pcm_format_t)format), rate, channels, (unsigned long)ps,
            (unsigned long)play->buffer_size, (double)play->buffer_size / rate * 1000.0);

    *out = play;
    return 0;

fail:
    snd_pcm_close(play->pcm);
    free(play);
    return -1;
}

int ovl_alsa_playback_write(struct ovl_alsa_playback *play, const void *buf, unsigned int frames) {
    snd_pcm_sframes_t r = snd_pcm_writei(play->pcm, buf, frames);
    if (r < 0) {
        r = snd_pcm_recover(play->pcm, (int)r, 1);
        if (r < 0) {
            ZF_LOGE("alsa playback write: %s", snd_strerror((int)r));
            return -1;
        }
        return 0;
    }
    return (int)r;
}

long ovl_alsa_playback_avail(struct ovl_alsa_playback *play) {
    snd_pcm_sframes_t avail = snd_pcm_avail(play->pcm);
    return avail < 0 ? 0 : (long)avail;
}

unsigned int ovl_alsa_playback_buffer_size(struct ovl_alsa_playback *play) {
    return (unsigned int)play->buffer_size;
}

void ovl_alsa_playback_free(struct ovl_alsa_playback *play) {
    if (!play)
        return;
    snd_pcm_drain(play->pcm);
    snd_pcm_close(play->pcm);
    free(play);
}

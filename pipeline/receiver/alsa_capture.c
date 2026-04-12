#include "alsa_capture.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include "../common/log.h"

#define PERIOD_FRAMES 256
#define PERIODS       3

struct ovl_alsa_capture {
    snd_pcm_t *pcm;
    snd_pcm_format_t format;
    unsigned int rate;
    unsigned int channels;
    snd_pcm_uframes_t period_size;
};

int ovl_alsa_capture_init(struct ovl_alsa_capture **out, const char *device,
                          const struct ovl_alsa_capture_config *cfg) {
    struct ovl_alsa_capture *cap = calloc(1, sizeof(*cap));
    if (!cap)
        return -1;

    int err = snd_pcm_open(&cap->pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        ZF_LOGE("alsa capture open %s: %s", device, snd_strerror(err));
        free(cap);
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(cap->pcm, params);

    if ((err = snd_pcm_hw_params_set_access(cap->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        ZF_LOGE("alsa capture set access: %s", snd_strerror(err));
        goto fail;
    }

    // Format
    if (cfg && cfg->format >= 0) {
        cap->format = (snd_pcm_format_t)cfg->format;
    } else {
        // Try S16_LE, S24_LE, S32_LE
        static const snd_pcm_format_t try_fmts[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE,
                                                    SND_PCM_FORMAT_S32_LE};
        cap->format = SND_PCM_FORMAT_UNKNOWN;
        for (size_t i = 0; i < sizeof(try_fmts) / sizeof(try_fmts[0]); i++) {
            if (snd_pcm_hw_params_test_format(cap->pcm, params, try_fmts[i]) == 0) {
                cap->format = try_fmts[i];
                break;
            }
        }
        if (cap->format == SND_PCM_FORMAT_UNKNOWN) {
            ZF_LOGE("alsa capture: no suitable format");
            goto fail;
        }
    }
    if ((err = snd_pcm_hw_params_set_format(cap->pcm, params, cap->format)) < 0) {
        ZF_LOGE("alsa capture set format: %s", snd_strerror(err));
        goto fail;
    }

    // Rate
    cap->rate = (cfg && cfg->rate) ? cfg->rate : 48000;
    if ((err = snd_pcm_hw_params_set_rate_near(cap->pcm, params, &cap->rate, NULL)) < 0) {
        ZF_LOGE("alsa capture set rate: %s", snd_strerror(err));
        goto fail;
    }

    // Channels
    cap->channels = (cfg && cfg->channels) ? cfg->channels : 2;
    if ((err = snd_pcm_hw_params_set_channels_near(cap->pcm, params, &cap->channels)) < 0) {
        ZF_LOGE("alsa capture set channels: %s", snd_strerror(err));
        goto fail;
    }

    // Period
    snd_pcm_uframes_t ps = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(cap->pcm, params, &ps, NULL);
    unsigned int periods = PERIODS;
    snd_pcm_hw_params_set_periods_near(cap->pcm, params, &periods, NULL);

    if ((err = snd_pcm_hw_params(cap->pcm, params)) < 0) {
        ZF_LOGE("alsa capture hw_params: %s", snd_strerror(err));
        goto fail;
    }

    snd_pcm_hw_params_get_period_size(params, &cap->period_size, NULL);

    ZF_LOGD("alsa capture: %s %uHz %uch period=%lu", snd_pcm_format_name(cap->format), cap->rate,
            cap->channels, (unsigned long)cap->period_size);

    *out = cap;
    return 0;

fail:
    snd_pcm_close(cap->pcm);
    free(cap);
    return -1;
}

int ovl_alsa_capture_read(struct ovl_alsa_capture *cap, void *buf, unsigned int frames) {
    if (!cap->pcm)
        return -1; // device closed from previous failed reopen

    snd_pcm_sframes_t r = snd_pcm_readi(cap->pcm, buf, frames);
    if (r >= 0)
        return (int)r;

    // Try standard recovery (handles EPIPE/underrun)
    r = snd_pcm_recover(cap->pcm, (int)r, 1);
    if (r >= 0)
        return 0;

    // EBADFD = device lost (HDMI signal change). Close and reopen.
    ZF_LOGD("alsa capture: %s, reopening device", snd_strerror((int)r));

    const char *dev_name = snd_pcm_name(cap->pcm);
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s", dev_name);

    snd_pcm_close(cap->pcm);
    cap->pcm = NULL;

    int err = snd_pcm_open(&cap->pcm, name_buf, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        ZF_LOGD("alsa capture reopen: %s", snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(cap->pcm, params);
    snd_pcm_hw_params_set_access(cap->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(cap->pcm, params, cap->format);
    snd_pcm_hw_params_set_rate_near(cap->pcm, params, &cap->rate, NULL);
    snd_pcm_hw_params_set_channels_near(cap->pcm, params, &cap->channels);
    snd_pcm_uframes_t ps = cap->period_size;
    snd_pcm_hw_params_set_period_size_near(cap->pcm, params, &ps, NULL);
    if (snd_pcm_hw_params(cap->pcm, params) < 0) {
        ZF_LOGE("alsa capture reconfigure failed");
        snd_pcm_close(cap->pcm);
        cap->pcm = NULL;
        return -1;
    }

    ZF_LOGD("alsa capture: reopened successfully");
    usleep(200000); // 200ms backoff after reopen to avoid spin-loop
    return 0;
}

unsigned int ovl_alsa_capture_rate(struct ovl_alsa_capture *cap) {
    return cap->rate;
}
unsigned int ovl_alsa_capture_channels(struct ovl_alsa_capture *cap) {
    return cap->channels;
}
int ovl_alsa_capture_format(struct ovl_alsa_capture *cap) {
    return (int)cap->format;
}
unsigned int ovl_alsa_capture_period_size(struct ovl_alsa_capture *cap) {
    return (unsigned int)cap->period_size;
}

int ovl_alsa_capture_frame_bytes(struct ovl_alsa_capture *cap) {
    return (int)(snd_pcm_format_physical_width(cap->format) / 8) * (int)cap->channels;
}

void ovl_alsa_capture_abort(struct ovl_alsa_capture *cap) {
    if (!cap || !cap->pcm)
        return;
    snd_pcm_abort(cap->pcm);
}

void ovl_alsa_capture_free(struct ovl_alsa_capture *cap) {
    if (!cap)
        return;
    if (cap->pcm) {
        snd_pcm_close(cap->pcm);
    }
    free(cap);
}

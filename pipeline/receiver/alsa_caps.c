#include "alsa_caps.h"

#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

#include "../common/log.h"

static snd_pcm_stream_t to_alsa_dir(enum ovl_alsa_dir dir) {
    return (dir == OVL_ALSA_PLAYBACK) ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
}

int ovl_alsa_find_device(const char *card_name_substr, enum ovl_alsa_dir dir, char *buf,
                         size_t buf_len) {
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *name = NULL;
        if (snd_card_get_name(card, &name) < 0 || !name)
            continue;
        if (strstr(name, card_name_substr)) {
            // Verify the device actually opens in the requested direction
            char dev[32];
            snprintf(dev, sizeof(dev), "hw:%d,0", card);
            snd_pcm_t *pcm;
            if (snd_pcm_open(&pcm, dev, to_alsa_dir(dir), SND_PCM_NONBLOCK) == 0) {
                snd_pcm_close(pcm);
                snprintf(buf, buf_len, "%s", dev);
                free(name);
                return 0;
            }
        }
        free(name);
    }
    return -1;
}

int ovl_alsa_find_by_bus(const char *bus_info, enum ovl_alsa_dir dir, char *buf, size_t buf_len) {
    if (!bus_info || !bus_info[0])
        return -1;

    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        // Read the card's sysfs longname which often contains the bus path
        char *longname = NULL;
        if (snd_card_get_longname(card, &longname) < 0 || !longname)
            continue;

        // Check if the bus_info appears in the longname
        // V4L2 bus_info looks like "usb-0000:06:00.3-2" or "platform:fdee0000.hdmirx-controller"
        // ALSA longname may contain the same bus address
        int match = (strstr(longname, bus_info) != NULL);

        // Also try matching just the device address part
        // e.g. bus_info "fdee0000.hdmirx-controller" → check for "fdee0000"
        if (!match) {
            // Extract first component before '.' or '-'
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s", bus_info);
            char *dot = strchr(prefix, '.');
            if (dot)
                *dot = '\0';
            if (prefix[0] && strstr(longname, prefix))
                match = 1;
        }

        free(longname);

        if (match) {
            char dev[32];
            snprintf(dev, sizeof(dev), "hw:%d,0", card);
            snd_pcm_t *pcm;
            if (snd_pcm_open(&pcm, dev, to_alsa_dir(dir), SND_PCM_NONBLOCK) == 0) {
                snd_pcm_close(pcm);
                snprintf(buf, buf_len, "%s", dev);
                return 0;
            }
        }
    }
    return -1;
}

static const unsigned int common_rates[] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000,
};

int ovl_alsa_query_caps(const char *device, enum ovl_alsa_dir dir, struct ovl_audio_caps *caps) {
    memset(caps, 0, sizeof(*caps));

    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, device, to_alsa_dir(dir), SND_PCM_NONBLOCK);
    if (err < 0) {
        ZF_LOGE("cannot open %s: %s", device, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);

    snd_pcm_info_t *info;
    snd_pcm_info_alloca(&info);
    if (snd_pcm_info(pcm, info) == 0)
        snprintf(caps->name, sizeof(caps->name), "%s", snd_pcm_info_get_name(info));

    snd_pcm_hw_params_get_channels_min(params, &caps->min_channels);
    snd_pcm_hw_params_get_channels_max(params, &caps->max_channels);

    snd_pcm_hw_params_get_rate_min(params, &caps->min_rate, NULL);
    snd_pcm_hw_params_get_rate_max(params, &caps->max_rate, NULL);

    for (size_t i = 0; i < sizeof(common_rates) / sizeof(common_rates[0]); i++) {
        if (caps->num_rates >= OVL_AUDIO_MAX_RATES)
            break;
        if (snd_pcm_hw_params_test_rate(pcm, params, common_rates[i], 0) == 0)
            caps->rates[caps->num_rates++] = common_rates[i];
    }

    for (int fmt = 0; fmt <= SND_PCM_FORMAT_LAST; fmt++) {
        if (caps->num_formats >= OVL_AUDIO_MAX_FORMATS)
            break;
        if (snd_pcm_hw_params_test_format(pcm, params, (snd_pcm_format_t)fmt) == 0)
            caps->formats[caps->num_formats++] = fmt;
    }

    snd_pcm_close(pcm);
    return 0;
}

void ovl_alsa_caps_print(const struct ovl_audio_caps *caps) {
    printf("Audio device: %s\n", caps->name);
    printf("  Channels: %u - %u\n", caps->min_channels, caps->max_channels);
    printf("  Sample rate range: %u - %u Hz\n", caps->min_rate, caps->max_rate);

    printf("  Supported rates:");
    for (size_t i = 0; i < caps->num_rates; i++)
        printf(" %u", caps->rates[i]);
    printf("\n");

    printf("  Supported formats:");
    for (size_t i = 0; i < caps->num_formats; i++)
        printf(" %s", snd_pcm_format_name((snd_pcm_format_t)caps->formats[i]));
    printf("\n");
}

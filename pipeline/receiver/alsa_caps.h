#ifndef OVERLAIER_ALSA_CAPS_H
#define OVERLAIER_ALSA_CAPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OVL_AUDIO_MAX_RATES   32
#define OVL_AUDIO_MAX_FORMATS 32

struct ovl_audio_caps {
    char name[128];
    unsigned int min_channels;
    unsigned int max_channels;
    unsigned int rates[OVL_AUDIO_MAX_RATES];
    size_t num_rates;
    int formats[OVL_AUDIO_MAX_FORMATS]; // snd_pcm_format_t values
    size_t num_formats;
    unsigned int min_rate;
    unsigned int max_rate;
};

enum ovl_alsa_dir { OVL_ALSA_CAPTURE, OVL_ALSA_PLAYBACK };

// Find an ALSA device whose card name contains the given substring and
// supports the given direction. Returns 0 and writes e.g. "hw:2,0" into buf.
// Find an ALSA device whose card name contains the given substring and
// supports the given direction. Returns 0 and writes e.g. "hw:2,0" into buf.
int ovl_alsa_find_device(const char *card_name_substr, enum ovl_alsa_dir dir, char *buf,
                         size_t buf_len);

// Find an ALSA device by matching its sysfs path against a bus_info string
// (e.g. "usb-0000:06:00.3-2" from V4L2 bus_info). Returns 0 on success.
int ovl_alsa_find_by_bus(const char *bus_info, enum ovl_alsa_dir dir, char *buf, size_t buf_len);

int ovl_alsa_query_caps(const char *device, enum ovl_alsa_dir dir, struct ovl_audio_caps *caps);
void ovl_alsa_caps_print(const struct ovl_audio_caps *caps);

#endif

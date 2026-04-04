#ifndef OVERLAIER_ALSA_CAPTURE_H
#define OVERLAIER_ALSA_CAPTURE_H

#include <stdint.h>

struct ovl_alsa_capture;

struct ovl_alsa_capture_config {
    unsigned int rate;     // 0 = negotiate
    unsigned int channels; // 0 = negotiate
    int format;            // snd_pcm_format_t, -1 = negotiate
};

int ovl_alsa_capture_init(struct ovl_alsa_capture **cap, const char *device,
                          const struct ovl_alsa_capture_config *cfg);
// Read interleaved frames. Returns frames read, or < 0 on error.
int ovl_alsa_capture_read(struct ovl_alsa_capture *cap, void *buf, unsigned int frames);
// Get negotiated parameters
unsigned int ovl_alsa_capture_rate(struct ovl_alsa_capture *cap);
unsigned int ovl_alsa_capture_channels(struct ovl_alsa_capture *cap);
int ovl_alsa_capture_format(struct ovl_alsa_capture *cap);
unsigned int ovl_alsa_capture_period_size(struct ovl_alsa_capture *cap);
int ovl_alsa_capture_frame_bytes(struct ovl_alsa_capture *cap);
// Abort capture (unblocks any blocking read). Call before joining the audio thread.
void ovl_alsa_capture_abort(struct ovl_alsa_capture *cap);
void ovl_alsa_capture_free(struct ovl_alsa_capture *cap);

#endif

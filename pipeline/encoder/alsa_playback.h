#ifndef OVERLAIER_ALSA_PLAYBACK_H
#define OVERLAIER_ALSA_PLAYBACK_H

#include <stdint.h>

struct ovl_alsa_playback;

// Init playback with specific format (matching capture output).
int ovl_alsa_playback_init(struct ovl_alsa_playback **play, const char *device, int format,
                           unsigned int rate, unsigned int channels);
// Write interleaved frames. Returns frames written, or < 0 on error.
int ovl_alsa_playback_write(struct ovl_alsa_playback *play, const void *buf, unsigned int frames);
// Get available space in playback buffer (frames). Used for drift monitoring.
long ovl_alsa_playback_avail(struct ovl_alsa_playback *play);
// Get buffer size in frames.
unsigned int ovl_alsa_playback_buffer_size(struct ovl_alsa_playback *play);
void ovl_alsa_playback_free(struct ovl_alsa_playback *play);

#endif

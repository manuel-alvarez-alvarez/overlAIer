#ifndef OVERLAIER_DRM_OUTPUT_H
#define OVERLAIER_DRM_OUTPUT_H

#include <stdint.h>
#include "../common/pixfmt.h"

#define OVL_DRM_MAX_PLANES  3
#define OVL_DRM_MAX_BUFFERS 8

struct ovl_drm_fb {
    uint32_t fb_id;
    uint32_t gem_handles[OVL_DRM_MAX_PLANES];
    int num_planes;
};

struct ovl_drm_flip {
    int capture_index;
    int fb_index;
    int in_use;
    int pending;
    int done;
    uint32_t flip_seq;       // DRM vblank sequence
    uint64_t flip_timestamp_us; // DRM vblank timestamp (microseconds)
};

struct ovl_drm_output {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t width; // source frame size
    uint32_t height;
    uint32_t crtc_w; // display output size (cached)
    uint32_t crtc_h;
    struct ovl_drm_fb fbs[OVL_DRM_MAX_BUFFERS];
    int num_fbs;
    int async_flip; // 1 = use DRM_MODE_PAGE_FLIP_ASYNC (tearing but lower latency)
    int async_supported;
    struct ovl_drm_flip flips[OVL_DRM_MAX_BUFFERS];

    // Overlay plane (for 2D overlay on top of video)
    uint32_t overlay_plane_id;   // 0 = no overlay plane available
    uint32_t overlay_fb_id;      // framebuffer for the overlay
    enum ovl_pixfmt overlay_fmt; // negotiated overlay format
    uint32_t overlay_handle;     // dumb buffer GEM handle
    uint32_t overlay_pitch;      // dumb buffer pitch
    uint32_t overlay_size;       // dumb buffer total size
    void *overlay_map;           // persistent mmap of the dumb buffer
};

// Open DRM device, find connected output and a plane that supports the given format.
// device: DRM device path, optionally with connector name (e.g. "/dev/dri/card0:HDMI-A-2")
// If out_w/out_h are non-zero, set the CRTC to that resolution (mode change).
// If out_fps is non-zero, prefer a mode with that refresh rate.
int ovl_drm_output_init(struct ovl_drm_output *out, const char *device, uint32_t fourcc,
                        uint32_t width, uint32_t height, uint32_t out_w, uint32_t out_h,
                        uint32_t out_fps);

// Import a DMABUF fd and create a framebuffer. Returns fb index or -1.
int ovl_drm_output_add_fb(struct ovl_drm_output *out, int dmabuf_fd, uint32_t fourcc,
                          uint32_t width, uint32_t height, uint32_t pitch);

// Import a multi-plane DMABUF and create a framebuffer. Returns fb index or -1.
int ovl_drm_output_add_fb_mp(struct ovl_drm_output *out, int *dmabuf_fds, uint32_t *pitches,
                             uint32_t *offsets, int num_planes, uint32_t fourcc, uint32_t width,
                             uint32_t height);

// Display a framebuffer on the overlay plane
// Returns 0 when a flip was queued, 1 if the frame was skipped (caller should
// requeue immediately), <0 on error.
int ovl_drm_output_show(struct ovl_drm_output *out, int fb_index, int capture_index);

// Poll for completed flips and return capture buffers ready to requeue.
// timeout_ms < 0 blocks indefinitely, 0 = non-blocking.
// Wait for a flip to complete. Returns 1 if a flip completed, 0 if timeout, -1 on error.
// capture_index: if non-NULL, receives the capture buffer index of the completed flip.
// flip_seq: if non-NULL, receives the DRM vblank sequence number.
// flip_timestamp_us: if non-NULL, receives the DRM vblank timestamp in microseconds.
int ovl_drm_output_acquire_ready(struct ovl_drm_output *out, int timeout_ms, int *capture_index,
                                 uint32_t *flip_seq, uint64_t *flip_timestamp_us);

// Number of pending flips (capture buffers currently owned by DRM).
int ovl_drm_output_pending(const struct ovl_drm_output *out);

// --- Overlay plane support ---

// Find an overlay plane on the same CRTC that supports one of the preferred formats.
// Returns the matched format, or OVL_PIXFMT_UNKNOWN if no suitable plane found.
enum ovl_pixfmt ovl_drm_output_find_overlay_plane(struct ovl_drm_output *out,
                                                  const enum ovl_pixfmt *preferred_fmts, int count);

// Create a dumb buffer for the overlay and register it as a framebuffer.
// The buffer is allocated internally. Returns 0 on success.
int ovl_drm_output_create_overlay_fb(struct ovl_drm_output *out, uint32_t width, uint32_t height);

// Update the overlay framebuffer content. Pass the pixel buffer to copy from.
int ovl_drm_output_update_overlay(struct ovl_drm_output *out, const void *pixels, uint32_t stride);

// Clean up
void ovl_drm_output_free(struct ovl_drm_output *out);

#endif

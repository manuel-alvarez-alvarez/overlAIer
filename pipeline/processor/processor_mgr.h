#ifndef OVERLAIER_PROCESSOR_MGR_H
#define OVERLAIER_PROCESSOR_MGR_H

#include "processor.h"
#include "../overlay/overlay.h"
#include "../encoder/drm_output.h"

#define OVL_MAX_PROCESSORS 16

struct ovl_processor_mgr;

// Create a processor manager.
// overlay: the Cairo overlay to render primitives into.
// output: the DRM output (for updating the overlay plane).
int ovl_processor_mgr_create(struct ovl_processor_mgr **mgr, struct ovl_overlay *overlay,
                             struct ovl_drm_output *output);

// Register a processor definition. Must be called before start.
int ovl_processor_mgr_register(struct ovl_processor_mgr *mgr, const struct ovl_processor_def *def);

// Load a single .so processor plugin by path.
// Each .so must export: const struct ovl_processor_def *ovl_processor_register(void);
// Returns 0 on success, -1 on error.
int ovl_processor_mgr_load_file(struct ovl_processor_mgr *mgr, const char *path);

// Start all processor threads. Call after capture is initialized.
// src_fmt/width/height describe the capture frame format.
int ovl_processor_mgr_start(struct ovl_processor_mgr *mgr, enum ovl_pixfmt src_fmt,
                            uint32_t src_width, uint32_t src_height);

// Post a new frame to all processors (non-blocking).
// The frame data is copied internally for processors that need it.
void ovl_processor_mgr_post_frame(struct ovl_processor_mgr *mgr, const void *frame_data,
                                  uint32_t width, uint32_t height, uint32_t stride);

// Stop all processor threads and free resources.
void ovl_processor_mgr_destroy(struct ovl_processor_mgr *mgr);

#endif

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

// Post a new captured frame to all processors (non-blocking).
// The frame data is copied internally for processors that need it.
void ovl_processor_mgr_post_frame(struct ovl_processor_mgr *mgr, const void *frame_data,
                                  const struct ovl_frame_info *info);

// Notify all processors that a frame has been displayed (DRM flip completed).
void ovl_processor_mgr_notify_flip(struct ovl_processor_mgr *mgr,
                                   const struct ovl_frame_info *info);

// Process a raw HID report through all processors in chain order.
// Processors can modify the report in-place or drop it.
// Returns 0 if the report should be forwarded, -1 if dropped.
int ovl_processor_mgr_process_hid(struct ovl_processor_mgr *mgr,
                                  uint8_t *report, int *report_len,
                                  const char *device_name, uint16_t vid, uint16_t pid);

// Stop all processor threads and free resources.
void ovl_processor_mgr_destroy(struct ovl_processor_mgr *mgr);

#endif

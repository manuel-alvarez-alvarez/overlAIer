#ifndef OVERLAIER_PROCESSOR_H
#define OVERLAIER_PROCESSOR_H

#include <stdint.h>
#include "../common/pixfmt.h"
#include "../overlay/overlay.h"

// --- Frame metadata ---

struct ovl_frame_info {
    uint32_t sequence;         // V4L2 buffer sequence number
    uint64_t timestamp_us;     // dequeue timestamp (microseconds, CLOCK_MONOTONIC)
    uint32_t width, height;
    uint32_t stride;
};

// --- Processor Interface ---
// Each processor implements this struct. Processors run in their own thread,
// receive video frames via on_frame, and are notified of display via on_flip.
// Processors always receive frames in the same format, resolution, and
// framerate as the capture source.

struct ovl_processor_def {
    const char *name; // e.g. "face-detect", "fps-counter"

    // Called once when the processor is started.
    // width/height/format describe the capture source.
    // Returns opaque state pointer (passed to on_frame/on_flip/destroy).
    void *(*init)(const struct ovl_processor_def *def, uint32_t width, uint32_t height,
                  enum ovl_pixfmt format);

    // Called when a new frame is captured.
    // frame_data: pixel buffer in capture format (may be NULL for zero-copy paths).
    // info: capture metadata (sequence, timestamp, dimensions).
    // Must set *prims_out and *count_out. The array must remain valid
    // until the next on_frame() call or destroy().
    void (*on_frame)(void *state, const void *frame_data, const struct ovl_frame_info *info,
                     struct ovl_primitive **prims_out, int *count_out);

    // Called when a frame has been displayed (DRM page flip completed).
    // info: display metadata (sequence from DRM, timestamp of vblank).
    // May be NULL if the processor does not need flip notification.
    void (*on_flip)(void *state, const struct ovl_frame_info *info);

    // Called with a raw HID report from a proxied USB device.
    // Processors form a chain: the output of one becomes the input of the next.
    // The processor can modify the report in-place.
    // Return 0 to pass the report downstream, -1 to drop it.
    // May be NULL if the processor does not handle HID.
    int (*on_hid_report)(void *state, uint8_t *report, int *report_len,
                         const char *device_name, uint16_t vid, uint16_t pid);

    // Called once when the processor is stopped.
    void (*destroy)(void *state);
};

// Shared library processors must export this symbol:
//   const struct ovl_processor_def *ovl_processor_register(void);
// The function returns a pointer to the processor definition.
#define OVL_PROCESSOR_EXPORT_SYMBOL "ovl_processor_register"

typedef const struct ovl_processor_def *(*ovl_processor_register_fn)(void);

#endif

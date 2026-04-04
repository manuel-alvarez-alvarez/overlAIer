#ifndef OVERLAIER_PROCESSOR_H
#define OVERLAIER_PROCESSOR_H

#include <stdint.h>
#include "../common/pixfmt.h"
#include "../overlay/overlay.h"

// --- Processor Interface ---
// Each processor implements this struct. Processors run in their own thread,
// receive video frames, and emit overlay primitives.

struct ovl_processor_def {
    const char *name; // e.g. "face-detect", "fps-counter"

    // Desired input format. The manager converts frames as needed.
    struct {
        enum ovl_pixfmt format; // OVL_PIXFMT_UNKNOWN = same as capture (zero-copy)
        uint32_t width, height; // 0 = same as capture
        int max_fps;            // 0 = every frame, >0 = throttle
    } input;

    // Lifecycle callbacks:

    // Called once when the processor is started.
    // width/height are the actual input dimensions after any scaling.
    // format is the pixel format of frames delivered to process().
    // Returns opaque state pointer (passed to process/destroy).
    void *(*init)(const struct ovl_processor_def *def, uint32_t width, uint32_t height,
                  enum ovl_pixfmt format);

    // Called with each frame (at the processor's requested rate).
    // frame_data: pixel buffer in the requested format.
    // Must set *prims_out and *count_out. The array must remain valid
    // until the next process() call or destroy().
    void (*process)(void *state, const void *frame_data, uint32_t width, uint32_t height,
                    uint32_t stride, struct ovl_primitive **prims_out, int *count_out);

    // Called once when the processor is stopped.
    void (*destroy)(void *state);
};

// Shared library processors must export this symbol:
//   const struct ovl_processor_def *ovl_processor_register(void);
// The function returns a pointer to the processor definition.
#define OVL_PROCESSOR_EXPORT_SYMBOL "ovl_processor_register"

typedef const struct ovl_processor_def *(*ovl_processor_register_fn)(void);

#endif

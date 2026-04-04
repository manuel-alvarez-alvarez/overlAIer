#ifndef OVERLAIER_PROC_FPS_COUNTER_H
#define OVERLAIER_PROC_FPS_COUNTER_H

#include "../pipeline/processor/processor.h"

// FPS counter processor: displays a frame counter overlay.
// Runs at 2fps (only needs to update twice per second).
extern const struct ovl_processor_def ovl_proc_fps_counter;

#endif

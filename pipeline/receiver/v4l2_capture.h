#ifndef OVERLAIER_V4L2_CAPTURE_H
#define OVERLAIER_V4L2_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define OVL_V4L2_MAX_PLANES  3
#define OVL_V4L2_NUM_BUFFERS 3

struct ovl_v4l2_buffer {
    int dmabuf_fds[OVL_V4L2_MAX_PLANES];
    uint32_t offsets[OVL_V4L2_MAX_PLANES];
    uint32_t pitches[OVL_V4L2_MAX_PLANES];
    int num_planes;
    int index;
};

struct ovl_v4l2_capture {
    int fd;
    uint32_t buf_type;
    uint32_t pixelformat;
    uint32_t width;
    uint32_t height;
    uint32_t fps;   // detected framerate (0 if unknown)
    int num_planes; // from format, not from buffer query
    uint32_t num_buffers;
    struct ovl_v4l2_buffer buffers[OVL_V4L2_NUM_BUFFERS];
};

struct ovl_v4l2_dequeue_info {
    uint32_t sequence;     // V4L2 buffer sequence number
    uint64_t timestamp_us; // dequeue timestamp in microseconds (CLOCK_MONOTONIC)
};

struct ovl_v4l2_capture_config {
    uint32_t pixelformat; // V4L2 fourcc, 0 = device default
    uint32_t width;       // 0 = device default
    uint32_t height;      // 0 = device default
    uint32_t framerate;   // fps, 0 = device default
};

// List V4L2 supported pixel formats. Returns count, fills fmts (up to max_fmts).
int ovl_v4l2_enum_formats(const char *device, uint32_t *fmts, int max_fmts);

// Open device, set format, allocate buffers, export DMABUFs.
// Fields in cfg that are 0 use the device default.
int ovl_v4l2_capture_init(struct ovl_v4l2_capture *cap, const char *device,
                          const struct ovl_v4l2_capture_config *cfg);

// Start streaming
int ovl_v4l2_capture_start(struct ovl_v4l2_capture *cap);

// Dequeue a filled buffer (blocks until frame available). Returns buffer index.
// If dq_info is non-NULL, fills it with sequence and timestamp.
int ovl_v4l2_capture_dequeue(struct ovl_v4l2_capture *cap, struct ovl_v4l2_dequeue_info *dq_info);

// Non-blocking dequeue. Returns buffer index, or -1 if no frame ready.
// If dq_info is non-NULL, fills it with sequence and timestamp.
int ovl_v4l2_capture_dequeue_nb(struct ovl_v4l2_capture *cap, struct ovl_v4l2_dequeue_info *dq_info);

// Requeue a buffer for capture
int ovl_v4l2_capture_queue(struct ovl_v4l2_capture *cap, int index);

// Stop streaming and free resources
void ovl_v4l2_capture_stop(struct ovl_v4l2_capture *cap);
void ovl_v4l2_capture_free(struct ovl_v4l2_capture *cap);

#endif

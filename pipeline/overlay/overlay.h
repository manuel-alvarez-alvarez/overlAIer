#ifndef OVERLAIER_OVERLAY_H
#define OVERLAIER_OVERLAY_H

#include <stdint.h>
#include "../common/pixfmt.h"

// --- Overlay Primitive DSL ---
// All coordinates and sizes are normalized [0.0, 1.0] relative to frame dimensions.
// (0,0) = top-left, (1,1) = bottom-right.
// Stroke widths and font sizes are relative to frame height.

enum ovl_prim_type {
    OVL_PRIM_RECT,
    OVL_PRIM_CIRCLE,
    OVL_PRIM_ELLIPSE,
    OVL_PRIM_LINE,
    OVL_PRIM_POLYLINE,
    OVL_PRIM_POLYGON,
    OVL_PRIM_ARC,
    OVL_PRIM_BEZIER,
    OVL_PRIM_TEXT,
    OVL_PRIM_IMAGE,
};

struct ovl_color {
    float r, g, b, a; // [0.0, 1.0]
};

struct ovl_stroke {
    float width; // relative to frame height (0.002 = 0.2% of height)
    struct ovl_color color;
};

struct ovl_point {
    float x, y; // normalized [0,1]
};

struct ovl_primitive {
    enum ovl_prim_type type;
    struct ovl_color fill;    // fill color (a=0 for no fill)
    struct ovl_stroke stroke; // stroke (width=0 for no stroke)

    union {
        struct {
            float x, y, w, h;
            float corner_radius; // 0 = sharp corners
        } rect;

        struct {
            float cx, cy, r;
        } circle;

        struct {
            float cx, cy, rx, ry;
        } ellipse;

        struct {
            float x1, y1, x2, y2;
        } line;

        struct {
            const struct ovl_point *points;
            int count;
        } polyline;

        struct {
            const struct ovl_point *points;
            int count;
        } polygon;

        struct {
            float cx, cy, r;
            float start_angle, end_angle; // radians
        } arc;

        struct {
            float x1, y1;   // start
            float cx1, cy1; // control point 1
            float cx2, cy2; // control point 2
            float x2, y2;   // end
        } bezier;

        struct {
            float x, y;
            const char *text;
            const char *font; // e.g. "Sans Bold", "Monospace 12"
            float size;       // relative to frame height (0.03 = 3%)
        } text;

        struct {
            float x, y, w, h;
            const void *data; // ARGB8888 pixel data
            int data_w, data_h;
        } image;
    };
};

// --- Overlay Renderer ---

struct ovl_overlay;

// Create overlay renderer. Cairo renders internally as ARGB32;
// if target_fmt differs, output is converted automatically.
int ovl_overlay_create(struct ovl_overlay **overlay, uint32_t width, uint32_t height,
                       enum ovl_pixfmt target_fmt);

// Clear to fully transparent.
void ovl_overlay_clear(struct ovl_overlay *overlay);

// Render an array of primitives.
void ovl_overlay_render(struct ovl_overlay *overlay, const struct ovl_primitive *prims, int count);

// Get the rendered pixel buffer (in target_fmt).
void *ovl_overlay_get_buffer(struct ovl_overlay *overlay);

// Get output format, stride, dimensions.
enum ovl_pixfmt ovl_overlay_get_format(const struct ovl_overlay *overlay);
uint32_t ovl_overlay_get_stride(const struct ovl_overlay *overlay);
uint32_t ovl_overlay_get_width(const struct ovl_overlay *overlay);
uint32_t ovl_overlay_get_height(const struct ovl_overlay *overlay);

void ovl_overlay_destroy(struct ovl_overlay *overlay);

#endif

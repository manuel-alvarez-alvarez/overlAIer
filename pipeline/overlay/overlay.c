#include "overlay.h"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../common/log.h"

struct ovl_overlay {
    uint32_t width, height;
    enum ovl_pixfmt target_fmt;

    // Cairo renders here (always ARGB32)
    cairo_surface_t *surface;
    cairo_t *cr;

    // If target_fmt != ARGB8888, we convert into this buffer
    void *convert_buf;
    uint32_t convert_stride;

    // Pango font cache
    PangoFontDescription *cached_font_desc;
    char cached_font_name[128];
    float cached_font_size;
};

// --- Coordinate helpers ---

static inline double nx(const struct ovl_overlay *o, float v) {
    return (double)v * o->width;
}
static inline double ny(const struct ovl_overlay *o, float v) {
    return (double)v * o->height;
}
static inline double ns(const struct ovl_overlay *o, float v) {
    return (double)v * o->height;
} // size relative to height

// --- Fill & stroke ---

static void apply_fill(cairo_t *cr, const struct ovl_color *c) {
    if (c->a <= 0.0f)
        return;
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    cairo_fill_preserve(cr);
}

static void apply_stroke(cairo_t *cr, const struct ovl_overlay *o, const struct ovl_stroke *s) {
    if (s->width <= 0.0f)
        return;
    cairo_set_source_rgba(cr, s->color.r, s->color.g, s->color.b, s->color.a);
    cairo_set_line_width(cr, ns(o, s->width));
    cairo_stroke_preserve(cr);
}

static void fill_and_stroke(cairo_t *cr, const struct ovl_overlay *o, const struct ovl_color *fill,
                            const struct ovl_stroke *stroke) {
    apply_fill(cr, fill);
    apply_stroke(cr, o, stroke);
    cairo_new_path(cr);
}

// --- Primitive renderers ---

static void render_rect(const struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    double x = nx(o, p->rect.x), y = ny(o, p->rect.y);
    double w = nx(o, p->rect.w), h = ny(o, p->rect.h);
    double r = ns(o, p->rect.corner_radius);

    if (r > 0) {
        double deg = M_PI / 180.0;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -90 * deg, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, 90 * deg);
        cairo_arc(cr, x + r, y + h - r, r, 90 * deg, 180 * deg);
        cairo_arc(cr, x + r, y + r, r, 180 * deg, 270 * deg);
        cairo_close_path(cr);
    } else {
        cairo_rectangle(cr, x, y, w, h);
    }
    fill_and_stroke(cr, o, &p->fill, &p->stroke);
}

static void render_circle(const struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    cairo_arc(cr, nx(o, p->circle.cx), ny(o, p->circle.cy), ns(o, p->circle.r), 0, 2 * M_PI);
    fill_and_stroke(cr, o, &p->fill, &p->stroke);
}

static void render_ellipse(const struct ovl_overlay *o, cairo_t *cr,
                           const struct ovl_primitive *p) {
    double cx = nx(o, p->ellipse.cx), cy = ny(o, p->ellipse.cy);
    double rx = nx(o, p->ellipse.rx), ry = ny(o, p->ellipse.ry);

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
    cairo_restore(cr);
    fill_and_stroke(cr, o, &p->fill, &p->stroke);
}

static void render_line(const struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    cairo_move_to(cr, nx(o, p->line.x1), ny(o, p->line.y1));
    cairo_line_to(cr, nx(o, p->line.x2), ny(o, p->line.y2));
    apply_stroke(cr, o, &p->stroke);
    cairo_new_path(cr);
}

static void render_polyline(const struct ovl_overlay *o, cairo_t *cr,
                            const struct ovl_primitive *p) {
    if (p->polyline.count < 2)
        return;
    cairo_move_to(cr, nx(o, p->polyline.points[0].x), ny(o, p->polyline.points[0].y));
    for (int i = 1; i < p->polyline.count; i++)
        cairo_line_to(cr, nx(o, p->polyline.points[i].x), ny(o, p->polyline.points[i].y));
    apply_stroke(cr, o, &p->stroke);
    cairo_new_path(cr);
}

static void render_polygon(const struct ovl_overlay *o, cairo_t *cr,
                           const struct ovl_primitive *p) {
    if (p->polygon.count < 3)
        return;
    cairo_move_to(cr, nx(o, p->polygon.points[0].x), ny(o, p->polygon.points[0].y));
    for (int i = 1; i < p->polygon.count; i++)
        cairo_line_to(cr, nx(o, p->polygon.points[i].x), ny(o, p->polygon.points[i].y));
    cairo_close_path(cr);
    fill_and_stroke(cr, o, &p->fill, &p->stroke);
}

static void render_arc(const struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    cairo_arc(cr, nx(o, p->arc.cx), ny(o, p->arc.cy), ns(o, p->arc.r), p->arc.start_angle,
              p->arc.end_angle);
    fill_and_stroke(cr, o, &p->fill, &p->stroke);
}

static void render_bezier(const struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    cairo_move_to(cr, nx(o, p->bezier.x1), ny(o, p->bezier.y1));
    cairo_curve_to(cr, nx(o, p->bezier.cx1), ny(o, p->bezier.cy1), nx(o, p->bezier.cx2),
                   ny(o, p->bezier.cy2), nx(o, p->bezier.x2), ny(o, p->bezier.y2));
    apply_stroke(cr, o, &p->stroke);
    cairo_new_path(cr);
}

static void render_text(struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    const char *font = p->text.font ? p->text.font : "Sans";
    float size = p->text.size;

    // Cache font description
    if (!o->cached_font_desc || strcmp(o->cached_font_name, font) != 0 ||
        o->cached_font_size != size) {
        if (o->cached_font_desc)
            pango_font_description_free(o->cached_font_desc);
        o->cached_font_desc = pango_font_description_from_string(font);
        pango_font_description_set_absolute_size(o->cached_font_desc, ns(o, size) * PANGO_SCALE);
        snprintf(o->cached_font_name, sizeof(o->cached_font_name), "%s", font);
        o->cached_font_size = size;
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, o->cached_font_desc);
    pango_layout_set_text(layout, p->text.text, -1);

    cairo_set_source_rgba(cr, p->fill.r, p->fill.g, p->fill.b, p->fill.a);
    cairo_move_to(cr, nx(o, p->text.x), ny(o, p->text.y));
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

static void render_image(const struct ovl_overlay *o, cairo_t *cr, const struct ovl_primitive *p) {
    if (!p->image.data || p->image.data_w <= 0 || p->image.data_h <= 0)
        return;

    cairo_surface_t *img =
        cairo_image_surface_create_for_data((unsigned char *)p->image.data, CAIRO_FORMAT_ARGB32,
                                            p->image.data_w, p->image.data_h, p->image.data_w * 4);

    double dx = nx(o, p->image.x), dy = ny(o, p->image.y);
    double dw = nx(o, p->image.w), dh = ny(o, p->image.h);
    double sx = dw / p->image.data_w, sy = dh / p->image.data_h;

    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(img);
}

// --- Format conversion ---

static void convert_argb32_to_format(const void *src, void *dst, uint32_t w, uint32_t h,
                                     uint32_t src_stride, uint32_t dst_stride,
                                     enum ovl_pixfmt fmt) {
    // Cairo ARGB32 on LE = B,G,R,A in memory = DRM ARGB8888
    if (fmt == OVL_PIXFMT_ARGB8888) {
        if (src != dst) {
            for (uint32_t y = 0; y < h; y++)
                memcpy((char *)dst + y * dst_stride, (const char *)src + y * src_stride, w * 4);
        }
        return;
    }

    // ABGR8888: swap R and B
    if (fmt == OVL_PIXFMT_ABGR8888) {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *s = (const uint32_t *)((const char *)src + y * src_stride);
            uint32_t *d = (uint32_t *)((char *)dst + y * dst_stride);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t px = s[x];
                d[x] = (px & 0xFF00FF00) | ((px & 0xFF) << 16) | ((px >> 16) & 0xFF);
            }
        }
        return;
    }

    // XRGB8888: force alpha to 0xFF
    if (fmt == OVL_PIXFMT_XRGB8888) {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *s = (const uint32_t *)((const char *)src + y * src_stride);
            uint32_t *d = (uint32_t *)((char *)dst + y * dst_stride);
            for (uint32_t x = 0; x < w; x++)
                d[x] = s[x] | 0xFF000000;
        }
        return;
    }

    // XBGR8888: swap R/B + force alpha
    if (fmt == OVL_PIXFMT_XBGR8888) {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *s = (const uint32_t *)((const char *)src + y * src_stride);
            uint32_t *d = (uint32_t *)((char *)dst + y * dst_stride);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t px = s[x];
                d[x] = 0xFF000000 | (px & 0x0000FF00) | ((px & 0xFF) << 16) | ((px >> 16) & 0xFF);
            }
        }
        return;
    }

    ZF_LOGW("overlay: unsupported target format %s, using ARGB8888", ovl_pixfmt_name(fmt));
}

// --- Public API ---

int ovl_overlay_create(struct ovl_overlay **out, uint32_t width, uint32_t height,
                       enum ovl_pixfmt target_fmt) {
    struct ovl_overlay *o = calloc(1, sizeof(*o));
    if (!o)
        return -1;

    o->width = width;
    o->height = height;
    o->target_fmt = target_fmt;

    o->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)width, (int)height);
    if (cairo_surface_status(o->surface) != CAIRO_STATUS_SUCCESS) {
        ZF_LOGE("overlay: cairo surface creation failed");
        free(o);
        return -1;
    }

    o->cr = cairo_create(o->surface);

    // Allocate conversion buffer if needed
    if (target_fmt != OVL_PIXFMT_ARGB8888) {
        int bpp = ovl_pixfmt_bpp(target_fmt);
        if (bpp <= 0)
            bpp = 4;
        o->convert_stride = width * (uint32_t)bpp;
        o->convert_buf = malloc(o->convert_stride * height);
        if (!o->convert_buf) {
            cairo_destroy(o->cr);
            cairo_surface_destroy(o->surface);
            free(o);
            return -1;
        }
    }

    ZF_LOGI("overlay: created %ux%u target=%s", width, height, ovl_pixfmt_name(target_fmt));
    *out = o;
    return 0;
}

void ovl_overlay_clear(struct ovl_overlay *o) {
    if (!o)
        return;
    cairo_save(o->cr);
    cairo_set_operator(o->cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(o->cr);
    cairo_restore(o->cr);
}

void ovl_overlay_render(struct ovl_overlay *o, const struct ovl_primitive *prims, int count) {
    if (!o || !prims || count <= 0)
        return;

    cairo_set_operator(o->cr, CAIRO_OPERATOR_OVER);

    for (int i = 0; i < count; i++) {
        const struct ovl_primitive *p = &prims[i];
        switch (p->type) {
        case OVL_PRIM_RECT:
            render_rect(o, o->cr, p);
            break;
        case OVL_PRIM_CIRCLE:
            render_circle(o, o->cr, p);
            break;
        case OVL_PRIM_ELLIPSE:
            render_ellipse(o, o->cr, p);
            break;
        case OVL_PRIM_LINE:
            render_line(o, o->cr, p);
            break;
        case OVL_PRIM_POLYLINE:
            render_polyline(o, o->cr, p);
            break;
        case OVL_PRIM_POLYGON:
            render_polygon(o, o->cr, p);
            break;
        case OVL_PRIM_ARC:
            render_arc(o, o->cr, p);
            break;
        case OVL_PRIM_BEZIER:
            render_bezier(o, o->cr, p);
            break;
        case OVL_PRIM_TEXT:
            render_text(o, o->cr, p);
            break;
        case OVL_PRIM_IMAGE:
            render_image(o, o->cr, p);
            break;
        }
    }

    cairo_surface_flush(o->surface);

    // Convert if needed
    if (o->convert_buf) {
        convert_argb32_to_format(cairo_image_surface_get_data(o->surface), o->convert_buf, o->width,
                                 o->height, (uint32_t)cairo_image_surface_get_stride(o->surface),
                                 o->convert_stride, o->target_fmt);
    }
}

void *ovl_overlay_get_buffer(struct ovl_overlay *o) {
    if (!o)
        return NULL;
    if (o->convert_buf)
        return o->convert_buf;
    return cairo_image_surface_get_data(o->surface);
}

enum ovl_pixfmt ovl_overlay_get_format(const struct ovl_overlay *o) {
    return o ? o->target_fmt : OVL_PIXFMT_UNKNOWN;
}

uint32_t ovl_overlay_get_stride(const struct ovl_overlay *o) {
    if (!o)
        return 0;
    if (o->convert_buf)
        return o->convert_stride;
    return (uint32_t)cairo_image_surface_get_stride(o->surface);
}

uint32_t ovl_overlay_get_width(const struct ovl_overlay *o) {
    return o ? o->width : 0;
}
uint32_t ovl_overlay_get_height(const struct ovl_overlay *o) {
    return o ? o->height : 0;
}

void ovl_overlay_destroy(struct ovl_overlay *o) {
    if (!o)
        return;
    if (o->cached_font_desc)
        pango_font_description_free(o->cached_font_desc);
    if (o->cr)
        cairo_destroy(o->cr);
    if (o->surface)
        cairo_surface_destroy(o->surface);
    free(o->convert_buf);
    free(o);
}

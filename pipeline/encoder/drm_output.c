#include "drm_output.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

#include "../common/log.h"

static int find_plane_for_format(int fd, uint32_t crtc_id, uint32_t fourcc,
                                 uint32_t *out_plane_id) {
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res)
        return -1;

    // Find crtc index for the pipe mask check
    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }
    drmModeFreeResources(res);
    if (crtc_index < 0)
        return -1;

    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
    if (!planes)
        return -1;

    // First try primary planes, then overlay
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < planes->count_planes; i++) {
            drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[i]);
            if (!plane)
                continue;

            // Check if this plane can work with our CRTC
            if (!(plane->possible_crtcs & (1u << crtc_index))) {
                drmModeFreePlane(plane);
                continue;
            }

            // Skip planes currently bound to a different CRTC
            // (e.g. in use by the compositor on another output)
            if (plane->crtc_id != 0 && plane->crtc_id != crtc_id) {
                drmModeFreePlane(plane);
                continue;
            }

            // Get plane type
            uint64_t type = UINT64_MAX;
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
                    if (prop && strcmp(prop->name, "type") == 0)
                        type = props->prop_values[j];
                    if (prop)
                        drmModeFreeProperty(prop);
                    if (type != UINT64_MAX)
                        break;
                }
                drmModeFreeObjectProperties(props);
            }

            // Pass 0: primary planes. Pass 1: overlay planes.
            if (pass == 0 && type != DRM_PLANE_TYPE_PRIMARY) {
                drmModeFreePlane(plane);
                continue;
            }
            if (pass == 1 && type != DRM_PLANE_TYPE_OVERLAY) {
                drmModeFreePlane(plane);
                continue;
            }

            // Check if the plane supports our format
            for (uint32_t f = 0; f < plane->count_formats; f++) {
                if (plane->formats[f] == fourcc) {
                    *out_plane_id = plane->plane_id;
                    drmModeFreePlane(plane);
                    drmModeFreePlaneResources(planes);
                    return 0;
                }
            }

            drmModeFreePlane(plane);
        }
    }

    drmModeFreePlaneResources(planes);
    return -1;
}

// Find a free CRTC that can drive the given connector.
// Returns the CRTC id, or 0 if none found.
static uint32_t find_crtc_for_connector(int fd, drmModeResPtr res, drmModeConnectorPtr conn) {
    // If connector already has an encoder with a CRTC, use it
    if (conn->encoder_id) {
        drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc && enc->crtc_id) {
            uint32_t crtc = enc->crtc_id;
            drmModeFreeEncoder(enc);
            return crtc;
        }
        if (enc)
            drmModeFreeEncoder(enc);
    }

    // Otherwise, find a free CRTC from the connector's possible encoders
    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
            continue;

        for (int c = 0; c < res->count_crtcs; c++) {
            if (!(enc->possible_crtcs & (1u << c)))
                continue;

            // Check if this CRTC is free (not used by another connector)
            int in_use = 0;
            for (int j = 0; j < res->count_connectors; j++) {
                drmModeConnectorPtr other = drmModeGetConnector(fd, res->connectors[j]);
                if (!other)
                    continue;
                if (other->connector_id != conn->connector_id && other->encoder_id) {
                    drmModeEncoderPtr other_enc = drmModeGetEncoder(fd, other->encoder_id);
                    if (other_enc && other_enc->crtc_id == res->crtcs[c])
                        in_use = 1;
                    if (other_enc)
                        drmModeFreeEncoder(other_enc);
                }
                drmModeFreeConnector(other);
                if (in_use)
                    break;
            }

            if (!in_use) {
                uint32_t crtc = res->crtcs[c];
                drmModeFreeEncoder(enc);
                return crtc;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return 0;
}

// require that exact connector. Otherwise use the first connected output with
// a usable CRTC. Handles both already-configured connectors and unconfigured
// ones (finds a free CRTC).
static int find_connected_output(int fd, const char *connector_name, uint32_t *connector_id,
                                 uint32_t *crtc_id) {
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res)
        return -1;

    // First pass: look for the named connector
    if (connector_name && connector_name[0]) {
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
            if (!conn)
                continue;

            char name[32];
            snprintf(name, sizeof(name), "%s-%u", drmModeGetConnectorTypeName(conn->connector_type),
                     conn->connector_type_id);

            if (conn->connection == DRM_MODE_CONNECTED && strcmp(name, connector_name) == 0) {
                uint32_t crtc = find_crtc_for_connector(fd, res, conn);
                if (crtc) {
                    *connector_id = conn->connector_id;
                    *crtc_id = crtc;
                    drmModeFreeConnector(conn);
                    drmModeFreeResources(res);
                    ZF_LOGI("drm: using requested connector %s (crtc=%u)", connector_name, crtc);
                    return 0;
                }
            }
            drmModeFreeConnector(conn);
        }
        ZF_LOGE("drm: requested connector %s not found, disconnected, or has no usable CRTC",
                connector_name);
        drmModeFreeResources(res);
        return -1;
    }

    // Second pass: first connected connector with a usable CRTC
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;

        if (conn->connection == DRM_MODE_CONNECTED) {
            uint32_t crtc = find_crtc_for_connector(fd, res, conn);
            if (crtc) {
                *connector_id = conn->connector_id;
                *crtc_id = crtc;
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                return 0;
            }
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return -1;
}

static struct ovl_drm_flip *alloc_flip(struct ovl_drm_output *out) {
    for (int i = 0; i < OVL_DRM_MAX_BUFFERS; i++) {
        if (!out->flips[i].in_use) {
            out->flips[i] = (struct ovl_drm_flip){0};
            out->flips[i].in_use = 1;
            return &out->flips[i];
        }
    }
    return NULL;
}

static void release_flip(struct ovl_drm_flip *flip) {
    if (!flip)
        return;
    *flip = (struct ovl_drm_flip){0};
}

static void flip_handler(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
                         void *user_data) {
    (void)fd;
    (void)seq;
    (void)sec;
    (void)usec;
    struct ovl_drm_flip *flip = user_data;
    if (!flip)
        return;
    flip->pending = 0;
    flip->done = 1;
}

int ovl_drm_output_init(struct ovl_drm_output *out, const char *device, uint32_t fourcc,
                        uint32_t width, uint32_t height, uint32_t out_w, uint32_t out_h,
                        uint32_t out_fps) {
    memset(out, 0, sizeof(*out));

    // Parse "device:connector" format (e.g. "/dev/dri/card0:HDMI-A-2")
    char dev_path[128];
    const char *connector_name = NULL;
    snprintf(dev_path, sizeof(dev_path), "%s", device);
    char *colon = strchr(dev_path, ':');
    if (colon) {
        *colon = '\0';
        connector_name = colon + 1;
    }

    out->fd = open(dev_path, O_RDWR);
    if (out->fd < 0) {
        ZF_LOGE("open: %s", strerror(errno));
        return -1;
    }

    drmSetClientCap(out->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    uint64_t cap_val = 0;
    if (drmGetCap(out->fd, DRM_CAP_ASYNC_PAGE_FLIP, &cap_val) == 0 && cap_val)
        out->async_supported = 1;

    if (find_connected_output(out->fd, connector_name, &out->connector_id, &out->crtc_id) < 0) {
        if (connector_name && connector_name[0])
            ZF_LOGE("drm: failed to claim requested connector %s", connector_name);
        else
            ZF_LOGE("no connected output found");
        close(out->fd);
        return -1;
    }

    if (find_plane_for_format(out->fd, out->crtc_id, fourcc, &out->plane_id) < 0) {
        ZF_LOGE("no plane supports format 0x%08x", fourcc);
        close(out->fd);
        return -1;
    }

    out->width = width;
    out->height = height;

    // Set output mode if requested, otherwise use current CRTC mode
    if (out_w && out_h) {
        drmModeConnectorPtr conn = drmModeGetConnector(out->fd, out->connector_id);
        if (conn) {
            drmModeModeInfoPtr best = NULL;
            for (int i = 0; i < conn->count_modes; i++) {
                drmModeModeInfoPtr m = &conn->modes[i];
                if (m->hdisplay == out_w && m->vdisplay == out_h) {
                    if (out_fps) {
                        // Match specific refresh rate
                        if (m->vrefresh == out_fps) {
                            best = m;
                            break;
                        }
                        // Keep closest if exact not found
                        if (!best || (unsigned)abs((int)m->vrefresh - (int)out_fps) <
                                         (unsigned)abs((int)best->vrefresh - (int)out_fps))
                            best = m;
                    } else {
                        // No fps specified: take first match (EDID preferred order)
                        best = m;
                        break;
                    }
                }
            }
            if (best) {
                ZF_LOGI("drm: setting mode %ux%u@%uHz", best->hdisplay, best->vdisplay,
                        best->vrefresh);
                // Get current fb to pass to SetCrtc (required, can't be 0)
                drmModeCrtcPtr cur = drmModeGetCrtc(out->fd, out->crtc_id);
                uint32_t cur_fb = cur ? cur->buffer_id : 0;
                if (cur)
                    drmModeFreeCrtc(cur);
                if (drmModeSetCrtc(out->fd, out->crtc_id, cur_fb, 0, 0, &out->connector_id, 1,
                                   best) < 0) {
                    ZF_LOGW("drm: drmModeSetCrtc failed: %s", strerror(errno));
                }
                out->crtc_w = best->hdisplay;
                out->crtc_h = best->vdisplay;
            } else {
                ZF_LOGW("drm: mode %ux%u not found, using current", out_w, out_h);
                drmModeCrtcPtr crtc = drmModeGetCrtc(out->fd, out->crtc_id);
                if (crtc) {
                    out->crtc_w = crtc->mode.hdisplay;
                    out->crtc_h = crtc->mode.vdisplay;
                    drmModeFreeCrtc(crtc);
                }
            }
            drmModeFreeConnector(conn);
        }
    } else {
        drmModeCrtcPtr crtc = drmModeGetCrtc(out->fd, out->crtc_id);
        if (crtc) {
            out->crtc_w = crtc->mode.hdisplay;
            out->crtc_h = crtc->mode.vdisplay;
            drmModeFreeCrtc(crtc);
        } else {
            out->crtc_w = width;
            out->crtc_h = height;
        }
    }

    // Enable atomic modesetting for non-blocking page flips
    drmSetClientCap(out->fd, DRM_CLIENT_CAP_ATOMIC, 1);

    // Disable all other planes on our CRTC (cursor, unused overlays)
    // to remove stale content like the kernel console cursor
    {
        drmModeResPtr r = drmModeGetResources(out->fd);
        int crtc_idx = -1;
        if (r) {
            for (int i = 0; i < r->count_crtcs; i++)
                if (r->crtcs[i] == out->crtc_id) {
                    crtc_idx = i;
                    break;
                }
            drmModeFreeResources(r);
        }
        if (crtc_idx >= 0) {
            drmModePlaneResPtr pl = drmModeGetPlaneResources(out->fd);
            if (pl) {
                for (uint32_t i = 0; i < pl->count_planes; i++) {
                    drmModePlanePtr p = drmModeGetPlane(out->fd, pl->planes[i]);
                    if (!p)
                        continue;
                    if ((p->possible_crtcs & (1u << crtc_idx)) && p->plane_id != out->plane_id) {
                        // Disable this plane
                        drmModeSetPlane(out->fd, p->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                    }
                    drmModeFreePlane(p);
                }
                drmModeFreePlaneResources(pl);
            }
        }
    }

    ZF_LOGD("drm connector=%u crtc=%u plane=%u %ux%u -> %ux%u", out->connector_id, out->crtc_id,
            out->plane_id, width, height, out->crtc_w, out->crtc_h);
    return 0;
}

int ovl_drm_output_add_fb(struct ovl_drm_output *out, int dmabuf_fd, uint32_t fourcc,
                          uint32_t width, uint32_t height, uint32_t pitch) {
    int fds[1] = {dmabuf_fd};
    uint32_t pitches[1] = {pitch};
    uint32_t offsets[1] = {0};
    return ovl_drm_output_add_fb_mp(out, fds, pitches, offsets, 1, fourcc, width, height);
}

// Returns the number of DRM planes for a given fourcc
static int drm_format_num_planes(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61:
    case DRM_FORMAT_NV24:
    case DRM_FORMAT_NV42:
    case DRM_FORMAT_NV15:
    case DRM_FORMAT_NV20:
    case DRM_FORMAT_NV30:
        return 2;
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
        return 3;
    default:
        return 1;
    }
}

// For semi-planar NV formats, compute plane 1 (CbCr) pitch and offset
// given the Y pitch, height, and subsampling from the fourcc.
static void nv_plane_layout(uint32_t fourcc, uint32_t y_pitch, uint32_t height, uint32_t *uv_pitch,
                            uint32_t *uv_offset) {
    // Y plane size = y_pitch * height
    *uv_offset = y_pitch * height;

    switch (fourcc) {
    case DRM_FORMAT_NV24:
    case DRM_FORMAT_NV42:
        // 4:4:4 — CbCr has 2 bytes per pixel
        *uv_pitch = y_pitch * 2;
        break;
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61:
        // 4:2:2 — CbCr is same width, half height implied by single offset
        *uv_pitch = y_pitch;
        break;
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    default:
        // 4:2:0 — CbCr is half width, 2 components
        *uv_pitch = y_pitch;
        break;
    }
}

int ovl_drm_output_add_fb_mp(struct ovl_drm_output *out, int *dmabuf_fds, uint32_t *pitches,
                             uint32_t *offsets, int num_planes, uint32_t fourcc, uint32_t width,
                             uint32_t height) {
    if (out->num_fbs >= OVL_DRM_MAX_BUFFERS)
        return -1;

    int drm_planes = drm_format_num_planes(fourcc);

    struct ovl_drm_fb *fb = &out->fbs[out->num_fbs];
    fb->num_planes = num_planes;

    uint32_t handles[4] = {0};
    uint32_t p[4] = {0};
    uint32_t o[4] = {0};

    // Import all V4L2 planes as GEM handles
    for (int i = 0; i < num_planes; i++) {
        uint32_t handle;
        if (drmPrimeFDToHandle(out->fd, dmabuf_fds[i], &handle) < 0) {
            ZF_LOGE("drmPrimeFDToHandle: %s", strerror(errno));
            return -1;
        }
        fb->gem_handles[i] = handle;
        handles[i] = handle;
        p[i] = pitches[i];
        o[i] = offsets[i];
    }

    // If V4L2 gave us fewer planes than DRM expects (e.g. single buffer
    // containing both Y and CbCr), construct the extra planes from the
    // same handle with computed offsets.
    if (num_planes < drm_planes && drm_planes == 2) {
        uint32_t uv_pitch, uv_offset;
        nv_plane_layout(fourcc, p[0], height, &uv_pitch, &uv_offset);
        handles[1] = handles[0]; // same buffer
        p[1] = uv_pitch;
        o[1] = o[0] + uv_offset;
        fb->gem_handles[1] = handles[0];
    }

    ZF_LOGD("drm fb fourcc=0x%08x %ux%u planes=%d", fourcc, width, height, drm_planes);

    if (drmModeAddFB2(out->fd, width, height, fourcc, handles, p, o, &fb->fb_id, 0) < 0) {
        ZF_LOGE("drmModeAddFB2: %s", strerror(errno));
        return -1;
    }

    ZF_LOGD("drm fb %u created", fb->fb_id);
    return out->num_fbs++;
}

// Property ID cache (looked up once, reused; reset by ovl_drm_output_free)
// Video plane props
static uint32_t prop_fb_id = 0;
static uint32_t prop_crtc_id = 0;
static uint32_t prop_crtc_x = 0, prop_crtc_y = 0;
static uint32_t prop_crtc_w = 0, prop_crtc_h = 0;
static uint32_t prop_src_x = 0, prop_src_y = 0;
static uint32_t prop_src_w = 0, prop_src_h = 0;
// Overlay plane props (separate because it's a different plane)
static uint32_t ovl_prop_fb_id = 0;
static uint32_t ovl_prop_crtc_id = 0;
static uint32_t ovl_prop_crtc_x = 0, ovl_prop_crtc_y = 0;
static uint32_t ovl_prop_crtc_w = 0, ovl_prop_crtc_h = 0;
static uint32_t ovl_prop_src_x = 0, ovl_prop_src_y = 0;
static uint32_t ovl_prop_src_w = 0, ovl_prop_src_h = 0;

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props)
        return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (prop && strcmp(prop->name, name) == 0)
            id = prop->prop_id;
        if (prop)
            drmModeFreeProperty(prop);
        if (id)
            break;
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static void cache_plane_props(int fd, uint32_t plane_id) {
    prop_fb_id = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    prop_crtc_id = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    prop_crtc_x = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    prop_crtc_y = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    prop_crtc_w = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    prop_crtc_h = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    prop_src_x = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    prop_src_y = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    prop_src_w = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    prop_src_h = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
}

int ovl_drm_output_show(struct ovl_drm_output *out, int fb_index, int capture_index) {
    if (fb_index < 0 || fb_index >= out->num_fbs)
        return -1;

    // Cache property IDs (reset to 0 by ovl_drm_output_free, re-cached here)
    if (!prop_fb_id) {
        cache_plane_props(out->fd, out->plane_id);
        if (prop_fb_id)
            ZF_LOGI("drm: atomic nonblock path (plane props cached)");
        else
            ZF_LOGI("drm: legacy SetPlane path (no plane props)");
    }

    // Use atomic commit with NONBLOCK for lowest latency
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (!req) {
        int ret =
            drmModeSetPlane(out->fd, out->plane_id, out->crtc_id, out->fbs[fb_index].fb_id, 0, 0, 0,
                            out->crtc_w, out->crtc_h, 0, 0, out->width << 16, out->height << 16);
        if (ret == 0) {
            struct ovl_drm_flip *flip = alloc_flip(out);
            if (flip) {
                flip->capture_index = capture_index;
                flip->done = 1;
            }
        }
        return ret;
    }

    struct ovl_drm_flip *flip = alloc_flip(out);
    if (!flip) {
        ovl_drm_output_acquire_ready(out, 16, NULL);
        flip = alloc_flip(out);
        if (!flip) {
            drmModeAtomicFree(req);
            ZF_LOGE("drm: no flip slots available");
            return -1;
        }
    }

    flip->capture_index = capture_index;
    flip->fb_index = fb_index;
    flip->pending = 1;
    flip->done = 0;

    drmModeAtomicAddProperty(req, out->plane_id, prop_fb_id, out->fbs[fb_index].fb_id);
    drmModeAtomicAddProperty(req, out->plane_id, prop_crtc_id, out->crtc_id);
    drmModeAtomicAddProperty(req, out->plane_id, prop_crtc_x, 0);
    drmModeAtomicAddProperty(req, out->plane_id, prop_crtc_y, 0);
    drmModeAtomicAddProperty(req, out->plane_id, prop_crtc_w, out->crtc_w);
    drmModeAtomicAddProperty(req, out->plane_id, prop_crtc_h, out->crtc_h);
    drmModeAtomicAddProperty(req, out->plane_id, prop_src_x, 0);
    drmModeAtomicAddProperty(req, out->plane_id, prop_src_y, 0);
    drmModeAtomicAddProperty(req, out->plane_id, prop_src_w, out->width << 16);
    drmModeAtomicAddProperty(req, out->plane_id, prop_src_h, out->height << 16);

    // Add overlay plane to the same atomic commit (if configured)
    if (out->overlay_plane_id && out->overlay_fb_id && ovl_prop_fb_id) {
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_fb_id, out->overlay_fb_id);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_crtc_id, out->crtc_id);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_crtc_x, 0);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_crtc_y, 0);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_crtc_w, out->crtc_w);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_crtc_h, out->crtc_h);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_src_x, 0);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_src_y, 0);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_src_w, out->crtc_w << 16);
        drmModeAtomicAddProperty(req, out->overlay_plane_id, ovl_prop_src_h, out->crtc_h << 16);
    }

    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    if (out->async_flip)
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;

    int ret = drmModeAtomicCommit(out->fd, req, flags, flip);

    // If async flip not supported, retry without it
    if (ret < 0 && out->async_flip && errno == EINVAL) {
        static int async_warned = 0;
        if (!async_warned) {
            ZF_LOGW("drm: async flip not supported, using nonblock vsync");
            async_warned = 1;
        }
        out->async_flip = 0;
        flags &= ~(uint32_t)DRM_MODE_PAGE_FLIP_ASYNC;
        ret = drmModeAtomicCommit(out->fd, req, flags, flip);
    }

    drmModeAtomicFree(req);

    if (ret < 0 && errno == EBUSY) {
        release_flip(flip);
        return 1; // previous flip pending, frame skipped
    }
    if (ret < 0 && (errno == EACCES || errno == EPERM)) {
        ZF_LOGE("drm: atomic commit denied — no DRM master. "
                "Is a compositor using this connector?");
        release_flip(flip);
        return -1;
    }
    if (ret < 0) {
        release_flip(flip);
        // Fallback to legacy as last resort
        int legacy =
            drmModeSetPlane(out->fd, out->plane_id, out->crtc_id, out->fbs[fb_index].fb_id, 0, 0, 0,
                            out->crtc_w, out->crtc_h, 0, 0, out->width << 16, out->height << 16);
        if (legacy == 0) {
            struct ovl_drm_flip *done = alloc_flip(out);
            if (done) {
                done->capture_index = capture_index;
                done->done = 1;
            }
        }
        return legacy < 0 ? legacy : 0;
    }

    return 0;
}

static int find_done_flip(struct ovl_drm_output *out) {
    for (int i = 0; i < OVL_DRM_MAX_BUFFERS; i++) {
        if (out->flips[i].in_use && out->flips[i].done)
            return i;
    }
    return -1;
}

int ovl_drm_output_acquire_ready(struct ovl_drm_output *out, int timeout_ms, int *capture_index) {
    int idx = find_done_flip(out);
    if (idx >= 0) {
        if (capture_index)
            *capture_index = out->flips[idx].capture_index;
        release_flip(&out->flips[idx]);
        return 1;
    }

    if (timeout_ms == 0)
        return 0;

    struct pollfd pfd = {.fd = out->fd, .events = POLLIN};
    int pret;
    do {
        pret = poll(&pfd, 1, timeout_ms);
    } while (pret < 0 && errno == EINTR);

    if (pret < 0) {
        ZF_LOGE("drm poll: %s", strerror(errno));
        return -1;
    }
    if (pret == 0)
        return 0;

    drmEventContext ev = {
        .version = 2,
        .page_flip_handler = flip_handler,
    };
    drmHandleEvent(out->fd, &ev);

    idx = find_done_flip(out);
    if (idx >= 0) {
        if (capture_index)
            *capture_index = out->flips[idx].capture_index;
        release_flip(&out->flips[idx]);
        return 1;
    }
    return 0;
}

int ovl_drm_output_pending(const struct ovl_drm_output *out) {
    int count = 0;
    for (int i = 0; i < OVL_DRM_MAX_BUFFERS; i++) {
        if (out->flips[i].in_use && out->flips[i].pending)
            count++;
    }
    return count;
}

// --- Overlay plane support ---

enum ovl_pixfmt ovl_drm_output_find_overlay_plane(struct ovl_drm_output *out,
                                                  const enum ovl_pixfmt *preferred_fmts,
                                                  int count) {
    drmModeResPtr res = drmModeGetResources(out->fd);
    if (!res)
        return OVL_PIXFMT_UNKNOWN;

    int crtc_idx = -1;
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == out->crtc_id) {
            crtc_idx = i;
            break;
        }
    drmModeFreeResources(res);
    if (crtc_idx < 0)
        return OVL_PIXFMT_UNKNOWN;

    drmModePlaneResPtr planes = drmModeGetPlaneResources(out->fd);
    if (!planes)
        return OVL_PIXFMT_UNKNOWN;

    // Find an overlay or cursor plane on our CRTC, not the video plane
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(out->fd, planes->planes[i]);
        if (!plane)
            continue;

        if (!(plane->possible_crtcs & (1u << crtc_idx)) || plane->plane_id == out->plane_id) {
            drmModeFreePlane(plane);
            continue;
        }

        // Skip planes bound to other CRTCs
        if (plane->crtc_id != 0 && plane->crtc_id != out->crtc_id) {
            drmModeFreePlane(plane);
            continue;
        }

        // Check if this plane supports any of our preferred formats
        for (int f = 0; f < count; f++) {
            uint32_t drm_fourcc = ovl_pixfmt_to_drm(preferred_fmts[f]);
            for (uint32_t pf = 0; pf < plane->count_formats; pf++) {
                if (plane->formats[pf] == drm_fourcc) {
                    out->overlay_plane_id = plane->plane_id;
                    out->overlay_fmt = preferred_fmts[f];
                    drmModeFreePlane(plane);
                    drmModeFreePlaneResources(planes);

                    // Cache overlay plane property IDs
                    ovl_prop_fb_id =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
                    ovl_prop_crtc_id =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
                    ovl_prop_crtc_x =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
                    ovl_prop_crtc_y =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
                    ovl_prop_crtc_w =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
                    ovl_prop_crtc_h =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
                    ovl_prop_src_x =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
                    ovl_prop_src_y =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
                    ovl_prop_src_w =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
                    ovl_prop_src_h =
                        find_prop(out->fd, out->overlay_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");

                    ZF_LOGI("drm: overlay plane %u, format %s", out->overlay_plane_id,
                            ovl_pixfmt_name(preferred_fmts[f]));
                    return preferred_fmts[f];
                }
            }
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    ZF_LOGW("drm: no overlay plane found for requested formats");
    return OVL_PIXFMT_UNKNOWN;
}

int ovl_drm_output_create_overlay_fb(struct ovl_drm_output *out, uint32_t width, uint32_t height) {
    if (!out->overlay_plane_id)
        return -1;

    uint32_t drm_fourcc = ovl_pixfmt_to_drm(out->overlay_fmt);
    int bpp = ovl_pixfmt_bpp(out->overlay_fmt);
    if (bpp <= 0)
        bpp = 4;

    struct drm_mode_create_dumb create = {
        .width = width,
        .height = height,
        .bpp = (uint32_t)(bpp * 8),
    };
    if (drmIoctl(out->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        ZF_LOGE("drm: create dumb overlay buffer: %s", strerror(errno));
        return -1;
    }

    uint32_t handles[4] = {create.handle, 0, 0, 0};
    uint32_t pitches[4] = {create.pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

    if (drmModeAddFB2(out->fd, width, height, drm_fourcc, handles, pitches, offsets,
                      &out->overlay_fb_id, 0) < 0) {
        ZF_LOGE("drm: add overlay fb: %s", strerror(errno));
        struct drm_mode_destroy_dumb destroy = {.handle = create.handle};
        drmIoctl(out->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return -1;
    }

    // Map the dumb buffer persistently
    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (drmIoctl(out->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        ZF_LOGE("drm: map overlay buffer: %s", strerror(errno));
        return -1;
    }

    out->overlay_map =
        mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, map.offset);
    if (out->overlay_map == MAP_FAILED) {
        ZF_LOGE("drm: mmap overlay buffer: %s", strerror(errno));
        out->overlay_map = NULL;
        return -1;
    }

    out->overlay_handle = create.handle;
    out->overlay_pitch = create.pitch;
    out->overlay_size = create.size;

    // Clear to transparent
    memset(out->overlay_map, 0, create.size);

    ZF_LOGI("drm: overlay fb %u created %ux%u %s (pitch=%u)", out->overlay_fb_id, width, height,
            ovl_pixfmt_name(out->overlay_fmt), create.pitch);
    return 0;
}

int ovl_drm_output_update_overlay(struct ovl_drm_output *out, const void *pixels, uint32_t stride) {
    if (!out->overlay_map || !pixels)
        return -1;

    // Copy pixels into the dumb buffer (line by line for different strides)
    uint32_t copy_bytes = stride < out->overlay_pitch ? stride : out->overlay_pitch;
    uint32_t h = out->crtc_h;
    for (uint32_t y = 0; y < h; y++) {
        memcpy((char *)out->overlay_map + y * out->overlay_pitch, (const char *)pixels + y * stride,
               copy_bytes);
    }
    return 0;
}

void ovl_drm_output_free(struct ovl_drm_output *out) {
    // Disable planes
    if (out->fd >= 0 && out->plane_id)
        drmModeSetPlane(out->fd, out->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (out->fd >= 0 && out->overlay_plane_id)
        drmModeSetPlane(out->fd, out->overlay_plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    // Clean up overlay
    if (out->overlay_map)
        munmap(out->overlay_map, out->overlay_size);
    if (out->overlay_fb_id && out->fd >= 0)
        drmModeRmFB(out->fd, out->overlay_fb_id);
    if (out->overlay_handle && out->fd >= 0) {
        struct drm_mode_destroy_dumb destroy = {.handle = out->overlay_handle};
        drmIoctl(out->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }

    for (int i = 0; i < out->num_fbs; i++) {
        if (out->fbs[i].fb_id)
            drmModeRmFB(out->fd, out->fbs[i].fb_id);
        for (int p = 0; p < out->fbs[i].num_planes; p++) {
            struct drm_gem_close close_req = {.handle = out->fbs[i].gem_handles[p]};
            drmIoctl(out->fd, DRM_IOCTL_GEM_CLOSE, &close_req);
        }
    }

    if (out->fd >= 0)
        close(out->fd);

    memset(out, 0, sizeof(*out));

    // Reset static caches
    prop_fb_id = prop_crtc_id = 0;
    prop_crtc_x = prop_crtc_y = prop_crtc_w = prop_crtc_h = 0;
    prop_src_x = prop_src_y = prop_src_w = prop_src_h = 0;
    ovl_prop_fb_id = ovl_prop_crtc_id = 0;
    ovl_prop_crtc_x = ovl_prop_crtc_y = ovl_prop_crtc_w = ovl_prop_crtc_h = 0;
    ovl_prop_src_x = ovl_prop_src_y = ovl_prop_src_w = ovl_prop_src_h = 0;
}

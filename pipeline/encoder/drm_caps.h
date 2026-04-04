#ifndef OVERLAIER_DRM_CAPS_H
#define OVERLAIER_DRM_CAPS_H

#include <stddef.h>
#include <stdint.h>

struct ovl_drm_plane_caps {
    uint32_t plane_id;
    uint32_t type; // DRM_PLANE_TYPE_PRIMARY, OVERLAY, CURSOR
    uint32_t *formats;
    size_t num_formats;
};

struct ovl_drm_mode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh; // mHz
    char name[32];
};

struct ovl_drm_connector_caps {
    uint32_t connector_id;
    uint32_t connector_type;
    char name[32];
    int connected;
    uint32_t crtc_id;
    struct ovl_drm_mode *modes;
    size_t num_modes;
    struct ovl_drm_mode current_mode;
};

struct ovl_drm_caps {
    char driver[32];
    struct ovl_drm_connector_caps *connectors;
    size_t num_connectors;
    struct ovl_drm_plane_caps *planes;
    size_t num_planes;
};

int ovl_drm_query_caps(const char *device, struct ovl_drm_caps *caps);
void ovl_drm_caps_print(const struct ovl_drm_caps *caps);
void ovl_drm_caps_free(struct ovl_drm_caps *caps);

#endif

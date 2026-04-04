#include "drm_caps.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <errno.h>
#include <xf86drmMode.h>

#include "../common/log.h"

static const char *fourcc_str(uint32_t fourcc, char buf[5]) {
    buf[0] = (char)(fourcc & 0xFF);
    buf[1] = (char)((fourcc >> 8) & 0xFF);
    buf[2] = (char)((fourcc >> 16) & 0xFF);
    buf[3] = (char)((fourcc >> 24) & 0xFF);
    buf[4] = '\0';
    return buf;
}

static const char *plane_type_str(uint64_t type) {
    switch (type) {
    case DRM_PLANE_TYPE_PRIMARY:
        return "Primary";
    case DRM_PLANE_TYPE_OVERLAY:
        return "Overlay";
    case DRM_PLANE_TYPE_CURSOR:
        return "Cursor";
    default:
        return "Unknown";
    }
}

static uint64_t get_plane_type(int fd, uint32_t plane_id) {
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return UINT64_MAX;

    uint64_t type = UINT64_MAX;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, "type") == 0)
            type = props->prop_values[i];
        drmModeFreeProperty(prop);
        if (type != UINT64_MAX)
            break;
    }
    drmModeFreeObjectProperties(props);
    return type;
}

static int query_planes(int fd, struct ovl_drm_caps *caps) {
    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
    if (!planes)
        return -1;

    caps->planes = calloc(planes->count_planes, sizeof(*caps->planes));
    if (!caps->planes) {
        drmModeFreePlaneResources(planes);
        return -1;
    }
    caps->num_planes = planes->count_planes;

    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[i]);
        if (!plane)
            continue;

        struct ovl_drm_plane_caps *pc = &caps->planes[i];
        pc->plane_id = plane->plane_id;
        pc->type = (uint32_t)get_plane_type(fd, plane->plane_id);

        pc->formats = calloc(plane->count_formats, sizeof(uint32_t));
        if (pc->formats) {
            memcpy(pc->formats, plane->formats, plane->count_formats * sizeof(uint32_t));
            pc->num_formats = plane->count_formats;
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    return 0;
}

static int query_connectors(int fd, struct ovl_drm_caps *caps) {
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res)
        return -1;

    caps->connectors = calloc(res->count_connectors, sizeof(*caps->connectors));
    if (!caps->connectors) {
        drmModeFreeResources(res);
        return -1;
    }
    caps->num_connectors = res->count_connectors;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;

        struct ovl_drm_connector_caps *cc = &caps->connectors[i];
        cc->connector_id = conn->connector_id;
        cc->connector_type = conn->connector_type;
        cc->connected = (conn->connection == DRM_MODE_CONNECTED);
        snprintf(cc->name, sizeof(cc->name), "%s-%u",
                 drmModeGetConnectorTypeName(conn->connector_type), conn->connector_type_id);

        // Current mode from encoder → CRTC
        if (conn->encoder_id) {
            drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id);
            if (enc) {
                cc->crtc_id = enc->crtc_id;
                drmModeCrtcPtr crtc = drmModeGetCrtc(fd, enc->crtc_id);
                if (crtc && crtc->mode_valid) {
                    cc->current_mode.width = crtc->mode.hdisplay;
                    cc->current_mode.height = crtc->mode.vdisplay;
                    cc->current_mode.refresh = crtc->mode.vrefresh * 1000;
                    snprintf(cc->current_mode.name, sizeof(cc->current_mode.name), "%s",
                             crtc->mode.name);
                    drmModeFreeCrtc(crtc);
                }
                drmModeFreeEncoder(enc);
            }
        }

        // All supported modes
        if (conn->count_modes > 0) {
            cc->modes = calloc(conn->count_modes, sizeof(*cc->modes));
            if (cc->modes) {
                cc->num_modes = conn->count_modes;
                for (int m = 0; m < conn->count_modes; m++) {
                    cc->modes[m].width = conn->modes[m].hdisplay;
                    cc->modes[m].height = conn->modes[m].vdisplay;
                    cc->modes[m].refresh = conn->modes[m].vrefresh * 1000;
                    snprintf(cc->modes[m].name, sizeof(cc->modes[m].name), "%s",
                             conn->modes[m].name);
                }
            }
        }

        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return 0;
}

int ovl_drm_query_caps(const char *device, struct ovl_drm_caps *caps) {
    memset(caps, 0, sizeof(*caps));

    // Strip connector suffix if present (e.g. "/dev/dri/card0:HDMI-A-2")
    char dev_path[128];
    snprintf(dev_path, sizeof(dev_path), "%s", device);
    char *colon = strchr(dev_path, ':');
    if (colon)
        *colon = '\0';

    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        ZF_LOGE("open: %s", strerror(errno));
        return -1;
    }

    // Need universal planes to see all planes and their formats
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        ZF_LOGE("DRM_CLIENT_CAP_UNIVERSAL_PLANES: %s", strerror(errno));
        close(fd);
        return -1;
    }

    drmVersionPtr ver = drmGetVersion(fd);
    if (ver) {
        snprintf(caps->driver, sizeof(caps->driver), "%s", ver->name);
        drmFreeVersion(ver);
    }

    int ret = query_connectors(fd, caps);
    if (ret == 0)
        ret = query_planes(fd, caps);

    close(fd);
    return ret;
}

void ovl_drm_caps_print(const struct ovl_drm_caps *caps) {
    printf("DRM driver: %s\n\n", caps->driver);

    printf("  Connectors: %zu\n", caps->num_connectors);
    for (size_t i = 0; i < caps->num_connectors; i++) {
        const struct ovl_drm_connector_caps *cc = &caps->connectors[i];
        printf("    [%u] %s — %s\n", cc->connector_id, cc->name,
               cc->connected ? "connected" : "disconnected");
        if (cc->current_mode.width > 0) {
            printf("      Current: %ux%u@%.1fHz (%s)\n", cc->current_mode.width,
                   cc->current_mode.height, cc->current_mode.refresh / 1000.0,
                   cc->current_mode.name);
        }
        printf("      Modes: %zu\n", cc->num_modes);
        for (size_t m = 0; m < cc->num_modes; m++) {
            printf("        %ux%u@%.1fHz (%s)\n", cc->modes[m].width, cc->modes[m].height,
                   cc->modes[m].refresh / 1000.0, cc->modes[m].name);
        }
    }

    char fcc[5];
    printf("\n  Planes: %zu\n", caps->num_planes);
    for (size_t i = 0; i < caps->num_planes; i++) {
        const struct ovl_drm_plane_caps *pc = &caps->planes[i];
        printf("    [%u] %s — %zu formats:", pc->plane_id, plane_type_str(pc->type),
               pc->num_formats);
        for (size_t f = 0; f < pc->num_formats; f++)
            printf(" %s", fourcc_str(pc->formats[f], fcc));
        printf("\n");
    }
}

void ovl_drm_caps_free(struct ovl_drm_caps *caps) {
    for (size_t i = 0; i < caps->num_connectors; i++)
        free(caps->connectors[i].modes);
    free(caps->connectors);

    for (size_t i = 0; i < caps->num_planes; i++)
        free(caps->planes[i].formats);
    free(caps->planes);

    memset(caps, 0, sizeof(*caps));
}

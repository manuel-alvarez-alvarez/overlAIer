#include "edid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/videodev2.h>
#include <libdisplay-info/cvt.h>

#include "log.h"

// --- EDID checksum ---

static uint8_t edid_checksum(const uint8_t *block) {
    uint8_t sum = 0;
    for (int i = 0; i < OVL_EDID_BLOCK_SIZE - 1; i++)
        sum += block[i];
    return (uint8_t)(256 - sum);
}

static void edid_fix_checksum(uint8_t *block) {
    block[OVL_EDID_BLOCK_SIZE - 1] = edid_checksum(block);
}

// --- Read EDID from DRM connector ---

static uint32_t find_edid_prop(int fd, uint32_t connector_id) {
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props)
        return 0;

    uint32_t edid_prop_id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, "EDID") == 0) {
            edid_prop_id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }

    if (edid_prop_id) {
        for (uint32_t i = 0; i < props->count_props; i++) {
            if (props->props[i] == edid_prop_id) {
                uint32_t blob_id = (uint32_t)props->prop_values[i];
                drmModeFreeObjectProperties(props);
                return blob_id;
            }
        }
    }

    drmModeFreeObjectProperties(props);
    return 0;
}

int ovl_edid_read_drm(const char *drm_device, const char *connector_name, uint8_t *edid,
                      size_t max_len) {
    int fd = open(drm_device, O_RDWR);
    if (fd < 0) {
        ZF_LOGE("edid: cannot open %s", drm_device);
        return -1;
    }

    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        close(fd);
        return -1;
    }

    int result = -1;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;

        if (conn->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(conn);
            continue;
        }

        if (connector_name && connector_name[0]) {
            char name[32];
            snprintf(name, sizeof(name), "%s-%u",
                     drmModeGetConnectorTypeName(conn->connector_type),
                     conn->connector_type_id);
            if (strcmp(name, connector_name) != 0) {
                drmModeFreeConnector(conn);
                continue;
            }
        }

        uint32_t blob_id = find_edid_prop(fd, conn->connector_id);
        if (blob_id) {
            drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, blob_id);
            if (blob && blob->length > 0) {
                size_t copy_len = blob->length < max_len ? blob->length : max_len;
                memcpy(edid, blob->data, copy_len);
                result = (int)copy_len;
                ZF_LOGI("edid: read %d bytes from %s", result,
                        connector_name ? connector_name : "first connected");
                drmModeFreePropertyBlob(blob);
            }
        }

        drmModeFreeConnector(conn);
        break;
    }

    drmModeFreeResources(res);
    close(fd);
    return result;
}

// --- Extract monitor name ---

int ovl_edid_get_name(const uint8_t *edid, size_t len, char *name, size_t name_len) {
    if (len < OVL_EDID_BLOCK_SIZE)
        return -1;

    for (int d = 0; d < 4; d++) {
        const uint8_t *desc = edid + 54 + d * 18;
        if (desc[0] == 0 && desc[1] == 0 && desc[3] == 0xFC) {
            size_t max = 13;
            if (max >= name_len)
                max = name_len - 1;
            int n = 0;
            for (int j = 0; j < (int)max; j++) {
                uint8_t c = desc[5 + j];
                if (c == 0x0A || c == 0)
                    break;
                name[n++] = (char)c;
            }
            name[n] = '\0';
            return 0;
        }
    }

    name[0] = '\0';
    return -1;
}

// --- Build passthrough EDID ---

// Timing parameters for DTD generation
struct cea_timing {
    uint32_t width, height, fps;
    uint32_t pixclk_khz;
    uint16_t h_blank, h_front, h_sync;
    uint16_t v_blank, v_front, v_sync;
    uint8_t vic; // CEA VIC code (0 = no standard VIC)
};

// Standard CEA timings with assigned VIC codes
static const struct cea_timing known_timings[] = {
    {1920, 1080, 24, 74250, 830, 638, 44, 45, 4, 5, 32},
    {1920, 1080, 30, 74250, 830, 638, 44, 45, 4, 5, 34},
    {1920, 1080, 50, 148500, 720, 528, 44, 45, 4, 5, 31},
    {1920, 1080, 60, 148500, 280, 88, 44, 45, 4, 5, 16},
    {1920, 1080, 120, 297000, 280, 88, 44, 45, 4, 5, 63},
    {1280, 720, 50, 74250, 700, 440, 40, 30, 5, 5, 19},
    {1280, 720, 60, 74250, 370, 110, 40, 30, 5, 5, 4},
    {1280, 720, 120, 148500, 370, 110, 40, 30, 5, 5, 47},
    {3840, 2160, 30, 297000, 560, 176, 88, 90, 8, 10, 95},
    {3840, 2160, 60, 594000, 560, 176, 88, 90, 8, 10, 97},
};
#define NUM_KNOWN_TIMINGS (sizeof(known_timings) / sizeof(known_timings[0]))

// Compute timing using libdisplay-info CVT v2 reduced blanking.
// Falls back to known CEA timings if a VIC match exists.
static void resolve_timing(struct cea_timing *out, uint32_t w, uint32_t h, uint32_t fps) {
    for (size_t i = 0; i < NUM_KNOWN_TIMINGS; i++) {
        if (known_timings[i].width == w && known_timings[i].height == h &&
            known_timings[i].fps == fps) {
            *out = known_timings[i];
            return;
        }
    }

    struct di_cvt_options cvt_opts = {
        .red_blank_ver = DI_CVT_REDUCED_BLANKING_V2,
        .h_pixels = (int32_t)w,
        .v_lines = (int32_t)h,
        .ip_freq_rqd = (double)fps,
    };
    struct di_cvt_timing cvt = {0};
    di_cvt_compute(&cvt, &cvt_opts);

    out->width = w;
    out->height = h;
    out->fps = fps;
    out->pixclk_khz = (uint32_t)(cvt.act_pixel_freq * 1000.0);
    out->h_blank = (uint16_t)(cvt.h_front_porch + cvt.h_sync + cvt.h_back_porch);
    out->h_front = (uint16_t)cvt.h_front_porch;
    out->h_sync = (uint16_t)cvt.h_sync;
    out->v_blank = (uint16_t)(cvt.v_front_porch + cvt.v_sync + cvt.v_back_porch);
    out->v_front = (uint16_t)cvt.v_front_porch;
    out->v_sync = (uint16_t)cvt.v_sync;
    out->vic = 0;

    ZF_LOGI("edid: CVT-RBv2 timing %ux%u@%u pixclk=%ukHz hblank=%u vblank=%u", w, h, fps,
            out->pixclk_khz, out->h_blank, out->v_blank);
}

// Write a DTD from timing parameters
static void write_dtd(uint8_t *dtd, const struct cea_timing *t) {
    memset(dtd, 0, 18);
    uint16_t pixclk_10khz = (uint16_t)(t->pixclk_khz / 10);
    dtd[0] = (uint8_t)(pixclk_10khz & 0xFF);
    dtd[1] = (uint8_t)(pixclk_10khz >> 8);
    dtd[2] = (uint8_t)(t->width & 0xFF);
    dtd[3] = (uint8_t)(t->h_blank & 0xFF);
    dtd[4] = (uint8_t)(((t->width >> 8) & 0x0F) << 4 | ((t->h_blank >> 8) & 0x0F));
    dtd[5] = (uint8_t)(t->height & 0xFF);
    dtd[6] = (uint8_t)(t->v_blank & 0xFF);
    dtd[7] = (uint8_t)(((t->height >> 8) & 0x0F) << 4 | ((t->v_blank >> 8) & 0x0F));
    dtd[8] = (uint8_t)(t->h_front & 0xFF);
    dtd[9] = (uint8_t)(t->h_sync & 0xFF);
    dtd[10] = (uint8_t)(((t->v_front & 0x0F) << 4) | (t->v_sync & 0x0F));
    dtd[11] = (uint8_t)(((t->h_front >> 8) & 0x03) << 6 | ((t->h_sync >> 8) & 0x03) << 4 |
                        ((t->v_front >> 4) & 0x03) << 2 | ((t->v_sync >> 4) & 0x03));
    dtd[12] = 0x0F;
    dtd[13] = 0x28;
    dtd[14] = 0x21;
    dtd[17] = 0x1E; // non-interlaced, digital separate sync, H+V positive
}

// Write a monitor name descriptor
static void write_name_descriptor(uint8_t *desc, const char *name) {
    memset(desc, 0, 18);
    desc[3] = 0xFC;
    int len = (int)strlen(name);
    if (len > OVL_EDID_MAX_NAME)
        len = OVL_EDID_MAX_NAME;
    for (int i = 0; i < 13; i++) {
        if (i < len)
            desc[5 + i] = (uint8_t)name[i];
        else if (i == len)
            desc[5 + i] = 0x0A;
        else
            desc[5 + i] = 0x20;
    }
}

// Write monitor range limits descriptor
static void write_range_descriptor(uint8_t *desc, const struct cea_timing *t) {
    memset(desc, 0, 18);
    desc[3] = 0xFD;
    desc[5] = 24;
    desc[6] = (uint8_t)(t->fps > 255 ? 255 : t->fps);
    desc[7] = 30;
    uint32_t htotal = t->width + t->h_blank;
    uint32_t max_h_khz = htotal ? (t->pixclk_khz / htotal + 1) : 140;
    desc[8] = (uint8_t)(max_h_khz > 255 ? 255 : max_h_khz);
    desc[9] = (uint8_t)((t->pixclk_khz / 1000 + 9) / 10);
    for (int i = 10; i < 18; i++)
        desc[i] = 0x0A;
}

int ovl_edid_build_passthrough(const uint8_t *src_edid, size_t src_len, const char *monitor_name,
                               uint32_t width, uint32_t height, uint32_t fps, uint8_t *dst,
                               size_t dst_max) {
    if (src_len < OVL_EDID_BLOCK_SIZE || dst_max < 256)
        return -1;

    struct cea_timing resolved;
    resolve_timing(&resolved, width, height, fps);
    const struct cea_timing *timing = &resolved;

    memset(dst, 0, 256);

    // --- Base EDID block (128 bytes) ---

    // Header
    dst[0] = 0x00;
    dst[1] = 0xFF;
    dst[2] = 0xFF;
    dst[3] = 0xFF;
    dst[4] = 0xFF;
    dst[5] = 0xFF;
    dst[6] = 0xFF;
    dst[7] = 0x00;

    // Copy manufacturer/product/serial/date from source
    memcpy(dst + 8, src_edid + 8, 10);

    // EDID version 1.3
    dst[18] = 0x01;
    dst[19] = 0x03;

    // Copy basic display params and chromaticity from source (bytes 20-34)
    memcpy(dst + 20, src_edid + 20, 15);

    // Established timings (bytes 35-37): none
    dst[35] = 0x00;
    dst[36] = 0x00;
    dst[37] = 0x00;

    // Standard timings (bytes 38-53): unused
    for (int i = 38; i < 54; i += 2) {
        dst[i] = 0x01;
        dst[i + 1] = 0x01;
    }

    // DTD 1: requested mode (preferred)
    write_dtd(dst + 54, timing);

    // Descriptor 2: Monitor name
    write_name_descriptor(dst + 72, monitor_name);

    // Descriptor 3: Monitor range limits
    write_range_descriptor(dst + 90, timing);

    // Descriptor 4: unused
    memset(dst + 108, 0, 18);
    dst[108 + 3] = 0x10; // dummy descriptor tag

    // Extension count: 1 (CEA block)
    dst[126] = 1;
    edid_fix_checksum(dst);

    // --- CEA-861 Extension Block (128 bytes) ---
    uint8_t *cea = dst + 128;
    cea[0] = 0x02; // CEA extension tag
    cea[1] = 0x03; // Revision 3

    // Flags: underscan + basic audio
    cea[3] = 0x83;

    int offset = 4;

    // Video Data Block (only if standard VIC exists)
    if (timing->vic) {
        cea[offset] = (2 << 5) | 1;
        cea[offset + 1] = timing->vic | 0x80;
        offset += 2;
    }

    // Audio Data Block: 2ch LPCM 48kHz 16-bit
    cea[offset] = (1 << 5) | 3;
    cea[offset + 1] = 0x09;
    cea[offset + 2] = 0x04;
    cea[offset + 3] = 0x01;
    offset += 4;

    // Speaker Allocation Data Block
    cea[offset] = (4 << 5) | 3;
    cea[offset + 1] = 0x01;
    cea[offset + 2] = 0x00;
    cea[offset + 3] = 0x00;
    offset += 4;

    // HDMI 1.4 Vendor Specific Data Block
    cea[offset] = (3 << 5) | 7;
    cea[offset + 1] = 0x03; // IEEE OUI 0x000C03 (LSB first)
    cea[offset + 2] = 0x0C;
    cea[offset + 3] = 0x00;
    cea[offset + 4] = 0x10; // physical address 1.0.0.0
    cea[offset + 5] = 0x00;
    cea[offset + 6] = 0x38; // AI, DC_48/36/30, DC_Y444
    cea[offset + 7] = 0x44; // max TMDS 340 MHz
    offset += 8;

    // HDMI Forum VSDB (HF-VSDB) for pixel clocks > 340 MHz
    if (timing->pixclk_khz > 340000) {
        cea[offset] = (3 << 5) | 7;
        cea[offset + 1] = 0xD8; // IEEE OUI 0xC45DD8 (LSB first)
        cea[offset + 2] = 0x5D;
        cea[offset + 3] = 0xC4;
        cea[offset + 4] = 0x01; // version 1
        uint32_t max_tmds = (timing->pixclk_khz / 1000 + 9) / 5 * 5 + 40;
        cea[offset + 5] = (uint8_t)(max_tmds / 5);
        cea[offset + 6] = 0x80; // SCDC present
        cea[offset + 7] = 0x03; // 10bpc, 12bpc
        offset += 8;
    }

    // DTD offset
    cea[2] = (uint8_t)offset;

    // DTD in extension block
    if (offset + 18 <= 127) {
        write_dtd(cea + offset, timing);
        offset += 18;
    }

    edid_fix_checksum(cea);

    ZF_LOGI("edid: built passthrough EDID '%s' %ux%u@%u (256 bytes)", monitor_name, width, height,
            fps);
    return 256;
}

// --- Write EDID to V4L2 device ---

int ovl_edid_write_v4l2(const char *v4l2_device, const uint8_t *edid, size_t len) {
    int fd = open(v4l2_device, O_RDWR);
    if (fd < 0) {
        ZF_LOGE("edid: cannot open %s", v4l2_device);
        return -1;
    }

    struct v4l2_edid v4l2_edid = {
        .pad = 0,
        .start_block = 0,
        .blocks = (uint32_t)(len / OVL_EDID_BLOCK_SIZE),
        .edid = (uint8_t *)edid,
    };

    if (ioctl(fd, VIDIOC_S_EDID, &v4l2_edid) < 0) {
        ZF_LOGE("edid: VIDIOC_S_EDID failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    ZF_LOGI("edid: wrote %zu bytes to %s (%u blocks)", len, v4l2_device, v4l2_edid.blocks);
    close(fd);
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "common/pixfmt.h"
#include "common/options.h"
#include "common/config.h"
#include "receiver/receiver.h"
#include "receiver/alsa_caps.h"
#include "receiver/v4l2_caps.h"
#include "receiver/v4l2_capture.h"
#include "encoder/drm_caps.h"
#include "encoder/drm_output.h"
#include "converter/converter.h"
#include "overlay/overlay.h"
#include "processor/processor_mgr.h"
#include "usb_proxy/usb_proxy.h"
#include "usb_proxy/usb_caps.h"
#include "receiver/alsa_capture.h"
#include "encoder/alsa_playback.h"
#include "common/edid.h"
#include "common/log.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <samplerate.h>

#define MAX_FORMATS        32
#define PLANE_TYPE_PRIMARY 1
#define PLANE_TYPE_OVERLAY 0
#define REINIT_DELAY_MS    1000 // delay before reinit on signal change/error

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static void delay_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

// --- Common types ---

// --- Parsing helpers in common/options.c ---

static void print_usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  run          Start the overlay pipeline (default)\n"
        "  query        Query device capabilities\n"
        "\n"
        "Device options:\n"
        "  --video-in,  -i DEV       V4L2 capture device  (default: first HDMI RX)\n"
        "  --video-out, -o DEV[:CON] DRM device[:connector] (e.g. /dev/dri/card0:HDMI-A-2)\n"
        "  --audio-in,  -a DEV       ALSA capture device   (default: HDMI input)\n"
        "  --audio-out, -A DEV       ALSA playback device  (default: HDMI output)\n"
        "\n"
        "Format options (run):\n"
        "  --fmt,                    Set format for both input and output\n"
        "  --res,            WxH     Set resolution for both input and output\n"
        "  --fps,            FPS     Set framerate for both input and output\n"
        "  --fmt-in,    -f FOURCC    Force V4L2 input format (e.g. NV24, BGR3)\n"
        "  --fmt-out,   -F FOURCC    Force DRM output format (e.g. BG24, NV24)\n"
        "  --res-in,    -r WxH       Force input resolution (e.g. 1920x1080)\n"
        "  --fps-in,    -R FPS       Force input framerate (e.g. 60)\n"
        "  --res-out,   -s WxH       Force output resolution (e.g. 2560x1440)\n"
        "  --fps-out,   -S FPS       Force output framerate (e.g. 60)\n"
        "\n"
        "Query options:\n"
        "  --format,    -O FORMAT    Output format: plain (default), json\n"
        "\n"
        "USB proxy options (run):\n"
        "  --usb-device VID:PID      Proxy a USB HID device (repeatable)\n"
        "\n"
        "General:\n"
        "  --config,    -C PATH      Config file (default: overlAIer.toml next to the binary)\n"
        "  --log-level, -L LEVEL     Log level: verbose, debug, info, warn, error, fatal, none\n"
        "  --async-flip              Enable async page flip (tearing, lower latency)\n"
        "  --help,      -h           Show this help\n",
        prog);
}

static int parse_args(int argc, char *argv[], struct options *opts) {
    memset(opts, 0, sizeof(*opts));

    // Extract command (first non-option argument)
    if (argc > 1 && argv[1][0] != '-') {
        opts->command = argv[1];
        // Shift argv so getopt doesn't see the command
        argv[1] = argv[0];
        argc--;
        argv++;
    } else {
        opts->command = "run";
    }

    static const struct option long_opts[] = {
        {"video-in", required_argument, NULL, 'i'},
        {"video-out", required_argument, NULL, 'o'},
        {"audio-in", required_argument, NULL, 'a'},
        {"audio-out", required_argument, NULL, 'A'},
        {"fmt", required_argument, NULL, 1},
        {"res", required_argument, NULL, 2},
        {"fps", required_argument, NULL, 3},
        {"fmt-in", required_argument, NULL, 'f'},
        {"fmt-out", required_argument, NULL, 'F'},
        {"res-in", required_argument, NULL, 'r'},
        {"fps-in", required_argument, NULL, 'R'},
        {"res-out", required_argument, NULL, 's'},
        {"fps-out", required_argument, NULL, 'S'},
        {"format", required_argument, NULL, 'O'},
        {"config", required_argument, NULL, 'C'},
        {"log-level", required_argument, NULL, 'L'},
        {"usb-device", required_argument, NULL, 4},
        {"async-flip", no_argument, NULL, 'T'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    optind = 1;
    int c;
    opts->log_level = -1; // default: don't change
    while ((c = getopt_long(argc, argv, "i:o:a:A:f:F:r:R:s:S:O:C:L:Th", long_opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            opts->video_in = optarg;
            break;
        case 'o':
            opts->video_out = optarg;
            break;
        case 'a':
            opts->audio_in = optarg;
            break;
        case 'A':
            opts->audio_out = optarg;
            break;
        case 1: { // --fmt (both)
            enum ovl_pixfmt f = parse_format(optarg);
            if (!f)
                return -1;
            opts->fmt_in = f;
            opts->fmt_out = f;
            break;
        }
        case 2: // --res (both)
            if (parse_resolution(optarg, &opts->res_in_w, &opts->res_in_h) < 0)
                return -1;
            opts->res_out_w = opts->res_in_w;
            opts->res_out_h = opts->res_in_h;
            break;
        case 3: { // --fps (both)
            uint32_t fps = (uint32_t)atoi(optarg);
            if (!fps) {
                ZF_LOGE("Invalid framerate '%s'", optarg);
                return -1;
            }
            opts->fps_in = fps;
            opts->fps_out = fps;
            break;
        }
        case 'f':
            opts->fmt_in = parse_format(optarg);
            if (!opts->fmt_in)
                return -1;
            break;
        case 'F':
            opts->fmt_out = parse_format(optarg);
            if (!opts->fmt_out)
                return -1;
            break;
        case 'r':
            if (parse_resolution(optarg, &opts->res_in_w, &opts->res_in_h) < 0)
                return -1;
            break;
        case 'R':
            opts->fps_in = (uint32_t)atoi(optarg);
            if (!opts->fps_in) {
                ZF_LOGE("Invalid framerate '%s'", optarg);
                return -1;
            }
            break;
        case 's':
            if (parse_resolution(optarg, &opts->res_out_w, &opts->res_out_h) < 0)
                return -1;
            break;
        case 'S':
            opts->fps_out = (uint32_t)atoi(optarg);
            if (!opts->fps_out) {
                ZF_LOGE("Invalid framerate '%s'", optarg);
                return -1;
            }
            break;
        case 'O':
            if (strcmp(optarg, "json") == 0)
                opts->out_fmt = FMT_JSON;
            else if (strcmp(optarg, "plain") == 0)
                opts->out_fmt = FMT_PLAIN;
            else {
                ZF_LOGE("Invalid format '%s' (use plain or json)", optarg);
                return -1;
            }
            break;
        case 4: // --usb-device
            if (opts->num_usb_devices < OPT_MAX_USB_DEVICES)
                opts->usb_devices[opts->num_usb_devices++] = optarg;
            break;
        case 'C':
            opts->config_path = optarg;
            break;
        case 'L':
            opts->log_level = parse_log_level(optarg);
            if (opts->log_level < 0) {
                ZF_LOGE("Invalid log level '%s'", optarg);
                return -1;
            }
            break;
        case 'T':
            opts->async_flip = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

// --- Device auto-detection ---

// Find first V4L2 capture device
static int find_video_in(char *buf, size_t len) {
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        struct ovl_video_caps caps;
        if (ovl_v4l2_query_caps(path, &caps) == 0) {
            snprintf(buf, len, "%s", path);
            ovl_v4l2_caps_free(&caps);
            return 0;
        }
    }
    return -1;
}

// Find first DRM device with a connected output
static int find_video_out(char *buf, size_t len) {
    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        struct ovl_drm_caps caps;
        if (ovl_drm_query_caps(path, &caps) == 0) {
            for (size_t c = 0; c < caps.num_connectors; c++) {
                if (caps.connectors[c].connected) {
                    snprintf(buf, len, "%s", path);
                    ovl_drm_caps_free(&caps);
                    return 0;
                }
            }
            ovl_drm_caps_free(&caps);
        }
    }
    return -1;
}

// Try to find an ALSA capture device associated with the V4L2 video input.
// Uses bus_info from V4L2 to match against ALSA card paths.
static int find_audio_for_video_in(const char *video_dev, char *buf, size_t len) {
    struct ovl_video_caps vcaps;
    if (ovl_v4l2_query_caps(video_dev, &vcaps) != 0)
        return -1;

    int found = ovl_alsa_find_by_bus(vcaps.bus_info, OVL_ALSA_CAPTURE, buf, len);
    ovl_v4l2_caps_free(&vcaps);
    return found;
}

// Try to find an ALSA playback device associated with the DRM video output.
// For HDMI/DP connectors, maps connector index to ALSA card name convention.
static int find_audio_for_video_out(const char *drm_dev, char *buf, size_t len) {
    struct ovl_drm_caps dcaps;
    if (ovl_drm_query_caps(drm_dev, &dcaps) != 0)
        return -1;

    int found = -1;
    for (size_t c = 0; c < dcaps.num_connectors && found != 0; c++) {
        if (!dcaps.connectors[c].connected)
            continue;

        const char *name = dcaps.connectors[c].name;

        // HDMI-A-N → "hdmi<N-1>", DP-N → "dp<N-1>"
        const char *prefix = NULL;
        if (strstr(name, "HDMI"))
            prefix = "hdmi";
        else if (strstr(name, "DP"))
            prefix = "dp";

        if (prefix) {
            int idx = 0;
            const char *dash = strrchr(name, '-');
            if (dash)
                idx = atoi(dash + 1) - 1;
            if (idx < 0)
                idx = 0;
            char search[16];
            snprintf(search, sizeof(search), "%s%d", prefix, idx);
            if (ovl_alsa_find_device(search, OVL_ALSA_PLAYBACK, buf, len) == 0)
                found = 0;
        }
    }

    ovl_drm_caps_free(&dcaps);
    return found;
}

static int resolve_devices(struct options *opts, char vib[32], char vob[32], char aib[32],
                           char aob[32], int require_all) {
    // 1. Resolve video devices first
    if (!opts->video_in) {
        if (find_video_in(vib, 32) == 0)
            opts->video_in = vib;
        else if (require_all) {
            ZF_LOGE("No video input found.");
            return -1;
        }
    }
    if (!opts->video_out) {
        if (find_video_out(vob, 32) == 0)
            opts->video_out = vob;
        else if (require_all) {
            ZF_LOGE("No video output found.");
            return -1;
        }
    }

    // 2. Try to find audio devices matching the video devices
    if (!opts->audio_in) {
        int found = -1;
        if (opts->video_in)
            found = find_audio_for_video_in(opts->video_in, aib, 32);
        // Fall back to first available capture device
        if (found != 0)
            found = ovl_alsa_find_device("", OVL_ALSA_CAPTURE, aib, 32);
        if (found == 0)
            opts->audio_in = aib;
    }

    if (!opts->audio_out) {
        int found = -1;
        if (opts->video_out)
            found = find_audio_for_video_out(opts->video_out, aob, 32);
        // Fall back to first available playback device
        if (found != 0)
            found = ovl_alsa_find_device("", OVL_ALSA_PLAYBACK, aob, 32);
        if (found == 0)
            opts->audio_out = aob;
    }
    return 0;
}

// --- JSON output helpers ---

static void query_video_in_json(const char *dev, const struct ovl_video_caps *caps) {
    printf("    {\n");
    printf("      \"device\": \"%s\",\n", dev);
    printf("      \"card\": \"%s\",\n", caps->card);
    printf("      \"driver\": \"%s\",\n", caps->driver);
    printf("      \"bus\": \"%s\",\n", caps->bus_info);
    printf("      \"modes\": [\n");
    char fcc[5];
    for (size_t i = 0; i < caps->num_modes; i++) {
        const struct ovl_video_mode *m = &caps->modes[i];
        fcc[0] = (char)(m->pixelformat & 0xFF);
        fcc[1] = (char)((m->pixelformat >> 8) & 0xFF);
        fcc[2] = (char)((m->pixelformat >> 16) & 0xFF);
        fcc[3] = (char)((m->pixelformat >> 24) & 0xFF);
        fcc[4] = '\0';
        printf("        {\"format\": \"%s\", \"width\": %u, \"height\": %u", fcc, m->width,
               m->height);
        if (m->fps_denominator > 0)
            printf(", \"fps\": %.2f", (double)m->fps_denominator / m->fps_numerator);
        printf("}%s\n", i + 1 < caps->num_modes ? "," : "");
    }
    printf("      ]\n");
    printf("    }");
}

static void query_audio_json(const char *dev, const struct ovl_audio_caps *caps) {
    printf("    {\n");
    printf("      \"device\": \"%s\",\n", dev);
    printf("      \"name\": \"%s\",\n", caps->name);
    printf("      \"channels\": {\"min\": %u, \"max\": %u},\n", caps->min_channels,
           caps->max_channels);
    printf("      \"rate_range\": {\"min\": %u, \"max\": %u},\n", caps->min_rate, caps->max_rate);
    printf("      \"rates\": [");
    for (size_t i = 0; i < caps->num_rates; i++)
        printf("%s%u", i ? ", " : "", caps->rates[i]);
    printf("]\n");
    printf("    }");
}

static void query_video_out_json(const char *dev, const struct ovl_drm_caps *caps) {
    printf("    {\n");
    printf("      \"device\": \"%s\",\n", dev);
    printf("      \"driver\": \"%s\",\n", caps->driver);
    printf("      \"connectors\": [\n");
    for (size_t i = 0; i < caps->num_connectors; i++) {
        const struct ovl_drm_connector_caps *cc = &caps->connectors[i];
        printf("        {\"id\": %u, \"name\": \"%s\", \"connected\": %s", cc->connector_id,
               cc->name, cc->connected ? "true" : "false");
        if (cc->current_mode.width > 0)
            printf(", \"current\": {\"width\": %u, \"height\": %u, \"refresh\": %.1f}",
                   cc->current_mode.width, cc->current_mode.height,
                   cc->current_mode.refresh / 1000.0);
        printf(", \"modes\": [");
        for (size_t m = 0; m < cc->num_modes; m++)
            printf("%s{\"width\": %u, \"height\": %u, \"refresh\": %.1f}", m ? ", " : "",
                   cc->modes[m].width, cc->modes[m].height, cc->modes[m].refresh / 1000.0);
        printf("]}%s\n", i + 1 < caps->num_connectors ? "," : "");
    }
    printf("      ],\n");
    printf("      \"planes\": [\n");
    char fcc[5];
    for (size_t i = 0; i < caps->num_planes; i++) {
        const struct ovl_drm_plane_caps *pc = &caps->planes[i];
        const char *type = pc->type == 1 ? "primary" : pc->type == 0 ? "overlay" : "cursor";
        printf("        {\"id\": %u, \"type\": \"%s\", \"formats\": [", pc->plane_id, type);
        for (size_t f = 0; f < pc->num_formats; f++) {
            fcc[0] = (char)(pc->formats[f] & 0xFF);
            fcc[1] = (char)((pc->formats[f] >> 8) & 0xFF);
            fcc[2] = (char)((pc->formats[f] >> 16) & 0xFF);
            fcc[3] = (char)((pc->formats[f] >> 24) & 0xFF);
            fcc[4] = '\0';
            printf("%s\"%s\"", f ? ", " : "", fcc);
        }
        printf("]}%s\n", i + 1 < caps->num_planes ? "," : "");
    }
    printf("      ]\n");
    printf("    }");
}

// --- query command ---

static int cmd_query(struct options *opts) {
    char vib[32], vob[32], aib[32], aob[32];
    resolve_devices(opts, vib, vob, aib, aob, 0);
    ovl_usb_init();

    if (opts->out_fmt == FMT_JSON) {
        printf("{\n");

        // --- Input ---
        printf("  \"input\": {\n");

        printf("    \"video\": ");
        if (opts->video_in) {
            struct ovl_video_caps vcaps;
            if (ovl_v4l2_query_caps(opts->video_in, &vcaps) == 0) {
                query_video_in_json(opts->video_in, &vcaps);
                ovl_v4l2_caps_free(&vcaps);
            } else {
                printf("null");
            }
        } else {
            printf("null");
        }
        printf(",\n");

        printf("    \"audio\": ");
        if (opts->audio_in) {
            struct ovl_audio_caps acaps;
            if (ovl_alsa_query_caps(opts->audio_in, OVL_ALSA_CAPTURE, &acaps) == 0)
                query_audio_json(opts->audio_in, &acaps);
            else
                printf("null");
        } else {
            printf("null");
        }
        printf("\n");

        printf("  },\n");

        // --- Output ---
        printf("  \"output\": {\n");

        printf("    \"video\": ");
        if (opts->video_out) {
            struct ovl_drm_caps dcaps;
            if (ovl_drm_query_caps(opts->video_out, &dcaps) == 0) {
                query_video_out_json(opts->video_out, &dcaps);
                ovl_drm_caps_free(&dcaps);
            } else {
                printf("null");
            }
        } else {
            printf("null");
        }
        printf(",\n");

        printf("    \"audio\": ");
        if (opts->audio_out) {
            struct ovl_audio_caps aocaps;
            if (ovl_alsa_query_caps(opts->audio_out, OVL_ALSA_PLAYBACK, &aocaps) == 0)
                query_audio_json(opts->audio_out, &aocaps);
            else
                printf("null");
        } else {
            printf("null");
        }
        printf("\n");

        printf("  },\n");

        // Converters
        printf("  \"converters\": [\n");
        struct ovl_converter_backend_info conv_infos[OVL_CONV_MAX_BACKENDS];
        int nconv = ovl_converter_query_backends(conv_infos, OVL_CONV_MAX_BACKENDS);
        for (int i = 0; i < nconv; i++) {
            struct ovl_converter_backend_info *ci = &conv_infos[i];
            printf("    {\"name\": \"%s\", \"type\": \"%s\"", ci->name, ci->type);
            if (ci->device)
                printf(", \"device\": \"%s\"", ci->device);
            printf(", \"input_formats\": [");
            for (int f = 0; f < ci->num_input_fmts; f++)
                printf("%s\"%s\"", f ? ", " : "", ovl_pixfmt_name(ci->input_fmts[f]));
            printf("], \"output_formats\": [");
            for (int f = 0; f < ci->num_output_fmts; f++)
                printf("%s\"%s\"", f ? ", " : "", ovl_pixfmt_name(ci->output_fmts[f]));
            printf("]");
            if (ci->max_input_w)
                printf(", \"max_input\": \"%ux%u\"", ci->max_input_w, ci->max_input_h);
            if (ci->max_output_w)
                printf(", \"max_output\": \"%ux%u\"", ci->max_output_w, ci->max_output_h);
            printf(", \"features\": [");
            int first = 1;
            if (ci->supports_csc) {
                printf("%s\"csc\"", first ? "" : ", ");
                first = 0;
            }
            if (ci->supports_scale) {
                printf("%s\"scale\"", first ? "" : ", ");
                first = 0;
            }
            if (ci->supports_rotate) {
                printf("%s\"rotate\"", first ? "" : ", ");
                first = 0;
            }
            printf("]}%s\n", i + 1 < nconv ? "," : "");
        }
        printf("  ],\n");

        // USB HID devices
        printf("  \"usb_hid\": [\n");
        struct ovl_usb_hid_info usb_devs[OVL_USB_MAX_DEVICES];
        int nusb = ovl_usb_enum_hid_devices(usb_devs, OVL_USB_MAX_DEVICES);
        for (int i = 0; i < nusb; i++) {
            struct ovl_usb_hid_info *u = &usb_devs[i];
            const char *type = u->protocol == 1 ? "keyboard" :
                               u->protocol == 2 ? "mouse" : "other";
            printf("    {\"name\": \"%s\", \"bus\": %d, \"address\": %d, \"interface\": %d, "
                   "\"vid_pid\": \"%04x:%04x\", \"type\": \"%s\"}%s\n",
                   u->name, u->bus, u->address, u->interface_number,
                   u->vid, u->pid, type,
                   i + 1 < nusb ? "," : "");
        }
        printf("  ]\n");

        printf("}\n");
    } else {
        // Plain text — inputs first, then outputs
        if (opts->video_in || opts->audio_in)
            printf("=== Input ===\n\n");

        if (opts->video_in) {
            struct ovl_video_caps vcaps;
            if (ovl_v4l2_query_caps(opts->video_in, &vcaps) == 0) {
                printf("Video: %s\n", opts->video_in);
                ovl_v4l2_caps_print(&vcaps);
                ovl_v4l2_caps_free(&vcaps);
                printf("\n");
            }
        }
        if (opts->audio_in) {
            struct ovl_audio_caps acaps;
            if (ovl_alsa_query_caps(opts->audio_in, OVL_ALSA_CAPTURE, &acaps) == 0) {
                printf("Audio: %s\n", opts->audio_in);
                ovl_alsa_caps_print(&acaps);
                printf("\n");
            }
        }

        if (opts->video_out || opts->audio_out)
            printf("=== Output ===\n\n");

        if (opts->video_out) {
            struct ovl_drm_caps dcaps;
            if (ovl_drm_query_caps(opts->video_out, &dcaps) == 0) {
                printf("Video: %s\n", opts->video_out);
                ovl_drm_caps_print(&dcaps);
                ovl_drm_caps_free(&dcaps);
                printf("\n");
            }
        }
        if (opts->audio_out) {
            struct ovl_audio_caps aocaps;
            if (ovl_alsa_query_caps(opts->audio_out, OVL_ALSA_PLAYBACK, &aocaps) == 0) {
                printf("Audio: %s\n", opts->audio_out);
                ovl_alsa_caps_print(&aocaps);
                printf("\n");
            }
        }

        // Converters
        printf("=== Converters ===\n\n");
        struct ovl_converter_backend_info conv_infos_plain[OVL_CONV_MAX_BACKENDS];
        int nconv_plain = ovl_converter_query_backends(conv_infos_plain, OVL_CONV_MAX_BACKENDS);
        for (int i = 0; i < nconv_plain; i++) {
            struct ovl_converter_backend_info *ci = &conv_infos_plain[i];
            printf("[%s] %s", ci->name, ci->type);
            if (ci->device)
                printf(" — %s", ci->device);
            printf("\n");
            printf("  Input:    ");
            for (int f = 0; f < ci->num_input_fmts; f++)
                printf("%s%s", f ? " " : "", ovl_pixfmt_name(ci->input_fmts[f]));
            printf("\n  Output:   ");
            for (int f = 0; f < ci->num_output_fmts; f++)
                printf("%s%s", f ? " " : "", ovl_pixfmt_name(ci->output_fmts[f]));
            printf("\n");
            if (ci->max_input_w)
                printf("  Max:      %ux%u -> %ux%u\n", ci->max_input_w, ci->max_input_h,
                       ci->max_output_w, ci->max_output_h);
            printf("  Features:");
            if (ci->supports_csc)
                printf(" csc");
            if (ci->supports_scale)
                printf(" scale");
            if (ci->supports_rotate)
                printf(" rotate");
            printf("\n\n");
        }

        // USB HID devices
        printf("=== USB HID Devices ===\n\n");
        struct ovl_usb_hid_info usb_devs_plain[OVL_USB_MAX_DEVICES];
        int nusb_plain = ovl_usb_enum_hid_devices(usb_devs_plain, OVL_USB_MAX_DEVICES);
        if (nusb_plain == 0) {
            printf("(none found)\n\n");
        }
        for (int i = 0; i < nusb_plain; i++) {
            struct ovl_usb_hid_info *u = &usb_devs_plain[i];
            const char *type = u->protocol == 1 ? "keyboard" :
                               u->protocol == 2 ? "mouse" : "gamepad/other";
            printf("[%s] %04x:%04x — %s (bus %d addr %d iface %d)\n",
                   type, u->vid, u->pid, u->name,
                   u->bus, u->address, u->interface_number);
        }
    }

    ovl_usb_exit();
    return 0;
}

// --- Format negotiation ---

// Check if a DRM plane of the given type supports the given internal format
static int plane_supports(const struct ovl_drm_caps *caps, uint32_t plane_type,
                          enum ovl_pixfmt fmt) {
    uint32_t drm_fourcc = ovl_pixfmt_to_drm(fmt);
    if (!drm_fourcc)
        return 0;
    for (size_t p = 0; p < caps->num_planes; p++) {
        if (caps->planes[p].type != plane_type)
            continue;
        for (size_t f = 0; f < caps->planes[p].num_formats; f++)
            if (caps->planes[p].formats[f] == drm_fourcc)
                return 1;
    }
    return 0;
}

// Conversion cost between internal formats (lower = cheaper, 0 = same)
static int conversion_cost(enum ovl_pixfmt src, enum ovl_pixfmt dst) {
    if (src == dst)
        return 0;

    // YUV chroma subsampling is cheapest
    if (src == OVL_PIXFMT_NV24 && (dst == OVL_PIXFMT_NV16 || dst == OVL_PIXFMT_NV12))
        return 1;
    if (src == OVL_PIXFMT_NV24 && dst == OVL_PIXFMT_YUYV)
        return 2;

    // Colorspace change (YUV→RGB)
    if (dst == OVL_PIXFMT_BGR888 || dst == OVL_PIXFMT_RGB888 || dst == OVL_PIXFMT_XRGB8888 ||
        dst == OVL_PIXFMT_ARGB8888)
        return 3;

    return 10;
}

struct negotiate_result {
    enum ovl_pixfmt fmt; // internal format for pipeline
    int needs_conversion;
    int use_primary;
};

static int negotiate(const enum ovl_pixfmt *src_fmts, int src_count,
                     const struct ovl_drm_caps *drm_caps, enum ovl_pixfmt forced_out,
                     struct negotiate_result *result) {
    // Forced output format
    if (forced_out != OVL_PIXFMT_UNKNOWN) {
        for (int i = 0; i < src_count; i++) {
            if (src_fmts[i] == forced_out) {
                *result = (struct negotiate_result){
                    .fmt = forced_out,
                    .needs_conversion = 0,
                    .use_primary = plane_supports(drm_caps, PLANE_TYPE_PRIMARY, forced_out),
                };
                return 0;
            }
        }
        *result = (struct negotiate_result){
            .fmt = forced_out,
            .needs_conversion = 1,
            .use_primary = plane_supports(drm_caps, PLANE_TYPE_PRIMARY, forced_out),
        };
        return 0;
    }

    // Pass 1: direct on primary
    for (int i = 0; i < src_count; i++) {
        if (plane_supports(drm_caps, PLANE_TYPE_PRIMARY, src_fmts[i])) {
            *result = (struct negotiate_result){.fmt = src_fmts[i], .use_primary = 1};
            ZF_LOGD("negotiate direct primary %s", ovl_pixfmt_name(src_fmts[i]));
            return 0;
        }
    }
    // Pass 2: direct on overlay
    for (int i = 0; i < src_count; i++) {
        if (plane_supports(drm_caps, PLANE_TYPE_OVERLAY, src_fmts[i])) {
            *result = (struct negotiate_result){.fmt = src_fmts[i]};
            ZF_LOGD("negotiate direct overlay %s", ovl_pixfmt_name(src_fmts[i]));
            return 0;
        }
    }
    // Pass 3: cheapest conversion (primary first, then overlay)
    int best_cost = INT32_MAX;
    enum ovl_pixfmt best_fmt = OVL_PIXFMT_UNKNOWN;
    int best_primary = 0;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t tt = (pass == 0) ? PLANE_TYPE_PRIMARY : PLANE_TYPE_OVERLAY;
        for (size_t p = 0; p < drm_caps->num_planes; p++) {
            if (drm_caps->planes[p].type != tt)
                continue;
            for (size_t f = 0; f < drm_caps->planes[p].num_formats; f++) {
                enum ovl_pixfmt df = ovl_pixfmt_from_drm(drm_caps->planes[p].formats[f]);
                if (df == OVL_PIXFMT_UNKNOWN)
                    continue;
                for (int i = 0; i < src_count; i++) {
                    int cost = conversion_cost(src_fmts[i], df);
                    if (cost > 0 && cost < best_cost) {
                        best_cost = cost;
                        best_fmt = df;
                        best_primary = (pass == 0);
                    }
                }
            }
        }
        if (best_fmt != OVL_PIXFMT_UNKNOWN && pass == 0)
            break;
    }
    if (best_fmt == OVL_PIXFMT_UNKNOWN) {
        ZF_LOGE("no format negotiation path found");
        return -1;
    }

    *result = (struct negotiate_result){
        .fmt = best_fmt,
        .needs_conversion = 1,
        .use_primary = best_primary,
    };
    ZF_LOGD("negotiate conversion -> %s (cost=%d, %s)", ovl_pixfmt_name(best_fmt), best_cost,
            best_primary ? "primary" : "overlay");
    return 0;
}

// --- Video/Audio thread contexts ---

static volatile int signal_lost = 0; // set by video thread on signal change

struct video_thread_ctx {
    struct ovl_v4l2_capture *cap;
    struct ovl_converter *conv;
    struct ovl_drm_output *output;
    struct ovl_overlay *overlay;
    struct ovl_processor_mgr *proc_mgr;
    int *fb_indices;
    uint64_t frame_count;
};

// Drain a completed flip and requeue the capture buffer.
static int drain_flips(struct video_thread_ctx *ctx, int timeout_ms) {
    int done_idx;
    int ready = ovl_drm_output_acquire_ready(ctx->output, timeout_ms, &done_idx,
                                              NULL, NULL);
    if (ready > 0)
        ovl_v4l2_capture_queue(ctx->cap, done_idx);
    return ready;
}

static void *video_thread_fn(void *arg) {
    struct video_thread_ctx *ctx = arg;
    int consecutive_errors = 0;
    int max_pending = 1; // keep latency low: max 1 pending flip

    while (running && !signal_lost) {
        int ready;
        while ((ready = drain_flips(ctx, 0)) > 0)
            ;
        if (ready < 0) {
            ZF_LOGE("video: failed to poll DRM events");
            signal_lost = 1;
            break;
        }

        while (ovl_drm_output_pending(ctx->output) >= max_pending) {
            ready = drain_flips(ctx, 16);
            if (ready > 0)
                continue;
            if (ready < 0) {
                ZF_LOGE("video: failed to poll DRM events");
                signal_lost = 1;
                break;
            }
        }
        if (signal_lost || !running)
            break;

        struct ovl_v4l2_dequeue_info dq_info = {0};
        int latest = ovl_v4l2_capture_dequeue(ctx->cap, &dq_info);
        if (latest < 0) {
            consecutive_errors++;
            if (consecutive_errors > 5) {
                ZF_LOGW("video: signal lost, requesting reinit");
                signal_lost = 1;
                break;
            }
            delay_ms(REINIT_DELAY_MS / 5);
            continue;
        }
        consecutive_errors = 0;

        int next;
        struct ovl_v4l2_dequeue_info next_dq = {0};
        while ((next = ovl_v4l2_capture_dequeue_nb(ctx->cap, &next_dq)) >= 0) {
            ovl_v4l2_capture_queue(ctx->cap, latest);
            latest = next;
            dq_info = next_dq;
        }

        int show_ret = 0;
        if (ctx->conv) {
            struct ovl_v4l2_buffer *buf = &ctx->cap->buffers[latest];
            int out_idx = ovl_converter_process(ctx->conv, buf->dmabuf_fds, buf->pitches,
                                                buf->offsets, buf->num_planes);
            if (out_idx < 0) {
                ovl_v4l2_capture_queue(ctx->cap, latest);
                continue;
            }
            show_ret = ovl_drm_output_show(ctx->output, ctx->fb_indices[out_idx], latest);
        } else {
            show_ret = ovl_drm_output_show(ctx->output, ctx->fb_indices[latest], latest);
        }

        if (show_ret < 0) {
            ZF_LOGE("video: drm flip failed");
            ovl_v4l2_capture_queue(ctx->cap, latest);
            signal_lost = 1;
            break;
        }

        // Notify processors of frame submit + capture metadata
        if (ctx->proc_mgr) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t submit_us = (uint64_t)now.tv_sec * 1000000ULL +
                                 (uint64_t)now.tv_nsec / 1000ULL;

            struct ovl_frame_info finfo = {
                .sequence = dq_info.sequence,
                .timestamp_us = dq_info.timestamp_us,
                .width = ctx->cap->width,
                .height = ctx->cap->height,
                .stride = ctx->cap->buffers[latest].pitches[0],
            };
            ovl_processor_mgr_post_frame(ctx->proc_mgr, NULL, &finfo);

            struct ovl_frame_info flip_info = {
                .sequence = dq_info.sequence,
                .timestamp_us = submit_us,
                .width = ctx->cap->width,
                .height = ctx->cap->height,
            };
            ovl_processor_mgr_notify_flip(ctx->proc_mgr, &flip_info);
        }

        ctx->frame_count++;

        if (show_ret > 0) {
            ovl_v4l2_capture_queue(ctx->cap, latest);
            continue;
        }

        while ((ready = drain_flips(ctx, 0)) > 0)
            ;
        if (ready < 0) {
            ZF_LOGE("video: failed to poll DRM events");
            signal_lost = 1;
            break;
        }
    }

    int flushed;
    int ready;
    while ((ready = ovl_drm_output_acquire_ready(ctx->output, 50, &flushed, NULL, NULL)) > 0)
        ovl_v4l2_capture_queue(ctx->cap, flushed);

    return NULL;
}

struct audio_thread_ctx {
    struct ovl_alsa_capture *capture;
    struct ovl_alsa_playback *playback;
};

static void *audio_thread_fn(void *arg) {
    struct audio_thread_ctx *ctx = arg;
    int period = (int)ovl_alsa_capture_period_size(ctx->capture);
    int channels = (int)ovl_alsa_capture_channels(ctx->capture);
    int fb = ovl_alsa_capture_frame_bytes(ctx->capture);
    int sample_bytes = fb / channels;
    int is_s32 = (sample_bytes == 4);
    int is_s24 = (sample_bytes == 3);

    // Buffers: capture PCM → float → resample → float → playback PCM
    void *pcm_in = malloc((size_t)(period * fb));
    float *float_in = malloc((size_t)(period * channels) * sizeof(float));
    // Output can be slightly larger due to ratio adjustment
    int out_max = period + 32;
    float *float_out = malloc((size_t)(out_max * channels) * sizeof(float));
    void *pcm_out = malloc((size_t)(out_max * fb));

    if (!pcm_in || !float_in || !float_out || !pcm_out) {
        ZF_LOGE("audio: malloc failed");
        goto done;
    }

    // Init resampler
    int src_err;
    SRC_STATE *src = src_new(SRC_SINC_FASTEST, channels, &src_err);
    if (!src) {
        ZF_LOGE("audio: src_new failed: %s", src_strerror(src_err));
        goto done;
    }

    // PI controller state for drift compensation
    double ratio = 1.0;
    double integral = 0.0;
    long buf_size = (long)ovl_alsa_playback_buffer_size(ctx->playback);
    long target_fill = buf_size / 2; // aim for half-full

    ZF_LOGD("audio: adaptive resampling, target fill=%ld/%ld", target_fill, buf_size);

    int audio_errors = 0;
    while (running && !signal_lost) {
        int frames = ovl_alsa_capture_read(ctx->capture, pcm_in, (unsigned int)period);
        if (frames <= 0) {
            if (frames < 0) {
                audio_errors++;
                if (audio_errors > 100) {
                    ZF_LOGE("audio: too many errors, stopping");
                    break;
                }
                delay_ms(REINIT_DELAY_MS / 10);
            }
            continue;
        }
        audio_errors = 0;

        // Convert PCM to float
        if (is_s32) {
            const int32_t *s = pcm_in;
            for (int i = 0; i < frames * channels; i++)
                float_in[i] = (float)s[i] / 2147483648.0f;
        } else if (is_s24) {
            // S24_LE: 3 bytes per sample, sign-extend to int32
            const uint8_t *s = pcm_in;
            for (int i = 0; i < frames * channels; i++) {
                int32_t v = (int32_t)(s[0] | (s[1] << 8) | (s[2] << 16));
                if (v & 0x800000)
                    v |= (int32_t)0xFF000000; // sign extend
                float_in[i] = (float)v / 8388608.0f;
                s += 3;
            }
        } else {
            // S16_LE
            src_short_to_float_array((const short *)pcm_in, float_in, frames * channels);
        }

        // Measure playback buffer fill and adjust ratio
        long avail = ovl_alsa_playback_avail(ctx->playback);
        long fill = buf_size - avail; // frames currently in buffer
        double error = (double)(fill - target_fill) / (double)buf_size;

        // PI controller: P=0.0005, I=0.0000005
        integral += error;
        if (integral > 10000.0)
            integral = 10000.0;
        if (integral < -10000.0)
            integral = -10000.0;
        ratio = 1.0 - error * 0.0005 - integral * 0.0000005;

        // Clamp
        if (ratio < 0.998)
            ratio = 0.998;
        if (ratio > 1.002)
            ratio = 1.002;

        // Resample
        SRC_DATA sd = {
            .data_in = float_in,
            .data_out = float_out,
            .input_frames = frames,
            .output_frames = out_max,
            .src_ratio = ratio,
        };
        if (src_process(src, &sd) != 0)
            continue;

        int out_frames = (int)sd.output_frames_gen;
        if (out_frames <= 0)
            continue;

        // Convert float back to PCM
        if (is_s32) {
            int32_t *d = pcm_out;
            for (int i = 0; i < out_frames * channels; i++) {
                float v = float_out[i] * 2147483648.0f;
                if (v > 2147483647.0f)
                    v = 2147483647.0f;
                if (v < -2147483648.0f)
                    v = -2147483648.0f;
                d[i] = (int32_t)v;
            }
        } else if (is_s24) {
            uint8_t *d = pcm_out;
            for (int i = 0; i < out_frames * channels; i++) {
                float v = float_out[i] * 8388608.0f;
                if (v > 8388607.0f)
                    v = 8388607.0f;
                if (v < -8388608.0f)
                    v = -8388608.0f;
                int32_t s = (int32_t)v;
                d[0] = (uint8_t)(s & 0xFF);
                d[1] = (uint8_t)((s >> 8) & 0xFF);
                d[2] = (uint8_t)((s >> 16) & 0xFF);
                d += 3;
            }
        } else {
            src_float_to_short_array(float_out, (short *)pcm_out, out_frames * channels);
        }

        // Write to playback
        int written = 0;
        while (written < out_frames && running) {
            int r = ovl_alsa_playback_write(ctx->playback, (char *)pcm_out + written * fb,
                                            (unsigned int)(out_frames - written));
            if (r < 0)
                goto cleanup;
            written += r;
        }
    }

cleanup:
    if (src)
        src_delete(src);
done:
    free(pcm_in);
    free(float_in);
    free(float_out);
    free(pcm_out);
    return NULL;
}

// --- run command ---

static int cmd_run(struct options *opts) {
    char vib[32], vob[32], aib[32], aob[32];
    if (resolve_devices(opts, vib, vob, aib, aob, 1) < 0)
        return 1;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    {
        char res_in[32] = "auto", res_out[32] = "auto";
        char fps_in[16] = "auto", fps_out[16] = "auto";
        if (opts->res_in_w)
            snprintf(res_in, sizeof(res_in), "%ux%u", opts->res_in_w, opts->res_in_h);
        if (opts->res_out_w)
            snprintf(res_out, sizeof(res_out), "%ux%u", opts->res_out_w, opts->res_out_h);
        if (opts->fps_in)
            snprintf(fps_in, sizeof(fps_in), "%u", opts->fps_in);
        if (opts->fps_out)
            snprintf(fps_out, sizeof(fps_out), "%u", opts->fps_out);
        ZF_LOGI("config: video-in=%s video-out=%s audio-in=%s audio-out=%s"
                " fmt-in=%s fmt-out=%s res-in=%s res-out=%s fps-in=%s fps-out=%s"
                " async-flip=%d",
                opts->video_in, opts->video_out, opts->audio_in ? opts->audio_in : "(none)",
                opts->audio_out ? opts->audio_out : "(none)",
                opts->fmt_in ? ovl_pixfmt_name(opts->fmt_in) : "auto",
                opts->fmt_out ? ovl_pixfmt_name(opts->fmt_out) : "auto", res_in, res_out, fps_in,
                fps_out, opts->async_flip);
    }

    uint64_t total_frames = 0;
    int edid_written = 0;

    // --- USB HID proxy (independent of video signal) ---
    ovl_usb_init();
    struct ovl_usb_proxy *usb_proxy = NULL;
    if (opts->num_usb_devices > 0) {
        if (ovl_usb_proxy_init(&usb_proxy, opts->usb_udc,
                               opts->usb_devices, opts->num_usb_devices, NULL) < 0) {
            ZF_LOGE("usb proxy init failed");
            return 1;
        }
        ovl_usb_proxy_start(usb_proxy);
    }

    // === Main loop: init → stream → teardown → repeat on signal change ===
    while (running) {
        signal_lost = 0;

        // --- Setup EDID on HDMI input (once) ---
        // Read the output monitor's EDID, append "*" to the name,
        // and write a passthrough EDID to the V4L2 capture device.
        if (!edid_written) {
            // Parse DRM device path from video_out ("device:connector")
            char drm_dev[128];
            const char *conn_name = NULL;
            snprintf(drm_dev, sizeof(drm_dev), "%s", opts->video_out);
            char *colon = strchr(drm_dev, ':');
            if (colon) {
                *colon = '\0';
                conn_name = colon + 1;
            }

            uint8_t out_edid[OVL_EDID_MAX_SIZE];
            int edid_len = ovl_edid_read_drm(drm_dev, conn_name, out_edid, sizeof(out_edid));
            if (edid_len > 0) {
                char mon_name[OVL_EDID_MAX_NAME + 1];
                if (ovl_edid_get_name(out_edid, (size_t)edid_len, mon_name, sizeof(mon_name)) < 0)
                    snprintf(mon_name, sizeof(mon_name), "Display");

                // Build new name: original + "*", truncated to 13 chars
                char pt_name[OVL_EDID_MAX_NAME + 1];
                snprintf(pt_name, sizeof(pt_name), "%.*s*", OVL_EDID_MAX_NAME - 1, mon_name);

                uint32_t edid_w = opts->res_in_w ? opts->res_in_w : 1920;
                uint32_t edid_h = opts->res_in_h ? opts->res_in_h : 1080;
                uint32_t edid_fps = opts->fps_in ? opts->fps_in : 120;

                uint8_t pt_edid[256];
                int pt_len = ovl_edid_build_passthrough(out_edid, (size_t)edid_len, pt_name,
                                                        edid_w, edid_h, edid_fps, pt_edid,
                                                        sizeof(pt_edid));
                if (pt_len > 0) {
                    if (ovl_edid_write_v4l2(opts->video_in, pt_edid, (size_t)pt_len) == 0) {
                        ZF_LOGI("edid: passthrough EDID set as '%s'", pt_name);
                        edid_written = 1;
                    }
                }
            } else {
                ZF_LOGW("edid: could not read output EDID, skipping passthrough setup");
            }
        }

        // --- Query & negotiate ---
        struct ovl_drm_caps display_caps;
        if (ovl_drm_query_caps(opts->video_out, &display_caps) < 0) {
            if (!running)
                return 1;
            ZF_LOGW("DRM query failed, retrying in 2s");
            delay_ms(REINIT_DELAY_MS);
            continue;
        }

        uint32_t v4l2_raw[MAX_FORMATS];
        enum ovl_pixfmt src_fmts[MAX_FORMATS];
        int src_count;

        if (opts->fmt_in) {
            src_fmts[0] = opts->fmt_in;
            src_count = 1;
        } else {
            int raw_count = ovl_v4l2_enum_formats(opts->video_in, v4l2_raw, MAX_FORMATS);
            if (raw_count <= 0) {
                ovl_drm_caps_free(&display_caps);
                if (!running)
                    return 1;
                ZF_LOGW("no V4L2 formats, retrying");
                delay_ms(REINIT_DELAY_MS);
                continue;
            }
            src_count = 0;
            for (int i = 0; i < raw_count; i++) {
                enum ovl_pixfmt f = ovl_pixfmt_from_v4l2(v4l2_raw[i]);
                if (f != OVL_PIXFMT_UNKNOWN)
                    src_fmts[src_count++] = f;
            }
        }

        struct negotiate_result neg;
        if (negotiate(src_fmts, src_count, &display_caps, opts->fmt_out, &neg) < 0) {
            ovl_drm_caps_free(&display_caps);
            if (!running)
                return 1;
            ZF_LOGW("negotiation failed, retrying in 2s");
            delay_ms(REINIT_DELAY_MS);
            continue;
        }

        // --- Init capture ---
        // Try with CLI params first; if capture fails or returns no signal,
        // retry with auto-detect (all zeros).
        struct ovl_v4l2_capture_config cap_cfg = {
            .pixelformat = ovl_pixfmt_to_v4l2(neg.fmt),
            .width = opts->res_in_w,
            .height = opts->res_in_h,
            .framerate = opts->fps_in,
        };
        struct ovl_v4l2_capture cap;
        if (ovl_v4l2_capture_init(&cap, opts->video_in, &cap_cfg) < 0) {
            // If CLI params were set, retry with auto-detect
            if (opts->res_in_w || opts->fps_in) {
                ZF_LOGW("capture init failed with requested params, trying auto-detect");
                struct ovl_v4l2_capture_config auto_cfg = {
                    .pixelformat = ovl_pixfmt_to_v4l2(neg.fmt),
                };
                if (ovl_v4l2_capture_init(&cap, opts->video_in, &auto_cfg) < 0) {
                    ovl_drm_caps_free(&display_caps);
                    if (!running)
                        return 1;
                    ZF_LOGW("capture init failed (%s), retrying in 2s", strerror(errno));
                    delay_ms(REINIT_DELAY_MS);
                    continue;
                }
            } else {
                ovl_drm_caps_free(&display_caps);
                if (!running)
                    return 1;
                ZF_LOGW("capture init failed (%s), retrying in 2s", strerror(errno));
                delay_ms(REINIT_DELAY_MS);
                continue;
            }
        }

        // No usable signal if resolution or fps is missing
        if (!cap.width || !cap.height || !cap.fps) {
            ZF_LOGW("no signal detected (capture=%ux%u@%u), retrying in 2s", cap.width, cap.height,
                    cap.fps);
            ovl_v4l2_capture_free(&cap);
            ovl_drm_caps_free(&display_caps);
            delay_ms(REINIT_DELAY_MS);
            continue;
        }

        // Log requested vs detected
        if (opts->res_in_w && (opts->res_in_w != cap.width || opts->res_in_h != cap.height))
            ZF_LOGW("requested %ux%u but signal is %ux%u, using detected", opts->res_in_w,
                    opts->res_in_h, cap.width, cap.height);
        if (opts->fps_in && opts->fps_in != cap.fps)
            ZF_LOGW("requested %ufps but signal is %ufps, using detected", opts->fps_in, cap.fps);

        // Re-negotiate if driver rejected format
        enum ovl_pixfmt actual = ovl_pixfmt_from_v4l2(cap.pixelformat);
        if (actual != neg.fmt) {
            if (opts->fmt_out) {
                neg.needs_conversion = 1;
                neg.use_primary = plane_supports(&display_caps, PLANE_TYPE_PRIMARY, neg.fmt);
            } else {
                neg.needs_conversion = 0;
                neg.fmt = actual;
                if (plane_supports(&display_caps, PLANE_TYPE_PRIMARY, actual))
                    neg.use_primary = 1;
                else if (plane_supports(&display_caps, PLANE_TYPE_OVERLAY, actual))
                    neg.use_primary = 0;
                else
                    neg.needs_conversion = 1;
            }
        }
        ovl_drm_caps_free(&display_caps);

        uint32_t out_drm_fourcc = ovl_pixfmt_to_drm(neg.fmt);

        ZF_LOGI("capture=%s %ux%u@%u", ovl_pixfmt_name(actual), cap.width, cap.height, cap.fps);
        ZF_LOGI("output=%s plane=%s", ovl_pixfmt_name(neg.fmt),
                neg.use_primary ? "primary" : "overlay");
        if (neg.needs_conversion)
            ZF_LOGD("converter=%s -> %s", ovl_pixfmt_name(actual), ovl_pixfmt_name(neg.fmt));

        // --- Converter ---
        struct ovl_converter *conv = NULL;
        if (neg.needs_conversion) {
            struct ovl_converter_config conv_cfg = {
                .src_fmt = actual,
                .dst_fmt = neg.fmt,
                .width = cap.width,
                .height = cap.height,
                .num_buffers = (int)cap.num_buffers,
            };
            for (uint32_t i = 0; i < cap.num_buffers; i++)
                conv_cfg.src_dmabuf_fds[i] = cap.buffers[i].dmabuf_fds[0];
            if (ovl_converter_create(&conv, &conv_cfg) < 0) {
                ovl_v4l2_capture_free(&cap);
                if (!running)
                    return 1;
                ZF_LOGW("converter init failed, retrying");
                delay_ms(REINIT_DELAY_MS);
                continue;
            }
            ZF_LOGD("backend=%s", ovl_converter_backend_name(conv));
        }

        // --- DRM output ---
        // If no output resolution/fps specified, match the capture to avoid scaling
        uint32_t drm_out_w = opts->res_out_w ? opts->res_out_w : cap.width;
        uint32_t drm_out_h = opts->res_out_h ? opts->res_out_h : cap.height;
        uint32_t drm_out_fps = opts->fps_out ? opts->fps_out : cap.fps;

        struct ovl_drm_output output;
        if (ovl_drm_output_init(&output, opts->video_out, out_drm_fourcc, cap.width, cap.height,
                                drm_out_w, drm_out_h, drm_out_fps) < 0) {
            ovl_converter_destroy(conv);
            ovl_v4l2_capture_free(&cap);
            if (!running)
                return 1;
            ZF_LOGW("DRM init failed, retrying in 2s");
            delay_ms(REINIT_DELAY_MS);
            continue;
        }
        if (opts->async_flip && output.async_supported) {
            output.async_flip = 1;
            ZF_LOGD("async page flip enabled (may cause tearing)");
        } else if (opts->async_flip) {
            ZF_LOGW("async flip requested but not supported; using vsync");
            output.async_flip = 0;
        } else {
            output.async_flip = 0;
        }

        // --- Framebuffers ---
        int fb_indices[OVL_V4L2_NUM_BUFFERS];
        int fb_ok = 1;
        for (uint32_t i = 0; i < cap.num_buffers; i++) {
            int *fds;
            uint32_t *pitches, *offsets;
            int nplanes;
            if (conv) {
                const struct ovl_converter_buffer *cb = ovl_converter_get_output(conv, (int)i);
                fds = (int *)cb->dmabuf_fds;
                pitches = (uint32_t *)cb->pitches;
                offsets = (uint32_t *)cb->offsets;
                nplanes = cb->num_planes;
            } else {
                struct ovl_v4l2_buffer *buf = &cap.buffers[i];
                fds = buf->dmabuf_fds;
                pitches = buf->pitches;
                offsets = buf->offsets;
                nplanes = buf->num_planes;
            }
            fb_indices[i] = ovl_drm_output_add_fb_mp(&output, fds, pitches, offsets, nplanes,
                                                     out_drm_fourcc, cap.width, cap.height);
            if (fb_indices[i] < 0) {
                fb_ok = 0;
                break;
            }
        }
        if (!fb_ok) {
            ovl_drm_output_free(&output);
            ovl_converter_destroy(conv);
            ovl_v4l2_capture_free(&cap);
            if (!running)
                return 1;
            ZF_LOGW("framebuffer creation failed, retrying in 2s");
            delay_ms(REINIT_DELAY_MS);
            continue;
        }

        // --- Setup overlay ---
        struct ovl_overlay *overlay = NULL;
        {
            enum ovl_pixfmt ovl_prefs[] = {
                OVL_PIXFMT_ARGB8888,
                OVL_PIXFMT_ABGR8888,
                OVL_PIXFMT_XRGB8888,
                OVL_PIXFMT_XBGR8888,
            };
            enum ovl_pixfmt ovl_fmt = ovl_drm_output_find_overlay_plane(&output, ovl_prefs, 4);
            if (ovl_fmt != OVL_PIXFMT_UNKNOWN) {
                if (ovl_drm_output_create_overlay_fb(&output, output.crtc_w, output.crtc_h) == 0) {
                    ovl_overlay_create(&overlay, output.crtc_w, output.crtc_h, ovl_fmt);
                }
            }
            if (!overlay)
                ZF_LOGW("overlay not available, running without");
        }

        // --- Processor manager ---
        struct ovl_processor_mgr *proc_mgr = NULL;
        if (overlay) {
            ovl_processor_mgr_create(&proc_mgr, overlay, &output);
            for (int i = 0; i < opts->num_processors; i++) {
                if (ovl_processor_mgr_load_file(proc_mgr, opts->processors[i]) < 0) {
                    ZF_LOGE("failed to load processor '%s', aborting", opts->processors[i]);
                    ovl_processor_mgr_destroy(proc_mgr);
                    ovl_drm_output_free(&output);
                    ovl_converter_destroy(conv);
                    return -1;
                }
            }
            ovl_processor_mgr_start(proc_mgr, actual, cap.width, cap.height);
        }

        // --- Start streaming ---
        if (ovl_v4l2_capture_start(&cap) < 0) {
            ovl_drm_output_free(&output);
            ovl_converter_destroy(conv);
            ovl_v4l2_capture_free(&cap);
            if (!running)
                return 1;
            ZF_LOGW("stream start failed, retrying in 2s");
            delay_ms(REINIT_DELAY_MS);
            continue;
        }

        // Video thread
        struct video_thread_ctx vctx = {
            .cap = &cap,
            .conv = conv,
            .output = &output,
            .overlay = overlay,
            .proc_mgr = proc_mgr,
            .fb_indices = fb_indices,
            .frame_count = 0,
        };
        pthread_t video_tid;
        pthread_create(&video_tid, NULL, video_thread_fn, &vctx);

        // Audio thread
        struct ovl_alsa_capture *acap = NULL;
        struct ovl_alsa_playback *aplay = NULL;
        pthread_t audio_tid = 0;
        int audio_running = 0;

        if (opts->audio_in && opts->audio_out) {
            if (ovl_alsa_capture_init(&acap, opts->audio_in, NULL) == 0) {
                if (ovl_alsa_playback_init(&aplay, opts->audio_out, ovl_alsa_capture_format(acap),
                                           ovl_alsa_capture_rate(acap),
                                           ovl_alsa_capture_channels(acap)) == 0) {
                    static struct audio_thread_ctx actx;
                    actx = (struct audio_thread_ctx){.capture = acap, .playback = aplay};
                    pthread_create(&audio_tid, NULL, audio_thread_fn, &actx);
                    audio_running = 1;
                } else {
                    ovl_alsa_capture_free(acap);
                    acap = NULL;
                }
            }
        }

        ZF_LOGI("streaming started (video%s)", audio_running ? "+audio" : " only");

        // --- Wait for video thread to exit ---
        pthread_join(video_tid, NULL);
        total_frames += vctx.frame_count;

        // --- Stop audio: abort to unblock, join, then free ---
        if (audio_running) {
            ovl_alsa_capture_abort(acap); // unblocks snd_pcm_readi
            pthread_join(audio_tid, NULL);
            ovl_alsa_capture_free(acap);
            ovl_alsa_playback_free(aplay);
        }

        // --- Teardown ---
        ovl_processor_mgr_destroy(proc_mgr);
        ovl_overlay_destroy(overlay);
        ovl_drm_output_free(&output);
        ovl_converter_destroy(conv);
        ovl_v4l2_capture_free(&cap);

        // --- If signal changed, wait for new signal and reinit ---
        if (signal_lost && running) {
            ZF_LOGI("signal lost, waiting for new signal...");
            while (running) {
                delay_ms(REINIT_DELAY_MS);
                struct ovl_video_caps vcaps;
                if (ovl_v4l2_query_caps(opts->video_in, &vcaps) == 0) {
                    if (vcaps.num_modes > 0) {
                        ZF_LOGI("new signal detected, reinitializing");
                        ovl_v4l2_caps_free(&vcaps);
                        delay_ms(REINIT_DELAY_MS);
                        break;
                    }
                    ovl_v4l2_caps_free(&vcaps);
                }
            }
            continue; // restart the main loop
        }
    }

    ovl_usb_proxy_destroy(usb_proxy);
    ovl_usb_exit();

    ZF_LOGI("stopped, total frames: %llu", (unsigned long long)total_frames);
    return 0;
}

// --- Main ---

int main(int argc, char *argv[]) {
    struct options opts;
    if (parse_args(argc, argv, &opts) < 0)
        return 1;

    // Load TOML config — CLI values already in opts take precedence
    char cfgbuf[512];
    const char *cfg_path = opts.config_path;
    if (!cfg_path) {
        if (ovl_config_default_path(cfgbuf, sizeof(cfgbuf)) == 0)
            cfg_path = cfgbuf;
    }
    if (cfg_path) {
        int rc = ovl_config_load(cfg_path, &opts);
        if (rc < 0)
            return 1;
        if (rc == 0)
            ZF_LOGI("loaded config from %s", cfg_path);
    }

    if (opts.log_level >= 0)
        zf_log_set_output_level(opts.log_level);

    if (strcmp(opts.command, "run") == 0)
        return cmd_run(&opts);
    else if (strcmp(opts.command, "query") == 0)
        return cmd_query(&opts);
    else {
        ZF_LOGE("Unknown command '%s'", opts.command);
        print_usage(argv[0]);
        return 1;
    }
}

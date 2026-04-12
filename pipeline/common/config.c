#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tomlc17.h>
#include "config.h"
#include "log.h"

int ovl_config_default_path(char *buf, int len) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return -1;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return -1;
    int w = snprintf(buf, (size_t)len, "%.*s/overlAIer.toml",
                     (int)(slash - exe), exe);
    return (w >= len) ? -1 : 0;
}

static const char *seek_string(toml_datum_t root, const char *key) {
    toml_datum_t d = toml_seek(root, key);
    return (d.type == TOML_STRING) ? d.u.s : NULL;
}

static int64_t seek_int(toml_datum_t root, const char *key, int64_t fallback) {
    toml_datum_t d = toml_seek(root, key);
    return (d.type == TOML_INT64) ? d.u.int64 : fallback;
}

static int seek_bool(toml_datum_t root, const char *key, int fallback) {
    toml_datum_t d = toml_seek(root, key);
    return (d.type == TOML_BOOLEAN) ? d.u.boolean : fallback;
}

int ovl_config_load(const char *path, struct options *opts) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 1;
        ZF_LOGE("config: cannot open '%s': %s", path, strerror(errno));
        return -1;
    }

    toml_result_t res = toml_parse_file(f);
    fclose(f);
    if (!res.ok) {
        ZF_LOGE("config: parse error in '%s': %s", path, res.errmsg);
        return -1;
    }

    toml_datum_t root = res.toptab;
    const char *s;

    // [device]
    if (!opts->video_in) {
        s = seek_string(root, "device.video_in");
        if (s) opts->video_in = strdup(s);
    }
    if (!opts->video_out) {
        s = seek_string(root, "device.video_out");
        if (s) opts->video_out = strdup(s);
    }
    if (!opts->audio_in) {
        s = seek_string(root, "device.audio_in");
        if (s) opts->audio_in = strdup(s);
    }
    if (!opts->audio_out) {
        s = seek_string(root, "device.audio_out");
        if (s) opts->audio_out = strdup(s);
    }

    // [format]
    if (!opts->fmt_in) {
        s = seek_string(root, "format.fmt_in");
        if (s) opts->fmt_in = parse_format(s);
    }
    if (!opts->fmt_out) {
        s = seek_string(root, "format.fmt_out");
        if (s) opts->fmt_out = parse_format(s);
    }
    if (!opts->res_in_w) {
        s = seek_string(root, "format.res_in");
        if (s) parse_resolution(s, &opts->res_in_w, &opts->res_in_h);
    }
    if (!opts->res_out_w) {
        s = seek_string(root, "format.res_out");
        if (s) parse_resolution(s, &opts->res_out_w, &opts->res_out_h);
    }
    if (!opts->fps_in)
        opts->fps_in = (uint32_t)seek_int(root, "format.fps_in", 0);
    if (!opts->fps_out)
        opts->fps_out = (uint32_t)seek_int(root, "format.fps_out", 0);

    // [general]
    if (opts->log_level < 0) {
        s = seek_string(root, "general.log_level");
        if (s) opts->log_level = parse_log_level(s);
    }
    if (!opts->async_flip)
        opts->async_flip = seek_bool(root, "general.async_flip", 0);

    // [[processor]] array
    if (!opts->num_processors) {
        toml_datum_t parr = toml_get(root, "processor");
        if (parr.type == TOML_ARRAY) {
            for (int i = 0; i < parr.u.arr.size && opts->num_processors < OPT_MAX_PROCESSORS; i++) {
                toml_datum_t entry = parr.u.arr.elem[i];
                if (entry.type != TOML_TABLE)
                    continue;
                toml_datum_t path = toml_get(entry, "path");
                if (path.type == TOML_STRING) {
                    opts->processors[opts->num_processors++] = strdup(path.u.s);
                }
            }
        }
    }

    toml_free(res);
    return 0;
}

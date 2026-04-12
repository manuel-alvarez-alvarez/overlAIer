#include <stdio.h>
#include <strings.h>
#include "options.h"
#include "log.h"

enum ovl_pixfmt parse_format(const char *s) {
    enum ovl_pixfmt f = ovl_pixfmt_from_str(s);
    if (f == OVL_PIXFMT_UNKNOWN)
        ZF_LOGE("Unknown format '%s'", s);
    return f;
}

int parse_resolution(const char *s, uint32_t *w, uint32_t *h) {
    if (sscanf(s, "%ux%u", w, h) == 2 && *w > 0 && *h > 0)
        return 0;
    ZF_LOGE("Invalid resolution '%s' (expected WIDTHxHEIGHT)", s);
    return -1;
}

int parse_log_level(const char *s) {
    if (strcasecmp(s, "verbose") == 0 || strcasecmp(s, "v") == 0)
        return ZF_LOG_VERBOSE;
    if (strcasecmp(s, "debug") == 0 || strcasecmp(s, "d") == 0)
        return ZF_LOG_DEBUG;
    if (strcasecmp(s, "info") == 0 || strcasecmp(s, "i") == 0)
        return ZF_LOG_INFO;
    if (strcasecmp(s, "warn") == 0 || strcasecmp(s, "w") == 0)
        return ZF_LOG_WARN;
    if (strcasecmp(s, "error") == 0 || strcasecmp(s, "e") == 0)
        return ZF_LOG_ERROR;
    if (strcasecmp(s, "fatal") == 0 || strcasecmp(s, "f") == 0)
        return ZF_LOG_FATAL;
    if (strcasecmp(s, "none") == 0 || strcasecmp(s, "n") == 0)
        return ZF_LOG_NONE;
    return -1;
}

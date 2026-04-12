#ifndef OVERLAIER_CONFIG_H
#define OVERLAIER_CONFIG_H

#include "options.h"

// Load TOML config from `path` and fill in any fields in `opts` that are
// still at their zero/default value (i.e., not set by CLI).
// Returns 0 on success, -1 on error, 1 if file does not exist.
int ovl_config_load(const char *path, struct options *opts);

// Write the default config path (~/.overlaier/overlAIer.toml) into buf.
// Returns 0 on success, -1 if HOME is unset or buf is too small.
int ovl_config_default_path(char *buf, int len);

#endif

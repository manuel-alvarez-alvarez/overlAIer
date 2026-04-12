#include "processor_mgr.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../common/log.h"

// Per-processor runtime state
struct proc_slot {
    const struct ovl_processor_def *def;
    void *state; // processor's opaque state
    pthread_t thread;
    int running;

    // Frame mailbox: video thread writes, processor thread reads
    pthread_mutex_t frame_lock;
    pthread_cond_t frame_cond;
    void *frame_buf;           // copy of the frame for this processor
    struct ovl_frame_info frame_info; // capture metadata
    int frame_ready;           // 1 = new frame available

    // Output primitives (owned by processor, read by compositor)
    struct ovl_primitive *prims;
    int prim_count;
    int prims_dirty; // 1 = new primitives since last composite
};

struct ovl_processor_mgr {
    struct ovl_overlay *overlay;
    struct ovl_drm_output *output;

    struct proc_slot slots[OVL_MAX_PROCESSORS];
    int num_processors;

    // Loaded shared libraries
    void *dl_handles[OVL_MAX_PROCESSORS];
    int num_dl_handles;

    // Source format info
    enum ovl_pixfmt src_fmt;
    uint32_t src_width, src_height;

    volatile int active; // 0 = shutting down
};

// --- Processor thread ---

static void *processor_thread_fn(void *arg) {
    struct proc_slot *slot = arg;
    const struct ovl_processor_def *def = slot->def;

    while (slot->running) {
        pthread_mutex_lock(&slot->frame_lock);
        while (!slot->frame_ready && slot->running)
            pthread_cond_wait(&slot->frame_cond, &slot->frame_lock);
        if (!slot->running) {
            pthread_mutex_unlock(&slot->frame_lock);
            break;
        }
        slot->frame_ready = 0;
        pthread_mutex_unlock(&slot->frame_lock);

        struct ovl_primitive *prims = NULL;
        int count = 0;
        def->on_frame(slot->state, slot->frame_buf, &slot->frame_info, &prims, &count);

        slot->prims = prims;
        slot->prim_count = count;
        slot->prims_dirty = 1;
    }

    return NULL;
}

// --- Compositor: collects primitives from all processors and renders ---

static void composite_overlay(struct ovl_processor_mgr *mgr) {
    int any_dirty = 0;
    for (int i = 0; i < mgr->num_processors; i++) {
        if (mgr->slots[i].prims_dirty) {
            any_dirty = 1;
            mgr->slots[i].prims_dirty = 0;
        }
    }
    if (!any_dirty)
        return;

    ovl_overlay_clear(mgr->overlay);

    // Render all processors' primitives in order
    for (int i = 0; i < mgr->num_processors; i++) {
        struct proc_slot *slot = &mgr->slots[i];
        if (slot->prims && slot->prim_count > 0)
            ovl_overlay_render(mgr->overlay, slot->prims, slot->prim_count);
    }

    // Update the DRM overlay plane
    ovl_drm_output_update_overlay(mgr->output, ovl_overlay_get_buffer(mgr->overlay),
                                  ovl_overlay_get_stride(mgr->overlay));
}

// --- Public API ---

int ovl_processor_mgr_create(struct ovl_processor_mgr **out, struct ovl_overlay *overlay,
                             struct ovl_drm_output *output) {
    struct ovl_processor_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr)
        return -1;

    mgr->overlay = overlay;
    mgr->output = output;
    mgr->active = 1;

    *out = mgr;
    return 0;
}

int ovl_processor_mgr_register(struct ovl_processor_mgr *mgr, const struct ovl_processor_def *def) {
    if (mgr->num_processors >= OVL_MAX_PROCESSORS) {
        ZF_LOGE("processor_mgr: max processors reached");
        return -1;
    }

    struct proc_slot *slot = &mgr->slots[mgr->num_processors++];
    memset(slot, 0, sizeof(*slot));
    slot->def = def;
    pthread_mutex_init(&slot->frame_lock, NULL);
    pthread_cond_init(&slot->frame_cond, NULL);

    ZF_LOGD("processor_mgr: registered '%s'", def->name);
    return 0;
}

int ovl_processor_mgr_load_dir(struct ovl_processor_mgr *mgr, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        ZF_LOGW("processor_mgr: cannot open plugin dir '%s'", dir);
        return 0; // not fatal — just no plugins
    }

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        void *handle = dlopen(path, RTLD_NOW);
        if (!handle) {
            ZF_LOGW("processor_mgr: failed to load '%s': %s", path, dlerror());
            continue;
        }

        ovl_processor_register_fn reg_fn =
            (ovl_processor_register_fn)dlsym(handle, OVL_PROCESSOR_EXPORT_SYMBOL);
        if (!reg_fn) {
            ZF_LOGW("processor_mgr: '%s' has no %s symbol", ent->d_name,
                    OVL_PROCESSOR_EXPORT_SYMBOL);
            dlclose(handle);
            continue;
        }

        const struct ovl_processor_def *def = reg_fn();
        if (!def || !def->name) {
            ZF_LOGW("processor_mgr: '%s' returned NULL definition", ent->d_name);
            dlclose(handle);
            continue;
        }

        if (ovl_processor_mgr_register(mgr, def) == 0) {
            mgr->dl_handles[mgr->num_dl_handles++] = handle;
            loaded++;
            ZF_LOGD("processor_mgr: loaded plugin '%s' from %s", def->name, ent->d_name);
        } else {
            dlclose(handle);
        }
    }

    closedir(d);
    return loaded;
}

int ovl_processor_mgr_load_file(struct ovl_processor_mgr *mgr, const char *path) {
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        ZF_LOGE("processor_mgr: failed to load '%s': %s", path, dlerror());
        return -1;
    }

    ovl_processor_register_fn reg_fn =
        (ovl_processor_register_fn)dlsym(handle, OVL_PROCESSOR_EXPORT_SYMBOL);
    if (!reg_fn) {
        ZF_LOGE("processor_mgr: '%s' has no %s symbol", path, OVL_PROCESSOR_EXPORT_SYMBOL);
        dlclose(handle);
        return -1;
    }

    const struct ovl_processor_def *def = reg_fn();
    if (!def || !def->name) {
        ZF_LOGE("processor_mgr: '%s' returned NULL definition", path);
        dlclose(handle);
        return -1;
    }

    if (ovl_processor_mgr_register(mgr, def) != 0) {
        dlclose(handle);
        return -1;
    }

    mgr->dl_handles[mgr->num_dl_handles++] = handle;
    ZF_LOGD("processor_mgr: loaded plugin '%s' from %s", def->name, path);
    return 0;
}

int ovl_processor_mgr_start(struct ovl_processor_mgr *mgr, enum ovl_pixfmt src_fmt,
                            uint32_t src_width, uint32_t src_height) {
    mgr->src_fmt = src_fmt;
    mgr->src_width = src_width;
    mgr->src_height = src_height;

    for (int i = 0; i < mgr->num_processors; i++) {
        struct proc_slot *slot = &mgr->slots[i];
        const struct ovl_processor_def *def = slot->def;

        // Allocate frame buffer (same format/resolution as capture)
        int bpp = ovl_pixfmt_bpp(src_fmt);
        if (bpp <= 0)
            bpp = 3;
        slot->frame_buf = malloc((size_t)(src_width * (uint32_t)bpp * src_height));
        slot->frame_info.width = src_width;
        slot->frame_info.height = src_height;
        slot->frame_info.stride = src_width * (uint32_t)bpp;

        slot->state = def->init(def, src_width, src_height, src_fmt);
        if (!slot->state) {
            ZF_LOGW("processor_mgr: '%s' init failed, skipping", def->name);
            free(slot->frame_buf);
            slot->frame_buf = NULL;
            slot->running = 0;
            continue;
        }

        // Start thread
        slot->running = 1;
        pthread_create(&slot->thread, NULL, processor_thread_fn, slot);

        ZF_LOGD("processor_mgr: started '%s' (%ux%u)", def->name, src_width, src_height);
    }

    return 0;
}

void ovl_processor_mgr_post_frame(struct ovl_processor_mgr *mgr, const void *frame_data,
                                  const struct ovl_frame_info *info) {
    if (!mgr || !mgr->active)
        return;

    for (int i = 0; i < mgr->num_processors; i++) {
        struct proc_slot *slot = &mgr->slots[i];
        if (!slot->running)
            continue;

        // TODO: convert format/scale if processor needs different input
        // For now, copy the frame as-is (works when processor wants same format)
        uint32_t w = slot->frame_info.width;
        uint32_t h = slot->frame_info.height;
        uint32_t src_stride = info->stride;
        uint32_t dst_stride = slot->frame_info.stride;
        uint32_t copy_bytes = dst_stride < src_stride ? dst_stride : src_stride;

        // Non-blocking: if processor is still busy, just overwrite the buffer
        pthread_mutex_lock(&slot->frame_lock);
        if (frame_data && w == info->width && h == info->height) {
            for (uint32_t y = 0; y < h; y++)
                memcpy((char *)slot->frame_buf + y * dst_stride,
                       (const char *)frame_data + y * src_stride, copy_bytes);
        }
        slot->frame_info.sequence = info->sequence;
        slot->frame_info.timestamp_us = info->timestamp_us;
        slot->frame_ready = 1;
        pthread_cond_signal(&slot->frame_cond);
        pthread_mutex_unlock(&slot->frame_lock);
    }

    // Composite after posting (check if any processor has new output)
    composite_overlay(mgr);
}

void ovl_processor_mgr_notify_flip(struct ovl_processor_mgr *mgr,
                                   const struct ovl_frame_info *info) {
    if (!mgr || !mgr->active)
        return;

    for (int i = 0; i < mgr->num_processors; i++) {
        struct proc_slot *slot = &mgr->slots[i];
        if (!slot->running || !slot->def->on_flip)
            continue;
        slot->def->on_flip(slot->state, info);
    }
}

void ovl_processor_mgr_destroy(struct ovl_processor_mgr *mgr) {
    if (!mgr)
        return;
    mgr->active = 0;

    // Signal all threads to stop
    for (int i = 0; i < mgr->num_processors; i++) {
        struct proc_slot *slot = &mgr->slots[i];
        slot->running = 0;
        pthread_mutex_lock(&slot->frame_lock);
        slot->frame_ready = 1; // unblock
        pthread_cond_signal(&slot->frame_cond);
        pthread_mutex_unlock(&slot->frame_lock);
    }

    // Join all threads
    for (int i = 0; i < mgr->num_processors; i++) {
        struct proc_slot *slot = &mgr->slots[i];
        pthread_join(slot->thread, NULL);
        if (slot->def->destroy && slot->state)
            slot->def->destroy(slot->state);
        free(slot->frame_buf);
        pthread_mutex_destroy(&slot->frame_lock);
        pthread_cond_destroy(&slot->frame_cond);
    }

    // Close loaded shared libraries
    for (int i = 0; i < mgr->num_dl_handles; i++)
        dlclose(mgr->dl_handles[i]);

    free(mgr);
}

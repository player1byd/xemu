/*
 * NV2A Vulkan preparation failure-state helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_FAILURE_STATE_H
#define HW_XBOX_NV2A_PGRAPH_VK_FAILURE_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*PGRAPHVkUnmapFunc)(void *opaque);
typedef void (*PGRAPHVkReleaseFunc)(void *data);

typedef struct PGRAPHVkMappedMemory {
    PGRAPHVkUnmapFunc unmap;
    void *opaque;
    bool mapped;
} PGRAPHVkMappedMemory;

static inline bool pgraph_vk_mapped_memory_begin(PGRAPHVkMappedMemory *mapping,
                                                 bool map_succeeded,
                                                 PGRAPHVkUnmapFunc unmap,
                                                 void *opaque)
{
    mapping->unmap = unmap;
    mapping->opaque = opaque;
    mapping->mapped = map_succeeded;
    return map_succeeded;
}

static inline void pgraph_vk_mapped_memory_cleanup(PGRAPHVkMappedMemory *mapping)
{
    if (mapping->mapped) {
        mapping->unmap(mapping->opaque);
        mapping->mapped = false;
    }
}

/* Decoded levels may either own storage or borrow guest VRAM. */
static inline void pgraph_vk_owned_payload_cleanup(
    bool owns_data, void **data, PGRAPHVkReleaseFunc release)
{
    if (owns_data && *data) {
        release(*data);
        *data = NULL;
    }
}

/* A failed readback must leave guest-visible data pending for a later retry. */
static inline bool pgraph_vk_surface_download_complete(bool succeeded,
                                                       bool *download_pending,
                                                       bool *draw_dirty)
{
    if (!succeeded) {
        *download_pending = true;
        *draw_dirty = true;
        return false;
    }

    *download_pending = false;
    *draw_dirty = false;
    return true;
}

/* Callers which join an existing batch must preserve its completion result. */
static inline bool pgraph_vk_download_batch_begin(bool downloads_pending,
                                                  bool *downloads_succeeded)
{
    if (downloads_pending) {
        return false;
    }

    *downloads_succeeded = true;
    return true;
}

/* A surface enrolled after traversal completes must start the next batch. */
static inline bool pgraph_vk_download_batch_enroll(bool downloads_pending,
                                                   bool surface_dirty,
                                                   bool *downloads_succeeded)
{
    return surface_dirty &&
           pgraph_vk_download_batch_begin(downloads_pending,
                                          downloads_succeeded);
}

/* A failed upload cannot advance the validated content hash. */
static inline bool pgraph_vk_texture_upload_complete(bool succeeded,
                                                     uint64_t content_hash,
                                                     uint64_t *stored_hash,
                                                     bool *possibly_dirty)
{
    if (!succeeded) {
        *possibly_dirty = true;
        return false;
    }

    *stored_hash = content_hash;
    *possibly_dirty = false;
    return true;
}

#endif

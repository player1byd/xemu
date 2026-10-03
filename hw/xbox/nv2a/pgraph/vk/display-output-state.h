/*
 * Completed Vulkan display output and UI host-copy upload identity.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_DISPLAY_OUTPUT_STATE_H
#define HW_XBOX_NV2A_PGRAPH_VK_DISPLAY_OUTPUT_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct PGRAPHVkHostCopyUploadState {
    bool valid;
    uint64_t generation;
    int width;
    int height;
} PGRAPHVkHostCopyUploadState;

static inline bool pgraph_vk_host_copy_upload_needed(
    const PGRAPHVkHostCopyUploadState *upload, uint64_t completed_generation,
    int width, int height)
{
    return !upload->valid || upload->generation != completed_generation ||
           upload->width != width || upload->height != height;
}

static inline void pgraph_vk_host_copy_mark_uploaded(
    PGRAPHVkHostCopyUploadState *upload, uint64_t completed_generation,
    int width, int height)
{
    upload->generation = completed_generation;
    upload->width = width;
    upload->height = height;
    upload->valid = true;
}

static inline void pgraph_vk_host_copy_invalidate_upload(
    PGRAPHVkHostCopyUploadState *upload)
{
    upload->valid = false;
}

static inline void pgraph_vk_host_copy_publish_completed(
    uint64_t *completed_generation, PGRAPHVkHostCopyUploadState *upload)
{
    ++*completed_generation;
    if (*completed_generation == 0) {
        *completed_generation = 1;
        pgraph_vk_host_copy_invalidate_upload(upload);
    }
}

#endif

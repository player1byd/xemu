/*
 * Vulkan driver pipeline-cache persistence policy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_PIPELINE_CACHE_DATA_H
#define HW_XBOX_NV2A_PGRAPH_VK_PIPELINE_CACHE_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGRAPH_VK_PIPELINE_CACHE_MAX_FILE_SIZE (64u * 1024u * 1024u)

typedef struct PGRAPHVkPipelineCacheIdentity {
    uint32_t vendor_id;
    uint32_t device_id;
    uint8_t uuid[16];
} PGRAPHVkPipelineCacheIdentity;

bool pgraph_vk_pipeline_cache_data_compatible(
    const void *data, size_t size,
    const PGRAPHVkPipelineCacheIdentity *identity);

#endif

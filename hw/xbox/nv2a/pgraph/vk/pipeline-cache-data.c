/*
 * Vulkan driver pipeline-cache persistence policy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pipeline-cache-data.h"

#include <string.h>

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool pgraph_vk_pipeline_cache_data_compatible(
    const void *data, size_t size,
    const PGRAPHVkPipelineCacheIdentity *identity)
{
    const uint8_t *bytes = data;

    if (!bytes || !identity || size < 32 ||
        size > PGRAPH_VK_PIPELINE_CACHE_MAX_FILE_SIZE) {
        return false;
    }

    /* VkPipelineCacheHeaderVersionOne has a fixed, little-endian header. */
    return read_le32(bytes) == 32 && read_le32(bytes + 4) == 1 &&
           read_le32(bytes + 8) == identity->vendor_id &&
           read_le32(bytes + 12) == identity->device_id &&
           !memcmp(bytes + 16, identity->uuid, sizeof(identity->uuid));
}

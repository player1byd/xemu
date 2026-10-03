/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "hw/xbox/nv2a/pgraph/vk/pipeline-cache-data.h"

static void put_u32(uint8_t *data, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; i++) {
        data[offset + i] = value >> (8 * i);
    }
}

static void test_cache_data_identity(void)
{
    uint8_t data[40] = { 0 };
    PGRAPHVkPipelineCacheIdentity identity = {
        .vendor_id = 0x10de,
        .device_id = 0x2204,
        .uuid = { 1, 2, 3, 4 },
    };

    put_u32(data, 0, 32); /* headerSize */
    put_u32(data, 4, 1); /* VK_PIPELINE_CACHE_HEADER_VERSION_ONE */
    put_u32(data, 8, identity.vendor_id);
    put_u32(data, 12, identity.device_id);
    memcpy(data + 16, identity.uuid, sizeof(identity.uuid));

    assert(pgraph_vk_pipeline_cache_data_compatible(data, sizeof(data),
                                                    &identity));
    assert(!pgraph_vk_pipeline_cache_data_compatible(data, 31, &identity));
    assert(!pgraph_vk_pipeline_cache_data_compatible(
        data, PGRAPH_VK_PIPELINE_CACHE_MAX_FILE_SIZE + 1u, &identity));

    PGRAPHVkPipelineCacheIdentity other = identity;
    other.uuid[0]++;
    assert(
        !pgraph_vk_pipeline_cache_data_compatible(data, sizeof(data), &other));
    other = identity;
    other.device_id++;
    assert(
        !pgraph_vk_pipeline_cache_data_compatible(data, sizeof(data), &other));

    put_u32(data, 4, 2);
    assert(!pgraph_vk_pipeline_cache_data_compatible(data, sizeof(data),
                                                     &identity));
    put_u32(data, 4, 1);
    put_u32(data, 0, 33);
    assert(!pgraph_vk_pipeline_cache_data_compatible(data, sizeof(data),
                                                     &identity));
    put_u32(data, 0, 41);
    assert(!pgraph_vk_pipeline_cache_data_compatible(data, sizeof(data),
                                                     &identity));
}

int main(void)
{
    test_cache_data_identity();
    return 0;
}

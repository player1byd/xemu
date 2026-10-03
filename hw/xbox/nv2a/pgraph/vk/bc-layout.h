/*
 * NV2A Vulkan block-compressed texture layout helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_BC_LAYOUT_H
#define HW_XBOX_NV2A_PGRAPH_VK_BC_LAYOUT_H

#include "qemu/osdep.h"

static inline size_t pgraph_vk_bc_staging_alignment(
    size_t device_alignment, size_t block_size)
{
    return MAX(MAX((size_t)4, device_alignment), block_size);
}

static inline size_t pgraph_vk_bc_mip_size(unsigned int width,
                                           unsigned int height,
                                           unsigned int block_size)
{
    width = MAX(width, 1);
    height = MAX(height, 1);
    return DIV_ROUND_UP(width, 4) * DIV_ROUND_UP(height, 4) * block_size;
}

static inline size_t pgraph_vk_bc_layer_size(unsigned int width,
                                             unsigned int height,
                                             unsigned int levels,
                                             unsigned int block_size)
{
    size_t size = 0;

    for (unsigned int level = 0; level < levels; level++) {
        size += pgraph_vk_bc_mip_size(width, height, block_size);
        width /= 2;
        height /= 2;
    }

    return size;
}

#endif

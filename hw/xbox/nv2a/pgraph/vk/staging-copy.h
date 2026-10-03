/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_STAGING_COPY_H
#define HW_XBOX_NV2A_PGRAPH_VK_STAGING_COPY_H

#include <volk.h>
#include <vk_mem_alloc.h>

#ifndef PGRAPH_VK_STAGING_FLUSH_ALLOCATION
#define PGRAPH_VK_STAGING_FLUSH_ALLOCATION vmaFlushAllocation
#endif

#ifndef PGRAPH_VK_STAGING_COPY_BUFFER
#define PGRAPH_VK_STAGING_COPY_BUFFER vkCmdCopyBuffer
#endif

#ifndef PGRAPH_VK_STAGING_PIPELINE_BARRIER
#define PGRAPH_VK_STAGING_PIPELINE_BARRIER vkCmdPipelineBarrier
#endif

static inline VkResult pgraph_vk_record_staging_copy(
    VmaAllocator allocator, VmaAllocation source_allocation,
    VkCommandBuffer command_buffer, VkBuffer source_buffer,
    VkBuffer destination_buffer, VkDeviceSize bytes,
    VkAccessFlags destination_access_mask,
    VkPipelineStageFlags destination_stage_mask)
{
    if (!bytes) {
        return VK_SUCCESS;
    }

    VkResult result = PGRAPH_VK_STAGING_FLUSH_ALLOCATION(
        allocator, source_allocation, 0, bytes);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkBufferCopy copy_region = { .size = bytes };
    PGRAPH_VK_STAGING_COPY_BUFFER(command_buffer, source_buffer,
                                  destination_buffer, 1, &copy_region);

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = destination_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = destination_buffer,
        .size = bytes,
    };
    PGRAPH_VK_STAGING_PIPELINE_BARRIER(
        command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        destination_stage_mask, 0, 0, NULL, 1, &barrier, 0, NULL);

    return VK_SUCCESS;
}

#endif

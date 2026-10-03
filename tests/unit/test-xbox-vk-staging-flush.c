/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include <volk.h>
#include <vk_mem_alloc.h>

enum TestEvent {
    TEST_EVENT_FLUSH,
    TEST_EVENT_COPY,
    TEST_EVENT_BARRIER,
};

static enum TestEvent events[3];
static unsigned int event_count;
static VmaAllocation flushed_allocation;
static VkDeviceSize flushed_offset;
static VkDeviceSize flushed_size;
static VkDeviceSize copied_size;
static VkDeviceSize barrier_size;

static VkResult test_flush_allocation(VmaAllocator allocator,
                                      VmaAllocation allocation,
                                      VkDeviceSize offset,
                                      VkDeviceSize size)
{
    g_assert_true(allocator == (VmaAllocator)(uintptr_t)7);
    events[event_count++] = TEST_EVENT_FLUSH;
    flushed_allocation = allocation;
    flushed_offset = offset;
    flushed_size = size;
    return VK_SUCCESS;
}

static void test_copy_buffer(VkCommandBuffer command_buffer,
                             VkBuffer source_buffer,
                             VkBuffer destination_buffer,
                             uint32_t region_count,
                             const VkBufferCopy *regions)
{
    g_assert_true(command_buffer == (VkCommandBuffer)(uintptr_t)8);
    g_assert_true(source_buffer == (VkBuffer)(uintptr_t)10);
    g_assert_true(destination_buffer == (VkBuffer)(uintptr_t)11);
    g_assert_cmpuint(region_count, ==, 1);
    events[event_count++] = TEST_EVENT_COPY;
    copied_size = regions[0].size;
}

static void test_pipeline_barrier(
    VkCommandBuffer command_buffer, VkPipelineStageFlags source_stage,
    VkPipelineStageFlags destination_stage, VkDependencyFlags dependencies,
    uint32_t memory_barrier_count, const VkMemoryBarrier *memory_barriers,
    uint32_t buffer_barrier_count,
    const VkBufferMemoryBarrier *buffer_barriers,
    uint32_t image_barrier_count, const VkImageMemoryBarrier *image_barriers)
{
    g_assert_true(command_buffer == (VkCommandBuffer)(uintptr_t)8);
    g_assert_cmpuint(source_stage, ==, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_assert_cmpuint(destination_stage, ==,
                     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    g_assert_cmpuint(dependencies, ==, 0);
    g_assert_cmpuint(memory_barrier_count, ==, 0);
    g_assert_null(memory_barriers);
    g_assert_cmpuint(buffer_barrier_count, ==, 1);
    g_assert_cmpuint(image_barrier_count, ==, 0);
    g_assert_null(image_barriers);
    g_assert_cmpuint(buffer_barriers[0].srcAccessMask, ==,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    g_assert_cmpuint(buffer_barriers[0].dstAccessMask, ==,
                     VK_ACCESS_INDEX_READ_BIT);
    g_assert_cmpuint(buffer_barriers[0].srcQueueFamilyIndex, ==,
                     VK_QUEUE_FAMILY_IGNORED);
    g_assert_cmpuint(buffer_barriers[0].dstQueueFamilyIndex, ==,
                     VK_QUEUE_FAMILY_IGNORED);
    g_assert_true(buffer_barriers[0].buffer ==
                  (VkBuffer)(uintptr_t)11);
    events[event_count++] = TEST_EVENT_BARRIER;
    barrier_size = buffer_barriers[0].size;
}

#define PGRAPH_VK_STAGING_FLUSH_ALLOCATION test_flush_allocation
#define PGRAPH_VK_STAGING_COPY_BUFFER test_copy_buffer
#define PGRAPH_VK_STAGING_PIPELINE_BARRIER test_pipeline_barrier
#include "hw/xbox/nv2a/pgraph/vk/staging-copy.h"

static void reset_observations(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0;
    flushed_allocation = VK_NULL_HANDLE;
    flushed_offset = 0;
    flushed_size = 0;
    copied_size = 0;
    barrier_size = 0;
}

static void test_empty_staging_is_ignored(void)
{
    reset_observations();

    VkResult result = pgraph_vk_record_staging_copy(
        (VmaAllocator)(uintptr_t)7, (VmaAllocation)(uintptr_t)42,
        (VkCommandBuffer)(uintptr_t)8, (VkBuffer)(uintptr_t)10,
        (VkBuffer)(uintptr_t)11, 0, VK_ACCESS_INDEX_READ_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

    g_assert_cmpint(result, ==, VK_SUCCESS);
    g_assert_cmpuint(event_count, ==, 0);
}

static void test_flush_precedes_copy_and_barrier(void)
{
    reset_observations();

    VkResult result = pgraph_vk_record_staging_copy(
        (VmaAllocator)(uintptr_t)7, (VmaAllocation)(uintptr_t)42,
        (VkCommandBuffer)(uintptr_t)8, (VkBuffer)(uintptr_t)10,
        (VkBuffer)(uintptr_t)11, 129, VK_ACCESS_INDEX_READ_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

    g_assert_cmpint(result, ==, VK_SUCCESS);
    g_assert_cmpuint(event_count, ==, 3);
    g_assert_cmpint(events[0], ==, TEST_EVENT_FLUSH);
    g_assert_cmpint(events[1], ==, TEST_EVENT_COPY);
    g_assert_cmpint(events[2], ==, TEST_EVENT_BARRIER);
    g_assert_true(flushed_allocation == (VmaAllocation)(uintptr_t)42);
    g_assert_cmpuint(flushed_offset, ==, 0);
    g_assert_cmpuint(flushed_size, ==, 129);
    g_assert_cmpuint(copied_size, ==, 129);
    g_assert_cmpuint(barrier_size, ==, 129);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/staging-copy/empty",
                    test_empty_staging_is_ignored);
    g_test_add_func("/xbox/vk/staging-copy/ordered",
                    test_flush_precedes_copy_and_barrier);
    return g_test_run();
}

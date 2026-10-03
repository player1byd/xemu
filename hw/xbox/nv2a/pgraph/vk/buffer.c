/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "renderer.h"
#include "vertex-version-policy.h"

/*
 * A 4096x4096 four-byte image is the largest unscaled linear guest image the
 * renderer needs to stage. Internal scaling and depth/stencil conversion can
 * require more, so those paths grow their scratch buffer before recording the
 * copy rather than reserving for a hypothetical 10K image at startup.
 */
static const VkDeviceSize BUFFER_LINEAR_SCRATCH_INITIAL_SIZE =
    4096ULL * 4096 * 4;

/*
 * Sustained mixed inline-vertex tests found 8 MiB to be the smallest initial
 * pair with lower final allocation and no measurable loss versus 4, 16, or
 * 32 MiB. Later growth uses the exact required size; Vulkan does not require
 * whole-MiB buffer sizes.
 */
static const size_t BUFFER_VERTEX_INLINE_INITIAL_SIZE = 8 * MiB;

/*
 * Texture uploads are commonly interleaved with draws. Keep their source data
 * alive in a bounded ring until the containing command buffer completes.
 */
static const VkDeviceSize BUFFER_TEXTURE_STAGING_INITIAL_SIZE = 16 * MiB;

/*
 * Morrowind stages about 4 MiB of vertex RAM updates per guest frame. Keep the
 * default above that measured floor, allow the evidence sweep to select 4, 8,
 * or 16 MiB, and never grow this dedicated per-submission storage past 16 MiB.
 */
static const VkDeviceSize BUFFER_VERTEX_RAM_STAGING_DEFAULT_SIZE = 8 * MiB;
static const VkDeviceSize BUFFER_VERTEX_RAM_STAGING_MAX_SIZE = 16 * MiB;

static VkDeviceSize vertex_ram_staging_initial_size(void)
{
    const char *value = g_getenv("XEMU_VK_VERTEX_STAGING_INITIAL_MIB");
    if (!value || !value[0]) {
        return BUFFER_VERTEX_RAM_STAGING_DEFAULT_SIZE;
    }

    char *end = NULL;
    uint64_t mib = g_ascii_strtoull(value, &end, 10);
    if (end == value || *end != '\0' ||
        (mib != 4 && mib != 8 && mib != 16)) {
        fprintf(stderr,
                "nv2a: XEMU_VK_VERTEX_STAGING_INITIAL_MIB must be 4, 8, "
                "or 16; using 8\n");
        return BUFFER_VERTEX_RAM_STAGING_DEFAULT_SIZE;
    }

    return mib * MiB;
}

static bool buffer_is_persistently_mapped(int index)
{
    switch (index) {
    case BUFFER_VERTEX_RAM:
    case BUFFER_VERTEX_RAM_STAGING:
    case BUFFER_TEXTURE_STAGING:
    case BUFFER_INDEX_STAGING:
    case BUFFER_VERTEX_INLINE_STAGING:
    case BUFFER_UNIFORM_STAGING:
        return true;
    default:
        return false;
    }
}

static int paired_buffer_index(int index)
{
    switch (index) {
    case BUFFER_INDEX_STAGING:
        return BUFFER_INDEX;
    case BUFFER_VERTEX_INLINE_STAGING:
        return BUFFER_VERTEX_INLINE;
    case BUFFER_UNIFORM_STAGING:
        return BUFFER_UNIFORM;
    default:
        return -1;
    }
}

static void create_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vmaCreateBuffer(r->allocator, &buffer_create_info,
                             &buffer->alloc_info, &buffer->buffer,
                             &buffer->allocation, NULL));
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (buffer->buffer == VK_NULL_HANDLE) {
        return;
    }

    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

static void resize_buffer(PGRAPHState *pg, int index, size_t size);

static VkDeviceSize grow_buffer_size(VkDeviceSize current,
                                     VkDeviceSize required)
{
    VkDeviceSize size = MAX(current, BUFFER_LINEAR_SCRATCH_INITIAL_SIZE);

    /* Keep a power-of-two capacity invariant so repeated scale changes need
     * at most logarithmically many device allocations. */
    while (size < required) {
        assert(size <= UINT64_MAX / 2);
        size *= 2;
    }

    return size;
}

void pgraph_vk_ensure_buffer_capacity(PGRAPHState *pg, int index,
                                      VkDeviceSize required_size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[index];

    assert(required_size);
    if (buffer->buffer_size >= required_size) {
        return;
    }

    /* Buffer objects may still be referenced by an active submission. Finish
     * it before replacing the allocation; callers invoke this before starting
     * their auxiliary command buffer. */
    if (r->in_command_buffer) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }
    assert(!r->in_command_buffer);
    assert(!r->in_aux_command_buffer);

    resize_buffer(pg, index,
                  grow_buffer_size(buffer->buffer_size, required_size));
}

static void resize_buffer(PGRAPHState *pg, int index, size_t size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[index];

    assert(!r->in_command_buffer);
    assert(!r->in_aux_command_buffer);

    if (buffer->mapped) {
        vmaUnmapMemory(r->allocator, buffer->allocation);
        buffer->mapped = NULL;
    }

    destroy_buffer(pg, buffer);
    buffer->buffer_offset = 0;
    buffer->buffer_size = size;
    create_buffer(pg, buffer);

    if (buffer_is_persistently_mapped(index)) {
        VK_CHECK(vmaMapMemory(r->allocator, buffer->allocation,
                              (void **)&buffer->mapped));
    }
}

void pgraph_vk_ensure_buffer_pair_capacity(PGRAPHState *pg, int index,
                                           size_t required_size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int paired_index = paired_buffer_index(index);

    assert(required_size);
    assert(paired_index >= 0);
    assert(!r->in_command_buffer);
    assert(!r->in_aux_command_buffer);

    StorageBuffer *buffer = &r->storage_buffers[index];
    StorageBuffer *paired = &r->storage_buffers[paired_index];
    if (buffer->buffer != VK_NULL_HANDLE &&
        paired->buffer != VK_NULL_HANDLE &&
        buffer->buffer_size >= required_size &&
        paired->buffer_size >= required_size) {
        return;
    }

    size_t new_size = MAX(buffer->buffer_size, paired->buffer_size);
    new_size = MAX(new_size, BUFFER_VERTEX_INLINE_INITIAL_SIZE);
    new_size = MAX(new_size, required_size);

    if (buffer->buffer == VK_NULL_HANDLE || buffer->buffer_size < new_size) {
        resize_buffer(pg, index, new_size);
    }
    if (paired->buffer == VK_NULL_HANDLE || paired->buffer_size < new_size) {
        resize_buffer(pg, paired_index, new_size);
    }
}

void pgraph_vk_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Profile buffer sizes

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = BUFFER_LINEAR_SCRATCH_INITIAL_SIZE,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_STAGING_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_TEXTURE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = BUFFER_TEXTURE_STAGING_INITIAL_SIZE,
    };

    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = BUFFER_LINEAR_SCRATCH_INITIAL_SIZE,
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .buffer_size = sizeof(pg->inline_elements) * 100,
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->storage_buffers[BUFFER_VERTEX_RAM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = vertex_ram_staging_initial_size(),
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = BUFFER_VERTEX_INLINE_INITIAL_SIZE,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size,
    };

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = 8 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        create_buffer(pg, &r->storage_buffers[i]);
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_VERTEX_RAM,
                             BUFFER_VERTEX_RAM_STAGING,
                             BUFFER_TEXTURE_STAGING,
                             BUFFER_INDEX_STAGING,
                             BUFFER_VERTEX_INLINE_STAGING,
                             BUFFER_UNIFORM_STAGING };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        VK_CHECK(vmaMapMemory(
            r->allocator, r->storage_buffers[buffers_to_map[i]].allocation,
            (void **)&r->storage_buffers[buffers_to_map[i]].mapped));
    }
    r->num_vertex_ram_read_pages =
        DIV_ROUND_UP(memory_region_size(d->vram), TARGET_PAGE_SIZE);
    r->vertex_ram_read_pages = g_malloc0(r->num_vertex_ram_read_pages);
    r->vertex_ram_stale_pages = g_malloc0(r->num_vertex_ram_read_pages);
    r->vertex_ram_stale_page_count = 0;
    r->vertex_version_scratch =
        g_malloc(PGRAPH_VK_VERTEX_VERSION_SCRATCH_SIZE);
    r->vertex_ram_read_tracking_active = false;
    r->vertex_ram_updated_in_batch = false;
    r->vertex_ram_read_tracking_idle_batches = 0;
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_free(r->vertex_ram_read_pages);
    r->vertex_ram_read_pages = NULL;
    g_free(r->vertex_ram_stale_pages);
    r->vertex_ram_stale_pages = NULL;
    r->vertex_ram_stale_page_count = 0;
    g_free(r->vertex_version_scratch);
    r->vertex_version_scratch = NULL;
    r->vertex_ram_read_tracking_active = false;
    r->vertex_ram_updated_in_batch = false;
    r->vertex_ram_read_tracking_idle_batches = 0;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }
}

void pgraph_vk_clear_vertex_ram_stale(PGRAPHVkState *r)
{
    assert(r->vertex_ram_stale_pages);
    pgraph_vk_vertex_version_clear_stale(
        r->vertex_ram_stale_pages, r->num_vertex_ram_read_pages,
        &r->vertex_ram_stale_page_count);
}

bool pgraph_vk_grow_vertex_ram_staging_buffer(PGRAPHState *pg,
                                               VkDeviceSize required_size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer =
        &r->storage_buffers[BUFFER_VERTEX_RAM_STAGING];

    assert(!r->in_command_buffer);
    assert(!r->in_aux_command_buffer);
    if (required_size > BUFFER_VERTEX_RAM_STAGING_MAX_SIZE ||
        buffer->buffer_size >= BUFFER_VERTEX_RAM_STAGING_MAX_SIZE) {
        return false;
    }

    VkDeviceSize new_size = MIN(buffer->buffer_size * 2,
                                BUFFER_VERTEX_RAM_STAGING_MAX_SIZE);
    new_size = MAX(new_size, required_size);
    if (new_size > BUFFER_VERTEX_RAM_STAGING_MAX_SIZE) {
        return false;
    }

    resize_buffer(pg, BUFFER_VERTEX_RAM_STAGING, new_size);
    return true;
}

VkDeviceSize pgraph_vk_buffer_required_size(PGRAPHState *pg, int index,
                                            VkDeviceSize size,
                                            VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize aligned_offset;

    assert(alignment);
    aligned_offset = ROUND_UP(b->buffer_offset, alignment);
    assert(aligned_offset >= b->buffer_offset);
    assert(size <= UINT64_MAX - aligned_offset);
    return aligned_offset + size;
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    return pgraph_vk_buffer_required_size(pg, index, size, alignment) <=
           b->buffer_size;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, alignment));

    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}

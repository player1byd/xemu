/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "renderer.h"
#include "hw/xbox/nv2a/pgraph/vertex-fetch-span.h"

VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    return pgraph_vk_append_to_buffer(pg, BUFFER_INDEX_STAGING, &data, &size, 1,
                                      1);
}

VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
    return pgraph_vk_append_to_buffer(pg, BUFFER_VERTEX_INLINE_STAGING, data,
                                      sizes, count, 1);
}

static void update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                     void *data, VkDeviceSize size,
                                     bool surface_readback_complete,
                                     bool allow_mapped_write)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *vertex = &r->storage_buffers[BUFFER_VERTEX_RAM];
    StorageBuffer *staging =
        &r->storage_buffers[BUFFER_VERTEX_RAM_STAGING];

    if (!size) {
        return;
    }
    assert(offset <= vertex->buffer_size);
    assert(size <= vertex->buffer_size - offset);

    if (!surface_readback_complete &&
        !pgraph_vk_download_surfaces_in_range_if_dirty(pg, offset, size)) {
        /* draw.c has already retired this range's NV2A dirty bit. Re-arm it
         * before refusing to consume stale guest memory. */
        memory_region_set_client_dirty(d->vram, offset, size,
                                       DIRTY_MEMORY_NV2A);
        error_report("Vulkan surface readback failed before vertex upload");
        abort();
    }

    /* Direct writes are safe before recording, or when draw.c proves this
     * page range has not been read by an earlier draw in the active command
     * buffer. Later draws see the write through finish's host-write barrier.
     * Rewritten pages already read in this batch still need an ordered copy. */
    if (!r->in_command_buffer || allow_mapped_write) {
        nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
        if (r->in_command_buffer) {
            pgraph_vk_perf_record_vertex_direct_copy(r, size);
        }
        memcpy(vertex->mapped + offset, data, size);
        return;
    }

    if (!pgraph_vk_buffer_has_space_for(
            pg, BUFFER_VERTEX_RAM_STAGING, size, 4)) {
        /* The staging allocation may still be referenced by commands recorded
         * in this submission. Wait before reusing or replacing it. */
        pgraph_vk_perf_record_vertex_staging_fallback(r);
        pgraph_vk_finish(pg, VK_FINISH_REASON_VERTEX_BUFFER_DIRTY);
        if (pgraph_vk_grow_vertex_ram_staging_buffer(pg, size)) {
            pgraph_vk_perf_record_vertex_staging_growth(r);
        }

        /* The finish made the direct path safe. A single update larger than
         * the hard staging cap is handled here without unbounded allocation. */
        nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
        memcpy(vertex->mapped + offset, data, size);
        return;
    }

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    void *copy_data[] = { data };
    VkDeviceSize copy_sizes[] = { size };
    VkDeviceSize staging_offset = pgraph_vk_append_to_buffer(
        pg, BUFFER_VERTEX_RAM_STAGING, copy_data, copy_sizes, 1, 4);
    assert((staging_offset & 3) == 0);
    assert((offset & 3) == 0);
    assert((size & 3) == 0);

    VK_CHECK(vmaFlushAllocation(r->allocator, staging->allocation,
                                staging_offset, size));

    VkBufferMemoryBarrier before_copy = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = vertex->buffer,
        .offset = offset,
        .size = size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &before_copy, 0, NULL);

    VkBufferCopy copy = {
        .srcOffset = staging_offset,
        .dstOffset = offset,
        .size = size,
    };
    vkCmdCopyBuffer(cmd, staging->buffer, vertex->buffer, 1, &copy);

    VkBufferMemoryBarrier after_copy = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = vertex->buffer,
        .offset = offset,
        .size = size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &after_copy, 0, NULL);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    pgraph_vk_perf_record_vertex_staging_copy(r, size);
}

void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                        void *data, VkDeviceSize size)
{
    update_vertex_ram_buffer(pg, offset, data, size, false, false);
}

void pgraph_vk_update_vertex_ram_buffer_after_surface_readback(
    PGRAPHState *pg, hwaddr offset, void *data, VkDeviceSize size)
{
    update_vertex_ram_buffer(pg, offset, data, size, true, false);
}

void pgraph_vk_update_unread_vertex_ram_buffer_after_surface_readback(
    PGRAPHState *pg, hwaddr offset, void *data, VkDeviceSize size)
{
    update_vertex_ram_buffer(pg, offset, data, size, true, true);
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(r->vertex_ram_buffer_syncs));
    r->vertex_ram_buffer_syncs[r->num_vertex_ram_buffer_syncs++] =
        (MemorySyncRequirement){ .addr = addr, .size = size };
}

static const VkFormat float_to_count[] = {
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,
};

static const VkFormat ub_to_count[] = {
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_R8G8B8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
};

static const VkFormat s1_to_count[] = {
    VK_FORMAT_R16_SNORM,
    VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16B16_SNORM,
    VK_FORMAT_R16G16B16A16_SNORM,
};

static const VkFormat s32k_to_count[] = {
    VK_FORMAT_R16_SSCALED,
    VK_FORMAT_R16G16_SSCALED,
    VK_FORMAT_R16G16B16_SSCALED,
    VK_FORMAT_R16G16B16A16_SSCALED,
};

static char const * const vertex_data_array_format_to_str[] = {
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D] = "UB_D3D",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL] = "UB_OGL",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1] = "S1",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F] = "F",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K] = "S32K",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP] = "CMP",
};

bool pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (max_element < min_element || provoking_element < min_element ||
        provoking_element > max_element) {
        error_report("Vulkan vertex draw has an invalid element range");
        return false;
    }
    unsigned int num_elements = max_element - min_element + 1;

    if (inline_data) {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        NV2A_VK_DGROUP_BEGIN("[attr %02d] format=%s, count=%d, stride=%d", i,
                             vertex_data_array_format_to_str[attr->format],
                             attr->count, attr->stride);
        r->vertex_attribute_to_description_location[i] = -1;
        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        VkFormat vk_format;
        bool needs_conversion = false;
        bool d3d_swizzle = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            assert(attr->count == 4);
            d3d_swizzle = true;
            /* fallthru */
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            assert(attr->count <= ARRAY_SIZE(ub_to_count));
            vk_format = ub_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            assert(attr->count <= ARRAY_SIZE(s1_to_count));
            vk_format = s1_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            assert(attr->count <= ARRAY_SIZE(float_to_count));
            vk_format = float_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            assert(attr->count <= ARRAY_SIZE(s32k_to_count));
            vk_format = s32k_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            vk_format =
                VK_FORMAT_R32_SINT; // VK_FORMAT_B10G11R11_UFLOAT_PACK32 ??
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;

        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        if (inline_data) {
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_limit = 0;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_limit);
            stride = attr->stride;
            uint64_t dma_base = (uintptr_t)attr_data -
                                (uintptr_t)d->vram_ptr;
            uint64_t vram_size = memory_region_size(d->vram);
            PGRAPHVertexFetchRange range;
            if (!pgraph_vertex_resolve_fetch_range(
                    dma_base, dma_limit, attr->offset, vram_size,
                    min_element, max_element, stride, element_size, &range)) {
                error_report("Vulkan vertex attribute %d exceeds DMA or VRAM",
                             i);
                NV2A_VK_DGROUP_END();
                NV2A_VK_DGROUP_END();
                return false;
            }
            attrib_data_addr = range.attribute_base;
            update_memory_buffer(d, range.fetch_start, range.fetch_size);
            r->vertex_attribute_offsets[i] = attrib_data_addr;
        }

        uint32_t provoking_element_index = provoking_element - min_element;
        const uint8_t *last_entry = inline_data ?
            (uint8_t *)pg->inline_array + attr->inline_array_offset : NULL;
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pg->uniform_attrs |= 1 << i;
            if (inline_data) {
                pgraph_update_inline_value(attr, last_entry);
            }
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        NV2A_VK_DPRINTF("offset = %08" HWADDR_PRIx, attrib_data_addr);
        if (inline_data) {
            pgraph_update_inline_value(
                attr, last_entry + stride * provoking_element_index);
        }

        r->vertex_attribute_to_description_location[i] =
            r->num_active_vertex_binding_descriptions;

        r->vertex_binding_descriptions
            [r->num_active_vertex_binding_descriptions++] =
            (VkVertexInputBindingDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .stride = stride,
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };

        r->vertex_attribute_descriptions
            [r->num_active_vertex_attribute_descriptions++] =
            (VkVertexInputAttributeDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .location = i,
                .format = vk_format,
            };

        r->vertex_attribute_offsets[i] = attrib_data_addr;

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }
        if (d3d_swizzle) {
            pg->swizzle_attrs |= (1 << i);
        }

        NV2A_VK_DGROUP_END();
    }

    NV2A_VK_DGROUP_END();
    return true;
}

/* A surface readback must complete before these CPU-side values are decoded. */
void pgraph_vk_refresh_vertex_inline_values_after_sync(
    PGRAPHState *pg, unsigned int provoking_element)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (!attr->count) {
            continue;
        }
        const uint8_t *entry = d->vram_ptr + r->vertex_attribute_offsets[i] +
            (size_t)attr->stride * provoking_element;
        pgraph_update_inline_value(attr, entry);
    }
}

void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->inline_buffer_populated) {
            r->vertex_attribute_to_description_location[i] =
                r->num_active_vertex_binding_descriptions;
            r->vertex_binding_descriptions
                [r->num_active_vertex_binding_descriptions++] =
                (VkVertexInputBindingDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .stride = 4 * sizeof(float),
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                };
            r->vertex_attribute_descriptions
                [r->num_active_vertex_attribute_descriptions++] =
                (VkVertexInputAttributeDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .location = i,
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                };
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            r->vertex_attribute_to_description_location[i] = -1;
            pg->uniform_attrs |= 1 << i;
        }
    }
}

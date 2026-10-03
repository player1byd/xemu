/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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
#include "qemu/fast-hash.h"
#include "renderer.h"
#include "hybrid-family-codec.h"
#include "hybrid-prewarm-runtime.h"
#include "hybrid-ready.h"
#include "pipeline-key.h"
#include "pipeline-cache-lifetime.h"
#include "pipeline-cache-data.h"
#include "staging-copy.h"
#include "vertex-version-policy.h"
#include "ui/xemu-tweaks.h"
#include "ui/xemu-settings.h"
#include <glib/gstdio.h>
#include <math.h>

static bool pgraph_vk_flush_draw_internal(NV2AState *d);

typedef struct PGRAPHVkGraphicsPipelineRecipe {
    VkGraphicsPipelineCreateInfo info;
    VkPipelineShaderStageCreateInfo stages[3];
    VkPipelineVertexInputStateCreateInfo vertex;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depth_stencil;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineColorBlendAttachmentState color_attachment;
    VkPipelineDynamicStateCreateInfo dynamic;
    VkDynamicState dynamic_states[4];
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    uint32_t dynamic_blend_constant_mask;
    bool has_dynamic_line_width;
} PGRAPHVkGraphicsPipelineRecipe;

/* Keep a short read-tracking window after a batch uses updated vertex data.
 * Quiet workloads keep the existing ordered upload path without paying for
 * page bookkeeping on every draw. */
#define VERTEX_READ_TRACKING_IDLE_BATCHES 16

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    int64_t start_us = r->perf.enabled ? g_get_monotonic_time() : 0;
    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);
    pgraph_vk_perf_record_cpu_region(
        r, VK_PERF_CPU_DRAW_BEGIN_SURFACE_UPDATE,
        r->perf.enabled ? g_get_monotonic_time() - start_us : 0);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(const ShaderState *state)
{
    int polygon_mode = state->geom.polygon_front_mode;
    int primitive_mode = state->geom.primitive_mode;

    // FIXME: Replace with LUT
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINE_LOOP:
        // FIXME: line strips, except that the first and last vertices are also used as a line
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME
        } else if (polygon_mode == POLY_MODE_FILL) {
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
    snode->prewarmed = false;
    snode->family_learn_state = PGRAPH_VK_FAMILY_UNCHECKED;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    pgraph_vk_pipeline_family_owner_evict(r, snode);

    assert((snode->pipeline == VK_NULL_HANDLE ||
            pgraph_vk_graphics_pipeline_can_evict(
                r->in_command_buffer, snode->draw_time,
                r->command_buffer_start_time)) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
    if (r->pipeline_binding == snode) {
        r->pipeline_binding = NULL;
        r->pipeline_binding_changed = true;
    }
}

static bool pipeline_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    return snode != r->pipeline_binding &&
           pgraph_vk_graphics_pipeline_can_evict(
        r->in_command_buffer, snode->draw_time,
        r->command_buffer_start_time);
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

static PGRAPHVkPipelineCacheIdentity pipeline_cache_identity(PGRAPHVkState *r)
{
    PGRAPHVkPipelineCacheIdentity identity = {
        .vendor_id = r->device_props.vendorID,
        .device_id = r->device_props.deviceID,
    };
    memcpy(identity.uuid, r->device_props.pipelineCacheUUID,
           sizeof(identity.uuid));
    return identity;
}

static bool pipeline_cache_log_enabled(void)
{
    return g_strcmp0(g_getenv("XEMU_VK_PIPELINE_CACHE_LOG"), "1") == 0;
}

static void pipeline_cache_init_path(PGRAPHVkState *r)
{
    const char *base = xemu_settings_get_base_path();
    if (pipeline_cache_log_enabled()) {
        fprintf(stderr,
                "nv2a/vk: pipeline cache init enabled=%d session=%d base=%s\n",
                g_config.perf.cache_shaders,
                r->spirv_cache_session_eligible, base ? base : "(none)");
    }
    if (!r->spirv_cache_session_eligible ||
        !g_config.perf.cache_shaders || !base || !base[0]) {
        return;
    }

    char uuid_hex[VK_UUID_SIZE * 2 + 1];
    for (size_t i = 0; i < VK_UUID_SIZE; i++) {
        g_snprintf(&uuid_hex[i * 2], 3, "%02x",
                   r->device_props.pipelineCacheUUID[i]);
    }
    char *filename = g_strdup_printf("pipeline-v1-%08x-%08x-%s.bin",
                                     r->device_props.vendorID,
                                     r->device_props.deviceID, uuid_hex);
    r->pipeline_cache_path =
        g_build_filename(base, "cache", "vulkan", filename, NULL);
    g_free(filename);
}

static uint8_t *pipeline_cache_read(PGRAPHVkState *r, size_t *size)
{
    GStatBuf stat_buf;
    if (!r->pipeline_cache_path ||
        g_stat(r->pipeline_cache_path, &stat_buf) ||
        stat_buf.st_size < 32 ||
        (uint64_t)stat_buf.st_size >
            PGRAPH_VK_PIPELINE_CACHE_MAX_FILE_SIZE) {
        return NULL;
    }

    *size = stat_buf.st_size;
    uint8_t *data = g_try_malloc(*size);
    FILE *file = qemu_fopen(r->pipeline_cache_path, "rb");
    if (!data || !file || fread(data, 1, *size, file) != *size ||
        fgetc(file) != EOF) {
        if (file) {
            fclose(file);
        }
        g_free(data);
        *size = 0;
        return NULL;
    }
    fclose(file);
    PGRAPHVkPipelineCacheIdentity identity = pipeline_cache_identity(r);
    if (!pgraph_vk_pipeline_cache_data_compatible(data, *size, &identity)) {
        g_free(data);
        *size = 0;
        return NULL;
    }
    return data;
}

static void pipeline_cache_save(PGRAPHVkState *r)
{
    if (!r->pipeline_cache_path || !g_config.perf.cache_shaders) {
        if (pipeline_cache_log_enabled()) {
            fprintf(stderr, "nv2a/vk: pipeline cache save disabled\n");
        }
        return;
    }

    size_t size = 0;
    VkResult query = vkGetPipelineCacheData(r->device, r->vk_pipeline_cache,
                                             &size, NULL);
    if (pipeline_cache_log_enabled()) {
        fprintf(stderr,
                "nv2a/vk: pipeline cache save query=%d bytes=%zu cap=%u\n",
                query, size, PGRAPH_VK_PIPELINE_CACHE_MAX_FILE_SIZE);
    }
    if (query != VK_SUCCESS ||
        size < 32 || size > PGRAPH_VK_PIPELINE_CACHE_MAX_FILE_SIZE) {
        return;
    }
    uint8_t *data = g_try_malloc(size);
    if (!data) {
        return;
    }
    VkResult result = vkGetPipelineCacheData(r->device, r->vk_pipeline_cache,
                                              &size, data);
    PGRAPHVkPipelineCacheIdentity identity = pipeline_cache_identity(r);
    bool compatible = result == VK_SUCCESS &&
        pgraph_vk_pipeline_cache_data_compatible(data, size, &identity);
    if (pipeline_cache_log_enabled()) {
        fprintf(stderr,
                "nv2a/vk: pipeline cache save fetch=%d bytes=%zu compatible=%d\n",
                result, size, compatible);
    }
    if (compatible) {
        char *directory = g_path_get_dirname(r->pipeline_cache_path);
        int directory_result = g_mkdir_with_parents(directory, 0700);
        if (!directory_result) {
            /* GLib handles replacement of an existing file on Windows. */
            GError *error = NULL;
            bool written = g_file_set_contents_full(
                r->pipeline_cache_path, (const char *)data, (gssize)size,
                G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error);
            if (pipeline_cache_log_enabled()) {
                fprintf(stderr,
                        "nv2a/vk: pipeline cache save write=%d error=%s\n",
                        written, error ? error->message : "(none)");
            }
            g_clear_error(&error);
        } else if (pipeline_cache_log_enabled()) {
            fprintf(stderr,
                    "nv2a/vk: pipeline cache save mkdir failed errno=%d\n",
                    errno);
        }
        g_free(directory);
    }
    g_free(data);
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pipeline_cache_init_path(r);
    size_t restored_size = 0;
    uint8_t *restored = pipeline_cache_read(r, &restored_size);

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = restored_size,
        .pInitialData = restored,
        .pNext = NULL,
    };
    VkResult result = vkCreatePipelineCache(r->device, &cache_info, NULL,
                                             &r->vk_pipeline_cache);
    g_free(restored);
    if (result != VK_SUCCESS && restored_size) {
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = NULL;
        result = vkCreatePipelineCache(r->device, &cache_info, NULL,
                                       &r->vk_pipeline_cache);
    }
    if (pipeline_cache_log_enabled()) {
        fprintf(stderr,
                "nv2a/vk: pipeline cache init restored_bytes=%zu create=%d\n",
                restored_size, result);
    }
    VK_CHECK(result);

    const size_t pipeline_cache_size = 2048;
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.pre_node_evict = pipeline_cache_entry_pre_evict;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static PipelineBinding *pipeline_cache_find_ready(PGRAPHVkState *r,
                                                  uint64_t hash,
                                                  const PipelineKey *key)
{
    return pgraph_vk_pipeline_cache_find_ready(&r->pipeline_cache,
                                                hash, key);
}

static PipelineBinding *pipeline_cache_get_or_create(
    PGRAPHState *pg, uint64_t hash, const PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    LruNode *node = lru_try_lookup(&r->pipeline_cache, hash, key);

    if (!node) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_RESOURCE_SHORTAGE,
            key->fragment_route, hash, 0, 0,
            VK_HYBRID_SHORTAGE_PIPELINE_CACHE,
            r->pipeline_cache.num_used, r->pipeline_cache.num_free, 0);
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        node = lru_try_lookup(&r->pipeline_cache, hash, key);
    }
    if (!node) {
        error_report("Vulkan graphics pipeline cache has no evictable entry");
        abort();
    }

    return container_of(node, PipelineBinding, node);
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);
    assert(!r->in_aux_command_buffer);
    /* The active binding is pinned during rendering, not during teardown. */
    r->pipeline_binding = NULL;
    r->pipeline_binding_changed = true;
    lru_flush(&r->pipeline_cache);
    assert(r->pipeline_cache.num_used == 0);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    pipeline_cache_save(r);
    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
    g_free(r->pipeline_cache_path);
    r->pipeline_cache_path = NULL;
}

void pgraph_vk_writeback_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->pipeline_cache_path || !g_config.perf.cache_shaders) {
        return;
    }
    /* This callback runs under PGRAPH after PFIFO was released. Stop the
     * pipeline worker before serializing the cache it also writes. */
    if (r->hybrid_pipeline_builder_initialized) {
        pgraph_vk_hybrid_pipeline_builder_join(&r->hybrid_pipeline_builder);
    }
    pipeline_cache_save(r);
}

static VkResult hybrid_pipeline_create(void *opaque, VkDevice device,
    VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *info,
    VkPipeline *pipeline)
{
    return vkCreateGraphicsPipelines(device, cache, 1, info, NULL, pipeline);
}

static void hybrid_pipeline_destroy(void *opaque, VkDevice device,
    VkPipeline pipeline)
{
    vkDestroyPipeline(device, pipeline, NULL);
}

static void hybrid_pipeline_work_clear(PGRAPHVkState *r,
                                       PGRAPHVkHybridPipelineWork *work)
{
    if (work->completed_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(r->device, work->completed_pipeline, NULL);
    }
    for (size_t i = 0; i < ARRAY_SIZE(work->modules); i++) {
        if (work->modules[i]) {
            pgraph_vk_unref_shader_module(r, work->modules[i]);
        }
    }
    if (work->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(r->device, work->layout, NULL);
    }
    memset(work, 0, sizeof(*work));
}

static void finalize_hybrid_pipeline_builder(PGRAPHVkState *r)
{
    if (!r->hybrid_pipeline_builder_initialized) {
        return;
    }

    /* Join while cache, render passes, layout, modules, and device still live.
     * The builder destroys untaken pipelines during stop. */
    pgraph_vk_hybrid_pipeline_builder_destroy(&r->hybrid_pipeline_builder);
    r->hybrid_pipeline_builder_initialized = false;
    for (size_t i = 0; i < ARRAY_SIZE(r->hybrid_pipeline_work); i++) {
        if (r->hybrid_pipeline_work[i].in_use) {
            hybrid_pipeline_work_clear(r, &r->hybrid_pipeline_work[i]);
        }
    }
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    if (r->ubershader_runtime_enabled) {
        const PGRAPHVkHybridPipelineBuilderConfig config = {
            .max_jobs = PGRAPH_VK_HYBRID_MAX_PIPELINE_JOBS,
            .create = hybrid_pipeline_create,
            .destroy = hybrid_pipeline_destroy,
            .opaque = r,
            .notify = pgraph_vk_hybrid_worker_notify,
            .notify_opaque = container_of(pg, NV2AState, pgraph),
        };
        r->hybrid_pipeline_builder_initialized =
            pgraph_vk_hybrid_pipeline_builder_init(
                &r->hybrid_pipeline_builder, &config);
    }

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                               &r->command_buffer_semaphore));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(
        vkCreateFence(r->device, &fence_info, NULL, &r->command_buffer_fence));
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    finalize_hybrid_pipeline_builder(r);
    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    vkDestroyFence(r->device, r->command_buffer_fence, NULL);
    vkDestroySemaphore(r->device, r->command_buffer_semaphore, NULL);
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r,
                                       const RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r,
                                        const RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r,
                                    const RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_RESOURCE_SHORTAGE,
            r->shader_binding ? r->shader_binding->fragment_route : 0,
            0, 0, 0, VK_HYBRID_SHORTAGE_FRAMEBUFFER,
            r->framebuffer_index, ARRAY_SIZE(r->framebuffers), 0);
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
}

static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    PipelineBinding *snode = pipeline_cache_get_or_create(pg, hash, &key);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

static uint32_t blend_factor_constant_component_mask(VkBlendFactor factor)
{
    if (factor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
        factor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR) {
        return UINT32_MAX;
    }
    if (factor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
        factor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) {
        return 0xff000000;
    }
    return 0;
}

static uint32_t pipeline_dynamic_blend_constant_mask(uint32_t blend)
{
    if (!(blend & NV_PGRAPH_BLEND_EN)) {
        return 0;
    }

    uint32_t sfactor = GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR);
    uint32_t dfactor = GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR);
    assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
    assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
    return blend_factor_constant_component_mask(
               pgraph_blend_factor_vk_map[sfactor]) |
           blend_factor_constant_component_mask(
               pgraph_blend_factor_vk_map[dfactor]);
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /*
     * Texture image/sampler identity belongs to descriptor sets, not
     * PipelineKey. Texture state that affects generated code is already part
     * of ShaderState and is covered by shader_bindings_changed.
     */
    if (!r->pipeline_binding || r->pipeline_binding->key.clear ||
        r->shader_bindings_changed || check_render_pass_dirty(pg)) {
        return true;
    }

    const unsigned int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_CONTROL_0,   NV_PGRAPH_CONTROL_1,
        NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,   NV_PGRAPH_SETUPRASTER,
        NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    // FIXME: Use dirty bits instead
    if (r->num_active_vertex_attribute_descriptions !=
            r->pipeline_binding->key.attribute_description_count ||
        r->num_active_vertex_binding_descriptions !=
            r->pipeline_binding->key.binding_description_count ||
        memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0]))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void trace_execution_candidates(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->hybrid_trace || !r->ubershader_runtime_enabled) {
        return;
    }

    /* The state extractor clears this hint; a diagnostic probe must not
     * consume it before the ordinary shader-binding path sees the draw. */
    bool program_data_dirty = pg->program_data_dirty;
    ShaderState state = pgraph_glsl_get_shader_state(pg);
    pg->program_data_dirty = program_data_dirty;
    uint64_t state_hash = fast_hash((const uint8_t *)&state, sizeof(state));
    const PGRAPHVkFragmentRoute routes[] = {
        PGRAPH_VK_FRAGMENT_SPECIALIZED,
        PGRAPH_VK_FRAGMENT_UBERSHADER,
    };
    for (size_t i = 0; i < ARRAY_SIZE(routes); i++) {
        PGRAPHVkFragmentRoute route = routes[i];
        ShaderBindingKey shader_key = {
            .state = state,
            .fragment_route = route,
        };
        uint64_t shader_key_hash = fast_hash(
            (const uint8_t *)&shader_key, sizeof(shader_key));
        LruNode *shader_node = lru_find_existing(
            &r->shader_cache, shader_key_hash, &shader_key);
        ShaderBinding *shader = pgraph_vk_shader_binding_find_ready(
            &r->shader_cache, shader_key_hash, &shader_key,
            pgraph_glsl_need_geom(&state.geom));
        PipelineKey pipeline_key;
        pgraph_vk_init_pipeline_key_for_state(pg, &state, route, &pipeline_key);
        uint64_t pipeline_hash = fast_hash(
            (const uint8_t *)&pipeline_key, sizeof(pipeline_key));
        PipelineBinding *pipeline = pipeline_cache_find_ready(
            r, pipeline_hash, &pipeline_key);

        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_SHADER_BINDING_PROBE,
            route, pipeline_hash, state_hash, 0,
            shader_node != NULL, shader != NULL, shader_key_hash, 1);
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_PROBE,
            route, pipeline_hash, state_hash, 0,
            pipeline != NULL, 0, shader != NULL, 1);
    }
}

static bool prepare_graphics_pipeline_recipe(PGRAPHState *pg,
    const PipelineKey *key, ShaderBinding *binding,
    PGRAPHVkGraphicsPipelineRecipe *recipe)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderState effective_state;
    if (!binding || !binding->vsh.module_info ||
        !binding->psh.module_info ||
        binding->fragment_route != key->fragment_route) {
        return false;
    }
    effective_state = binding->state;
    if (key->fragment_route == PGRAPH_VK_FRAGMENT_UBERSHADER) {
        pgraph_vk_canonicalize_uber_combiner_state(&effective_state.psh);
    }
    if (memcmp(&effective_state, &key->shader_state,
               sizeof(effective_state)) != 0 ||
        (pgraph_glsl_need_geom(&binding->state.geom) &&
         !binding->geom.module_info)) {
        return false;
    }
    bool has_dynamic_line_width;
    const uint32_t blend_reg = key->regs[0];
    const uint32_t control_0_reg = key->regs[1];
    const uint32_t control_1_reg = key->regs[2];
    const uint32_t control_2_reg = key->regs[3];
    const uint32_t setup_raster_reg = key->regs[5];
    const bool has_color =
        key->render_pass_state.color_format != VK_FORMAT_UNDEFINED;
    const bool has_zeta =
        key->render_pass_state.zeta_format != VK_FORMAT_UNDEFINED;

    uint32_t control_0 = control_0_reg;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test =
        control_1_reg & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3] = { 0 };

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = binding->vsh.module_info->module,
            .pName = "main",
        };
    if (binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = binding->psh.module_info->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            key->binding_description_count,
        .pVertexBindingDescriptions = key->binding_descriptions,
        .vertexAttributeDescriptionCount =
            key->attribute_description_count,
        .pVertexAttributeDescriptions = key->attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = get_primitive_topology(&binding->state),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = pgraph_polygon_mode_vk_map[binding->state
                                                      .geom.polygon_front_mode],
        .lineWidth = 1.0f,
        .frontFace = (setup_raster_reg &
                      NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                         VK_FRONT_FACE_COUNTER_CLOCKWISE :
                         VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

    if (setup_raster_reg & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(setup_raster_reg,
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
    } else {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
    };

    if (depth_test) {
        depth_stencil.depthTestEnable = VK_TRUE;
        uint32_t depth_func =
            GET_MASK(control_0_reg, NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
        depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
    }

    if (stencil_test) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        uint32_t stencil_func = GET_MASK(control_1_reg,
                                         NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(control_1_reg,
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t mask_read = GET_MASK(control_1_reg,
                                      NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t mask_write = GET_MASK(control_1_reg,
                                       NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        uint32_t op_fail = GET_MASK(control_2_reg,
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(control_2_reg,
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(control_2_reg,
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

        depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
        depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
        depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
        depth_stencil.front.compareOp =
            pgraph_stencil_func_vk_map[stencil_func];
        depth_stencil.front.compareMask = mask_read;
        depth_stencil.front.writeMask = mask_write;
        depth_stencil.front.reference = stencil_ref;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };

    uint32_t dynamic_blend_constant_mask = 0;

    if (blend_reg & NV_PGRAPH_BLEND_EN) {
        color_blend_attachment.blendEnable = VK_TRUE;

        uint32_t sfactor =
            GET_MASK(blend_reg, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor =
            GET_MASK(blend_reg, NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        color_blend_attachment.srcColorBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstColorBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];
        color_blend_attachment.srcAlphaBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstAlphaBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];

        uint32_t equation =
            GET_MASK(blend_reg, NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        color_blend_attachment.colorBlendOp =
            pgraph_blend_equation_vk_map[equation];
        color_blend_attachment.alphaBlendOp =
            pgraph_blend_equation_vk_map[equation];

        if (has_color) {
            dynamic_blend_constant_mask =
                pipeline_dynamic_blend_constant_mask(blend_reg);
        }
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = has_color ? 1 : 0,
        .pAttachments = has_color ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[4] = { VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR };
    int num_dynamic_states = 2;
    if (dynamic_blend_constant_mask != 0) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    }

    has_dynamic_line_width =
        (r->enabled_physical_device_features.wideLines == VK_TRUE) &&
        (binding->state.geom.polygon_front_mode == POLY_MODE_LINE ||
         binding->state.geom.primitive_mode == PRIM_TYPE_LINES ||
         binding->state.geom.primitive_mode == PRIM_TYPE_LINE_LOOP ||
         binding->state.geom.primitive_mode == PRIM_TYPE_LINE_STRIP);
    if (has_dynamic_line_width) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = num_dynamic_states,
        .pDynamicStates = dynamic_states,
    };

    // FIXME: Dither
    // if (control_0_reg &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && setup_raster_reg &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && setup_raster_reg &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };

    VkPushConstantRange push_constant_range;
    if (r->use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            __builtin_popcount(binding->state.vsh.uniform_attrs);
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    int64_t layout_start_us = r->hybrid_trace ?
        g_get_monotonic_time() : 0;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));
    if (r->hybrid_trace) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_LAYOUT_CREATE,
            key->fragment_route,
            fast_hash((const uint8_t *)key, sizeof(*key)),
            fast_hash((const uint8_t *)&key->shader_state,
                      sizeof(key->shader_state)), 0,
            layout_start_us, g_get_monotonic_time(), 0, 0);
    }

    int64_t render_pass_start_us = r->hybrid_trace ?
        g_get_monotonic_time() : 0;
    VkRenderPass render_pass = get_render_pass(r, &key->render_pass_state);
    if (r->hybrid_trace) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_RENDER_PASS_LOOKUP,
            key->fragment_route,
            fast_hash((const uint8_t *)key, sizeof(*key)),
            0, 0, render_pass_start_us, g_get_monotonic_time(),
            key->render_pass_state.color_format,
            key->render_pass_state.zeta_format);
    }

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = has_zeta ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    /* The worker copies the complete recipe at submission. Repoint every
     * stack-backed create-info field before either creation path uses it. */
    memset(recipe, 0, sizeof(*recipe));
    recipe->layout = layout;
    recipe->render_pass = pipeline_create_info.renderPass;
    recipe->dynamic_blend_constant_mask = dynamic_blend_constant_mask;
    recipe->has_dynamic_line_width = has_dynamic_line_width;
    memcpy(recipe->stages, shader_stages, sizeof(recipe->stages));
    recipe->vertex = vertex_input;
    recipe->assembly = input_assembly;
    recipe->viewport = viewport_state;
    recipe->raster = rasterizer;
    recipe->multisample = multisampling;
    recipe->depth_stencil = depth_stencil;
    recipe->color_attachment = color_blend_attachment;
    recipe->blend = color_blending;
    recipe->blend.pAttachments = color_blending.attachmentCount ?
        &recipe->color_attachment : NULL;
    memcpy(recipe->dynamic_states, dynamic_states,
           sizeof(recipe->dynamic_states));
    recipe->dynamic = dynamic_state;
    recipe->dynamic.pDynamicStates = recipe->dynamic_states;
    recipe->info = pipeline_create_info;
    recipe->info.pStages = recipe->stages;
    recipe->info.pVertexInputState = &recipe->vertex;
    recipe->info.pInputAssemblyState = &recipe->assembly;
    recipe->info.pViewportState = &recipe->viewport;
    recipe->info.pRasterizationState = &recipe->raster;
    recipe->info.pMultisampleState = &recipe->multisample;
    recipe->info.pDepthStencilState = pipeline_create_info.pDepthStencilState ?
        &recipe->depth_stencil : NULL;
    recipe->info.pColorBlendState = &recipe->blend;
    recipe->info.pDynamicState = &recipe->dynamic;
    return true;
}

static PGRAPHVkHybridPipelineSubmitResult request_hybrid_pipeline(
    PGRAPHState *pg, const PipelineKey *key, ShaderBinding *ready_binding,
    bool prewarm, PGRAPHVkHybridPriority priority)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->hybrid_pipeline_builder_initialized || !key || !ready_binding ||
        key->clear || (key->fragment_route != PGRAPH_VK_FRAGMENT_SPECIALIZED &&
                       key->fragment_route != PGRAPH_VK_FRAGMENT_UBERSHADER)) {
        return PGRAPH_VK_HYBRID_PIPELINE_STOPPED;
    }

    if (key->fragment_route == PGRAPH_VK_FRAGMENT_SPECIALIZED) {
        /* Active specialization must still match the live draw. A queued
         * fallback family instead owns its complete earlier pipeline key. */
        PipelineKey current_key;
        pgraph_vk_init_pipeline_key_for_state(pg, &ready_binding->state,
                                    PGRAPH_VK_FRAGMENT_SPECIALIZED,
                                    &current_key);
        if (memcmp(key, &current_key, sizeof(*key)) != 0) {
            return PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE;
        }
    }

    uint64_t hash = fast_hash((const uint8_t *)key, sizeof(*key));
    if (pipeline_cache_find_ready(r, hash, key)) {
        return PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED;
    }
    PGRAPHVkHybridPipelineWork *work = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(r->hybrid_pipeline_work); i++) {
        PGRAPHVkHybridPipelineWork *candidate = &r->hybrid_pipeline_work[i];
        if (candidate->in_use &&
            candidate->generation == r->hybrid_generation &&
            candidate->key_hash == hash &&
            memcmp(&candidate->key, key, sizeof(*key)) == 0) {
            pgraph_vk_hybrid_pipeline_note_prewarm(candidate, prewarm);
            candidate->priority = pgraph_vk_hybrid_priority_max(
                candidate->priority, priority);
            pgraph_vk_hybrid_pipeline_builder_promote(
                &r->hybrid_pipeline_builder, candidate->generation,
                candidate->ticket, priority);
            return PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED;
        }
        if (!candidate->in_use && !work) {
            work = candidate;
        }
    }
    if (!work) {
        return PGRAPH_VK_HYBRID_PIPELINE_QUEUE_FULL;
    }

    PGRAPHVkGraphicsPipelineRecipe recipe;
    if (!prepare_graphics_pipeline_recipe(pg, key, ready_binding, &recipe)) {
        return PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE;
    }

    work->in_use = true;
    pgraph_vk_hybrid_pipeline_note_prewarm(work, prewarm);
    work->priority = priority;
    work->generation = r->hybrid_generation;
    work->ticket = ++r->hybrid_pipeline_next_ticket;
    if (!work->ticket) {
        work->ticket = ++r->hybrid_pipeline_next_ticket;
    }
    work->key_hash = hash;
    work->key = *key;
    work->layout = recipe.layout;
    work->render_pass = recipe.render_pass;
    work->dynamic_blend_constant_mask =
        recipe.dynamic_blend_constant_mask;
    work->has_dynamic_line_width = recipe.has_dynamic_line_width;
    work->modules[0] = ready_binding->vsh.module_info;
    work->modules[1] = ready_binding->geom.module_info;
    work->modules[2] = ready_binding->psh.module_info;
    for (size_t i = 0; i < ARRAY_SIZE(work->modules); i++) {
        if (work->modules[i]) {
            pgraph_vk_ref_shader_module(work->modules[i]);
        }
    }

    PGRAPHVkHybridPipelineBuildRequest request = {
        .generation = work->generation,
        .ticket = work->ticket,
        .key_hash = hash,
        .priority = priority,
        .device = r->device,
        .cache = r->vk_pipeline_cache,
        .create_info = &recipe.info,
    };
    PGRAPHVkHybridPipelineSubmitResult status =
        pgraph_vk_hybrid_pipeline_builder_submit(
            &r->hybrid_pipeline_builder, &request);
    if (r->hybrid_trace) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_SUBMIT,
            key->fragment_route, hash,
            fast_hash((const uint8_t *)&key->shader_state,
                      sizeof(key->shader_state)), work->ticket,
            status, r->pipeline_cache.num_free, 0, 0);
    }
    if (status != PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED) {
        hybrid_pipeline_work_clear(r, work);
    }
    return status;
}

PGRAPHVkHybridPipelineSubmitResult pgraph_vk_request_hybrid_pipeline(
    PGRAPHState *pg, const PipelineKey *key, ShaderBinding *ready_binding)
{
    return request_hybrid_pipeline(
        pg, key, ready_binding, false, PGRAPH_VK_HYBRID_PRIORITY_VISIBLE);
}

typedef struct PGRAPHVkFallbackShaderPreparationContext {
    PGRAPHState *pg;
    const ShaderState *state;
    PGRAPHVkHybridPriority priority;
} PGRAPHVkFallbackShaderPreparationContext;

static ShaderBinding *fallback_family_probe_ready_binding(void *opaque)
{
    PGRAPHVkFallbackShaderPreparationContext *context = opaque;

    return pgraph_vk_prepare_binding_from_ready_modules(
        context->pg, context->state, PGRAPH_VK_FRAGMENT_UBERSHADER);
}

static bool fallback_family_prepare_fragment(void *opaque)
{
    PGRAPHVkFallbackShaderPreparationContext *context = opaque;

    return pgraph_vk_request_fallback_family_modules_priority(
               context->pg, context->state, context->priority) !=
           PGRAPH_VK_CACHED_FAMILY_MODULES_REJECTED;
}

void pgraph_vk_process_fallback_families(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->ubershader_runtime_enabled ||
        !r->hybrid_compiler_initialized ||
        !r->hybrid_pipeline_builder_initialized) {
        return;
    }

    pgraph_vk_enqueue_retained_fallback_families(r);

    unsigned int visited = 0;
    unsigned int processed = 0;
    int64_t now_us = g_get_monotonic_time();
    while (visited < ARRAY_SIZE(r->fallback_family_requests) &&
           processed < 2 &&
           pgraph_vk_hybrid_owner_budget_available(r)) {
        unsigned int index = r->fallback_family_cursor++ %
            ARRAY_SIZE(r->fallback_family_requests);
        PGRAPHVkFallbackFamilyRequest *request =
            &r->fallback_family_requests[index];
        visited++;
        if (!request->in_use) {
            continue;
        }
        uint64_t hash = fast_hash((const uint8_t *)&request->key,
                                  sizeof(request->key));
        if (pipeline_cache_find_ready(r, hash, &request->key)) {
            pgraph_vk_fallback_family_finish_request(r, request,
                                           PGRAPH_VK_FAMILY_READY);
            continue;
        }
        if (!pgraph_vk_fallback_family_retry_due(request, now_us)) {
            pgraph_vk_hybrid_schedule_service(pg, request->retry_after_us);
            continue;
        }
        processed++;
        PGRAPHVkFallbackShaderPreparationContext context = {
            .pg = pg,
            .state = &request->state,
            .priority = request->priority,
        };
        ShaderBinding *binding = NULL;
        PGRAPHVkFallbackShaderPreparation preparation =
            pgraph_vk_fallback_family_prepare_shader(
                &context, fallback_family_probe_ready_binding,
                fallback_family_prepare_fragment, &binding);
        if (preparation == PGRAPH_VK_FALLBACK_SHADER_REJECTED) {
            pgraph_vk_fallback_family_finish_request(
                r, request, PGRAPH_VK_FAMILY_REJECTED);
            continue;
        }
        if (preparation == PGRAPH_VK_FALLBACK_SHADER_WAITING) {
            request->status = PGRAPH_VK_FAMILY_WAITING_FOR_SHADER;
            request->retry_after_us =
                now_us + PGRAPH_VK_FAMILY_RETRY_BASE_US;
            pgraph_vk_hybrid_schedule_service(pg, request->retry_after_us);
            continue;
        }
        assert(binding);
        PGRAPHVkHybridPipelineSubmitResult status =
            request_hybrid_pipeline(pg, &request->key, binding,
                                    request->from_prewarm,
                                    request->priority);
        if (!pgraph_vk_fallback_family_note_pipeline_submit(
                request, status, now_us)) {
            pgraph_vk_fallback_family_mark_pipeline_owners(
                r, &request->key, PGRAPH_VK_FAMILY_REJECTED);
        } else if (request->status == PGRAPH_VK_FAMILY_QUEUE_DEFERRED) {
            pgraph_vk_hybrid_schedule_service(pg, request->retry_after_us);
        }
    }
    if (visited < ARRAY_SIZE(r->fallback_family_requests) &&
        !pgraph_vk_hybrid_owner_budget_available(r)) {
        qatomic_set(&r->hybrid_prewarm_service_pending, true);
        pgraph_vk_hybrid_schedule_service(pg, g_get_monotonic_time());
    }
}

static uint32_t hybrid_prewarm_in_flight(PGRAPHVkState *r)
{
    uint32_t count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(r->fallback_family_requests); i++) {
        if (r->fallback_family_requests[i].in_use &&
            r->fallback_family_requests[i].priority ==
                PGRAPH_VK_HYBRID_PRIORITY_PREWARM) {
            count++;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(r->hybrid_pipeline_work); i++) {
        PGRAPHVkHybridPipelineWork *work = &r->hybrid_pipeline_work[i];
        if (!work->in_use ||
            work->priority != PGRAPH_VK_HYBRID_PRIORITY_PREWARM) {
            continue;
        }
        bool retained = false;
        for (size_t j = 0; j <
             ARRAY_SIZE(r->fallback_family_requests); j++) {
            PGRAPHVkFallbackFamilyRequest *request =
                &r->fallback_family_requests[j];
            if (request->in_use &&
                memcmp(&request->key, &work->key,
                       sizeof(work->key)) == 0) {
                retained = true;
                break;
            }
        }
        if (!retained) {
            count++;
        }
    }
    return count;
}

static void prewarm_get_format_properties(void *opaque, VkFormat format,
                                          VkFormatProperties *properties)
{
    PGRAPHVkState *r = opaque;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, format, properties);
}

static bool prewarm_key_device_supported(void *opaque, const PipelineKey *key)
{
    PGRAPHState *pg = opaque;
    PGRAPHVkState *r = pg->vk_renderer_state;
    const VkPhysicalDeviceLimits *limits = &r->device_props.limits;
    if (key->binding_description_count > limits->maxVertexInputBindings ||
        key->attribute_description_count > limits->maxVertexInputAttributes ||
        (pgraph_glsl_need_geom(&key->shader_state.geom) &&
         !r->enabled_physical_device_features.geometryShader) ||
        (key->shader_state.geom.polygon_front_mode != POLY_MODE_FILL &&
         !r->enabled_physical_device_features.fillModeNonSolid)) {
        return false;
    }
    VkFormat zeta = key->render_pass_state.zeta_format;
    if (zeta != VK_FORMAT_UNDEFINED &&
        zeta != r->kelvin_surface_zeta_vk_map[
                    key->shader_state.psh.surface_zeta_format].vk_format) {
        return false;
    }
    for (size_t i = 0; i < key->binding_description_count; i++) {
        const VkVertexInputBindingDescription *binding =
            &key->binding_descriptions[i];
        if (binding->binding >= limits->maxVertexInputBindings ||
            binding->stride > limits->maxVertexInputBindingStride) {
            return false;
        }
    }
    for (size_t i = 0; i < key->attribute_description_count; i++) {
        const VkVertexInputAttributeDescription *attribute =
            &key->attribute_descriptions[i];
        if (attribute->location >= limits->maxVertexInputAttributes ||
            attribute->offset > limits->maxVertexInputAttributeOffset) {
            return false;
        }
    }
    if (r->use_push_constants_for_uniform_attrs &&
        __builtin_popcount(key->shader_state.vsh.uniform_attrs) *
            sizeof(float) * 4 > limits->maxPushConstantsSize) {
        return false;
    }
    /* Saved history may have been learned on a different physical device.
     * Optional vertex formats must be admitted by this device before any
     * shader or pipeline preparation begins. */
    return pgraph_vk_hybrid_prewarm_vertex_formats_supported(
        key, prewarm_get_format_properties, r);
}

static bool prewarm_pipeline_ready(void *opaque, const PipelineKey *key)
{
    PGRAPHState *pg = opaque;
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint64_t hash = fast_hash((const uint8_t *)key, sizeof(*key));
    return pipeline_cache_find_ready(r, hash, key) != NULL;
}

static PGRAPHVkCachedFamilyModulesResult prewarm_cached_modules(
    void *opaque, const ShaderState *state)
{
    return pgraph_vk_materialize_cached_family_modules(opaque, state);
}

static PGRAPHVkHybridPrewarmAttemptResult prewarm_retain_missing_family(
    void *opaque, const PipelineKey *key)
{
    PGRAPHState *pg = opaque;
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool retained = pgraph_vk_fallback_family_enqueue(
        r->fallback_family_requests,
        ARRAY_SIZE(r->fallback_family_requests), key,
        &key->shader_state, true);

    return retained ? PGRAPH_VK_HYBRID_PREWARM_SUBMITTED :
                      PGRAPH_VK_HYBRID_PREWARM_DEFERRED;
}

static ShaderBinding *prewarm_ready_binding(void *opaque,
                                             const ShaderState *state)
{
    return pgraph_vk_prepare_binding_from_ready_modules(
        opaque, state, PGRAPH_VK_FRAGMENT_UBERSHADER);
}

static PGRAPHVkHybridPipelineSubmitResult prewarm_submit_pipeline(
    void *opaque, const PipelineKey *key, ShaderBinding *binding)
{
    return request_hybrid_pipeline(
        opaque, key, binding, true, PGRAPH_VK_HYBRID_PRIORITY_PREWARM);
}

static PGRAPHVkHybridPrewarmAttemptResult prewarm_one_family(
    void *opaque, const PGRAPHVkFamilyHistoryRecord *record)
{
    static const PGRAPHVkHybridPrewarmPrepareOps ops = {
        .device_supported = prewarm_key_device_supported,
        .pipeline_ready = prewarm_pipeline_ready,
        .cached_modules = prewarm_cached_modules,
        .retain_missing_family = prewarm_retain_missing_family,
        .ready_binding = prewarm_ready_binding,
        .submit_pipeline = prewarm_submit_pipeline,
    };
    return pgraph_vk_hybrid_prewarm_prepare_record(record, &ops, opaque);
}

void pgraph_vk_process_hybrid_prewarm(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->hybrid_prewarm.enabled ||
        !r->fallback_family_history_initialized ||
        !r->hybrid_compiler_initialized ||
        !r->hybrid_pipeline_builder_initialized) {
        return;
    }

    PGRAPHVkHybridPrewarmAttemptResult result =
        pgraph_vk_hybrid_prewarm_service(
        &r->hybrid_prewarm, &r->fallback_family_history,
        !pgraph_vk_hybrid_prewarm_can_admit(
            hybrid_prewarm_in_flight(r),
            r->hybrid_prewarm.max_in_flight),
        prewarm_one_family, pg);
    bool service_again = r->hybrid_prewarm.enabled &&
        (result == PGRAPH_VK_HYBRID_PREWARM_READY ||
         result == PGRAPH_VK_HYBRID_PREWARM_REJECTED ||
         (result == PGRAPH_VK_HYBRID_PREWARM_SUBMITTED &&
          pgraph_vk_hybrid_prewarm_can_admit(
              hybrid_prewarm_in_flight(r),
              r->hybrid_prewarm.max_in_flight)));
    qatomic_set(&r->hybrid_prewarm_service_pending, service_again);
    if (service_again) {
        pgraph_vk_hybrid_schedule_service(pg, g_get_monotonic_time());
    }
}

static bool hybrid_pipeline_work_publish(PGRAPHVkState *r,
                                         PGRAPHVkHybridPipelineWork *work)
{
    if (!work->completed_pipeline) {
        return false;
    }
    if (pipeline_cache_find_ready(r, work->key_hash, &work->key)) {
        pgraph_vk_fallback_family_note_pipeline_ready(r, &work->key);
        hybrid_pipeline_work_clear(r, work);
        return false;
    }

    /* Only a finished object may displace an old executable. A busy cache
     * leaves the result owned by work until command-buffer retirement. */
    PipelineBinding *binding = pgraph_vk_pipeline_cache_publish_slot(
        &r->pipeline_cache, work->key_hash, &work->key);
    if (!binding) {
        return false;
    }
    if (binding->pipeline != VK_NULL_HANDLE) {
        hybrid_pipeline_work_clear(r, work);
        return false;
    }

    binding->key = work->key;
    binding->pipeline = work->completed_pipeline;
    binding->layout = work->layout;
    binding->render_pass = work->render_pass;
    binding->dynamic_blend_constant_mask =
        work->dynamic_blend_constant_mask;
    binding->has_dynamic_line_width = work->has_dynamic_line_width;
    binding->draw_time = 0;
    pgraph_vk_hybrid_prewarm_note_publication(
        &r->hybrid_prewarm, binding, work->prewarm);
    work->completed_pipeline = VK_NULL_HANDLE;
    work->layout = VK_NULL_HANDLE;
    r->hybrid_selection_epoch = pgraph_vk_hybrid_next_selection_epoch(
        r->hybrid_selection_epoch);
    pgraph_vk_fallback_family_note_pipeline_ready(r, &work->key);
    hybrid_pipeline_work_clear(r, work);
    return true;
}

static void retry_hybrid_pipeline_publications(PGRAPHVkState *r)
{
    for (size_t i = 0; i < ARRAY_SIZE(r->hybrid_pipeline_work); i++) {
        PGRAPHVkHybridPipelineWork *work = &r->hybrid_pipeline_work[i];
        if (work->in_use && work->completed_pipeline) {
            hybrid_pipeline_work_publish(r, work);
        }
    }
}

static void process_hybrid_pipeline_result(
    PGRAPHState *pg, PGRAPHVkHybridPipelineBuildResult *result)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool adopted = false;
    PGRAPHVkHybridPipelineWork *work = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(r->hybrid_pipeline_work); i++) {
        PGRAPHVkHybridPipelineWork *candidate =
            &r->hybrid_pipeline_work[i];
        if (candidate->in_use &&
            candidate->generation == result->generation &&
            candidate->ticket == result->ticket &&
            candidate->key_hash == result->key_hash) {
            work = candidate;
            break;
        }
    }
    uint64_t shader_hash = work ?
        fast_hash((const uint8_t *)&work->key.shader_state,
                  sizeof(work->key.shader_state)) : 0;
    PGRAPHVkFragmentRoute route = work ? work->key.fragment_route :
                                        PGRAPH_VK_FRAGMENT_SPECIALIZED;
    if (work && work->prewarm && result->started_us &&
        result->finished_us >= result->started_us) {
        uint64_t create_us = result->finished_us - result->started_us;
        r->hybrid_prewarm.worker_completions++;
        r->hybrid_prewarm.worker_create_us_total += create_us;
        r->hybrid_prewarm.worker_create_us_max = MAX(
            r->hybrid_prewarm.worker_create_us_max, create_us);
    }
    if (work && result->generation == r->hybrid_generation &&
        result->vk_result == VK_SUCCESS &&
        result->pipeline != VK_NULL_HANDLE) {
        work->completed_pipeline = result->pipeline;
        result->pipeline = VK_NULL_HANDLE;
        adopted = hybrid_pipeline_work_publish(r, work);
    } else if (work && result->generation == r->hybrid_generation) {
        if (work->prewarm) {
            r->hybrid_prewarm.rejected++;
        }
        pgraph_vk_fallback_family_note_pipeline_failure_at(
            r, &work->key, g_get_monotonic_time());
    }
    if (r->hybrid_trace) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_ADOPT,
            route, result->key_hash, shader_hash,
            result->ticket, result->submitted_us, result->started_us,
            result->finished_us, adopted);
    }
    pgraph_vk_hybrid_pipeline_build_result_destroy(
        &r->hybrid_pipeline_builder, result);
    if (work && work->in_use && !work->completed_pipeline) {
        hybrid_pipeline_work_clear(r, work);
    }
}

static bool process_targeted_hybrid_pipeline_completion(
    PGRAPHState *pg, PGRAPHVkHybridPipelineWork *work)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkHybridPipelineBuildResult result;

    if (!work || !pgraph_vk_hybrid_pipeline_builder_take_result_for(
                     &r->hybrid_pipeline_builder, work->generation,
                     work->ticket, &result)) {
        return false;
    }
    process_hybrid_pipeline_result(pg, &result);
    return true;
}

void pgraph_vk_process_hybrid_pipeline_completions(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->hybrid_pipeline_builder_initialized) {
        return;
    }

    /* A result already owns the expensive object. Publication may only
     * perform a cache insertion or discard; it must never compile or finish. */
    PGRAPHVkHybridPipelineBuildResult result;
    unsigned int published = 0;
    while (published < 2 &&
           pgraph_vk_hybrid_owner_budget_available(r) &&
           pgraph_vk_hybrid_pipeline_builder_take_result(
                                &r->hybrid_pipeline_builder, &result)) {
        published++;
        process_hybrid_pipeline_result(pg, &result);
    }
    if (pgraph_vk_hybrid_pipeline_builder_has_result(
            &r->hybrid_pipeline_builder)) {
        qatomic_set(&r->hybrid_prewarm_service_pending, true);
        pgraph_vk_hybrid_schedule_service(pg, g_get_monotonic_time());
    }
}

static void request_complete_specialization(PGRAPHState *pg,
                                            const ShaderState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->shader_binding || !r->pipeline_binding ||
        r->shader_binding->fragment_route !=
            PGRAPH_VK_FRAGMENT_UBERSHADER ||
        r->pipeline_binding->pipeline == VK_NULL_HANDLE ||
        !r->uber_controls_valid) {
        return;
    }
    PipelineKey fallback_key;
    pgraph_vk_init_pipeline_key_for_state(
        pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER, &fallback_key);
    bool fallback_pipeline_ready =
        memcmp(&r->pipeline_binding->key, &fallback_key,
               sizeof(fallback_key)) == 0;
    bool fallback_resources_ready =
        pgraph_vk_fallback_draw_resource_state(pg, r->shader_binding) !=
        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE;
    if (!fallback_pipeline_ready || !fallback_resources_ready) {
        return;
    }
    pgraph_vk_enqueue_specialized_fragment(pg, state,
                                           fallback_pipeline_ready,
                                           fallback_resources_ready);
    ShaderBinding *binding = pgraph_vk_prepare_binding_from_ready_modules(
        pg, state, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    if (!binding) {
        return;
    }
    PipelineKey key;
    pgraph_vk_init_pipeline_key_for_state(
        pg, state, PGRAPH_VK_FRAGMENT_SPECIALIZED, &key);
    (void)pgraph_vk_request_hybrid_pipeline(pg, &key, binding);
}

static void maybe_request_complete_specialization(PGRAPHState *pg,
                                                   const ShaderState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderBinding *binding = r->shader_binding;
    if (!binding ||
        binding->fragment_route != PGRAPH_VK_FRAGMENT_UBERSHADER) {
        return;
    }
    int64_t now_us = g_get_monotonic_time();
    if (!pgraph_vk_hybrid_promotion_due(
            now_us, binding->next_promotion_probe_us)) {
        return;
    }
    /* A pending shader or pipeline is checked at most once per short time
     * window, rather than being rediscovered for every fallback draw. */
    binding->next_promotion_probe_us = now_us + 16000;
    request_complete_specialization(pg, state);
}

static bool create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (unlikely(r->hybrid_trace)) {
        pgraph_vk_hybrid_trace_draw(r->hybrid_trace);
    }

    if (!pgraph_vk_bind_textures(d)) {
        NV2A_VK_DGROUP_END();
        return false;
    }
    bool hybrid = r->ubershader_runtime_enabled &&
                  r->hybrid_compiler_initialized;
    bool force_ubershader = r->ubershader_force_interpreter;
    bool schedule_specialization = false;
    bool track_specialized_family = false;
    bool family_controls_supported = false;
    bool family_fallback_pipeline_ready = false;
    ShaderState requested_state;
    if (hybrid) {
        pgraph_vk_hybrid_owner_budget_begin(r);
        if (unlikely(pgraph_vk_hybrid_pipeline_builder_has_result(
                &r->hybrid_pipeline_builder))) {
            pgraph_vk_process_hybrid_pipeline_completions(pg);
        }
        if (unlikely(pgraph_vk_hybrid_compiler_has_result(
                &r->hybrid_compiler))) {
            pgraph_vk_process_hybrid_completions(pg);
        }

        /* Optional conservative shortcut: no register value changed, the
         * non-register shader inputs still match, and the complete current
         * executable is usable. Dynamic uniforms and controls still update. */
        if (xemu_tweak_enabled(XEMU_TWEAK_VK_SHADER_FASTPATH) &&
            !pg->regs_written_since_draw && !pg->program_data_dirty &&
            r->shader_binding && r->pipeline_binding &&
            r->pipeline_binding->pipeline != VK_NULL_HANDLE &&
            !r->pipeline_binding->key.clear &&
            r->pipeline_binding->key.fragment_route ==
                r->shader_binding->fragment_route &&
            !pgraph_vk_hybrid_selection_changed(
                r->hybrid_bound_selection_epoch,
                r->hybrid_selection_epoch) &&
            !pgraph_glsl_nonregister_shader_state_changed(
                pg, &r->shader_binding->state) &&
            !check_pipeline_dirty(pg) &&
            pgraph_vk_hybrid_fastpath_route_allowed(
                force_ubershader, r->shader_binding->fragment_route) &&
            (r->shader_binding->fragment_route !=
                 PGRAPH_VK_FRAGMENT_UBERSHADER ||
             pgraph_vk_refresh_fallback_controls(
                 pg, &r->shader_binding->state.psh))) {
            PGRAPHVkShaderPreparation unchanged = {
                .state = r->shader_binding->state,
                .bound_state_equal = true,
            };
            /* Match the ordinary preparation path's per-draw reset before
             * updating dynamic uniforms on the retained binding. */
            r->shader_bindings_changed = false;
            r->uniform_layout_changed[PGRAPH_UNIFORM_STAGE_VSH] = false;
            r->uniform_layout_changed[PGRAPH_UNIFORM_STAGE_PSH] = false;
            pgraph_vk_activate_shaders(
                pg, &unchanged, r->shader_binding->fragment_route,
                r->shader_binding);
            if (pgraph_vk_hybrid_should_schedule_specialization(
                    force_ubershader,
                    r->shader_binding->fragment_route)) {
                maybe_request_complete_specialization(
                    pg, &unchanged.state);
            }
            pgraph_clear_dirty_reg_map(pg);
            NV2A_VK_DGROUP_END();
            return true;
        }

        PGRAPHVkShaderPreparation preparation;
        pgraph_vk_prepare_shaders(pg, &preparation);
        requested_state = preparation.state;

        /* Most draws reuse the current specialized executable. Preserve the
         * original cheap dirty path and still update uniforms as needed. */
        if (!force_ubershader && preparation.bound_state_equal &&
            r->shader_binding &&
            r->shader_binding->fragment_route ==
                PGRAPH_VK_FRAGMENT_SPECIALIZED &&
            r->pipeline_binding &&
            r->pipeline_binding->pipeline != VK_NULL_HANDLE &&
            !r->pipeline_binding->key.clear &&
            r->pipeline_binding->key.fragment_route ==
                PGRAPH_VK_FRAGMENT_SPECIALIZED &&
            !check_pipeline_dirty(pg)) {
            pgraph_vk_activate_shaders(pg, &preparation,
                                       PGRAPH_VK_FRAGMENT_SPECIALIZED,
                                       r->shader_binding);
            pgraph_clear_dirty_reg_map(pg);
            NV2A_VK_DGROUP_END();
            return true;
        }

        /* A stable fallback updates only its dynamic inputs. Fallback mode
         * retries promotion on a timed gate; Always keeps the interpreter. */
        if (preparation.bound_state_equal &&
            !preparation.selection_changed && r->shader_binding &&
            r->shader_binding->fragment_route ==
                PGRAPH_VK_FRAGMENT_UBERSHADER &&
            r->pipeline_binding &&
            r->pipeline_binding->pipeline != VK_NULL_HANDLE &&
            !r->pipeline_binding->key.clear &&
            r->pipeline_binding->key.fragment_route ==
                PGRAPH_VK_FRAGMENT_UBERSHADER &&
            !check_pipeline_dirty(pg) &&
            pgraph_vk_refresh_fallback_controls(
                pg, &requested_state.psh)) {
            pgraph_vk_activate_shaders(pg, &preparation,
                                       PGRAPH_VK_FRAGMENT_UBERSHADER,
                                       r->shader_binding);
            if (pgraph_vk_hybrid_should_schedule_specialization(
                    force_ubershader,
                    PGRAPH_VK_FRAGMENT_UBERSHADER)) {
                maybe_request_complete_specialization(pg, &requested_state);
            }
            pgraph_clear_dirty_reg_map(pg);
            NV2A_VK_DGROUP_END();
            return true;
        }

        PGRAPHVkReadyExecutionCandidates candidates;
        pgraph_vk_resolve_ready_execution_candidates(
            pg, &requested_state, force_ubershader, &candidates);
        PGRAPHVkReadyDrawCandidate specialized = candidates.specialized;
        PGRAPHVkReadyDrawCandidate fallback = candidates.fallback;
        PGRAPHUberControls controls = candidates.controls;
        bool controls_supported = candidates.controls_supported;
        PGRAPHVkFallbackResourceState fallback_resources =
            PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE;
        bool specialized_complete = specialized.shader &&
                                    specialized.pipeline;
        PGRAPHVkExecutionRoute selected =
            PGRAPH_VK_EXECUTION_SPECIALIZED;

        if (specialized_complete) {
            /* Warm specialized draws do no fallback work after their one-time
             * family eligibility decision. The family pipeline probe does not
             * require full-state fallback binding metadata. */
            if (!force_ubershader &&
                pgraph_vk_fallback_family_learning_needed(
                    specialized_complete,
                    specialized.pipeline->family_learn_state)) {
                PipelineKey family_key;
                pgraph_vk_fallback_family_key_from_specialized(
                    specialized.pipeline, &family_key);
                uint64_t family_hash = fast_hash(
                    (const uint8_t *)&family_key, sizeof(family_key));
                controls_supported = pgraph_vk_pack_fallback_controls(
                    pg, &requested_state.psh, &controls);
                family_fallback_pipeline_ready =
                    pipeline_cache_find_ready(r, family_hash,
                                              &family_key) != NULL;
                family_controls_supported = controls_supported;
                track_specialized_family = true;
            }
        } else {
            if (controls_supported && fallback.shader && fallback.pipeline) {
                fallback_resources = pgraph_vk_fallback_draw_resource_state(
                    pg, fallback.shader);
            }
            selected = pgraph_vk_hybrid_choose_execution_route(
                force_ubershader && controls_supported,
                specialized.shader != NULL, specialized.pipeline != NULL,
                fallback.shader != NULL, fallback.pipeline != NULL,
                fallback_resources);
            family_controls_supported = controls_supported;
            family_fallback_pipeline_ready = fallback.pipeline != NULL;
            track_specialized_family = !force_ubershader;
        }

        PGRAPHVkFragmentRoute route;
        ShaderBinding *ready_shader;
        PipelineBinding *ready_pipeline;
        if (selected == PGRAPH_VK_EXECUTION_SPECIALIZED) {
            route = PGRAPH_VK_FRAGMENT_SPECIALIZED;
            ready_shader = specialized.shader;
            ready_pipeline = specialized.pipeline;
        } else if (selected == PGRAPH_VK_EXECUTION_UBERSHADER ||
                   selected == PGRAPH_VK_EXECUTION_UBERSHADER_AFTER_ROLLOVER) {
            route = PGRAPH_VK_FRAGMENT_UBERSHADER;
            ready_shader = fallback.shader;
            ready_pipeline = fallback.pipeline;
            schedule_specialization =
                pgraph_vk_hybrid_should_schedule_specialization(
                    force_ubershader, route);
        } else {
            /* Preserve first-family fallback construction when neither
             * binding exists. When one binding is prepared, construct only
             * its missing pipeline instead of a colder alternate route. */
            route = pgraph_vk_hybrid_choose_uncovered_route(
                force_ubershader && controls_supported,
                specialized.shader != NULL, fallback.shader != NULL,
                fallback.pipeline != NULL, controls_supported,
                fallback_resources);
            ready_shader = NULL;
            ready_pipeline = NULL;
            schedule_specialization =
                pgraph_vk_hybrid_should_schedule_specialization(
                    force_ubershader, route);
            if (r->hybrid_trace) {
                pgraph_vk_hybrid_trace_record(
                    r->hybrid_trace, VK_HYBRID_TRACE_UNCOVERED,
                    route, specialized.hash,
                    fast_hash((const uint8_t *)&requested_state,
                              sizeof(requested_state)), 0,
                    specialized.shader != NULL,
                    specialized.pipeline != NULL,
                    fallback.shader != NULL,
                    (uint64_t)(fallback.pipeline != NULL) |
                        ((uint64_t)fallback_resources << 1) |
                        ((uint64_t)controls_supported << 3));
            }
        }
        r->uber_controls_valid = route == PGRAPH_VK_FRAGMENT_UBERSHADER &&
                                 controls_supported;
        if (r->uber_controls_valid) {
            pgraph_vk_publish_fallback_controls(r, &controls);
        }
        pgraph_vk_activate_shaders(pg, &preparation, route, ready_shader);

        if (ready_pipeline) {
            /* The borrowed probes did not affect LRU order. Only the chosen
             * executable is touched, and its key is already exact. */
            lru_touch_existing(&r->pipeline_cache, &ready_pipeline->node);
            r->pipeline_binding_changed =
                r->pipeline_binding != ready_pipeline;
            r->pipeline_binding = ready_pipeline;
            pgraph_vk_hybrid_prewarm_note_demand(
                &r->hybrid_prewarm, ready_pipeline);
            if (route == PGRAPH_VK_FRAGMENT_SPECIALIZED &&
                track_specialized_family) {
                pgraph_vk_track_specialized_fallback_family(
                    r, ready_pipeline, family_controls_supported,
                    family_fallback_pipeline_ready, 0);
            }
            pgraph_clear_dirty_reg_map(pg);

            if (schedule_specialization) {
                maybe_request_complete_specialization(pg, &requested_state);
            }
            NV2A_VK_DGROUP_END();
            return true;
        }
    } else {
        trace_execution_candidates(pg);
        pgraph_vk_bind_shaders(pg);
    }

    // FIXME: If nothing was dirty, don't even try creating the key or hashing.
    //        Just use the same pipeline.
    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    // FIXME: We could clear less

    if (r->pipeline_binding && !pipeline_dirty) {
        if (r->hybrid_trace) {
            PipelineKey *ready_key = &r->pipeline_binding->key;
            pgraph_vk_hybrid_trace_record(
                r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_PROBE,
                ready_key->fragment_route,
                fast_hash((const uint8_t *)ready_key, sizeof(*ready_key)),
                fast_hash((const uint8_t *)&ready_key->shader_state,
                          sizeof(ready_key->shader_state)),
                0, 1, 1, 0, 0);
        }
        NV2A_VK_DPRINTF("Cache hit");
        if (hybrid && track_specialized_family &&
            r->shader_binding->fragment_route ==
                PGRAPH_VK_FRAGMENT_SPECIALIZED) {
            pgraph_vk_track_specialized_fallback_family(
                r, r->pipeline_binding, family_controls_supported,
                family_fallback_pipeline_ready, 0);
        }
        if (schedule_specialization) {
            maybe_request_complete_specialization(pg, &requested_state);
        }
        NV2A_VK_DGROUP_END();
        return true;
    }

    PipelineKey key;
    pgraph_vk_init_pipeline_key_for_state(
        pg, &r->shader_binding->state,
        r->shader_binding->fragment_route, &key);
    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    if (r->hybrid_trace) {
        PipelineBinding *ready = pipeline_cache_find_ready(r, hash, &key);
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_PROBE,
            key.fragment_route, hash,
            fast_hash((const uint8_t *)&key.shader_state,
                      sizeof(key.shader_state)),
            0, ready != NULL, 0, 0, 0);
    }

    PGRAPHVkHybridPipelineWork *demanded_work = NULL;
    if (hybrid && r->hybrid_pipeline_builder_initialized) {
        for (size_t i = 0; i < ARRAY_SIZE(r->hybrid_pipeline_work); i++) {
            PGRAPHVkHybridPipelineWork *work =
                &r->hybrid_pipeline_work[i];
            if (!work->in_use || work->key_hash != hash ||
                memcmp(&work->key, &key, sizeof(key)) != 0) {
                continue;
            }
            demanded_work = work;
            work->priority = pgraph_vk_hybrid_priority_max(
                work->priority, PGRAPH_VK_HYBRID_PRIORITY_REQUIRED);
            pgraph_vk_hybrid_pipeline_builder_promote(
                &r->hybrid_pipeline_builder, work->generation,
                work->ticket, PGRAPH_VK_HYBRID_PRIORITY_REQUIRED);
            if (work->completed_pipeline) {
                hybrid_pipeline_work_publish(r, work);
            } else {
                process_targeted_hybrid_pipeline_completion(pg, work);
            }
            break;
        }
    }

    if (demanded_work && demanded_work->in_use &&
        demanded_work->completed_pipeline != VK_NULL_HANDLE &&
        !pipeline_cache_find_ready(r, hash, &key)) {
        /*
         * If publication was deferred only because every cache slot was
         * pinned, use the ordinary resource-rollover boundary and publish
         * the completed exact object afterward. Never compile it again.
         */
        pipeline_cache_get_or_create(pg, hash, &key);
        hybrid_pipeline_work_publish(r, demanded_work);
    }

    PipelineBinding *snode = pipeline_cache_get_or_create(pg, hash, &key);
    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        pgraph_vk_hybrid_prewarm_note_demand(&r->hybrid_prewarm, snode);
        if (hybrid && track_specialized_family &&
            r->shader_binding->fragment_route ==
                PGRAPH_VK_FRAGMENT_SPECIALIZED) {
            pgraph_vk_track_specialized_fallback_family(
                r, snode, family_controls_supported,
                family_fallback_pipeline_ready, 0);
        }
        if (schedule_specialization) {
            maybe_request_complete_specialization(pg, &requested_state);
        }
        NV2A_VK_DGROUP_END();
        return true;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    memcpy(&snode->key, &key, sizeof(key));

    PGRAPHVkGraphicsPipelineRecipe recipe;
    if (!prepare_graphics_pipeline_recipe(pg, &key, r->shader_binding,
                                          &recipe)) {
        NV2A_VK_DGROUP_END();
        return false;
    }
    VkPipeline pipeline;
    int64_t pipeline_start_us = g_get_monotonic_time();
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &recipe.info, NULL, &pipeline));
    int64_t pipeline_finish_us = g_get_monotonic_time();
    uint64_t pipeline_create_us = MAX(
        (int64_t)0, pipeline_finish_us - pipeline_start_us);
    if (r->hybrid_trace) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_CREATE,
            key.fragment_route, hash,
            fast_hash((const uint8_t *)&key.shader_state,
                      sizeof(key.shader_state)),
            0, pipeline_start_us, pipeline_finish_us, 0, 0);
    }

    snode->pipeline = pipeline;
    snode->has_dynamic_line_width = recipe.has_dynamic_line_width;
    snode->layout = recipe.layout;
    snode->render_pass = recipe.render_pass;
    snode->draw_time = pg->draw_time;
    snode->dynamic_blend_constant_mask = recipe.dynamic_blend_constant_mask;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    if (hybrid && force_ubershader &&
        key.fragment_route == PGRAPH_VK_FRAGMENT_UBERSHADER) {
        pgraph_vk_note_interpreter_family(r, &key, pipeline_create_us);
    }

    if (hybrid && track_specialized_family &&
        r->shader_binding->fragment_route ==
            PGRAPH_VK_FRAGMENT_SPECIALIZED) {
        pgraph_vk_track_specialized_fallback_family(
            r, snode, family_controls_supported,
            family_fallback_pipeline_ready, pipeline_create_us);
    }

    if (schedule_specialization) {
        maybe_request_complete_specialization(pg, &requested_state);
    }

    NV2A_VK_DGROUP_END();
    return true;
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->use_push_constants_for_uniform_attrs) {
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= 1);
    uint32_t uber_control_offset =
        r->shader_binding->fragment_route == PGRAPH_VK_FRAGMENT_UBERSHADER ?
            r->uber_control_offset : 0;
    uint32_t dynamic_offset_count = pgraph_vk_descriptor_dynamic_offset_count(
        r->ubershader_runtime_enabled);

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            &r->descriptor_sets[r->descriptor_set_index - 1],
                            dynamic_offset_count,
                            dynamic_offset_count ? &uber_control_offset : NULL);
}

static void begin_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    assert(r->num_queries_in_flight < r->max_queries_in_flight);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        r->num_queries_in_flight, 1);
    vkCmdBeginQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight,
                    VK_QUERY_CONTROL_PRECISE_BIT);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(r->query_in_flight);

    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    if (!b_src->buffer_offset) {
        return;
    }

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VK_CHECK(pgraph_vk_record_staging_copy(
        r->allocator, b_src->allocation, cmd, b_src->buffer, b_dst->buffer,
        b_src->buffer_offset, dst_access_mask, dst_stage_mask));

    b_src->buffer_offset = 0;
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_CHECK(vmaFlushAllocation(
        r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation, 0,
        VK_WHOLE_SIZE));

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    assert(r->framebuffer_index > 0);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;

}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        vkCmdEndRenderPass(r->command_buffer);
        r->in_render_pass = false;
    }
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
    [VK_FINISH_REASON_TEXTURE_DIRTY] = NV2A_PROF_FINISH_TEXTURE_DIRTY,
};

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool trace_had_command_buffer = r->in_command_buffer;
    uint64_t trace_submit_us = 0;
    uint64_t trace_wait_us = 0;

    assert(!r->in_draw);
    assert(r->debug_depth == 0);
    pgraph_vk_perf_record_finish_call(r, finish_reason);

    if (r->in_command_buffer) {
        uint64_t staged_bytes = 0;
        if (r->perf.enabled) {
            staged_bytes =
                r->storage_buffers[BUFFER_INDEX_STAGING].buffer_offset +
                r->storage_buffers[BUFFER_TEXTURE_STAGING].buffer_offset +
                r->storage_buffers[BUFFER_VERTEX_RAM_STAGING].buffer_offset +
                r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING].buffer_offset +
                r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset;
        }
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        if (r->in_render_pass) {
            end_render_pass(r);
        }
        if (r->query_in_flight) {
            end_query(r);
        }
        VK_CHECK(vkEndCommandBuffer(r->command_buffer));

        VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg); // FIXME: Cleanup
        sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
        sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                                BUFFER_VERTEX_INLINE);
        sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM);
        flush_memory_buffer(pg, cmd);
        VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
        r->in_aux_command_buffer = false;

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->aux_command_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &r->command_buffer_semaphore,
            },
            {

                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->command_buffer,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &r->command_buffer_semaphore,
                .pWaitDstStageMask = &wait_stage,
            }
        };
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        vkResetFences(r->device, 1, &r->command_buffer_fence);
        bool time_submit =
            pgraph_vk_perf_should_time_finish(r, finish_reason) ||
            r->hybrid_trace != NULL;
        int64_t submit_start = time_submit ?
            qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
        VkResult result = vkQueueSubmit(
            r->queue, ARRAY_SIZE(submit_infos), submit_infos,
            r->command_buffer_fence);
        uint64_t submit_cpu_us = time_submit ?
            MAX(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - submit_start, 0) : 0;
        trace_submit_us = submit_cpu_us;
        VK_CHECK(result);
        nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_GPU_SUBMIT);
        r->submit_count += 1;

        bool check_budget = false;

        // Periodically check memory budget
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {

            // VMA queries budget via vmaSetCurrentFrameIndex
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            check_budget = true;
        }

        int64_t wait_start = time_submit ?
            qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
        result = vkWaitForFences(r->device, 1, &r->command_buffer_fence,
                                 VK_TRUE, UINT64_MAX);
        uint64_t wait_us = time_submit ?
            MAX(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - wait_start, 0) : 0;
        trace_wait_us = wait_us;
        VK_CHECK(result);
        pgraph_vk_perf_record_finish_submit(
            r, finish_reason, time_submit, submit_cpu_us, wait_us, staged_bytes,
            ARRAY_SIZE(submit_infos), 2);
        r->storage_buffers[BUFFER_VERTEX_RAM_STAGING].buffer_offset = 0;
        r->storage_buffers[BUFFER_TEXTURE_STAGING].buffer_offset = 0;

        r->descriptor_set_index = 0;
        r->in_command_buffer = false;
        if (r->vertex_ram_read_pages) {
            memset(r->vertex_ram_read_pages, 0,
                   r->num_vertex_ram_read_pages);
        }
        if (!xemu_tweak_enabled(XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS)) {
            r->vertex_ram_read_tracking_active = false;
            r->vertex_ram_read_tracking_idle_batches = 0;
        } else if (r->vertex_ram_updated_in_batch) {
            r->vertex_ram_read_tracking_active = true;
            r->vertex_ram_read_tracking_idle_batches = 0;
        } else if (r->vertex_ram_read_tracking_active &&
                   ++r->vertex_ram_read_tracking_idle_batches >=
                       VERTEX_READ_TRACKING_IDLE_BATCHES) {
            r->vertex_ram_read_tracking_active = false;
            r->vertex_ram_read_tracking_idle_batches = 0;
        }
        r->vertex_ram_updated_in_batch = false;
        destroy_framebuffers(pg);

        if (check_budget) {
            pgraph_vk_check_memory_budget(pg);
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    if (r->perf.enabled && !trace_had_command_buffer) {
        r->perf.report_cpu_only_retirements += r->report_queue_depth;
    }
    pgraph_vk_process_pending_reports_internal(d);

    pgraph_vk_compute_finish_complete(r);
    retry_hybrid_pipeline_publications(r);
    pgraph_vk_hybrid_trace_record(
        r->hybrid_trace, VK_HYBRID_TRACE_FINISH,
        r->shader_binding ? r->shader_binding->fragment_route : 0,
        0, 0, 0, finish_reason, trace_wait_us, trace_submit_us,
        trace_had_command_buffer);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(!r->in_command_buffer);

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    pgraph_vk_invalidate_blend_constants(pg);
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass(r);
    if (r->query_in_flight) {
        end_query(r);
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static bool begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        int64_t start_us = r->perf.enabled ? g_get_monotonic_time() : 0;
        bool pipeline_ready = create_pipeline(pg);
        pgraph_vk_perf_record_cpu_region(
            r, VK_PERF_CPU_PIPELINE_PREPARE,
            r->perf.enabled ? g_get_monotonic_time() - start_us : 0);
        if (!pipeline_ready) {
            error_report("Vulkan draw skipped because texture preparation "
                         "failed");
            return false;
        }
    }

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (!pg->clearing) {
        int64_t start_us = r->perf.enabled ? g_get_monotonic_time() : 0;
        pgraph_vk_update_descriptor_sets(pg);
        pgraph_vk_perf_record_cpu_region(
            r, VK_PERF_CPU_UPDATE_DESCRIPTOR_SETS,
            r->perf.enabled ? g_get_monotonic_time() - start_us : 0);
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
    return true;
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

void pgraph_vk_invalidate_blend_constants(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_blend_constants_cache_invalidate(&r->blend_constants);
}

static void update_blend_constants(PGRAPHState *pg,
                                   uint32_t relevant_component_mask)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint32_t guest_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);

    if (!pgraph_vk_blend_constants_cache_update(
            &r->blend_constants, guest_color, relevant_component_mask)) {
        return;
    }

    if (pgraph_vk_blend_constants_cache_needs_pack(&r->blend_constants,
                                                   guest_color)) {
        pgraph_argb_pack32_to_rgba_float(
            guest_color, r->blend_constants.packed_color);
    }
    vkCmdSetBlendConstants(r->command_buffer,
                           r->blend_constants.packed_color);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);

    // Visibility testing
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (r->new_query_needed && r->query_in_flight) {
            end_render_pass(r);
            end_query(r);
        }
        if (!r->query_in_flight) {
            end_render_pass(r);
            begin_query(r);
        }
    } else if (r->query_in_flight) {
        end_render_pass(r);
        end_query(r);
    }

    if (pg->clearing) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        pgraph_vk_blend_constants_cache_pipeline_bound(
            &r->blend_constants,
            r->pipeline_binding->dynamic_blend_constant_mask != 0);
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        float vp_offset = pg->surface_scale_factor > 1
                          ? 0.5f * (pg->surface_scale_factor - 1) + 0.25f
                          : 0.0f;

        VkViewport viewport = {
            .x = -vp_offset,
            .y = -vp_offset,
            .width = vp_width + vp_offset,
            .height = vp_height + vp_offset,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);

        if (r->pipeline_binding->has_dynamic_line_width) {
            float line_width =
                clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
            vkCmdSetLineWidth(r->command_buffer, line_width);
        }
        r->pipeline_binding_changed = false;
    }

    if (!pg->clearing) {
        /*
         * Blend color is dynamic and may change without a pipeline bind.
         * The cache is scoped to this command buffer and invalidated by the
         * partial-clear path when it overwrites the Vulkan dynamic state.
         */
        if (r->pipeline_binding->dynamic_blend_constant_mask != 0) {
            update_blend_constants(
                pg, r->pipeline_binding->dynamic_blend_constant_mask);
        }

        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
    }

    /* Preparation may finish the old batch; mark only the draw actually
     * recorded in the current command buffer. */
    if (!pg->clearing &&
        xemu_tweak_enabled(XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS) &&
        r->vertex_ram_read_tracking_active) {
        for (size_t i = 0; i < r->num_pending_vertex_ram_reads; i++) {
            const MemorySyncRequirement *read =
                &r->pending_vertex_ram_reads[i];
            size_t first_page = read->addr / TARGET_PAGE_SIZE;
            size_t page_count = read->size / TARGET_PAGE_SIZE;
            assert(first_page <= r->num_vertex_ram_read_pages);
            assert(page_count <= r->num_vertex_ram_read_pages - first_page);
            memset(r->vertex_ram_read_pages + first_page, 1, page_count);
        }
    }
    r->num_pending_vertex_ram_reads = 0;
    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

    int64_t start_us = r->perf.enabled ? g_get_monotonic_time() : 0;
    bool draw_recorded = pgraph_vk_flush_draw_internal(d);
    pgraph_vk_perf_record_cpu_region(
        r, VK_PERF_CPU_DRAW_FLUSH,
        r->perf.enabled ? g_get_monotonic_time() - start_us : 0);
    if (!draw_recorded) {
        return;
    }

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size,
                                          size_t *count);

/* Version only small, non-overlapping, DMA/VRAM-bounded draws. The inline
 * staging pair is copied before the recorded draw batch executes, so older
 * draws keep seeing their earlier fixed-buffer contents. */
static bool can_version_vertex_draw(PGRAPHState *pg, uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint64_t budget = PGRAPH_VK_VERTEX_VERSION_COPY_BUDGET;
    uint64_t vram_size = memory_region_size(d->vram);

    if (!xemu_tweak_enabled(XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS) ||
        !r->vertex_ram_read_tracking_active || !num_vertices ||
        num_vertices > PGRAPH_VK_VERTEX_VERSION_MAX_VERTICES ||
        !r->num_active_vertex_binding_descriptions) {
        return false;
    }

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_id = r->vertex_attribute_descriptions[i].location;
        VertexAttribute *attr = &pg->vertex_attributes[attr_id];
        size_t element_size, element_count;
        get_size_and_count_for_format(
            r->vertex_attribute_descriptions[i].format,
            &element_size, &element_count);
        uint64_t element_bytes = element_size * element_count;
        uint64_t stride = r->vertex_binding_descriptions[i].stride;
        uint64_t addr = r->vertex_attribute_offsets[attr_id];
        hwaddr dma_len = 0;
        uint8_t *dma = nv_dma_map(
            d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
            &dma_len);

        if (!dma || !pgraph_vk_vertex_version_source_fits(
                num_vertices, stride, element_bytes, attr->offset,
                dma_len, addr, vram_size) ||
            !pgraph_vk_vertex_version_copy_fits(
                num_vertices, element_bytes, budget)) {
            return false;
        }
        budget -= (uint64_t)num_vertices * element_bytes;
    }
    return true;
}

static void set_vertex_ram_stale_pages(PGRAPHVkState *r, size_t first_page,
                                       size_t page_count, bool stale)
{
    pgraph_vk_vertex_version_set_stale(
        r->vertex_ram_stale_pages, r->num_vertex_ram_read_pages,
        &r->vertex_ram_stale_page_count, first_page, page_count, stale);
}

typedef enum PGRAPHVkVertexBacking {
    PGRAPH_VK_VERTEX_BACKING_FIXED,
    PGRAPH_VK_VERTEX_BACKING_PRIVATE,
} PGRAPHVkVertexBacking;

static PGRAPHVkVertexBacking prepare_vertex_ram_backing(
    PGRAPHState *pg, uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return PGRAPH_VK_VERTEX_BACKING_FIXED;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    bool any_surface_overlap = false;
    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *sync = &r->vertex_ram_buffer_syncs[i];
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        sync->addr, sync->size);

        /* Page alignment is needed for dirty tracking and buffer uploads,
         * but it must not turn adjacent vertices into a surface readback. */
        sync->surface_overlap = sync->size &&
            pgraph_vk_surface_overlaps_range(pg, sync->addr, sync->size);
        any_surface_overlap |= sync->surface_overlap;
        if (sync->surface_overlap &&
            !pgraph_vk_download_surfaces_in_range_if_dirty(
                pg, sync->addr, sync->size)) {
            error_report("Vulkan surface readback failed before vertex upload");
            abort();
        }

        hwaddr start_addr = sync->addr & TARGET_PAGE_MASK;
        hwaddr end_addr = sync->addr + sync->size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        sync->addr, sync->size, start_addr,
                        end_addr - start_addr);

        sync->addr = start_addr;
        sync->size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
            p->surface_overlap |= t->surface_overlap;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    bool version_policy_checked = false;
    bool version_eligible = false;
    bool versioned = false;
    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

        bool memory_dirty = memory_region_test_and_clear_dirty(
            d->vram, addr, size, DIRTY_MEMORY_NV2A);
        size_t first_page = addr / TARGET_PAGE_SIZE;
        size_t page_count = size / TARGET_PAGE_SIZE;
        assert(first_page <= r->num_vertex_ram_read_pages);
        assert(page_count <= r->num_vertex_ram_read_pages - first_page);
        bool mirror_stale = r->vertex_ram_stale_page_count &&
            memchr(r->vertex_ram_stale_pages + first_page, 1, page_count);
        /* A successful GPU readback marks this range NV2A-dirty. An overlap
         * with a clean surface needs no mirror upload unless a versioned draw
         * deliberately left the fixed mirror stale. */
        if (memory_dirty || mirror_stale) {
            /* A byte-per-page conservative footprint keeps direct host
             * writes away from vertex data already captured by this batch. */
            bool shortcuts_enabled = xemu_tweak_enabled(
                XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS);
            bool can_write_directly = shortcuts_enabled &&
                                      r->vertex_ram_read_tracking_active;
            if (can_write_directly) {
                for (size_t page = first_page;
                     page < first_page + page_count; page++) {
                    if (r->vertex_ram_read_pages[page]) {
                        can_write_directly = false;
                        break;
                    }
                }
            }
            if (shortcuts_enabled && r->in_command_buffer) {
                r->vertex_ram_updated_in_batch = true;
            }
            if (shortcuts_enabled && !can_write_directly &&
                !any_surface_overlap) {
                if (!version_policy_checked) {
                    version_eligible = can_version_vertex_draw(
                        pg, num_vertices);
                    version_policy_checked = true;
                }
                if (version_eligible) {
                    set_vertex_ram_stale_pages(r, first_page, page_count,
                                               true);
                    if (r->perf.enabled) {
                        r->perf.vertex_version_selected_ranges++;
                    }
                    versioned = true;
                    continue;
                }
            }
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            if (can_write_directly) {
                pgraph_vk_update_unread_vertex_ram_buffer_after_surface_readback(
                    pg, addr, d->vram_ptr + addr, size);
            } else {
                pgraph_vk_update_vertex_ram_buffer_after_surface_readback(
                    pg, addr, d->vram_ptr + addr, size);
            }
            set_vertex_ram_stale_pages(r, first_page, page_count, false);
        }
    }

    /* Versioned draws bind every active attribute from the private inline
     * slice. They do not read the fixed vertex mirror, so these ranges must
     * not make later writes look like conflicts with an earlier mirror draw. */
    if (versioned) {
        r->num_pending_vertex_ram_reads = 0;
    } else {
        assert(num_syncs <= ARRAY_SIZE(r->pending_vertex_ram_reads));
        memcpy(r->pending_vertex_ram_reads, merged,
               num_syncs * sizeof(merged[0]));
        r->num_pending_vertex_ram_reads = num_syncs;
    }
    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
    return versioned ? PGRAPH_VK_VERTEX_BACKING_PRIVATE :
                       PGRAPH_VK_VERTEX_BACKING_FIXED;
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    bool prepared = begin_pre_draw(pg);
    assert(prepared);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            pgraph_vk_invalidate_blend_constants(pg);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0,
                           r->num_active_vertex_binding_descriptions, buffers,
                           offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size,
                                VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[index];
    VkDeviceSize required_size =
        pgraph_vk_buffer_required_size(pg, index, size, alignment);

    assert(required_size >= size);

    if (!pgraph_vk_buffer_has_space_for(pg, index, size, alignment)) {
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_RESOURCE_SHORTAGE,
            r->shader_binding ? r->shader_binding->fragment_route : 0,
            0, 0, 0, VK_HYBRID_SHORTAGE_BUFFER, index,
            required_size, buffer->buffer_size);
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        /*
         * Finishing submits the accumulated staging data and resets its
         * offset. Size the next allocation for the request at that reset
         * offset, not for the stale end of the previous command buffer.
         */
        if (!xemu_tweak_enabled(XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH)) {
            required_size =
                pgraph_vk_buffer_required_size(pg, index, size, alignment);
        }
        pgraph_vk_ensure_buffer_pair_capacity(pg, index, required_size);
        return true;
    }

    if (buffer->buffer == VK_NULL_HANDLE || buffer->buffer_size < size) {
        if (r->in_command_buffer || r->in_aux_command_buffer) {
            pgraph_vk_hybrid_trace_record(
                r->hybrid_trace, VK_HYBRID_TRACE_RESOURCE_SHORTAGE,
                r->shader_binding ? r->shader_binding->fragment_route : 0,
                0, 0, 0, VK_HYBRID_SHORTAGE_BUFFER, index,
                required_size, buffer->buffer_size);
            pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
            required_size =
                pgraph_vk_buffer_required_size(pg, index, size, alignment);
        }
        pgraph_vk_ensure_buffer_pair_capacity(pg, index, required_size);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

/* Maximum NV2A vertex attribute width: four 32-bit components. */
static const VkDeviceSize REMAPPED_VERTEX_BLOCK_ALIGNMENT = 4 * sizeof(float);

static VertexBufferRemap prepare_vertex_attribute_layout(
    PGRAPHState *pg, uint32_t num_vertices,
    PGRAPHVkVertexBacking backing)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid &&
            backing == PGRAPH_VK_VERTEX_BACKING_FIXED) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    return remap;
}

static void reserve_remapped_attributes(PGRAPHState *pg,
                                        VertexBufferRemap remap)
{
    if (!remap.attributes) {
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
    ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                        remap.buffer_space_required,
                        REMAPPED_VERTEX_BLOCK_ALIGNMENT);
    buffer->buffer_offset = ROUND_UP(buffer->buffer_offset,
                                     REMAPPED_VERTEX_BLOCK_ALIGNMENT);
}

#define COPY_REMAPPED_ATTRS(n)                                    \
    do {                                                          \
        for (uint32_t vertex_id = 0; vertex_id < copy_count;      \
             vertex_id++) {                                       \
            memcpy(out_ptr, in_ptr, (n));                         \
            out_ptr += (n);                                       \
            in_ptr += old_stride;                                 \
        }                                                         \
    } while (0)

static void pack_remapped_attributes(PGRAPHState *pg, VertexBufferRemap remap,
                                     uint32_t start_vertex,
                                     uint32_t num_vertices,
                                     uint8_t *destination)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // Copy vertex data
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        size_t new_stride = remap.map[attr_id].new_stride;
        size_t old_stride = remap.map[attr_id].old_stride;
        uint32_t first_vertex =
            xemu_tweak_enabled(XEMU_TWEAK_VK_BOUNDED_VERTEX_UPLOADS) ?
                start_vertex : 0;
        uint32_t copy_count = num_vertices - first_vertex;

        uint8_t *out_ptr = destination + remap.map[attr_id].offset +
                           (size_t)first_vertex * new_stride;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id] +
                          (size_t)first_vertex * old_stride;

        switch (new_stride) {
        case 4:
            COPY_REMAPPED_ATTRS(4);
            break;
        case 8:
            COPY_REMAPPED_ATTRS(8);
            break;
        case 12:
            COPY_REMAPPED_ATTRS(12);
            break;
        case 16:
            COPY_REMAPPED_ATTRS(16);
            break;
        default:
            for (uint32_t vertex_id = 0; vertex_id < copy_count; vertex_id++) {
                memcpy(out_ptr, in_ptr, new_stride);
                out_ptr += new_stride;
                in_ptr += old_stride;
            }
            break;
        }

    }
}

typedef struct PGRAPHVkPreparedVertexData {
    PGRAPHVkVertexBacking backing;
    VertexBufferRemap remap;
} PGRAPHVkPreparedVertexData;

/* Resolve mirror ownership, refresh CPU-decoded values, and capture a private
 * generation before buffer reservation or descriptor preparation can finish
 * the command buffer containing earlier draws. */
static PGRAPHVkPreparedVertexData prepare_vertex_data(
    PGRAPHState *pg, uint32_t start_vertex, uint32_t num_vertices,
    uint32_t provoking_vertex)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkPreparedVertexData prepared = {
        .backing = prepare_vertex_ram_backing(pg, num_vertices),
    };

    pgraph_vk_refresh_vertex_inline_values_after_sync(pg, provoking_vertex);
    prepared.remap = prepare_vertex_attribute_layout(
        pg, num_vertices, prepared.backing);

    if (prepared.backing == PGRAPH_VK_VERTEX_BACKING_PRIVATE) {
        assert(prepared.remap.buffer_space_required <=
               PGRAPH_VK_VERTEX_VERSION_SCRATCH_SIZE);
        memset(r->vertex_version_scratch, 0,
               prepared.remap.buffer_space_required);
        pack_remapped_attributes(pg, prepared.remap, start_vertex,
                                 num_vertices, r->vertex_version_scratch);
    }

    reserve_remapped_attributes(pg, prepared.remap);
    return prepared;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices,
                                                      PGRAPHVkVertexBacking backing)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    if (!remap.attributes) {
        return;
    }

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required,
                                          REMAPPED_VERTEX_BLOCK_ALIGNMENT));
    assert(buffer->mapped);

    uint8_t *destination = buffer->mapped + buffer->buffer_offset;
    if (backing == PGRAPH_VK_VERTEX_BACKING_PRIVATE) {
        /* The guest bytes were captured before reservation or descriptor
         * preparation could finish the earlier command buffer. */
        memcpy(destination, r->vertex_version_scratch,
               remap.buffer_space_required);
    } else {
        pack_remapped_attributes(pg, remap, start_vertex, num_vertices,
                                 destination);
    }

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (remap.attributes & (1 << attr_id)) {
            r->vertex_attribute_offsets[attr_id] =
                buffer->buffer_offset + remap.map[attr_id].offset;
        }
    }

    buffer->buffer_offset += remap.buffer_space_required;
}

static void publish_prepared_vertex_data(PGRAPHState *pg,
                                         PGRAPHVkPreparedVertexData prepared,
                                         uint32_t start_vertex,
                                         uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    copy_remapped_attributes_to_inline_buffer(
        pg, prepared.remap, start_vertex, num_vertices, prepared.backing);
    if (prepared.backing == PGRAPH_VK_VERTEX_BACKING_PRIVATE &&
        r->perf.enabled) {
        r->perf.vertex_version_draw_count++;
        r->perf.vertex_version_bytes += prepared.remap.buffer_space_required;
    }
}

static bool pgraph_vk_flush_draw_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return false;
    }

    r->num_vertex_ram_buffer_syncs = 0;
    r->num_pending_vertex_ram_reads = 0;

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        if (!pgraph_vk_bind_vertex_attributes(
                d, pg->draw_arrays_min_start,
                pg->draw_arrays_max_count - 1, false, 0,
                pg->draw_arrays_max_count - 1)) {
            NV2A_VK_DGROUP_END();
            return false;
        }
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        PGRAPHVkPreparedVertexData vertex_data = prepare_vertex_data(
            pg, min_element, max_element, pg->draw_arrays_max_count - 1);

        if (!begin_pre_draw(pg)) {
            NV2A_VK_DGROUP_END();
            return false;
        }
        publish_prepared_vertex_data(pg, vertex_data, min_element,
                                     max_element);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        bind_vertex_buffer(pg, vertex_data.remap.attributes, 0);
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            uint32_t start = pg->draw_arrays_start[i],
                     count = pg->draw_arrays_count[i];
            NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
            vkCmdDraw(r->command_buffer, count, 1, start, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        size_t index_data_size =
            pg->inline_elements_length * sizeof(pg->inline_elements[0]);

        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size, 1);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        uint32_t provoking_element =
            pg->inline_elements[pg->inline_elements_length - 1];
        if (!pgraph_vk_bind_vertex_attributes(
                d, min_element, max_element, false, 0,
                provoking_element)) {
            NV2A_VK_DGROUP_END();
            return false;
        }
        PGRAPHVkPreparedVertexData vertex_data = prepare_vertex_data(
            pg, min_element, max_element + 1, provoking_element);

        if (!begin_pre_draw(pg)) {
            NV2A_VK_DGROUP_END();
            return false;
        }
        publish_prepared_vertex_data(pg, vertex_data, min_element,
                                     max_element + 1);
        VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
            pg, pg->inline_elements, index_data_size);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        bind_vertex_buffer(pg, vertex_data.remap.attributes, 0);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r->command_buffer, pg->inline_elements_length, 1, 0, 0,
                         0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset, 1);

        if (!begin_pre_draw(pg)) {
            NV2A_VK_DGROUP_END();
            return false;
        }
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                            inline_array_data_size, 1);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        if (!pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                              vertex_size, index_count - 1)) {
            NV2A_VK_DGROUP_END();
            return false;
        }

        if (!begin_pre_draw(pg)) {
            NV2A_VK_DGROUP_END();
            return false;
        }
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
    return true;
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    pgraph_vk_flush_draw_internal(d);
}

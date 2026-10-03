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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/polygon-offset.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <volk.h>
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>

#include "blend-constants-cache.h"
#include "device-selection.h"
#include "display-output-state.h"
#include "debug.h"
#include "constants.h"
#include "glsl.h"
#include "hybrid-compiler.h"
#include "hybrid-family-history.h"
#include "hybrid-pipeline-builder.h"
#include "hybrid-prewarm.h"
#include "hybrid-trace.h"
#include "hybrid-policy.h"
#include "spirv-prewarm.h"
#include "ubershader-controls.h"

#define HAVE_EXTERNAL_MEMORY 1
#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2

typedef struct QueueFamilyIndices {
    int queue_family;
} QueueFamilyIndices;

typedef struct MemorySyncRequirement {
    hwaddr addr, size;
    bool surface_overlap;
} MemorySyncRequirement;

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
} RenderPassState;

typedef struct RenderPass {
    RenderPassState state;
    VkRenderPass render_pass;
} RenderPass;

typedef enum PGRAPHVkFragmentRoute {
    PGRAPH_VK_FRAGMENT_SPECIALIZED,
    PGRAPH_VK_FRAGMENT_UBERSHADER,
} PGRAPHVkFragmentRoute;

#define PGRAPH_VK_PSH_UBER_UBO_BINDING 6
#define PGRAPH_VK_BASE_DESCRIPTOR_BINDING_COUNT (2 + NV2A_MAX_TEXTURES)

/*
 * The fragment interpreter consumes these words from its dynamic UBO.  The
 * remaining PshState fields describe the generated fragment shell and must
 * continue to participate in shader and pipeline cache identity.
 */
static inline void pgraph_vk_canonicalize_uber_combiner_state(PshState *state)
{
    state->combiner_control = 0;
    memset(state->rgb_inputs, 0, sizeof(state->rgb_inputs));
    memset(state->rgb_outputs, 0, sizeof(state->rgb_outputs));
    memset(state->alpha_inputs, 0, sizeof(state->alpha_inputs));
    memset(state->alpha_outputs, 0, sizeof(state->alpha_outputs));
    state->final_inputs_0 = 0;
    state->final_inputs_1 = 0;
}

typedef struct PipelineKey {
    bool clear;
    PGRAPHVkFragmentRoute fragment_route;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    uint32_t regs[8];
    uint32_t binding_description_count;
    uint32_t attribute_description_count;
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

typedef enum PGRAPHVkFamilyLearnState {
    PGRAPH_VK_FAMILY_UNCHECKED,
    PGRAPH_VK_FAMILY_RETRY_PENDING,
    PGRAPH_VK_FAMILY_TRACKED,
    PGRAPH_VK_FAMILY_READY,
    PGRAPH_VK_FAMILY_REJECTED,
} PGRAPHVkFamilyLearnState;

typedef enum PGRAPHVkFallbackFamilyStatus {
    PGRAPH_VK_FAMILY_WAITING_FOR_SHADER,
    PGRAPH_VK_FAMILY_QUEUE_DEFERRED,
    PGRAPH_VK_FAMILY_PIPELINE_PENDING,
    PGRAPH_VK_FAMILY_PIPELINE_RETRY_BACKOFF,
    PGRAPH_VK_FAMILY_REQUEST_READY,
    PGRAPH_VK_FAMILY_REQUEST_REJECTED,
} PGRAPHVkFallbackFamilyStatus;

typedef enum PGRAPHVkFallbackShaderPreparation {
    PGRAPH_VK_FALLBACK_SHADER_REJECTED,
    PGRAPH_VK_FALLBACK_SHADER_WAITING,
    PGRAPH_VK_FALLBACK_SHADER_READY,
} PGRAPHVkFallbackShaderPreparation;

typedef struct PipelineBinding {
    LruNode node;
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkRenderPass render_pass;
    unsigned int draw_time;
    bool prewarmed;
    bool has_dynamic_line_width;
    uint32_t dynamic_blend_constant_mask;
    PGRAPHVkFamilyLearnState family_learn_state;
} PipelineBinding;

#define PGRAPH_VK_HYBRID_MAX_PIPELINE_JOBS 16
#define PGRAPH_VK_HYBRID_MAX_FALLBACK_FAMILIES 16

typedef struct PGRAPHVkFallbackFamilyRequest {
    bool in_use;
    bool from_prewarm;
    PGRAPHVkHybridPriority priority;
    ShaderState state;
    PipelineKey key;
    PGRAPHVkFallbackFamilyStatus status;
    unsigned int attempts;
    int64_t retry_after_us;
} PGRAPHVkFallbackFamilyRequest;

typedef struct PGRAPHVkHybridPipelineWork {
    bool in_use;
    bool prewarm;
    PGRAPHVkHybridPriority priority;
    uint64_t generation;
    uint64_t ticket;
    uint64_t key_hash;
    PipelineKey key;
    /* A completed result waits here if every LRU entry is still in use. */
    VkPipeline completed_pipeline;
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    uint32_t dynamic_blend_constant_mask;
    bool has_dynamic_line_width;
    struct ShaderModuleInfo *modules[3];
} PGRAPHVkHybridPipelineWork;

enum Buffer {
    BUFFER_STAGING_DST,
    BUFFER_STAGING_SRC,
    BUFFER_TEXTURE_STAGING,
    BUFFER_COMPUTE_DST,
    BUFFER_COMPUTE_SRC,
    BUFFER_INDEX,
    BUFFER_INDEX_STAGING,
    BUFFER_VERTEX_RAM,
    BUFFER_VERTEX_RAM_STAGING,
    BUFFER_VERTEX_INLINE,
    BUFFER_VERTEX_INLINE_STAGING,
    BUFFER_UNIFORM,
    BUFFER_UNIFORM_STAGING,
    BUFFER_COUNT
};

typedef struct StorageBuffer {
    VkBuffer buffer;
    VkBufferUsageFlags usage;
    VmaAllocationCreateInfo alloc_info;
    VmaAllocation allocation;
    VkMemoryPropertyFlags properties;
    size_t buffer_offset;
    size_t buffer_size;
    uint8_t *mapped;
} StorageBuffer;

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    struct MemAccessCallback *access_cb;
    NV2AState *d;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;
    /* An observed guest write makes older GPU contents ineligible for RAM
     * readback until a successful upload or logical binding replacement. */
    bool readback_superseded_by_guest;

    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    VkImage image;
    VkImageView image_view;
    VmaAllocation allocation;

    // Used for scaling
    VkImage image_scratch;
    VkImageLayout image_scratch_current_layout;
    VmaAllocation allocation_scratch;

    bool initialized;

    /* Identifies this logical binding even when its allocation is recycled. */
    uint64_t lifetime_id;
} SurfaceBinding;

typedef struct ShaderModuleInfo {
    int refcnt;
    char *glsl;
    GByteArray *spirv;
    VkShaderModule module;
    SpvReflectShaderModule reflect_module;
    bool reflect_module_initialized;
    SpvReflectDescriptorSet **descriptor_sets;
    ShaderUniformLayout uniforms;
    ShaderUniformLayout push_constants;
    bool uses_uber_controls;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
    PGRAPHVkFragmentRoute fragment_route;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

static inline size_t pgraph_vk_shader_module_key_active_size(
    const ShaderModuleCacheKey *key)
{
    switch (key->kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        return offsetof(ShaderModuleCacheKey, vsh) + sizeof(key->vsh);
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return offsetof(ShaderModuleCacheKey, geom) + sizeof(key->geom);
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return offsetof(ShaderModuleCacheKey, psh) + sizeof(key->psh);
    default:
        g_assert_not_reached();
    }
}

static inline bool pgraph_vk_shader_module_key_equal(
    const ShaderModuleCacheKey *a, const ShaderModuleCacheKey *b)
{
    return a->kind == b->kind &&
           memcmp(a, b, pgraph_vk_shader_module_key_active_size(a)) == 0;
}

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
} ShaderModuleCacheEntry;

#define PGRAPH_VK_HYBRID_MAX_WORK 64

typedef struct PGRAPHVkHybridShaderWork {
    bool in_use;
    bool prewarm;
    PGRAPHVkHybridPriority priority;
    uint64_t last_epoch;
    int64_t retry_after_us;
    PGRAPHVkHybridWork metadata;
    ShaderModuleCacheKey module_key;
    char *glsl;
    /* PR70/cache identity length; glsl[glsl_size] is the owned NUL. */
    size_t glsl_size;
    GByteArray *completed_spirv;
    uint32_t completed_stage;
} PGRAPHVkHybridShaderWork;

typedef enum PGRAPHVkAsyncModuleRequestResult {
    PGRAPH_VK_ASYNC_MODULE_READY,
    PGRAPH_VK_ASYNC_MODULE_ACCEPTED,
    PGRAPH_VK_ASYNC_MODULE_DUPLICATE,
    PGRAPH_VK_ASYNC_MODULE_DEFERRED,
    PGRAPH_VK_ASYNC_MODULE_FAILED,
} PGRAPHVkAsyncModuleRequestResult;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
    PGRAPHVkFragmentRoute fragment_route;
    int64_t next_promotion_probe_us;
    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
    } geom;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
    } psh;
} ShaderBinding;

typedef struct ShaderBindingKey {
    ShaderState state;
    PGRAPHVkFragmentRoute fragment_route;
} ShaderBindingKey;

typedef struct PGRAPHVkShaderPreparation {
    ShaderState state;
    bool shader_state_dirty;
    bool bound_state_equal;
    bool selection_changed;
} PGRAPHVkShaderPreparation;

typedef struct PGRAPHVkReadyDrawCandidate {
    ShaderBinding *shader;
    PipelineBinding *pipeline;
    PipelineKey key;
    uint64_t hash;
} PGRAPHVkReadyDrawCandidate;

typedef struct PGRAPHVkReadyExecutionCandidates {
    PGRAPHVkReadyDrawCandidate specialized;
    PGRAPHVkReadyDrawCandidate fallback;
    PGRAPHUberControls controls;
    bool controls_checked;
    bool controls_supported;
} PGRAPHVkReadyExecutionCandidates;

static inline bool pgraph_vk_shader_binding_key_equal(
    const ShaderBindingKey *a, const ShaderBindingKey *b)
{
    return a->fragment_route == b->fragment_route &&
           memcmp(&a->state, &b->state, sizeof(a->state)) == 0;
}

static inline bool pgraph_vk_shader_binding_key_different(
    const ShaderBindingKey *a, const ShaderBindingKey *b)
{
    return !pgraph_vk_shader_binding_key_equal(a, b);
}

static inline uint32_t pgraph_vk_descriptor_layout_binding_count(
    bool runtime_enabled)
{
    return PGRAPH_VK_BASE_DESCRIPTOR_BINDING_COUNT + runtime_enabled;
}

static inline uint32_t pgraph_vk_descriptor_dynamic_offset_count(
    bool runtime_enabled)
{
    return runtime_enabled;
}

static inline bool pgraph_vk_descriptor_update_needed(bool textures_changed,
                                                       bool force_reupload,
                                                       bool uniforms_changed)
{
    return textures_changed || force_reupload || uniforms_changed;
}

static inline bool pgraph_vk_reuses_descriptor_set_for_control_update(
    bool controls_changed, bool descriptor_update_needed)
{
    return controls_changed && !descriptor_update_needed;
}

typedef struct TextureKey {
    TextureShape state;
    VkFormat vk_format;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    VkImage image;
    VkImageLayout current_layout;
    VkImageView image_view;
    VmaAllocation allocation;
    VkSampler sampler;
    bool possibly_dirty;
    uint64_t hash;
    unsigned int draw_time;
    uint32_t submit_time;
} TextureBinding;

#define NV2A_VK_NATIVE_BC_FORMAT_COUNT 3

typedef struct NativeBCFormatSupport {
    VkFormatProperties format_properties;
    VkImageFormatProperties image_properties[2];
    bool image_supported[2];
} NativeBCFormatSupport;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    hwaddr dma_report;
    uint32_t parameter;
    unsigned int query_count;
    uint64_t enqueue_frame;
} QueryReport;

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

typedef struct PGRAPHVkDisplayState {
    ShaderModuleInfo *display_frag;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_set;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkRenderPass render_pass;
    VkFramebuffer framebuffer;

    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkSampler sampler;

    struct {
        PvideoState state;
        int width, height;
        VkImage image;
        VkImageView image_view;
        VmaAllocation allocation;
        VkSampler sampler;
    } pvideo;

    int width, height;
    int draw_time;
    bool shared_presentation;
    bool presentation_reported;
    uint64_t completed_output_generation;

    struct {
        VkBuffer buffer;
        VmaAllocation allocation;
        void *mapped;
        size_t size;
        GLuint gl_texture_id;
        PGRAPHVkHostCopyUploadState upload;
    } host_copy;

    struct {
        bool valid;
        uint64_t surface_lifetime_id;
        int surface_draw_time;
        int guest_frame_time;
        hwaddr scanout_address;
        uint32_t vga_line_offset;
        uint32_t display_width;
        uint32_t display_height;
        uint32_t surface_scale_factor;
        uint8_t interlace_mode;
    } reuse;

    // OpenGL Interop
#ifdef WIN32
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_memory_obj;
    GLuint gl_texture_id;
} PGRAPHVkDisplayState;

typedef struct ComputePipelineKey {
    VkFormat host_fmt;
    bool pack;
    int workgroup_size;
} ComputePipelineKey;

typedef struct ComputePipeline {
    LruNode node;
    ComputePipelineKey key;
    VkPipeline pipeline;
} ComputePipeline;

typedef struct PGRAPHVkComputeState {
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1024];
    int descriptor_set_index;
    VkPipelineLayout pipeline_layout;
    Lru pipeline_cache;
    ComputePipeline *pipeline_cache_entries;
} PGRAPHVkComputeState;

typedef enum FinishReason {
    VK_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    VK_FINISH_REASON_SURFACE_CREATE,
    VK_FINISH_REASON_SURFACE_DOWN,
    VK_FINISH_REASON_NEED_BUFFER_SPACE,
    VK_FINISH_REASON_FRAMEBUFFER_DIRTY,
    VK_FINISH_REASON_PRESENTING,
    VK_FINISH_REASON_FLIP_STALL,
    VK_FINISH_REASON_FLUSH,
    VK_FINISH_REASON_STALLED,
    VK_FINISH_REASON_TEXTURE_DIRTY,
    VK_FINISH_REASON_COUNT,
} FinishReason;

typedef enum SingleTimeReason {
    VK_SINGLE_TIME_PVIDEO_UPLOAD,
    VK_SINGLE_TIME_DISPLAY_RENDER,
    VK_SINGLE_TIME_SURFACE_DOWNLOAD,
    VK_SINGLE_TIME_SURFACE_CREATE,
    VK_SINGLE_TIME_SURFACE_UPLOAD,
    VK_SINGLE_TIME_TEXTURE_UPLOAD,
    VK_SINGLE_TIME_DUMMY_TEXTURE_CREATE,
    VK_SINGLE_TIME_REASON_COUNT,
} SingleTimeReason;

typedef struct PGRAPHVkWaitStats {
    uint64_t call_count;
    uint64_t submit_count;
    uint64_t timed_submit_count;
    uint64_t submit_cpu_us;
    uint64_t wait_count;
    uint64_t wait_us;
} PGRAPHVkWaitStats;

typedef enum PerfCpuRegion {
    VK_PERF_CPU_DRAW_BEGIN_SURFACE_UPDATE,
    VK_PERF_CPU_DRAW_FLUSH,
    VK_PERF_CPU_PIPELINE_PREPARE,
    VK_PERF_CPU_BIND_TEXTURES,
    VK_PERF_CPU_TEXTURE_UPLOAD,
    VK_PERF_CPU_UPDATE_DESCRIPTOR_SETS,
    VK_PERF_CPU_REGION_COUNT,
} PerfCpuRegion;

typedef struct PGRAPHVkCpuStats {
    uint64_t call_count;
    uint64_t cpu_us;
} PGRAPHVkCpuStats;

typedef struct PGRAPHVkPerfTelemetry {
    FILE *file;
    bool enabled;
    uint64_t frame;
    int64_t last_flush_us;
    PGRAPHVkWaitStats finish[VK_FINISH_REASON_COUNT];
    PGRAPHVkWaitStats single_time[VK_SINGLE_TIME_REASON_COUNT];
    PGRAPHVkCpuStats cpu_regions[VK_PERF_CPU_REGION_COUNT];
    uint64_t submit_info_count;
    uint64_t command_buffer_count;
    uint64_t staged_bytes;
    uint64_t vertex_staged_bytes;
    uint64_t vertex_staging_copy_count;
    uint64_t vertex_direct_bytes;
    uint64_t vertex_direct_copy_count;
    uint64_t vertex_version_draw_count;
    uint64_t vertex_version_bytes;
    uint64_t vertex_version_selected_ranges;
    uint64_t vertex_staging_capacity_growth_count;
    uint64_t vertex_staging_fallback_finish_count;
    uint64_t native_bc_upload_count;
    uint64_t native_bc_source_bytes;
    uint64_t native_bc_staged_bytes;
    uint64_t native_bc_prepare_cpu_us;
    uint64_t decoded_bc_upload_count;
    uint64_t decoded_bc_source_bytes;
    uint64_t decoded_bc_staged_bytes;
    uint64_t decoded_bc_prepare_cpu_us;
    uint64_t descriptor_update_calls;
    uint64_t descriptor_reuse_returns;
    uint64_t descriptor_set_writes;
    uint64_t descriptor_control_only_reuses;
    uint64_t descriptor_texture_change_requests;
    uint64_t descriptor_force_reupload_requests;
    uint64_t descriptor_uniform_write_requests;
    uint64_t descriptor_capacity_requests;
    uint64_t uniform_capacity_requests;
    uint64_t uniform_stage_writes[PGRAPH_UNIFORM_STAGE_COUNT];
    uint64_t surface_upload_attempts;
    uint64_t surface_upload_color_attempts;
    uint64_t surface_upload_depth_attempts;
    uint64_t surface_upload_force_attempts;
    uint64_t surface_upload_requested_bytes;
    uint64_t surface_upload_new_causes;
    uint64_t surface_upload_guest_write_causes;
    uint64_t surface_upload_dirty_memory_causes;
    uint64_t surface_upload_overlap_guest_write_causes;
    uint64_t in_flight_submission_count;
    uint64_t peak_in_flight_submission_count;
    uint64_t oldest_in_flight_serial;
    uint64_t newest_submitted_serial;
    uint64_t submission_serial;
    uint64_t retirement_queue_objects;
    uint64_t retirement_queue_bytes;
    uint64_t report_entries_enqueued;
    uint64_t report_clears_enqueued;
    uint64_t report_max_queue_depth;
    uint64_t report_stalled_finish_calls;
    uint64_t report_retirements;
    uint64_t report_write_attempts;
    uint64_t report_query_result_calls;
    uint64_t report_query_results_waited;
    uint64_t report_query_result_wait_us;
    uint64_t report_cpu_only_retirements;
    uint64_t report_enqueue_to_retire_frames_total;
    uint64_t report_enqueue_to_retire_frames_max;
    /* Cumulative UI-thread totals, read by the renderer-thread perf writer. */
    uint64_t framebuffer_acquire_calls_total QEMU_ALIGNED(8);
    uint64_t valid_sync_requests_total QEMU_ALIGNED(8);
    uint64_t host_copy_uploads_total QEMU_ALIGNED(8);
    uint64_t host_copy_upload_skips_total QEMU_ALIGNED(8);
    uint64_t host_copy_uploaded_bytes_total QEMU_ALIGNED(8);
} PGRAPHVkPerfTelemetry;

typedef struct PGRAPHVkState {
    uint32_t vk_api_version;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;
    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;
    bool demote_to_helper_extension_enabled;

    VkPhysicalDevice physical_device;
    PGRAPHVkDeviceRecord selected_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

    VkQueue queue;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[2];

    VkCommandBuffer command_buffer;
    VkSemaphore command_buffer_semaphore;
    VkFence command_buffer_fence;
    unsigned int command_buffer_start_time;
    bool in_command_buffer;
    uint32_t submit_count;
    PGRAPHVkBlendConstantsCache blend_constants;

    VkCommandBuffer aux_command_buffer;
    bool in_aux_command_buffer;

    PGRAPHVkPerfTelemetry perf;

    uint64_t next_surface_lifetime_id;

    VkFramebuffer framebuffers[50];
    int framebuffer_index;
    bool framebuffer_dirty;

    VkRenderPass render_pass;
    GArray *render_passes; // RenderPass
    bool in_render_pass;
    bool in_draw;

    Lru pipeline_cache;
    VkPipelineCache vk_pipeline_cache;
    char *pipeline_cache_path;
    PipelineBinding *pipeline_cache_entries;
    PipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;
    PGRAPHVkHybridPipelineBuilder hybrid_pipeline_builder;
    bool hybrid_pipeline_builder_initialized;
    uint64_t hybrid_pipeline_next_ticket;
    PGRAPHVkHybridPipelineWork
        hybrid_pipeline_work[PGRAPH_VK_HYBRID_MAX_PIPELINE_JOBS];

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1024];
    int descriptor_set_index;

    StorageBuffer storage_buffers[BUFFER_COUNT];

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    MemorySyncRequirement pending_vertex_ram_reads[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_pending_vertex_ram_reads;
    uint8_t *vertex_ram_read_pages;
    /* Pages left stale in the fixed mirror when a draw uses an inline slice. */
    uint8_t *vertex_ram_stale_pages;
    size_t vertex_ram_stale_page_count;
    /* Captures one bounded version before any pre-draw finish can wait. */
    uint8_t *vertex_version_scratch;
    size_t num_vertex_ram_read_pages;
    bool vertex_ram_read_tracking_active;
    bool vertex_ram_updated_in_batch;
    unsigned int vertex_ram_read_tracking_idle_batches;

    VkVertexInputAttributeDescription vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;

    VkVertexInputBindingDescription vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_binding_descriptions;
    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    unsigned long *surface_dirty_page_bits;
    size_t surface_dirty_page_words;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    bool downloads_succeeded;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    bool texture_binding_source_is_surface[NV2A_MAX_TEXTURES];
    TextureBinding dummy_texture;
    bool texture_descriptor_publication_pending;
    VkFormatProperties *texture_format_properties;
    NativeBCFormatSupport
        native_bc_format_support[NV2A_VK_NATIVE_BC_FORMAT_COUNT];

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    ShaderModuleInfo *quad_vert_module, *solid_frag_module;
    bool shader_bindings_changed;
    bool use_push_constants_for_uniform_attrs;
    bool ubershader_runtime_enabled;
    bool ubershader_force_interpreter;
    PGRAPHVkHybridPrewarmState hybrid_prewarm;
    bool hybrid_prewarm_service_pending;
    int hybrid_completion_kick_pending;
    QEMUTimer *hybrid_service_timer;
    int64_t hybrid_owner_service_deadline_us;
    bool hybrid_compiler_initialized;
    uint64_t hybrid_generation;
    uint64_t hybrid_route_epoch;
    uint64_t hybrid_selection_epoch;
    uint64_t hybrid_bound_selection_epoch;
    uint32_t uber_constant_regs[18];
    bool uber_constant_regs_valid;
    size_t hybrid_pending_jobs;
    PGRAPHVkHybridTicketAllocator hybrid_ticket_allocator;
    PGRAPHVkHybridCompiler hybrid_compiler;
    PGRAPHVkHybridTrace *hybrid_trace;
    PGRAPHVkHybridShaderWork
        hybrid_work[PGRAPH_VK_HYBRID_MAX_WORK];
    PGRAPHVkFallbackFamilyRequest fallback_family_requests[
        PGRAPH_VK_HYBRID_MAX_FALLBACK_FAMILIES];
    unsigned int fallback_family_cursor;
    unsigned int fallback_family_pipeline_cursor;
    size_t fallback_family_retry_count;
    const ShaderModuleCacheKey *hybrid_materializing_key;
    ShaderModuleInfo *hybrid_materialized_module_info;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;
    PGRAPHVkSpirvCache spirv_cache;
    PGRAPHVkFamilyHistory fallback_family_history;
    char *spirv_cache_directory;
    char *spirv_cache_path;
    char *fallback_family_history_path;
    bool spirv_cache_initialized;
    bool fallback_family_history_initialized;
    bool spirv_cache_session_eligible;
    bool spirv_cache_writeback_pending;
    bool spirv_cache_writeback_requested;
    bool spirv_cache_writeback_complete_initialized;
    QemuEvent spirv_cache_writeback_complete;

    // FIXME: Merge these into a structure
    size_t uniform_buffer_offsets[2];
    bool uniforms_changed;
    bool uniform_stage_dirty[PGRAPH_UNIFORM_STAGE_COUNT];
    bool uniform_layout_changed[PGRAPH_UNIFORM_STAGE_COUNT];
    PGRAPHUniformSourceEpochs last_uniform_source_epochs;
    bool polygon_offset_key_valid;
    PGRAPHPolygonOffsetUniformKey polygon_offset_key;
    PGRAPHUberControls uber_controls;
    PGRAPHUberControls uploaded_uber_controls;
    VkDeviceSize uber_control_offset;
    bool uber_controls_valid;
    bool uploaded_uber_controls_valid;

    VkQueryPool query_pool;
    int max_queries_in_flight; // FIXME: Move out to constant
    int num_queries_in_flight;
    bool new_query_needed;
    bool query_in_flight;
    uint32_t zpass_pixel_count_result;
    uint32_t report_queue_depth;
    QSIMPLEQ_HEAD(, QueryReport) report_queue; // FIXME: Statically allocate

    SurfaceFormatInfo kelvin_surface_zeta_vk_map[3];

    uint32_t clear_parameter;

    PGRAPHVkDisplayState display;
    PGRAPHVkComputeState compute;
} PGRAPHVkState;

// renderer.c
void pgraph_vk_check_memory_budget(PGRAPHState *pg);

// debug.c
#define RGBA_RED     (float[4]){1,0,0,1}
#define RGBA_YELLOW  (float[4]){1,1,0,1}
#define RGBA_GREEN   (float[4]){0,1,0,1}
#define RGBA_BLUE    (float[4]){0,0,1,1}
#define RGBA_PINK    (float[4]){1,0,1,1}
#define RGBA_DEFAULT (float[4]){0,0,0,0}

void pgraph_vk_debug_init(void);
void pgraph_vk_insert_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                   float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_end_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd);

// instance.c
void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_vk_finalize_instance(PGRAPHState *pg);
QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device);
uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties);

// glsl.c
typedef struct PGRAPHVkGlslCompileConfig {
    uint32_t api_version;
    /* Fixed-width fields keep bytewise worker-job identity deterministic. */
    uint32_t debug_shaders;
} PGRAPHVkGlslCompileConfig;

void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);
void pgraph_vk_glsl_target_versions(
    uint32_t api_version, glslang_target_client_version_t *client_version,
    glslang_target_language_version_t *language_version);
GByteArray *pgraph_vk_compile_glsl_to_spv(PGRAPHVkState *r,
                                          glslang_stage_t stage,
                                          const char *glsl_source);
GByteArray *pgraph_vk_compile_glsl_to_spv_config(
    const PGRAPHVkGlslCompileConfig *config, glslang_stage_t stage,
    const char *glsl_source);
bool pgraph_vk_uber_controls_block_matches_abi(
    const SpvReflectBlockVariable *block);
bool pgraph_vk_init_shader_module_layout_from_spv(
    ShaderModuleInfo *info, VkShaderStageFlagBits expected_stage);
void pgraph_vk_clear_shader_module_layout(ShaderModuleInfo *info);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_spirv(
    PGRAPHVkState *r, VkShaderStageFlagBits expected_stage, const char *glsl,
    GByteArray *spirv);
void pgraph_vk_ref_shader_module(ShaderModuleInfo *info);
void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);
void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);

// buffer.c
void pgraph_vk_init_buffers(NV2AState *d);
void pgraph_vk_clear_vertex_ram_stale(PGRAPHVkState *r);
void pgraph_vk_finalize_buffers(NV2AState *d);
bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment);
bool pgraph_vk_grow_vertex_ram_staging_buffer(PGRAPHState *pg,
                                               VkDeviceSize required_size);
VkDeviceSize pgraph_vk_buffer_required_size(PGRAPHState *pg, int index,
                                            VkDeviceSize size,
                                            VkDeviceAddress alignment);
void pgraph_vk_ensure_buffer_capacity(PGRAPHState *pg, int index,
                                      VkDeviceSize required_size);
void pgraph_vk_ensure_buffer_pair_capacity(PGRAPHState *pg, int index,
                                           size_t required_size);
VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment);

// command.c
void pgraph_vk_init_command_buffers(PGRAPHState *pg);
void pgraph_vk_finalize_command_buffers(PGRAPHState *pg);
VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg);
void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd,
                                        SingleTimeReason reason,
                                        uint64_t staged_bytes);

// perf.c
void pgraph_vk_perf_init(PGRAPHVkState *r);
void pgraph_vk_perf_finalize(PGRAPHVkState *r);
void pgraph_vk_perf_record_finish_call(PGRAPHVkState *r, FinishReason reason);
bool pgraph_vk_perf_should_time_finish(PGRAPHVkState *r, FinishReason reason);
void pgraph_vk_perf_record_finish_submit(PGRAPHVkState *r,
                                         FinishReason reason,
                                         bool timed,
                                         uint64_t submit_cpu_us,
                                         uint64_t wait_us,
                                         uint64_t staged_bytes,
                                         uint64_t submit_info_count,
                                         uint64_t command_buffer_count);
void pgraph_vk_perf_record_single_time_submit(PGRAPHVkState *r,
                                               SingleTimeReason reason,
                                               uint64_t submit_cpu_us,
                                               uint64_t wait_us,
                                               uint64_t staged_bytes);
void pgraph_vk_perf_record_vertex_staging_copy(PGRAPHVkState *r,
                                                uint64_t bytes);
void pgraph_vk_perf_record_vertex_direct_copy(PGRAPHVkState *r,
                                               uint64_t bytes);
void pgraph_vk_perf_record_vertex_staging_growth(PGRAPHVkState *r);
void pgraph_vk_perf_record_vertex_staging_fallback(PGRAPHVkState *r);
void pgraph_vk_perf_record_bc_upload(PGRAPHVkState *r, bool native,
                                     uint64_t source_bytes,
                                     uint64_t staged_bytes,
                                     uint64_t prepare_cpu_us);
void pgraph_vk_perf_record_cpu_region(PGRAPHVkState *r, PerfCpuRegion region,
                                      uint64_t cpu_us);
void pgraph_vk_perf_record_framebuffer_acquire(PGRAPHVkState *r);
void pgraph_vk_perf_record_valid_sync_request(PGRAPHVkState *r);
void pgraph_vk_perf_record_host_copy_result(PGRAPHVkState *r, bool skipped,
                                            uint64_t uploaded_bytes);
void pgraph_vk_perf_frame(PGRAPHVkState *r);

// image.c
void pgraph_vk_transition_image_layout(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkImage image, VkFormat format,
                                       VkImageLayout oldLayout,
                                       VkImageLayout newLayout);

// vertex.c
bool pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element);
/* Non-inline vertex draws must validate/bind their fetch ranges and complete
 * vertex-backing preparation before decoding values from CPU-visible VRAM. */
void pgraph_vk_refresh_vertex_inline_values_after_sync(
    PGRAPHState *pg, unsigned int provoking_element);
void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset, void *data,
                                        VkDeviceSize size);
void pgraph_vk_update_vertex_ram_buffer_after_surface_readback(
    PGRAPHState *pg, hwaddr offset, void *data, VkDeviceSize size);
void pgraph_vk_update_unread_vertex_ram_buffer_after_surface_readback(
    PGRAPHState *pg, hwaddr offset, void *data, VkDeviceSize size);
VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count);

// surface.c
void pgraph_vk_init_surfaces(PGRAPHState *pg);
void pgraph_vk_finalize_surfaces(PGRAPHState *pg);
void pgraph_vk_surface_flush(NV2AState *d);
void pgraph_vk_process_pending_downloads(NV2AState *d);
bool pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr);
bool pgraph_vk_wait_for_surface_download(SurfaceBinding *e);
void pgraph_vk_download_dirty_surfaces(NV2AState *d);
bool pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start,
                                                    hwaddr size);
bool pgraph_vk_surface_overlaps_range(PGRAPHState *pg, hwaddr start,
                                      hwaddr size);
bool pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force);
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write);
SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr);
void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d);
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg);

// surface-compute.c
void pgraph_vk_init_compute(PGRAPHState *pg);
bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r);
void pgraph_vk_compute_finish_complete(PGRAPHVkState *r);
void pgraph_vk_finalize_compute(PGRAPHState *pg);
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale);
void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst);

// display.c
void pgraph_vk_init_display(PGRAPHState *pg);
void pgraph_vk_finalize_display(PGRAPHState *pg);
void pgraph_vk_render_display(PGRAPHState *pg);

// texture.c
void pgraph_vk_init_textures(PGRAPHState *pg);
void pgraph_vk_finalize_textures(PGRAPHState *pg);
bool pgraph_vk_bind_textures(NV2AState *d);
void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                            hwaddr size);
void pgraph_vk_trim_texture_cache(PGRAPHState *pg);

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_process_hybrid_completions(PGRAPHState *pg);
PGRAPHVkAsyncModuleRequestResult pgraph_vk_request_shader_module_async(
    PGRAPHState *pg, const ShaderModuleCacheKey *key);
void pgraph_vk_stop_hybrid_compiler(PGRAPHState *pg);
void pgraph_vk_process_spirv_cache_writeback(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
bool pgraph_vk_pack_fallback_controls(PGRAPHState *pg,
                                     const PshState *state,
                                     PGRAPHUberControls *packet);
void pgraph_vk_publish_fallback_controls(PGRAPHVkState *r,
                                         const PGRAPHUberControls *packet);
bool pgraph_vk_refresh_fallback_controls(PGRAPHState *pg,
                                         const PshState *state);
typedef enum PGRAPHVkFallbackResourceState {
    PGRAPH_VK_FALLBACK_RESOURCES_READY,
    PGRAPH_VK_FALLBACK_RESOURCES_NEED_ROLLOVER,
    PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE,
} PGRAPHVkFallbackResourceState;
PGRAPHVkFallbackResourceState pgraph_vk_fallback_draw_resource_state(
    PGRAPHState *pg, ShaderBinding *binding);
void pgraph_vk_enqueue_specialized_fragment(PGRAPHState *pg,
                                            const ShaderState *state,
                                            bool fallback_pipeline_ready,
                                            bool fallback_resources_ready);
bool pgraph_vk_enqueue_fallback_fragment(PGRAPHState *pg,
                                        const ShaderState *state);
PGRAPHVkCachedFamilyModulesResult
pgraph_vk_request_fallback_family_modules(PGRAPHState *pg,
                                           const ShaderState *state);
PGRAPHVkCachedFamilyModulesResult
pgraph_vk_request_fallback_family_modules_priority(
    PGRAPHState *pg, const ShaderState *state,
    PGRAPHVkHybridPriority priority);
PGRAPHVkCachedFamilyModulesResult
pgraph_vk_materialize_cached_family_modules(PGRAPHState *pg,
                                             const ShaderState *state);
void pgraph_vk_process_fallback_families(PGRAPHState *pg);
void pgraph_vk_process_hybrid_prewarm(PGRAPHState *pg);
void pgraph_vk_hybrid_worker_notify(void *opaque);
void pgraph_vk_hybrid_schedule_service(PGRAPHState *pg,
                                       int64_t deadline_us);

// hybrid-family.c
void pgraph_vk_resolve_ready_execution_candidates(
    PGRAPHState *pg, const ShaderState *state, bool force_ubershader,
    PGRAPHVkReadyExecutionCandidates *candidates);
void pgraph_vk_init_pipeline_key_for_state(
    PGRAPHState *pg, const ShaderState *shader_state,
    PGRAPHVkFragmentRoute route, PipelineKey *key);
void pgraph_vk_pipeline_family_set_state(
    PGRAPHVkState *r, PipelineBinding *binding,
    PGRAPHVkFamilyLearnState state);
void pgraph_vk_track_specialized_fallback_family(
    PGRAPHVkState *r, PipelineBinding *owner,
    bool controls_supported, bool fallback_pipeline_ready,
    uint64_t synchronous_create_us);
void pgraph_vk_note_interpreter_family(PGRAPHVkState *r,
                                      const PipelineKey *key,
                                      uint64_t synchronous_create_us);
void pgraph_vk_enqueue_retained_fallback_families(PGRAPHVkState *r);
void pgraph_vk_fallback_family_note_pipeline_ready(
    PGRAPHVkState *r, const PipelineKey *key);
void pgraph_vk_fallback_family_note_pipeline_failure_at(
    PGRAPHVkState *r, const PipelineKey *key, int64_t now_us);
void pgraph_vk_pipeline_family_owner_evict(
    PGRAPHVkState *r, PipelineBinding *binding);
void pgraph_vk_fallback_family_key_from_specialized(
    const PipelineBinding *binding, PipelineKey *key);
void pgraph_vk_fallback_family_mark_pipeline_owners(
    PGRAPHVkState *r, const PipelineKey *family_key,
    PGRAPHVkFamilyLearnState state);
void pgraph_vk_fallback_family_finish_request(
    PGRAPHVkState *r, PGRAPHVkFallbackFamilyRequest *request,
    PGRAPHVkFamilyLearnState state);
typedef ShaderBinding *(*PGRAPHVkFallbackBindingProbeFunc)(void *opaque);
typedef bool (*PGRAPHVkFallbackFragmentPrepareFunc)(void *opaque);
PGRAPHVkFallbackShaderPreparation
pgraph_vk_fallback_family_prepare_shader(
    void *opaque, PGRAPHVkFallbackBindingProbeFunc probe_binding,
    PGRAPHVkFallbackFragmentPrepareFunc prepare_fragment,
    ShaderBinding **binding);
ShaderBinding *pgraph_vk_prepare_binding_from_ready_modules(
    PGRAPHState *pg, const ShaderState *state, PGRAPHVkFragmentRoute route);
void pgraph_vk_prepare_shaders(PGRAPHState *pg,
                              PGRAPHVkShaderPreparation *preparation);
void pgraph_vk_activate_shaders(PGRAPHState *pg,
                               const PGRAPHVkShaderPreparation *preparation,
                               PGRAPHVkFragmentRoute route,
                               ShaderBinding *ready_binding);
void pgraph_vk_bind_shaders(PGRAPHState *pg);

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_process_pending_reports_internal(NV2AState *d);

// draw.c
void pgraph_vk_init_pipelines(PGRAPHState *pg);
void pgraph_vk_finalize_pipelines(PGRAPHState *pg);
void pgraph_vk_writeback_pipeline_cache(PGRAPHState *pg);
PGRAPHVkHybridPipelineSubmitResult pgraph_vk_request_hybrid_pipeline(
    PGRAPHState *pg, const PipelineKey *key, ShaderBinding *ready_binding);
void pgraph_vk_process_hybrid_pipeline_completions(PGRAPHState *pg);
void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_vk_draw_begin(NV2AState *d);
void pgraph_vk_draw_end(NV2AState *d);
void pgraph_vk_finish(PGRAPHState *pg, FinishReason why);
void pgraph_vk_flush_draw(NV2AState *d);
void pgraph_vk_invalidate_blend_constants(PGRAPHState *pg);
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

// gpuprops.c
void pgraph_vk_determine_gpu_properties(NV2AState *d);
GPUProperties *pgraph_vk_get_gpu_properties(void);

#endif

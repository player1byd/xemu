/*
 * Bounded worker for immutable Vulkan graphics-pipeline recipes
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_PIPELINE_BUILDER_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_PIPELINE_BUILDER_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "hw/xbox/nv2a/pgraph/vk/hybrid-compiler.h"

typedef struct PGRAPHVkHybridPipelineBuildRequest {
    uint64_t generation;
    uint64_t ticket;
    uint64_t key_hash;
    PGRAPHVkHybridPriority priority;
    VkDevice device;
    VkPipelineCache cache;
    const VkGraphicsPipelineCreateInfo *create_info;
} PGRAPHVkHybridPipelineBuildRequest;

typedef VkResult (*PGRAPHVkHybridPipelineCreateFunc)(
    void *opaque, VkDevice device, VkPipelineCache cache,
    const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline);
typedef void (*PGRAPHVkHybridPipelineDestroyFunc)(
    void *opaque, VkDevice device, VkPipeline pipeline);

typedef struct PGRAPHVkHybridPipelineBuildResult {
    uint64_t generation;
    uint64_t ticket;
    uint64_t key_hash;
    PGRAPHVkHybridPriority priority;
    VkDevice device;
    VkPipeline pipeline;
    VkResult vk_result;
    uint64_t submitted_us;
    uint64_t started_us;
    uint64_t finished_us;
    /* Destruction remains possible after the builder has been joined and
     * destroyed. The caller retains destroy_opaque until result disposal. */
    PGRAPHVkHybridPipelineDestroyFunc destroy;
    void *destroy_opaque;
} PGRAPHVkHybridPipelineBuildResult;

typedef struct PGRAPHVkHybridPipelineBuilderConfig {
    size_t max_jobs; /* Includes queued, active, and untaken results. */
    PGRAPHVkHybridPipelineCreateFunc create;
    PGRAPHVkHybridPipelineDestroyFunc destroy;
    void *opaque;
    PGRAPHVkHybridCompletionNotifyFunc notify;
    void *notify_opaque;
} PGRAPHVkHybridPipelineBuilderConfig;

typedef enum PGRAPHVkHybridPipelineSubmitResult {
    PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED,
    PGRAPH_VK_HYBRID_PIPELINE_QUEUE_FULL,
    PGRAPH_VK_HYBRID_PIPELINE_STOPPED,
    PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE,
} PGRAPHVkHybridPipelineSubmitResult;

typedef struct PGRAPHVkHybridPipelineBuilder {
    void *state;
} PGRAPHVkHybridPipelineBuilder;

/* The caller must retain VkShaderModule, VkRenderPass, VkPipelineLayout, and
 * VkPipelineCache objects referenced by accepted jobs until join or result
 * handoff. Only the VkGraphicsPipelineCreateInfo recipe is deep-copied. */
bool pgraph_vk_hybrid_pipeline_builder_init(
    PGRAPHVkHybridPipelineBuilder *builder,
    const PGRAPHVkHybridPipelineBuilderConfig *config);
PGRAPHVkHybridPipelineSubmitResult pgraph_vk_hybrid_pipeline_builder_submit(
    PGRAPHVkHybridPipelineBuilder *builder,
    const PGRAPHVkHybridPipelineBuildRequest *request);
bool pgraph_vk_hybrid_pipeline_builder_promote(
    PGRAPHVkHybridPipelineBuilder *builder, uint64_t generation,
    uint64_t ticket, PGRAPHVkHybridPriority priority);
bool pgraph_vk_hybrid_pipeline_builder_take_result(
    PGRAPHVkHybridPipelineBuilder *builder,
    PGRAPHVkHybridPipelineBuildResult *result);
bool pgraph_vk_hybrid_pipeline_builder_take_result_for(
    PGRAPHVkHybridPipelineBuilder *builder, uint64_t generation,
    uint64_t ticket, PGRAPHVkHybridPipelineBuildResult *result);
/* Lock-free empty-queue hint. A true result still requires take_result(). */
bool pgraph_vk_hybrid_pipeline_builder_has_result(
    const PGRAPHVkHybridPipelineBuilder *builder);
/* Superseded results are destroyed, including a result produced by an active
 * job after the generation changes. */
void pgraph_vk_hybrid_pipeline_builder_cancel_before_generation(
    PGRAPHVkHybridPipelineBuilder *builder, uint64_t generation);
/* A taken result owns its pipeline. This remains valid after builder_destroy;
 * builder may be NULL. Keep the callback's opaque context alive until then. */
void pgraph_vk_hybrid_pipeline_build_result_destroy(
    PGRAPHVkHybridPipelineBuilder *builder,
    PGRAPHVkHybridPipelineBuildResult *result);
void pgraph_vk_hybrid_pipeline_builder_stop(
    PGRAPHVkHybridPipelineBuilder *builder);
void pgraph_vk_hybrid_pipeline_builder_join(
    PGRAPHVkHybridPipelineBuilder *builder);
void pgraph_vk_hybrid_pipeline_builder_destroy(
    PGRAPHVkHybridPipelineBuilder *builder);

#endif

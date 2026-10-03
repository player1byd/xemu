/*
 * NV2A Vulkan fallback-family prewarm preparation boundary
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_PREWARM_RUNTIME_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_PREWARM_RUNTIME_H

#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

typedef struct PGRAPHVkHybridPrewarmPrepareOps {
    bool (*device_supported)(void *opaque, const PipelineKey *key);
    bool (*pipeline_ready)(void *opaque, const PipelineKey *key);
    PGRAPHVkCachedFamilyModulesResult (*cached_modules)(
        void *opaque, const ShaderState *state);
    PGRAPHVkHybridPrewarmAttemptResult (*retain_missing_family)(
        void *opaque, const PipelineKey *key);
    ShaderBinding *(*ready_binding)(void *opaque, const ShaderState *state);
    PGRAPHVkHybridPipelineSubmitResult (*submit_pipeline)(
        void *opaque, const PipelineKey *key, ShaderBinding *binding);
} PGRAPHVkHybridPrewarmPrepareOps;

typedef void (*PGRAPHVkHybridPrewarmFormatPropertiesFunc)(
    void *opaque, VkFormat format, VkFormatProperties *properties);

bool pgraph_vk_hybrid_prewarm_vertex_formats_supported(
    const PipelineKey *key,
    PGRAPHVkHybridPrewarmFormatPropertiesFunc get_properties,
    void *opaque);

PGRAPHVkHybridPrewarmAttemptResult pgraph_vk_hybrid_prewarm_prepare_record(
    const PGRAPHVkFamilyHistoryRecord *record,
    const PGRAPHVkHybridPrewarmPrepareOps *ops, void *opaque);

static inline void pgraph_vk_hybrid_prewarm_note_publication(
    PGRAPHVkHybridPrewarmState *state, PipelineBinding *binding,
    bool from_prewarm)
{
    binding->prewarmed = from_prewarm;
    if (from_prewarm) {
        state->ready++;
    }
}

static inline void pgraph_vk_hybrid_prewarm_note_demand(
    PGRAPHVkHybridPrewarmState *state, PipelineBinding *binding)
{
    if (binding && binding->prewarmed) {
        binding->prewarmed = false;
        state->demand_hits++;
    }
}

#endif

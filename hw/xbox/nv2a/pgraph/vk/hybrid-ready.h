/*
 * Non-creating Vulkan hybrid execution probes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_READY_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_READY_H

#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

#define PGRAPH_VK_HYBRID_OWNER_SERVICE_BUDGET_US 1000

static inline void pgraph_vk_hybrid_owner_budget_begin(PGRAPHVkState *r)
{
    int64_t now_us = g_get_monotonic_time();
    if (r->hybrid_owner_service_deadline_us <= now_us) {
        r->hybrid_owner_service_deadline_us = now_us +
            PGRAPH_VK_HYBRID_OWNER_SERVICE_BUDGET_US;
    }
}

static inline bool pgraph_vk_hybrid_owner_budget_available(
    const PGRAPHVkState *r)
{
    return r->hybrid_owner_service_deadline_us <= 0 ||
           g_get_monotonic_time() < r->hybrid_owner_service_deadline_us;
}

/* Borrowed exact hits. These functions neither reserve entries nor refresh
 * LRU order. The renderer must keep cache mutation on its owning thread. */
static inline PipelineBinding *pgraph_vk_pipeline_cache_find_ready(
    Lru *cache, uint64_t hash, const PipelineKey *key)
{
    LruNode *node = lru_find_existing(cache, hash, key);
    if (!node) {
        return NULL;
    }
    PipelineBinding *binding = container_of(node, PipelineBinding, node);
    return binding->pipeline != VK_NULL_HANDLE ? binding : NULL;
}

static inline ShaderBinding *pgraph_vk_shader_binding_find_ready(
    Lru *cache, uint64_t hash, const ShaderBindingKey *key,
    bool geometry_required)
{
    LruNode *node = lru_find_existing(cache, hash, key);
    if (!node) {
        return NULL;
    }
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return binding->vsh.module_info && binding->psh.module_info &&
           (!geometry_required || binding->geom.module_info) ?
           binding : NULL;
}

typedef enum PGRAPHVkExecutionRoute {
    PGRAPH_VK_EXECUTION_SPECIALIZED,
    PGRAPH_VK_EXECUTION_UBERSHADER,
    PGRAPH_VK_EXECUTION_UBERSHADER_AFTER_ROLLOVER,
    PGRAPH_VK_EXECUTION_UNCOVERED,
} PGRAPHVkExecutionRoute;

static inline bool pgraph_vk_hybrid_fastpath_route_allowed(
    bool force_ubershader, PGRAPHVkFragmentRoute route)
{
    return !force_ubershader || route == PGRAPH_VK_FRAGMENT_UBERSHADER;
}

static inline bool pgraph_vk_hybrid_should_schedule_specialization(
    bool force_ubershader, PGRAPHVkFragmentRoute route)
{
    return !force_ubershader && route == PGRAPH_VK_FRAGMENT_UBERSHADER;
}

/* Module presence or an isolated pipeline hit is insufficient for a draw.
 * A ready fallback remains usable even when the speculative queue is full. */
static inline PGRAPHVkExecutionRoute pgraph_vk_hybrid_choose_execution_route(
    bool force_ubershader,
    bool specialized_shader_ready, bool specialized_pipeline_ready,
    bool fallback_shader_ready, bool fallback_pipeline_ready,
    PGRAPHVkFallbackResourceState fallback_resources)
{
    if (!force_ubershader && specialized_shader_ready &&
        specialized_pipeline_ready) {
        return PGRAPH_VK_EXECUTION_SPECIALIZED;
    }
    if (fallback_shader_ready && fallback_pipeline_ready) {
        if (fallback_resources == PGRAPH_VK_FALLBACK_RESOURCES_READY) {
            return PGRAPH_VK_EXECUTION_UBERSHADER;
        }
        if (fallback_resources == PGRAPH_VK_FALLBACK_RESOURCES_NEED_ROLLOVER) {
            return PGRAPH_VK_EXECUTION_UBERSHADER_AFTER_ROLLOVER;
        }
    }
    return PGRAPH_VK_EXECUTION_UNCOVERED;
}

/* A family pipeline can already be executable even when no full-state
 * fallback binding has been encountered. Materialize only the missing
 * metadata, and only when specialization is not already complete. */
static inline bool pgraph_vk_hybrid_should_prepare_fallback_binding(
    bool specialized_shader_ready, bool specialized_pipeline_ready,
    bool fallback_shader_ready, bool fallback_pipeline_ready,
    bool fallback_controls_supported)
{
    return !(specialized_shader_ready && specialized_pipeline_ready) &&
           !fallback_shader_ready && fallback_pipeline_ready &&
           fallback_controls_supported;
}

static inline bool pgraph_vk_fallback_family_learning_needed(
    bool specialized_complete, PGRAPHVkFamilyLearnState state)
{
    return specialized_complete && state == PGRAPH_VK_FAMILY_UNCHECKED;
}

/* A partially prepared specialized route is closer than a cold fallback. */
static inline PGRAPHVkFragmentRoute pgraph_vk_hybrid_choose_uncovered_route(
    bool force_ubershader, bool specialized_shader_ready,
    bool fallback_shader_ready, bool fallback_pipeline_ready,
    bool fallback_controls_supported,
    PGRAPHVkFallbackResourceState fallback_resources)
{
    bool complete_fallback_unavailable =
        fallback_shader_ready && fallback_pipeline_ready &&
        fallback_resources == PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE;
    if (force_ubershader && fallback_controls_supported) {
        return complete_fallback_unavailable ?
               PGRAPH_VK_FRAGMENT_SPECIALIZED :
               PGRAPH_VK_FRAGMENT_UBERSHADER;
    }
    if (specialized_shader_ready) {
        return PGRAPH_VK_FRAGMENT_SPECIALIZED;
    }
    return fallback_shader_ready && fallback_controls_supported ?
           PGRAPH_VK_FRAGMENT_UBERSHADER :
           PGRAPH_VK_FRAGMENT_SPECIALIZED;
}

static inline bool pgraph_vk_hybrid_promotion_due(int64_t now_us,
                                                  int64_t next_probe_us)
{
    return next_probe_us <= 0 || now_us >= next_probe_us;
}

/* Call only after a speculative pipeline is complete. A miss with no safely
 * evictable node leaves the existing executable cache unchanged. */
static inline PipelineBinding *pgraph_vk_pipeline_cache_publish_slot(
    Lru *cache, uint64_t hash, const PipelineKey *key)
{
    LruNode *node = lru_try_lookup(cache, hash, key);
    return node ? container_of(node, PipelineBinding, node) : NULL;
}

/* Capture an exact family without preparing it on the current draw. */
static inline bool pgraph_vk_fallback_family_enqueue(
    PGRAPHVkFallbackFamilyRequest *requests, size_t capacity,
    const PipelineKey *key, const ShaderState *state, bool from_prewarm)
{
    PGRAPHVkFallbackFamilyRequest *free_request = NULL;
    for (size_t i = 0; i < capacity; i++) {
        if (requests[i].in_use &&
            memcmp(&requests[i].key, key, sizeof(*key)) == 0) {
            requests[i].from_prewarm |= from_prewarm;
            if (!from_prewarm) {
                requests[i].priority = PGRAPH_VK_HYBRID_PRIORITY_VISIBLE;
            }
            return true;
        }
        if (!requests[i].in_use && !free_request) {
            free_request = &requests[i];
        }
    }
    if (!free_request) {
        return false;
    }
    free_request->key = *key;
    free_request->state = *state;
    free_request->in_use = true;
    free_request->from_prewarm = from_prewarm;
    free_request->priority = from_prewarm ?
        PGRAPH_VK_HYBRID_PRIORITY_PREWARM :
        PGRAPH_VK_HYBRID_PRIORITY_VISIBLE;
    free_request->status = PGRAPH_VK_FAMILY_WAITING_FOR_SHADER;
    free_request->attempts = 0;
    free_request->retry_after_us = 0;
    return true;
}

static inline void pgraph_vk_hybrid_pipeline_note_prewarm(
    PGRAPHVkHybridPipelineWork *work, bool from_prewarm)
{
    work->prewarm |= from_prewarm;
}

static inline PGRAPHVkHybridPriority pgraph_vk_hybrid_priority_max(
    PGRAPHVkHybridPriority a, PGRAPHVkHybridPriority b)
{
    return a > b ? a : b;
}

static inline size_t pgraph_vk_hybrid_shader_promote_aliases(
    PGRAPHVkHybridShaderWork *work, size_t capacity, uint64_t generation,
    uint64_t ticket, PGRAPHVkHybridPriority priority)
{
    size_t promoted = 0;
    for (size_t i = 0; i < capacity; i++) {
        if (!work[i].in_use || work[i].metadata.generation != generation ||
            work[i].metadata.ticket != ticket) {
            continue;
        }
        work[i].priority = pgraph_vk_hybrid_priority_max(work[i].priority,
                                                         priority);
        promoted++;
    }
    return promoted;
}

static inline bool pgraph_vk_hybrid_shader_owned_source(
    const PGRAPHVkHybridShaderWork *work, const char **glsl,
    size_t *glsl_size)
{
    if (!work || !work->in_use || !work->glsl || !work->glsl_size ||
        !glsl || !glsl_size) {
        return false;
    }
    *glsl = work->glsl;
    *glsl_size = work->glsl_size;
    return true;
}

typedef bool (*PGRAPHVkHybridCompletionAliasFunc)(
    void *opaque, PGRAPHVkHybridShaderWork *work);

/* A source-deduplicated compiler result still owns one independently
 * materialized module for every exact-key alias that joined its ticket. */
static inline size_t pgraph_vk_hybrid_completion_fanout(
    PGRAPHVkHybridShaderWork *work, size_t capacity,
    uint64_t completion_generation, uint64_t completion_ticket,
    uint64_t current_generation, PGRAPHVkHybridCompletionAliasFunc publish,
    void *opaque, bool *all_published)
{
    size_t matching = 0;
    bool all = true;

    for (size_t i = 0; i < capacity; i++) {
        if (!work[i].in_use ||
            pgraph_vk_hybrid_validate_completion_metadata(
                &work[i].metadata, completion_generation,
                completion_ticket, current_generation) !=
                PGRAPH_VK_HYBRID_COMPLETION_METADATA_MATCH) {
            continue;
        }
        matching++;
        all &= publish(opaque, &work[i]);
    }
    if (all_published) {
        *all_published = matching > 0 && all;
    }
    return matching;
}

#define PGRAPH_VK_FAMILY_MAX_PIPELINE_ATTEMPTS 3
#define PGRAPH_VK_FAMILY_RETRY_BASE_US 16000

static inline bool pgraph_vk_fallback_family_retry_due(
    const PGRAPHVkFallbackFamilyRequest *request, int64_t now_us)
{
    return request->in_use &&
           request->status != PGRAPH_VK_FAMILY_PIPELINE_PENDING &&
           now_us >= request->retry_after_us;
}

static inline bool pgraph_vk_fallback_family_wake_for_module(
    PGRAPHVkFallbackFamilyRequest *request,
    const ShaderModuleCacheKey *requested_key,
    const ShaderModuleCacheKey *published_key)
{
    if (!request->in_use ||
        request->status != PGRAPH_VK_FAMILY_WAITING_FOR_SHADER ||
        !pgraph_vk_shader_module_key_equal(requested_key, published_key)) {
        return false;
    }
    request->retry_after_us = 0;
    return true;
}

/* ACCEPTED means queued, in flight, or already present. Keep the family
 * owner until the exact executable is observed or completion reports a
 * failure. Queue pressure is a deferral and does not consume an attempt. */
static inline bool pgraph_vk_fallback_family_note_pipeline_submit(
    PGRAPHVkFallbackFamilyRequest *request,
    PGRAPHVkHybridPipelineSubmitResult result, int64_t now_us)
{
    switch (result) {
    case PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED:
        request->status = PGRAPH_VK_FAMILY_PIPELINE_PENDING;
        request->retry_after_us = 0;
        return true;
    case PGRAPH_VK_HYBRID_PIPELINE_QUEUE_FULL:
        request->status = PGRAPH_VK_FAMILY_QUEUE_DEFERRED;
        request->retry_after_us = now_us + PGRAPH_VK_FAMILY_RETRY_BASE_US;
        return true;
    case PGRAPH_VK_HYBRID_PIPELINE_STOPPED:
    case PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE:
        request->status = PGRAPH_VK_FAMILY_REQUEST_REJECTED;
        request->in_use = false;
        return false;
    }
    g_assert_not_reached();
}

static inline bool pgraph_vk_fallback_family_note_pipeline_failure(
    PGRAPHVkFallbackFamilyRequest *request, int64_t now_us)
{
    request->attempts++;
    if (request->attempts >= PGRAPH_VK_FAMILY_MAX_PIPELINE_ATTEMPTS) {
        request->status = PGRAPH_VK_FAMILY_REQUEST_REJECTED;
        request->in_use = false;
        return false;
    }
    request->status = PGRAPH_VK_FAMILY_PIPELINE_RETRY_BACKOFF;
    request->retry_after_us = now_us +
        (int64_t)PGRAPH_VK_FAMILY_RETRY_BASE_US * request->attempts;
    return true;
}

#endif

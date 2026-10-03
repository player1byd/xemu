/*
 * NV2A Vulkan opportunistic fallback-family prewarm policy
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/pgraph/vk/hybrid-prewarm.h"

PGRAPHVkHybridPrewarmAttemptResult pgraph_vk_hybrid_prewarm_service(
    PGRAPHVkHybridPrewarmState *state, PGRAPHVkFamilyHistory *history,
    bool demand_work_waiting,
    PGRAPHVkHybridPrewarmAttemptFunc attempt, void *opaque)
{
    if (!state || !history || !state->enabled || demand_work_waiting || !attempt ||
        state->attempted >= PGRAPH_VK_HYBRID_PREWARM_MAX_CANDIDATES) {
        return PGRAPH_VK_HYBRID_PREWARM_IDLE;
    }
    state->service_id++;
    const PGRAPHVkFamilyHistoryRecord *record =
        pgraph_vk_family_history_next_eligible(
            history, state->service_id,
            state->considered < PGRAPH_VK_HYBRID_PREWARM_MAX_CANDIDATES);
    if (!record) {
        /* If no candidate is cooling down, this launch is finished. */
        if (state->considered == state->attempted) {
            state->enabled = false;
        }
        return PGRAPH_VK_HYBRID_PREWARM_NO_CANDIDATE;
    }
    if (pgraph_vk_family_history_mark_considered(history, record)) {
        state->considered++;
    }

    int64_t started_us = g_get_monotonic_time();
    PGRAPHVkHybridPrewarmAttemptResult result = attempt(opaque, record);
    uint64_t elapsed_us = MAX((int64_t)0,
                              g_get_monotonic_time() - started_us);
    state->owner_attempts++;
    state->owner_prepare_us_total += elapsed_us;
    state->owner_prepare_us_max = MAX(state->owner_prepare_us_max,
                                      elapsed_us);

    switch (result) {
    case PGRAPH_VK_HYBRID_PREWARM_READY:
        pgraph_vk_family_history_mark_attempted(history, record);
        state->attempted++;
        state->ready++;
        break;
    case PGRAPH_VK_HYBRID_PREWARM_SUBMITTED:
        pgraph_vk_family_history_mark_attempted(history, record);
        state->attempted++;
        state->scheduled++;
        break;
    case PGRAPH_VK_HYBRID_PREWARM_MISSING_ARTIFACT:
        pgraph_vk_family_history_mark_attempted(history, record);
        state->attempted++;
        state->missing++;
        break;
    case PGRAPH_VK_HYBRID_PREWARM_DEFERRED:
        state->deferred++;
        if (!pgraph_vk_family_history_defer(
                history, record, state->service_id)) {
            state->attempted++;
            state->retry_exhausted++;
        }
        break;
    case PGRAPH_VK_HYBRID_PREWARM_REJECTED:
        pgraph_vk_family_history_mark_attempted(history, record);
        state->attempted++;
        state->rejected++;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

PGRAPHVkCachedFamilyModulesResult pgraph_vk_hybrid_prewarm_modules(
    bool geometry_required, PGRAPHVkHybridPrewarmStageFunc materialize,
    void *opaque)
{
    if (!materialize) {
        return PGRAPH_VK_CACHED_FAMILY_MODULES_REJECTED;
    }
    const PGRAPHVkHybridPrewarmStage stages[] = {
        PGRAPH_VK_HYBRID_PREWARM_VERTEX,
        PGRAPH_VK_HYBRID_PREWARM_GEOMETRY,
        PGRAPH_VK_HYBRID_PREWARM_FRAGMENT,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(stages); i++) {
        if (stages[i] == PGRAPH_VK_HYBRID_PREWARM_GEOMETRY &&
            !geometry_required) {
            continue;
        }
        PGRAPHVkCachedFamilyModulesResult result =
            materialize(opaque, stages[i]);
        if (result != PGRAPH_VK_CACHED_FAMILY_MODULES_READY) {
            return result;
        }
    }
    return PGRAPH_VK_CACHED_FAMILY_MODULES_READY;
}

PGRAPHVkCachedFamilyModulesResult pgraph_vk_hybrid_prepare_family_modules(
    bool geometry_required, PGRAPHVkHybridPrewarmStageFunc prepare,
    void *opaque)
{
    if (!prepare) {
        return PGRAPH_VK_CACHED_FAMILY_MODULES_REJECTED;
    }
    const PGRAPHVkHybridPrewarmStage stages[] = {
        PGRAPH_VK_HYBRID_PREWARM_VERTEX,
        PGRAPH_VK_HYBRID_PREWARM_GEOMETRY,
        PGRAPH_VK_HYBRID_PREWARM_FRAGMENT,
    };
    PGRAPHVkCachedFamilyModulesResult aggregate =
        PGRAPH_VK_CACHED_FAMILY_MODULES_READY;

    for (size_t i = 0; i < G_N_ELEMENTS(stages); i++) {
        if (stages[i] == PGRAPH_VK_HYBRID_PREWARM_GEOMETRY &&
            !geometry_required) {
            continue;
        }
        PGRAPHVkCachedFamilyModulesResult result =
            prepare(opaque, stages[i]);
        if (result == PGRAPH_VK_CACHED_FAMILY_MODULES_REJECTED) {
            return result;
        }
        if (result == PGRAPH_VK_CACHED_FAMILY_MODULES_DEFERRED) {
            aggregate = result;
        } else if (result == PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING &&
                   aggregate == PGRAPH_VK_CACHED_FAMILY_MODULES_READY) {
            aggregate = result;
        }
    }
    return aggregate;
}

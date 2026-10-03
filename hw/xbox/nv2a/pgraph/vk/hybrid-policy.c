/*
 * Geforce NV2A PGRAPH Vulkan hybrid specialization metadata policy
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/hybrid-policy.h"

#include <limits.h>
#include <string.h>

static PGRAPHVkHybridDecision decision(PGRAPHVkHybridRoute route,
                                       PGRAPHVkHybridReason reason,
                                       bool request_specialization)
{
    return (PGRAPHVkHybridDecision) {
        .route = route,
        .reason = reason,
        .request_specialization = request_specialization,
    };
}

PGRAPHVkHybridDecision pgraph_vk_hybrid_choose(
    const PGRAPHVkHybridRouteInput *input)
{
    if (input->specialized_ready) {
        return decision(PGRAPH_VK_HYBRID_USE_SPECIALIZED,
                        PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_READY, false);
    }
    if (!input->fallback_pipeline_ready) {
        return decision(PGRAPH_VK_HYBRID_USE_SYNCHRONOUS,
                        PGRAPH_VK_HYBRID_REASON_NO_FALLBACK_PIPELINE, false);
    }
    if (!input->fallback_draw_resources_ready) {
        return decision(PGRAPH_VK_HYBRID_USE_SYNCHRONOUS,
                        PGRAPH_VK_HYBRID_REASON_NO_DRAW_RESOURCES, false);
    }

    switch (input->matching_status) {
    case PGRAPH_VK_HYBRID_WORK_PENDING:
        return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                        PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_PENDING,
                        false);
    case PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF:
        if (input->epoch < input->retry_after_epoch) {
            return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                            PGRAPH_VK_HYBRID_REASON_QUEUE_BACKOFF, false);
        }
        break;
    case PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF:
        if (input->attempts >= input->max_attempts) {
            return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                            PGRAPH_VK_HYBRID_REASON_FAILURE_PERMANENT, false);
        }
        if (input->epoch < input->retry_after_epoch) {
            return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                            PGRAPH_VK_HYBRID_REASON_FAILURE_BACKOFF, false);
        }
        break;
    case PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT:
        return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                        PGRAPH_VK_HYBRID_REASON_FAILURE_PERMANENT, false);
    case PGRAPH_VK_HYBRID_WORK_ABSENT:
        break;
    }

    if (input->attempts >= input->max_attempts) {
        return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                        PGRAPH_VK_HYBRID_REASON_FAILURE_PERMANENT, false);
    }
    if (!input->queue_has_capacity) {
        return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                        PGRAPH_VK_HYBRID_REASON_QUEUE_FULL, false);
    }
    return decision(PGRAPH_VK_HYBRID_USE_FALLBACK,
                    PGRAPH_VK_HYBRID_REASON_FALLBACK_AND_ENQUEUE, true);
}

bool pgraph_vk_hybrid_work_init(PGRAPHVkHybridWork *work,
                                unsigned int max_attempts)
{
    if (!work || max_attempts == 0) {
        return false;
    }

    *work = (PGRAPHVkHybridWork) {
        .max_attempts = max_attempts,
        .status = PGRAPH_VK_HYBRID_WORK_ABSENT,
    };
    return true;
}

static bool retry_is_blocked(const PGRAPHVkHybridWork *work, uint64_t epoch)
{
    return (work->status == PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF ||
            work->status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF) &&
           epoch < work->retry_after_epoch;
}

bool pgraph_vk_hybrid_mark_pending(PGRAPHVkHybridWork *work,
                                   bool specialized_ready,
                                   uint64_t generation, uint64_t ticket,
                                   uint64_t epoch)
{
    if (!work || specialized_ready || ticket == 0 ||
        work->max_attempts == 0 ||
        work->status == PGRAPH_VK_HYBRID_WORK_PENDING ||
        work->status == PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT ||
        work->attempts >= work->max_attempts || retry_is_blocked(work, epoch)) {
        return false;
    }

    work->generation = generation;
    work->ticket = ticket;
    work->retry_after_epoch = 0;
    work->attempts++;
    work->status = PGRAPH_VK_HYBRID_WORK_PENDING;
    return true;
}

static uint64_t retry_epoch(uint64_t epoch, uint64_t backoff)
{
    return UINT64_MAX - epoch < backoff ? UINT64_MAX : epoch + backoff;
}

bool pgraph_vk_hybrid_note_queue_deferral(PGRAPHVkHybridWork *work,
                                          bool specialized_ready,
                                          uint64_t current_generation,
                                          uint64_t epoch, uint64_t backoff)
{
    if (!work || specialized_ready || backoff == 0 ||
        work->max_attempts == 0 ||
        work->status == PGRAPH_VK_HYBRID_WORK_PENDING ||
        work->status == PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT ||
        work->attempts >= work->max_attempts || retry_is_blocked(work, epoch)) {
        return false;
    }

    work->generation = current_generation;
    work->ticket = 0;
    work->retry_after_epoch = retry_epoch(epoch, backoff);
    work->status = PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF;
    return true;
}

bool pgraph_vk_hybrid_rearm_queue_deferral(PGRAPHVkHybridWork *work,
                                           bool queue_has_capacity)
{
    if (!work || !queue_has_capacity ||
        work->status != PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF) {
        return false;
    }

    work->retry_after_epoch = 0;
    work->status = PGRAPH_VK_HYBRID_WORK_ABSENT;
    return true;
}

bool pgraph_vk_hybrid_note_compile_failure(PGRAPHVkHybridWork *work,
                                           uint64_t completion_generation,
                                           uint64_t completion_ticket,
                                           uint64_t current_generation,
                                           uint64_t epoch,
                                           uint64_t backoff)
{
    if (!work || backoff == 0 ||
        work->status != PGRAPH_VK_HYBRID_WORK_PENDING) {
        return false;
    }
    if (pgraph_vk_hybrid_validate_completion_metadata(
            work, completion_generation, completion_ticket,
            current_generation) !=
        PGRAPH_VK_HYBRID_COMPLETION_METADATA_MATCH) {
        return false;
    }

    work->ticket = 0;
    if (work->attempts >= work->max_attempts) {
        work->retry_after_epoch = 0;
        work->status = PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT;
    } else {
        work->retry_after_epoch = retry_epoch(epoch, backoff);
        work->status = PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF;
    }
    return true;
}

uint64_t pgraph_vk_hybrid_allocate_ticket(
    PGRAPHVkHybridTicketAllocator *allocator)
{
    if (!allocator || allocator->last_ticket == UINT64_MAX) {
        return 0;
    }
    allocator->last_ticket++;
    return allocator->last_ticket;
}

PGRAPHVkHybridCompletionMetadataResult
pgraph_vk_hybrid_validate_completion_metadata(
    const PGRAPHVkHybridWork *work, uint64_t completion_generation,
    uint64_t completion_ticket, uint64_t current_generation)
{
    if (!work || work->status != PGRAPH_VK_HYBRID_WORK_PENDING) {
        return PGRAPH_VK_HYBRID_COMPLETION_NOT_PENDING;
    }
    if (work->generation != current_generation ||
        completion_generation != current_generation) {
        return PGRAPH_VK_HYBRID_COMPLETION_GENERATION_MISMATCH;
    }
    if (work->ticket == 0 || completion_ticket == 0) {
        return PGRAPH_VK_HYBRID_COMPLETION_INVALID_TICKET;
    }
    if (work->ticket != completion_ticket) {
        return PGRAPH_VK_HYBRID_COMPLETION_TICKET_MISMATCH;
    }
    return PGRAPH_VK_HYBRID_COMPLETION_METADATA_MATCH;
}

uint64_t pgraph_vk_hybrid_next_selection_epoch(uint64_t current_epoch)
{
    current_epoch++;
    return current_epoch == 0 ? 1 : current_epoch;
}

bool pgraph_vk_hybrid_selection_changed(uint64_t bound_epoch,
                                        uint64_t selection_epoch)
{
    return bound_epoch != selection_epoch;
}

bool pgraph_vk_hybrid_source_matches(uint32_t first_stage,
                                     const void *first_source,
                                     size_t first_size,
                                     uint32_t second_stage,
                                     const void *second_source,
                                     size_t second_size)
{
    return first_source && second_source && first_stage == second_stage &&
           first_size == second_size &&
           memcmp(first_source, second_source, first_size) == 0;
}

bool pgraph_vk_hybrid_key_matches(const void *first_key, size_t first_size,
                                  const void *second_key, size_t second_size)
{
    return first_key && second_key && first_size == second_size &&
           memcmp(first_key, second_key, first_size) == 0;
}

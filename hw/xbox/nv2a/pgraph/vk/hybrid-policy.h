/*
 * Geforce NV2A PGRAPH Vulkan hybrid specialization metadata policy
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_POLICY_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This policy deliberately does not define a shader or pipeline key. The
 * renderer owns the complete recipe and uses lru_find_existing() to identify
 * its exact LRU node before passing that node's metadata to these helpers.
 */
typedef enum PGRAPHVkHybridWorkStatus {
    PGRAPH_VK_HYBRID_WORK_ABSENT,
    PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF,
    PGRAPH_VK_HYBRID_WORK_PENDING,
    PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF,
    PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT,
} PGRAPHVkHybridWorkStatus;

typedef enum PGRAPHVkHybridRoute {
    PGRAPH_VK_HYBRID_USE_SPECIALIZED,
    PGRAPH_VK_HYBRID_USE_FALLBACK,
    PGRAPH_VK_HYBRID_USE_SYNCHRONOUS,
} PGRAPHVkHybridRoute;

typedef enum PGRAPHVkHybridReason {
    PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_READY,
    PGRAPH_VK_HYBRID_REASON_FALLBACK_AND_ENQUEUE,
    PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_PENDING,
    PGRAPH_VK_HYBRID_REASON_QUEUE_BACKOFF,
    PGRAPH_VK_HYBRID_REASON_FAILURE_BACKOFF,
    PGRAPH_VK_HYBRID_REASON_FAILURE_PERMANENT,
    PGRAPH_VK_HYBRID_REASON_QUEUE_FULL,
    PGRAPH_VK_HYBRID_REASON_NO_FALLBACK_PIPELINE,
    PGRAPH_VK_HYBRID_REASON_NO_DRAW_RESOURCES,
} PGRAPHVkHybridReason;

/*
 * matching_status and its retry metadata are valid only after the caller has
 * found the exact key and confirmed that its node belongs to the current
 * renderer generation. Otherwise matching_status must be WORK_ABSENT.
 */
typedef struct PGRAPHVkHybridRouteInput {
    bool specialized_ready;
    PGRAPHVkHybridWorkStatus matching_status;
    uint64_t epoch;
    uint64_t retry_after_epoch;
    unsigned int attempts;
    unsigned int max_attempts;
    bool fallback_pipeline_ready;
    bool fallback_draw_resources_ready;
    bool queue_has_capacity;
} PGRAPHVkHybridRouteInput;

typedef struct PGRAPHVkHybridDecision {
    PGRAPHVkHybridRoute route;
    PGRAPHVkHybridReason reason;
    bool request_specialization;
} PGRAPHVkHybridDecision;

/* Metadata stored beside the caller-owned exact recipe/key. */
typedef struct PGRAPHVkHybridWork {
    uint64_t generation;
    uint64_t ticket;
    uint64_t retry_after_epoch;
    unsigned int attempts;
    unsigned int max_attempts;
    PGRAPHVkHybridWorkStatus status;
} PGRAPHVkHybridWork;

typedef struct PGRAPHVkHybridTicketAllocator {
    uint64_t last_ticket;
} PGRAPHVkHybridTicketAllocator;

typedef enum PGRAPHVkHybridCompletionMetadataResult {
    PGRAPH_VK_HYBRID_COMPLETION_METADATA_MATCH,
    PGRAPH_VK_HYBRID_COMPLETION_NOT_PENDING,
    PGRAPH_VK_HYBRID_COMPLETION_GENERATION_MISMATCH,
    PGRAPH_VK_HYBRID_COMPLETION_INVALID_TICKET,
    PGRAPH_VK_HYBRID_COMPLETION_TICKET_MISMATCH,
} PGRAPHVkHybridCompletionMetadataResult;

PGRAPHVkHybridDecision pgraph_vk_hybrid_choose(
    const PGRAPHVkHybridRouteInput *input);

bool pgraph_vk_hybrid_work_init(PGRAPHVkHybridWork *work,
                                unsigned int max_attempts);
bool pgraph_vk_hybrid_mark_pending(PGRAPHVkHybridWork *work,
                                   bool specialized_ready,
                                   uint64_t generation, uint64_t ticket,
                                   uint64_t epoch);
bool pgraph_vk_hybrid_note_queue_deferral(PGRAPHVkHybridWork *work,
                                          bool specialized_ready,
                                          uint64_t current_generation,
                                          uint64_t epoch, uint64_t backoff);
/* Queue deferral is an admission-control condition, not a draw-selection
 * failure. A retained request may rearm as soon as real capacity exists even
 * when no draw advances the renderer epoch. */
bool pgraph_vk_hybrid_rearm_queue_deferral(PGRAPHVkHybridWork *work,
                                           bool queue_has_capacity);
bool pgraph_vk_hybrid_note_compile_failure(PGRAPHVkHybridWork *work,
                                           uint64_t completion_generation,
                                           uint64_t completion_ticket,
                                           uint64_t current_generation,
                                           uint64_t epoch,
                                           uint64_t backoff);

/* Returns zero for NULL or exhausted session state; zero is never a ticket. */
uint64_t pgraph_vk_hybrid_allocate_ticket(
    PGRAPHVkHybridTicketAllocator *allocator);

/*
 * Validate only asynchronous completion metadata. A match does not establish
 * that an executable shader/pipeline bundle is complete and does not publish
 * or mark the caller-owned LRU entry ready.
 */
PGRAPHVkHybridCompletionMetadataResult
pgraph_vk_hybrid_validate_completion_metadata(
    const PGRAPHVkHybridWork *work, uint64_t completion_generation,
    uint64_t completion_ticket, uint64_t current_generation);

/* Call this only after the renderer actually adopts relevant new state. */
uint64_t pgraph_vk_hybrid_next_selection_epoch(uint64_t current_epoch);
bool pgraph_vk_hybrid_selection_changed(uint64_t bound_epoch,
                                        uint64_t selection_epoch);
bool pgraph_vk_hybrid_source_matches(uint32_t first_stage,
                                     const void *first_source,
                                     size_t first_size,
                                     uint32_t second_stage,
                                     const void *second_source,
                                     size_t second_size);
bool pgraph_vk_hybrid_key_matches(const void *first_key, size_t first_size,
                                  const void *second_key, size_t second_size);

#endif

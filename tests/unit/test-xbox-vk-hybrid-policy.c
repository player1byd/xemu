/*
 * NV2A Vulkan hybrid specialization metadata policy tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hw/xbox/nv2a/pgraph/vk/hybrid-policy.h"

static void assert_work_equal(const PGRAPHVkHybridWork *actual,
                              const PGRAPHVkHybridWork *expected)
{
    assert(actual->generation == expected->generation);
    assert(actual->ticket == expected->ticket);
    assert(actual->retry_after_epoch == expected->retry_after_epoch);
    assert(actual->attempts == expected->attempts);
    assert(actual->max_attempts == expected->max_attempts);
    assert(actual->status == expected->status);
}

static PGRAPHVkHybridDecision choose(bool specialized_ready,
                                     PGRAPHVkHybridWorkStatus status,
                                     uint64_t epoch,
                                     uint64_t retry_after_epoch,
                                     unsigned int attempts,
                                     unsigned int max_attempts,
                                     bool fallback_pipeline,
                                     bool fallback_resources,
                                     bool queue_capacity)
{
    return pgraph_vk_hybrid_choose(
        &(PGRAPHVkHybridRouteInput) {
            .specialized_ready = specialized_ready,
            .matching_status = status,
            .epoch = epoch,
            .retry_after_epoch = retry_after_epoch,
            .attempts = attempts,
            .max_attempts = max_attempts,
            .fallback_pipeline_ready = fallback_pipeline,
            .fallback_draw_resources_ready = fallback_resources,
            .queue_has_capacity = queue_capacity,
        });
}

static void test_eight_readiness_combinations(void)
{
    static const struct {
        bool specialized_ready;
        bool fallback_pipeline;
        bool fallback_resources;
        PGRAPHVkHybridRoute route;
        PGRAPHVkHybridReason reason;
        bool enqueue;
    } cases[] = {
        { false, false, false, PGRAPH_VK_HYBRID_USE_SYNCHRONOUS,
          PGRAPH_VK_HYBRID_REASON_NO_FALLBACK_PIPELINE, false },
        { false, false, true, PGRAPH_VK_HYBRID_USE_SYNCHRONOUS,
          PGRAPH_VK_HYBRID_REASON_NO_FALLBACK_PIPELINE, false },
        { false, true, false, PGRAPH_VK_HYBRID_USE_SYNCHRONOUS,
          PGRAPH_VK_HYBRID_REASON_NO_DRAW_RESOURCES, false },
        { false, true, true, PGRAPH_VK_HYBRID_USE_FALLBACK,
          PGRAPH_VK_HYBRID_REASON_FALLBACK_AND_ENQUEUE, true },
        { true, false, false, PGRAPH_VK_HYBRID_USE_SPECIALIZED,
          PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_READY, false },
        { true, false, true, PGRAPH_VK_HYBRID_USE_SPECIALIZED,
          PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_READY, false },
        { true, true, false, PGRAPH_VK_HYBRID_USE_SPECIALIZED,
          PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_READY, false },
        { true, true, true, PGRAPH_VK_HYBRID_USE_SPECIALIZED,
          PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_READY, false },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHVkHybridDecision decision = choose(
            cases[i].specialized_ready, PGRAPH_VK_HYBRID_WORK_ABSENT,
            10, 0, 0, 3, cases[i].fallback_pipeline,
            cases[i].fallback_resources, true);

        assert(decision.route == cases[i].route);
        assert(decision.reason == cases[i].reason);
        assert(decision.request_specialization == cases[i].enqueue);
    }
}

static void test_queue_full_keeps_the_ready_fallback(void)
{
    PGRAPHVkHybridDecision decision = choose(
        false, PGRAPH_VK_HYBRID_WORK_ABSENT, 10, 0, 0, 3,
        true, true, false);

    assert(decision.route == PGRAPH_VK_HYBRID_USE_FALLBACK);
    assert(decision.reason == PGRAPH_VK_HYBRID_REASON_QUEUE_FULL);
    assert(!decision.request_specialization);
}

static void test_matching_work_suppresses_duplicate_enqueues(void)
{
    PGRAPHVkHybridDecision pending = choose(
        false, PGRAPH_VK_HYBRID_WORK_PENDING, 12, 0, 1, 3,
        true, true, true);
    PGRAPHVkHybridDecision queue_backoff = choose(
        false, PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF, 12, 13, 0, 3,
        true, true, true);
    PGRAPHVkHybridDecision failure_backoff = choose(
        false, PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF, 12, 13, 1, 3,
        true, true, true);
    PGRAPHVkHybridDecision permanent = choose(
        false, PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT, 12, 0, 3, 3,
        true, true, true);

    assert(pending.route == PGRAPH_VK_HYBRID_USE_FALLBACK);
    assert(pending.reason == PGRAPH_VK_HYBRID_REASON_SPECIALIZATION_PENDING);
    assert(!pending.request_specialization);
    assert(queue_backoff.reason == PGRAPH_VK_HYBRID_REASON_QUEUE_BACKOFF);
    assert(!queue_backoff.request_specialization);
    assert(failure_backoff.reason == PGRAPH_VK_HYBRID_REASON_FAILURE_BACKOFF);
    assert(!failure_backoff.request_specialization);
    assert(permanent.reason == PGRAPH_VK_HYBRID_REASON_FAILURE_PERMANENT);
    assert(!permanent.request_specialization);
}

static void test_backoff_expiry_and_attempt_saturation(void)
{
    PGRAPHVkHybridDecision queue_expired = choose(
        false, PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF, 20, 20, 0, 3,
        true, true, true);
    PGRAPHVkHybridDecision failure_expired = choose(
        false, PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF, 20, 20, 2, 3,
        true, true, true);
    PGRAPHVkHybridDecision exhausted = choose(
        false, PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF, 20, 20, 3, 3,
        true, true, true);

    assert(queue_expired.reason ==
           PGRAPH_VK_HYBRID_REASON_FALLBACK_AND_ENQUEUE);
    assert(queue_expired.request_specialization);
    assert(failure_expired.reason ==
           PGRAPH_VK_HYBRID_REASON_FALLBACK_AND_ENQUEUE);
    assert(failure_expired.request_specialization);
    assert(exhausted.reason == PGRAPH_VK_HYBRID_REASON_FAILURE_PERMANENT);
    assert(!exhausted.request_specialization);
}

static void test_work_transitions_are_guarded_and_bounded(void)
{
    PGRAPHVkHybridWork work;

    assert(!pgraph_vk_hybrid_work_init(&work, 0));
    assert(pgraph_vk_hybrid_work_init(&work, 2));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_ABSENT);
    assert(work.attempts == 0);

    assert(!pgraph_vk_hybrid_note_queue_deferral(&work, true, 4, 10, 3));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_ABSENT);
    assert(!pgraph_vk_hybrid_mark_pending(&work, true, 4, 11, 10));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_ABSENT);
    assert(!pgraph_vk_hybrid_mark_pending(&work, false, 4, 0, 10));
    assert(pgraph_vk_hybrid_mark_pending(&work, false, 4, 11, 10));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_PENDING);
    assert(work.generation == 4);
    assert(work.ticket == 11);
    assert(work.attempts == 1);

    assert(!pgraph_vk_hybrid_mark_pending(&work, false, 5, 12, 10));
    assert(work.generation == 4);
    assert(work.ticket == 11);
    assert(!pgraph_vk_hybrid_note_queue_deferral(&work, false, 4, 10, 3));
    assert(pgraph_vk_hybrid_note_compile_failure(&work, 4, 11, 4, 10, 3));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF);
    assert(work.ticket == 0);
    assert(work.retry_after_epoch == 13);
    assert(!pgraph_vk_hybrid_note_queue_deferral(&work, false, 4, 12, 8));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF);
    assert(work.retry_after_epoch == 13);

    assert(!pgraph_vk_hybrid_mark_pending(&work, false, 4, 12, 12));
    assert(pgraph_vk_hybrid_mark_pending(&work, false, 4, 12, 13));
    assert(work.attempts == 2);
    assert(pgraph_vk_hybrid_note_compile_failure(&work, 4, 12, 4, 13, 3));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_PERMANENT);
    assert(work.retry_after_epoch == 0);
    assert(!pgraph_vk_hybrid_mark_pending(&work, false, 4, 13, 99));
}

static void test_queue_deferral_is_not_a_compile_attempt(void)
{
    PGRAPHVkHybridWork work;

    assert(pgraph_vk_hybrid_work_init(&work, 3));
    assert(pgraph_vk_hybrid_note_queue_deferral(&work, false, 8, 30, 5));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF);
    assert(work.generation == 8);
    assert(work.retry_after_epoch == 35);
    assert(work.attempts == 0);
    assert(!pgraph_vk_hybrid_note_queue_deferral(&work, false, 8, 34, 9));
    assert(work.retry_after_epoch == 35);
    assert(!pgraph_vk_hybrid_mark_pending(&work, false, 8, 31, 34));
    assert(pgraph_vk_hybrid_mark_pending(&work, false, 8, 31, 35));
    assert(work.attempts == 1);
}

static void test_queue_capacity_rearms_without_selection_epoch_progress(void)
{
    PGRAPHVkHybridWork work;
    const uint64_t frozen_epoch = 30;

    assert(pgraph_vk_hybrid_work_init(&work, 3));
    assert(pgraph_vk_hybrid_note_queue_deferral(
        &work, false, 8, frozen_epoch, 8));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF);
    assert(work.retry_after_epoch == 38);

    /* A retained family may be the only renderer activity. Its retry must
     * follow real queue capacity, not an unrelated draw-selection epoch. */
    assert(!pgraph_vk_hybrid_rearm_queue_deferral(&work, false));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_QUEUE_BACKOFF);
    assert(pgraph_vk_hybrid_rearm_queue_deferral(&work, true));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_ABSENT);
    assert(work.retry_after_epoch == 0);
    assert(pgraph_vk_hybrid_mark_pending(
        &work, false, 8, 31, frozen_epoch));
    assert(work.attempts == 1);

    assert(pgraph_vk_hybrid_note_compile_failure(
        &work, 8, 31, 8, frozen_epoch, 8));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF);
    assert(!pgraph_vk_hybrid_rearm_queue_deferral(&work, true));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF);
}

static void test_backoff_rejects_zero_and_saturates(void)
{
    PGRAPHVkHybridWork work;

    assert(pgraph_vk_hybrid_work_init(&work, 2));
    assert(!pgraph_vk_hybrid_note_queue_deferral(&work, false, 3, 10, 0));
    assert(pgraph_vk_hybrid_note_queue_deferral(
        &work, false, 3, UINT64_MAX - 1, 8));
    assert(work.generation == 3);
    assert(work.retry_after_epoch == UINT64_MAX);

    assert(pgraph_vk_hybrid_work_init(&work, 2));
    assert(pgraph_vk_hybrid_mark_pending(&work, false, 1, 1, 0));
    assert(!pgraph_vk_hybrid_note_compile_failure(&work, 1, 1, 1, 10, 0));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_PENDING);
    assert(pgraph_vk_hybrid_note_compile_failure(
        &work, 1, 1, 1, UINT64_MAX - 1, 8));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF);
    assert(work.retry_after_epoch == UINT64_MAX);
}

static void test_compile_failure_rejects_stale_metadata_without_mutation(void)
{
    PGRAPHVkHybridWork work;
    PGRAPHVkHybridWork original;

    assert(pgraph_vk_hybrid_work_init(&work, 3));
    assert(pgraph_vk_hybrid_mark_pending(&work, false, 7, 44, 2));
    original = work;

    assert(!pgraph_vk_hybrid_note_compile_failure(
        &work, 6, 44, 7, 10, 3));
    assert_work_equal(&work, &original);
    assert(!pgraph_vk_hybrid_note_compile_failure(
        &work, 7, 44, 8, 10, 3));
    assert_work_equal(&work, &original);
    assert(!pgraph_vk_hybrid_note_compile_failure(
        &work, 7, 45, 7, 10, 3));
    assert_work_equal(&work, &original);
    assert(!pgraph_vk_hybrid_note_compile_failure(
        &work, 7, 0, 7, 10, 3));
    assert_work_equal(&work, &original);

    assert(pgraph_vk_hybrid_note_compile_failure(
        &work, 7, 44, 7, 10, 3));
    assert(work.status == PGRAPH_VK_HYBRID_WORK_FAILED_BACKOFF);
}

static void test_ticket_allocator_is_nonzero_monotonic_and_fails_closed(void)
{
    PGRAPHVkHybridTicketAllocator allocator = { 0 };

    assert(pgraph_vk_hybrid_allocate_ticket(NULL) == 0);
    assert(pgraph_vk_hybrid_allocate_ticket(&allocator) == 1);
    assert(pgraph_vk_hybrid_allocate_ticket(&allocator) == 2);

    allocator.last_ticket = UINT64_MAX - 1;
    assert(pgraph_vk_hybrid_allocate_ticket(&allocator) == UINT64_MAX);
    assert(pgraph_vk_hybrid_allocate_ticket(&allocator) == 0);
    assert(pgraph_vk_hybrid_allocate_ticket(&allocator) == 0);
    assert(allocator.last_ticket == UINT64_MAX);
}

static void test_completion_metadata_requires_current_matching_ticket(void)
{
    PGRAPHVkHybridWork work;

    assert(pgraph_vk_hybrid_work_init(&work, 2));
    assert(pgraph_vk_hybrid_mark_pending(&work, false, 7, 44, 0));
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 7, 44, 7) ==
           PGRAPH_VK_HYBRID_COMPLETION_METADATA_MATCH);
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 8, 44, 7) ==
           PGRAPH_VK_HYBRID_COMPLETION_GENERATION_MISMATCH);
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 7, 44, 8) ==
           PGRAPH_VK_HYBRID_COMPLETION_GENERATION_MISMATCH);
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 7, 0, 7) ==
           PGRAPH_VK_HYBRID_COMPLETION_INVALID_TICKET);
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 7, 45, 7) ==
           PGRAPH_VK_HYBRID_COMPLETION_TICKET_MISMATCH);

    work.ticket = 0;
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 7, 44, 7) ==
           PGRAPH_VK_HYBRID_COMPLETION_INVALID_TICKET);
    work.status = PGRAPH_VK_HYBRID_WORK_ABSENT;
    assert(pgraph_vk_hybrid_validate_completion_metadata(&work, 7, 44, 7) ==
           PGRAPH_VK_HYBRID_COMPLETION_NOT_PENDING);
}

static void test_selection_epoch_is_pure_and_skips_zero(void)
{
    uint64_t current = 19;

    assert(pgraph_vk_hybrid_next_selection_epoch(current) == 20);
    assert(current == 19);
    assert(pgraph_vk_hybrid_next_selection_epoch(UINT64_MAX) == 1);
    assert(pgraph_vk_hybrid_selection_changed(19, 20));
    assert(!pgraph_vk_hybrid_selection_changed(20, 20));
}

static void test_source_identity_uses_stage_size_and_exact_bytes(void)
{
    static const char source[] = "void main(){}";
    static const char other[] = "void main(){ }";

    assert(pgraph_vk_hybrid_source_matches(1, source, sizeof(source) - 1,
                                           1, source,
                                           sizeof(source) - 1));
    assert(!pgraph_vk_hybrid_source_matches(1, source, sizeof(source) - 1,
                                            2, source,
                                            sizeof(source) - 1));
    assert(!pgraph_vk_hybrid_source_matches(1, source, sizeof(source) - 1,
                                            1, source,
                                            sizeof(source) - 2));
    assert(!pgraph_vk_hybrid_source_matches(1, source, sizeof(source) - 1,
                                            1, other,
                                            sizeof(other) - 1));
    assert(!pgraph_vk_hybrid_source_matches(1, NULL, 0, 1, source,
                                            sizeof(source) - 1));
}

static void test_recipe_key_identity_uses_size_and_exact_bytes(void)
{
    static const uint32_t key[] = { 1, 2, 3, 4 };
    static const uint32_t other[] = { 1, 2, 3, 5 };

    assert(pgraph_vk_hybrid_key_matches(key, sizeof(key), key,
                                        sizeof(key)));
    assert(!pgraph_vk_hybrid_key_matches(key, sizeof(key), other,
                                         sizeof(other)));
    assert(!pgraph_vk_hybrid_key_matches(key, sizeof(key), key,
                                         sizeof(key) - 1));
    assert(!pgraph_vk_hybrid_key_matches(NULL, 0, key, sizeof(key)));
}

int main(void)
{
    test_eight_readiness_combinations();
    test_queue_full_keeps_the_ready_fallback();
    test_matching_work_suppresses_duplicate_enqueues();
    test_backoff_expiry_and_attempt_saturation();
    test_work_transitions_are_guarded_and_bounded();
    test_queue_deferral_is_not_a_compile_attempt();
    test_queue_capacity_rearms_without_selection_epoch_progress();
    test_backoff_rejects_zero_and_saturates();
    test_compile_failure_rejects_stale_metadata_without_mutation();
    test_ticket_allocator_is_nonzero_monotonic_and_fails_closed();
    test_completion_metadata_requires_current_matching_ticket();
    test_selection_epoch_is_pure_and_skips_zero();
    test_source_identity_uses_stage_size_and_exact_bytes();
    test_recipe_key_identity_uses_size_and_exact_bytes();
    puts("hybrid policy metadata tests passed");
    return 0;
}

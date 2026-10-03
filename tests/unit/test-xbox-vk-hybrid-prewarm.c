/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/pgraph/vk/hybrid-prewarm.h"
#include "hw/xbox/nv2a/pgraph/vk/hybrid-prewarm-runtime.h"
#include "hw/xbox/nv2a/pgraph/vk/hybrid-family-codec.h"

typedef struct AttemptFixture {
    PGRAPHVkHybridPrewarmAttemptResult result;
    unsigned int calls;
    unsigned int a_calls;
    unsigned int b_calls;
    bool defer_a_once;
} AttemptFixture;

static PGRAPHVkHybridPrewarmAttemptResult attempt_one(
    void *opaque, const PGRAPHVkFamilyHistoryRecord *record)
{
    AttemptFixture *fixture = opaque;
    fixture->calls++;
    if (record->payload[0] == 'A') {
        fixture->a_calls++;
        if (fixture->defer_a_once && fixture->a_calls == 1) {
            return PGRAPH_VK_HYBRID_PREWARM_DEFERRED;
        }
    } else if (record->payload[0] == 'B') {
        fixture->b_calls++;
    }
    return fixture->result;
}

static void init_history(PGRAPHVkFamilyHistory *history)
{
    g_assert_true(pgraph_vk_family_history_init(history, 4));
    g_assert_true(pgraph_vk_family_history_note_cold_miss(
        history, "A", 1, 0));
}

static void test_demand_has_priority(void)
{
    PGRAPHVkFamilyHistory history;
    init_history(&history);
    PGRAPHVkHybridPrewarmState state = { .enabled = true };
    AttemptFixture fixture = { .result = PGRAPH_VK_HYBRID_PREWARM_SUBMITTED };

    g_assert_cmpint(pgraph_vk_hybrid_prewarm_service(
                        &state, &history, true, attempt_one, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_IDLE);
    g_assert_cmpuint(fixture.calls, ==, 0);
    g_assert_cmpuint(state.considered, ==, 0);
    pgraph_vk_family_history_destroy(&history);
}

static void test_in_flight_window_is_bounded(void)
{
    g_assert_true(pgraph_vk_hybrid_prewarm_can_admit(0, 1));
    g_assert_false(pgraph_vk_hybrid_prewarm_can_admit(1, 1));
    g_assert_true(pgraph_vk_hybrid_prewarm_can_admit(1, 2));
    g_assert_false(pgraph_vk_hybrid_prewarm_can_admit(2, 2));
    g_assert_true(pgraph_vk_hybrid_prewarm_can_admit(3, 4));
    g_assert_false(pgraph_vk_hybrid_prewarm_can_admit(4, 4));
    g_assert_false(pgraph_vk_hybrid_prewarm_can_admit(0, 0));
    g_assert_false(pgraph_vk_hybrid_prewarm_can_admit(0, 5));
}

static void test_one_candidate_per_service(void)
{
    PGRAPHVkFamilyHistory history;
    init_history(&history);
    PGRAPHVkHybridPrewarmState state = { .enabled = true };
    AttemptFixture fixture = { .result = PGRAPH_VK_HYBRID_PREWARM_SUBMITTED };

    g_assert_cmpint(pgraph_vk_hybrid_prewarm_service(
                        &state, &history, false, attempt_one, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_SUBMITTED);
    g_assert_cmpuint(fixture.calls, ==, 1);
    g_assert_cmpuint(state.considered, ==, 1);
    g_assert_cmpuint(state.attempted, ==, 1);
    g_assert_cmpuint(state.scheduled, ==, 1);
    g_assert_cmpuint(state.owner_attempts, ==, 1);
    pgraph_vk_family_history_destroy(&history);
}

static void test_launch_bound(void)
{
    PGRAPHVkFamilyHistory history;
    init_history(&history);
    PGRAPHVkHybridPrewarmState state = {
        .enabled = true,
        .considered = PGRAPH_VK_HYBRID_PREWARM_MAX_CANDIDATES,
        .attempted = PGRAPH_VK_HYBRID_PREWARM_MAX_CANDIDATES,
    };
    AttemptFixture fixture = { .result = PGRAPH_VK_HYBRID_PREWARM_SUBMITTED };

    g_assert_cmpint(pgraph_vk_hybrid_prewarm_service(
                        &state, &history, false, attempt_one, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_IDLE);
    g_assert_cmpuint(fixture.calls, ==, 0);
    pgraph_vk_family_history_destroy(&history);
}

static void test_outcome_accounting(void)
{
    const PGRAPHVkHybridPrewarmAttemptResult results[] = {
        PGRAPH_VK_HYBRID_PREWARM_READY,
        PGRAPH_VK_HYBRID_PREWARM_MISSING_ARTIFACT,
        PGRAPH_VK_HYBRID_PREWARM_REJECTED,
        PGRAPH_VK_HYBRID_PREWARM_DEFERRED,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(results); i++) {
        PGRAPHVkFamilyHistory history;
        init_history(&history);
        PGRAPHVkHybridPrewarmState state = { .enabled = true };
        AttemptFixture fixture = { .result = results[i] };
        g_assert_cmpint(pgraph_vk_hybrid_prewarm_service(
                            &state, &history, false, attempt_one, &fixture),
                        ==, results[i]);
        g_assert_cmpuint(state.considered, ==, 1);
        g_assert_cmpuint(fixture.calls, ==, 1);
        if (results[i] == PGRAPH_VK_HYBRID_PREWARM_DEFERRED) {
            g_assert_cmpuint(state.attempted, ==, 0);
            g_assert_cmpuint(state.deferred, ==, 1);
        } else {
            g_assert_cmpuint(state.attempted, ==, 1);
        }
        pgraph_vk_family_history_destroy(&history);
    }
}

static void test_deferred_candidate_does_not_starve_next(void)
{
    PGRAPHVkFamilyHistory history;
    init_history(&history);
    g_assert_true(pgraph_vk_family_history_note_cold_miss(
        &history, "A", 1, 0));
    g_assert_true(pgraph_vk_family_history_note_cold_miss(
        &history, "B", 1, 0));
    PGRAPHVkHybridPrewarmState state = { .enabled = true };
    AttemptFixture fixture = {
        .result = PGRAPH_VK_HYBRID_PREWARM_READY,
        .defer_a_once = true,
    };

    g_assert_cmpint(pgraph_vk_hybrid_prewarm_service(
                        &state, &history, false, attempt_one, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_DEFERRED);
    g_assert_cmpuint(fixture.a_calls, ==, 1);
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_service(
                        &state, &history, false, attempt_one, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_READY);
    g_assert_cmpuint(fixture.b_calls, ==, 1);
    for (unsigned int i = 0; i < 16 && fixture.a_calls == 1; i++) {
        pgraph_vk_hybrid_prewarm_service(
            &state, &history, false, attempt_one, &fixture);
    }
    g_assert_cmpuint(fixture.a_calls, ==, 2);
    g_assert_cmpuint(state.considered, ==, 2);
    g_assert_cmpuint(state.attempted, ==, 2);
    pgraph_vk_family_history_destroy(&history);
}

static void test_retry_is_bounded(void)
{
    PGRAPHVkFamilyHistory history;
    init_history(&history);
    PGRAPHVkHybridPrewarmState state = { .enabled = true };
    AttemptFixture fixture = { .result = PGRAPH_VK_HYBRID_PREWARM_DEFERRED };

    for (unsigned int i = 0; i < 128; i++) {
        pgraph_vk_hybrid_prewarm_service(
            &state, &history, false, attempt_one, &fixture);
    }
    g_assert_cmpuint(fixture.calls, ==, 3);
    g_assert_cmpuint(state.considered, ==, 1);
    g_assert_cmpuint(state.attempted, ==, 1);
    g_assert_cmpuint(state.retry_exhausted, ==, 1);
    pgraph_vk_family_history_destroy(&history);
}

typedef struct RuntimeFixture {
    PipelineKey submitted_key;
    ShaderBinding binding;
    bool published;
    bool device_accept;
    VkFormatFeatureFlags format_features;
    unsigned int format_queries;
    PGRAPHVkCachedFamilyModulesResult modules_result;
    PGRAPHVkHybridPrewarmAttemptResult retain_result;
    PipelineKey retained_key;
    unsigned int device_checks;
    unsigned int ready_probes;
    unsigned int module_preparations;
    unsigned int retained_families;
    unsigned int binding_preparations;
    unsigned int worker_submissions;
} RuntimeFixture;

static void runtime_format_properties(void *opaque, VkFormat format,
                                      VkFormatProperties *properties)
{
    RuntimeFixture *fixture = opaque;
    g_assert_cmpint(format, ==, VK_FORMAT_R8G8B8_UNORM);
    fixture->format_queries++;
    properties->bufferFeatures = fixture->format_features;
}

static bool runtime_device_supported(void *opaque, const PipelineKey *key)
{
    RuntimeFixture *fixture = opaque;
    fixture->device_checks++;
    return fixture->device_accept &&
           pgraph_vk_hybrid_prewarm_vertex_formats_supported(
               key, runtime_format_properties, fixture);
}

static bool runtime_pipeline_ready(void *opaque, const PipelineKey *key)
{
    RuntimeFixture *fixture = opaque;
    fixture->ready_probes++;
    return fixture->published &&
           memcmp(key, &fixture->submitted_key, sizeof(*key)) == 0;
}

static PGRAPHVkCachedFamilyModulesResult runtime_cached_modules(
    void *opaque, const ShaderState *state)
{
    RuntimeFixture *fixture = opaque;
    (void)state;
    fixture->module_preparations++;
    return fixture->modules_result;
}

static PGRAPHVkHybridPrewarmAttemptResult runtime_retain_missing_family(
    void *opaque, const PipelineKey *key)
{
    RuntimeFixture *fixture = opaque;
    fixture->retained_families++;
    fixture->retained_key = *key;
    return fixture->retain_result;
}

static ShaderBinding *runtime_ready_binding(void *opaque,
                                             const ShaderState *state)
{
    RuntimeFixture *fixture = opaque;
    (void)state;
    fixture->binding_preparations++;
    return &fixture->binding;
}

static PGRAPHVkHybridPipelineSubmitResult runtime_submit_pipeline(
    void *opaque, const PipelineKey *key, ShaderBinding *binding)
{
    RuntimeFixture *fixture = opaque;
    g_assert_true(binding == &fixture->binding);
    fixture->worker_submissions++;
    fixture->submitted_key = *key;
    return PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED;
}

static const PGRAPHVkHybridPrewarmPrepareOps runtime_ops = {
    .device_supported = runtime_device_supported,
    .pipeline_ready = runtime_pipeline_ready,
    .cached_modules = runtime_cached_modules,
    .retain_missing_family = runtime_retain_missing_family,
    .ready_binding = runtime_ready_binding,
    .submit_pipeline = runtime_submit_pipeline,
};

static PipelineKey runtime_key(void)
{
    PipelineKey key = { 0 };
    key.fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER;
    key.render_pass_state.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    key.shader_state.vsh.is_fixed_function = true;
    key.shader_state.geom.primitive_mode = PRIM_TYPE_TRIANGLES;
    key.shader_state.geom.polygon_front_mode = POLY_MODE_FILL;
    key.shader_state.geom.polygon_back_mode = POLY_MODE_FILL;
    key.shader_state.psh.surface_zeta_format =
        NV097_SET_SURFACE_FORMAT_ZETA_Z16;
    key.shader_state.psh.depth_format = DEPTH_FORMAT_D16;
    return key;
}

static void runtime_history(PGRAPHVkFamilyHistory *history,
                            const PipelineKey *key)
{
    PGRAPHVkFamilyKeyBlob blob = { 0 };
    g_assert_true(pgraph_vk_family_history_init(history, 2));
    g_assert_true(pgraph_vk_family_key_encode(key, &blob));
    g_assert_true(pgraph_vk_family_history_note(
        history, blob.data, blob.size));
    pgraph_vk_family_key_blob_destroy(&blob);
}

static void test_stored_recipe_to_published_pipeline(void)
{
    PipelineKey stored = runtime_key();
    stored.regs[0] = 0x01000000;
    PGRAPHVkFamilyHistory history;
    runtime_history(&history, &stored);
    RuntimeFixture fixture = {
        .device_accept = true,
        .modules_result = PGRAPH_VK_CACHED_FAMILY_MODULES_READY,
    };
    const PGRAPHVkFamilyHistoryRecord *record = &history.records[0];

    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        record, &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_SUBMITTED);
    g_assert_cmpuint(fixture.device_checks, ==, 1);
    g_assert_cmpuint(fixture.module_preparations, ==, 1);
    g_assert_cmpuint(fixture.binding_preparations, ==, 1);
    g_assert_cmpuint(fixture.worker_submissions, ==, 1);
    g_assert_cmpuint(fixture.submitted_key.regs[0], ==, stored.regs[0]);

    /* Controlled worker completion/publication: the next exact-key lookup
     * must be ready without another shader or worker request. */
    fixture.published = true;
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        record, &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_READY);
    g_assert_cmpuint(fixture.module_preparations, ==, 1);
    g_assert_cmpuint(fixture.worker_submissions, ==, 1);
    pgraph_vk_family_history_destroy(&history);
}

static void test_missing_artifact_transfers_to_retained_family(void)
{
    PipelineKey key = runtime_key();
    PGRAPHVkFamilyHistory history;
    runtime_history(&history, &key);
    RuntimeFixture fixture = {
        .device_accept = true,
        .modules_result = PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING,
        .retain_result = PGRAPH_VK_HYBRID_PREWARM_SUBMITTED,
    };
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        &history.records[0], &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_SUBMITTED);
    g_assert_cmpuint(fixture.retained_families, ==, 1);
    g_assert_cmpmem(&fixture.retained_key, sizeof(fixture.retained_key),
                    &key, sizeof(key));
    g_assert_cmpuint(fixture.binding_preparations, ==, 0);
    g_assert_cmpuint(fixture.worker_submissions, ==, 0);

    /* Registry pressure must defer the record instead of consuming it
     * without a live owner for the exact family. */
    fixture = (RuntimeFixture) {
        .device_accept = true,
        .modules_result = PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING,
        .retain_result = PGRAPH_VK_HYBRID_PREWARM_DEFERRED,
    };
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        &history.records[0], &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_DEFERRED);
    g_assert_cmpuint(fixture.retained_families, ==, 1);
    g_assert_cmpuint(fixture.binding_preparations, ==, 0);
    g_assert_cmpuint(fixture.worker_submissions, ==, 0);
    pgraph_vk_family_history_destroy(&history);
}

static void test_invalid_artifacts_stop_before_worker(void)
{
    PipelineKey key;
    PGRAPHVkFamilyHistory history;
    RuntimeFixture fixture;

    key = runtime_key();
    key.shader_state.vsh.is_fixed_function = false;
    key.shader_state.vsh.programmable.program_length = INT_MAX;
    runtime_history(&history, &key);
    fixture = (RuntimeFixture) {
        .device_accept = true,
        .modules_result = PGRAPH_VK_CACHED_FAMILY_MODULES_READY,
        .retain_result = PGRAPH_VK_HYBRID_PREWARM_SUBMITTED,
    };
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        &history.records[0], &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_REJECTED);
    g_assert_cmpuint(fixture.device_checks, ==, 0);
    g_assert_cmpuint(fixture.module_preparations, ==, 0);
    g_assert_cmpuint(fixture.worker_submissions, ==, 0);
    pgraph_vk_family_history_destroy(&history);

    key = runtime_key();
    runtime_history(&history, &key);
    fixture.device_accept = false;
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        &history.records[0], &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_REJECTED);
    g_assert_cmpuint(fixture.module_preparations, ==, 0);
    pgraph_vk_family_history_destroy(&history);
}

static void test_unsupported_vertex_format_stops_before_preparation(void)
{
    PipelineKey key = runtime_key();
    key.binding_description_count = 1;
    key.binding_descriptions[0] = (VkVertexInputBindingDescription) {
        .binding = 0,
        .stride = 3,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    key.attribute_description_count = 1;
    key.attribute_descriptions[0] = (VkVertexInputAttributeDescription) {
        .location = 0,
        .binding = 0,
        .format = VK_FORMAT_R8G8B8_UNORM,
        .offset = 0,
    };

    PGRAPHVkFamilyHistory history;
    runtime_history(&history, &key);
    RuntimeFixture fixture = {
        .device_accept = true,
        .modules_result = PGRAPH_VK_CACHED_FAMILY_MODULES_READY,
    };
    const PGRAPHVkFamilyHistoryRecord *record = &history.records[0];

    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        record, &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_REJECTED);
    g_assert_cmpuint(fixture.module_preparations, ==, 0);
    g_assert_cmpuint(fixture.binding_preparations, ==, 0);
    g_assert_cmpuint(fixture.worker_submissions, ==, 0);
    g_assert_cmpuint(fixture.format_queries, ==, 1);

    fixture.format_features = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_prepare_record(
                        record, &runtime_ops, &fixture),
                    ==, PGRAPH_VK_HYBRID_PREWARM_SUBMITTED);
    g_assert_cmpuint(fixture.module_preparations, ==, 1);
    g_assert_cmpuint(fixture.binding_preparations, ==, 1);
    g_assert_cmpuint(fixture.worker_submissions, ==, 1);
    g_assert_cmpuint(fixture.format_queries, ==, 2);
    pgraph_vk_family_history_destroy(&history);
}

static void test_published_pipeline_counts_first_demand(void)
{
    PGRAPHVkHybridPrewarmState state = { 0 };
    PipelineBinding binding = { 0 };

    pgraph_vk_hybrid_prewarm_note_publication(&state, &binding, true);
    g_assert_cmpuint(state.ready, ==, 1);
    g_assert_true(binding.prewarmed);

    pgraph_vk_hybrid_prewarm_note_demand(&state, &binding);
    g_assert_cmpuint(state.demand_hits, ==, 1);
    g_assert_false(binding.prewarmed);
    pgraph_vk_hybrid_prewarm_note_demand(&state, &binding);
    g_assert_cmpuint(state.demand_hits, ==, 1);
}

typedef struct StageFixture {
    PGRAPHVkHybridPrewarmStage stages[3];
    unsigned int count;
    PGRAPHVkHybridPrewarmStage fail_stage;
    PGRAPHVkCachedFamilyModulesResult failure;
} StageFixture;

static PGRAPHVkCachedFamilyModulesResult materialize_stage(
    void *opaque, PGRAPHVkHybridPrewarmStage stage)
{
    StageFixture *fixture = opaque;
    fixture->stages[fixture->count++] = stage;
    return stage == fixture->fail_stage ? fixture->failure :
           PGRAPH_VK_CACHED_FAMILY_MODULES_READY;
}

static void test_cached_stage_plan(void)
{
    StageFixture fixture = { .fail_stage = (PGRAPHVkHybridPrewarmStage)-1 };
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_modules(
                        false, materialize_stage, &fixture),
                    ==, PGRAPH_VK_CACHED_FAMILY_MODULES_READY);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpint(fixture.stages[0], ==, PGRAPH_VK_HYBRID_PREWARM_VERTEX);
    g_assert_cmpint(fixture.stages[1], ==, PGRAPH_VK_HYBRID_PREWARM_FRAGMENT);

    fixture = (StageFixture) { .fail_stage = (PGRAPHVkHybridPrewarmStage)-1 };
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_modules(
                        true, materialize_stage, &fixture),
                    ==, PGRAPH_VK_CACHED_FAMILY_MODULES_READY);
    g_assert_cmpuint(fixture.count, ==, 3);
    g_assert_cmpint(fixture.stages[1], ==, PGRAPH_VK_HYBRID_PREWARM_GEOMETRY);

    fixture = (StageFixture) {
        .fail_stage = PGRAPH_VK_HYBRID_PREWARM_GEOMETRY,
        .failure = PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING,
    };
    g_assert_cmpint(pgraph_vk_hybrid_prewarm_modules(
                        true, materialize_stage, &fixture),
                    ==, PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING);
    g_assert_cmpuint(fixture.count, ==, 2);
}

static void test_requested_stage_plan_visits_every_required_stage(void)
{
    StageFixture fixture = {
        .fail_stage = PGRAPH_VK_HYBRID_PREWARM_VERTEX,
        .failure = PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING,
    };

    g_assert_cmpint(pgraph_vk_hybrid_prepare_family_modules(
                        true, materialize_stage, &fixture),
                    ==, PGRAPH_VK_CACHED_FAMILY_MODULES_MISSING);
    g_assert_cmpuint(fixture.count, ==, 3);
    g_assert_cmpint(fixture.stages[0], ==,
                    PGRAPH_VK_HYBRID_PREWARM_VERTEX);
    g_assert_cmpint(fixture.stages[1], ==,
                    PGRAPH_VK_HYBRID_PREWARM_GEOMETRY);
    g_assert_cmpint(fixture.stages[2], ==,
                    PGRAPH_VK_HYBRID_PREWARM_FRAGMENT);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/demand-priority",
                    test_demand_has_priority);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/in-flight-window",
                    test_in_flight_window_is_bounded);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/one-per-service",
                    test_one_candidate_per_service);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/launch-bound", test_launch_bound);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/accounting",
                    test_outcome_accounting);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/deferred-progress",
                    test_deferred_candidate_does_not_starve_next);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/bounded-retry",
                    test_retry_is_bounded);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/recipe-publication",
                    test_stored_recipe_to_published_pipeline);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/missing-retained",
                    test_missing_artifact_transfers_to_retained_family);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/invalid-stops-before-worker",
                    test_invalid_artifacts_stop_before_worker);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/vertex-format-capability",
                    test_unsupported_vertex_format_stops_before_preparation);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/first-demand",
                    test_published_pipeline_counts_first_demand);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/cached-stage-plan",
                    test_cached_stage_plan);
    g_test_add_func("/nv2a/vk/hybrid-prewarm/requested-stage-plan",
                    test_requested_stage_plan_visits_every_required_stage);
    return g_test_run();
}

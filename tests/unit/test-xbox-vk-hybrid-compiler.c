/*
 * NV2A Vulkan hybrid compiler queue tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"

#include "hw/xbox/nv2a/pgraph/vk/hybrid-compiler.h"
#include "hw/xbox/nv2a/pgraph/vk/hybrid-policy.h"

typedef struct TestCompiler {
    QemuMutex lock;
    QemuCond started;
    QemuCond release;
    bool block;
    bool released;
    bool fail;
    unsigned int calls;
    unsigned int notifies;
    char call_order[16];
    char source[32];
    char config[32];
} TestCompiler;

static void test_notify(void *opaque)
{
    TestCompiler *compiler = opaque;
    qemu_mutex_lock(&compiler->lock);
    compiler->notifies++;
    qemu_mutex_unlock(&compiler->lock);
}

typedef struct BlockingSubmit {
    PGRAPHVkHybridCompiler *compiler;
    PGRAPHVkHybridCompileRequest request;
    bool submitted;
    PGRAPHVkHybridCompileResult result;
} BlockingSubmit;

static void test_compiler_init(TestCompiler *compiler, bool block)
{
    *compiler = (TestCompiler) { .block = block };
    qemu_mutex_init(&compiler->lock);
    qemu_cond_init(&compiler->started);
    qemu_cond_init(&compiler->release);
}

static void test_compiler_destroy(TestCompiler *compiler)
{
    qemu_cond_destroy(&compiler->release);
    qemu_cond_destroy(&compiler->started);
    qemu_mutex_destroy(&compiler->lock);
}

static bool test_compile(void *opaque,
                         const PGRAPHVkHybridCompileRequest *request,
                         uint8_t **spirv, size_t *spirv_size)
{
    TestCompiler *compiler = opaque;

    qemu_mutex_lock(&compiler->lock);
    compiler->calls++;
    compiler->call_order[compiler->calls - 1] =
        ((const char *)request->glsl)[0];
    g_assert_cmpuint(request->glsl_size, <, sizeof(compiler->source));
    g_assert_cmpuint(request->config_size, <, sizeof(compiler->config));
    memcpy(compiler->source, request->glsl, request->glsl_size);
    compiler->source[request->glsl_size] = '\0';
    memcpy(compiler->config, request->config, request->config_size);
    compiler->config[request->config_size] = '\0';
    qemu_cond_broadcast(&compiler->started);
    while (compiler->block && !compiler->released) {
        qemu_cond_wait(&compiler->release, &compiler->lock);
    }
    bool fail = compiler->fail;
    qemu_mutex_unlock(&compiler->lock);

    if (fail) {
        return false;
    }
    *spirv_size = request->glsl_size;
    *spirv = g_memdup2(request->glsl, request->glsl_size);
    return *spirv != NULL;
}

static void test_wait_for_calls(TestCompiler *compiler, unsigned int calls)
{
    qemu_mutex_lock(&compiler->lock);
    while (compiler->calls < calls) {
        qemu_cond_wait(&compiler->started, &compiler->lock);
    }
    qemu_mutex_unlock(&compiler->lock);
}

static void test_release(TestCompiler *compiler)
{
    qemu_mutex_lock(&compiler->lock);
    compiler->released = true;
    qemu_cond_broadcast(&compiler->release);
    qemu_mutex_unlock(&compiler->lock);
}

static PGRAPHVkHybridCompilerConfig test_config(TestCompiler *test,
                                                size_t jobs, size_t bytes)
{
    return (PGRAPHVkHybridCompilerConfig) {
        .max_async_jobs = jobs,
        .max_async_bytes = bytes,
        .compile = test_compile,
        .opaque = test,
        .notify = test_notify,
        .notify_opaque = test,
    };
}

static PGRAPHVkHybridCompileRequest test_request_stage(
    uint64_t generation, uint64_t ticket, uint32_t stage,
    const char *glsl, const char *config)
{
    return (PGRAPHVkHybridCompileRequest) {
        .generation = generation,
        .ticket = ticket,
        .stage = stage,
        .priority = PGRAPH_VK_HYBRID_PRIORITY_VISIBLE,
        .glsl = glsl,
        .glsl_size = strlen(glsl),
        .config = config,
        .config_size = strlen(config),
    };
}

static PGRAPHVkHybridCompileRequest test_request(
    uint64_t generation, uint64_t ticket, const char *glsl,
    const char *config);
static bool init_compiler(PGRAPHVkHybridCompiler *compiler,
                          TestCompiler *test, size_t jobs, size_t bytes);
static bool take_result(PGRAPHVkHybridCompiler *compiler,
                        PGRAPHVkHybridCompileResult *result);

static void test_visible_work_overtakes_queued_prewarm(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 3, 128));

    PGRAPHVkHybridCompileRequest active =
        test_request(1, 1, "active", "config");
    active.priority = PGRAPH_VK_HYBRID_PRIORITY_PREWARM;
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &active, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_wait_for_calls(&test, 1);

    PGRAPHVkHybridCompileRequest prewarm =
        test_request(1, 2, "background", "config");
    prewarm.priority = PGRAPH_VK_HYBRID_PRIORITY_PREWARM;
    PGRAPHVkHybridCompileRequest visible =
        test_request(1, 3, "consumer", "config");
    visible.priority = PGRAPH_VK_HYBRID_PRIORITY_VISIBLE;
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &prewarm, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &visible, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);

    test_release(&test);
    test_wait_for_calls(&test, 3);
    g_assert_cmpmem(test.call_order, 3, "acb", 3);
    for (unsigned int i = 0; i < 3; i++) {
        g_assert_true(take_result(&compiler, &result));
        pgraph_vk_hybrid_compile_result_destroy(&result);
    }
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_deduplicated_alias_promotes_one_ticket(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileIdentity owner;
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 3, 128));
    PGRAPHVkHybridCompileRequest active =
        test_request(1, 1, "active", "config");
    active.priority = PGRAPH_VK_HYBRID_PRIORITY_PREWARM;
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &active, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_wait_for_calls(&test, 1);

    PGRAPHVkHybridCompileRequest shared =
        test_request(1, 2, "shared", "config");
    shared.priority = PGRAPH_VK_HYBRID_PRIORITY_PREWARM;
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &shared, &owner),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    shared.generation = 2;
    shared.ticket = 99;
    shared.priority = PGRAPH_VK_HYBRID_PRIORITY_VISIBLE;
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &shared, &owner),
                    ==, PGRAPH_VK_HYBRID_COMPILER_DUPLICATE);
    g_assert_cmpuint(owner.generation, ==, 1);
    g_assert_cmpuint(owner.ticket, ==, 2);

    PGRAPHVkHybridCompileRequest background =
        test_request(1, 3, "z-last", "config");
    background.priority = PGRAPH_VK_HYBRID_PRIORITY_PREWARM;
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &background, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_release(&test);
    test_wait_for_calls(&test, 3);
    g_assert_cmpmem(test.call_order, 3, "asz", 3);

    for (unsigned int i = 0; i < 3; i++) {
        g_assert_true(take_result(&compiler, &result));
        if (result.ticket == 2) {
            g_assert_cmpint(result.priority, ==,
                            PGRAPH_VK_HYBRID_PRIORITY_VISIBLE);
        }
        pgraph_vk_hybrid_compile_result_destroy(&result);
    }
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static PGRAPHVkHybridCompileRequest test_request(uint64_t generation,
                                                  uint64_t ticket,
                                                  const char *glsl,
                                                  const char *config)
{
    return test_request_stage(generation, ticket, 16, glsl, config);
}

static bool init_compiler(PGRAPHVkHybridCompiler *compiler,
                          TestCompiler *test, size_t jobs, size_t bytes)
{
    PGRAPHVkHybridCompilerConfig config = test_config(test, jobs, bytes);

    return pgraph_vk_hybrid_compiler_init(compiler, &config);
}

static PGRAPHVkHybridCompilerSubmitResult submit_async(
    PGRAPHVkHybridCompiler *compiler, uint64_t generation, uint64_t ticket,
    const char *glsl, const char *config, PGRAPHVkHybridCompileIdentity *owner)
{
    PGRAPHVkHybridCompileRequest request =
        test_request(generation, ticket, glsl, config);

    return pgraph_vk_hybrid_compiler_submit_async(compiler, &request, owner);
}

static bool take_result(PGRAPHVkHybridCompiler *compiler,
                        PGRAPHVkHybridCompileResult *result)
{
    for (unsigned int i = 0; i < 1000; i++) {
        if (pgraph_vk_hybrid_compiler_take_result(compiler, result)) {
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

static void test_async_deduplicates_and_deep_owns_input(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    char glsl[] = "shader-a";
    char config[] = "config-a";
    PGRAPHVkHybridCompileRequest request =
        test_request(7, 19, glsl, config);
    PGRAPHVkHybridCompileResult result;
    PGRAPHVkHybridCompileIdentity owner;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 2, 64));
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(&compiler, &request,
                                                            &owner),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_cmpuint(owner.ticket, ==, 19);
    g_assert_cmpint(submit_async(&compiler, 8, 20, "shader-a", "config-a",
                                 &owner),
                    ==, PGRAPH_VK_HYBRID_COMPILER_DUPLICATE);
    g_assert_cmpuint(owner.generation, ==, 7);
    g_assert_cmpuint(owner.ticket, ==, 19);

    memcpy(glsl, "mutated!", sizeof(glsl));
    memcpy(config, "mutated!", sizeof(config));
    test_wait_for_calls(&test, 1);
    test_release(&test);

    g_assert_true(take_result(&compiler, &result));
    g_assert_true(result.success);
    g_assert_cmpuint(result.generation, ==, 7);
    g_assert_cmpuint(result.ticket, ==, 19);
    g_assert_cmpstr(test.source, ==, "shader-a");
    g_assert_cmpstr(test.config, ==, "config-a");
    g_assert_cmpmem(result.spirv, result.spirv_size, "shader-a", 8);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_async_limits_release_when_result_is_taken(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, false);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 8, 8),
                    ==, PGRAPH_VK_HYBRID_COMPILER_STOPPED);
    g_assert_true(init_compiler(&compiler, &test, 1, 16));
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 0, 8),
                    ==, PGRAPH_VK_HYBRID_COMPILER_INVALID);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 9, 8),
                    ==, PGRAPH_VK_HYBRID_COMPILER_BYTE_LIMIT);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 8, 8),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_true(pgraph_vk_hybrid_compiler_can_submit_async(&compiler, 8, 8));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "abcdefgh", "12345678",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_false(pgraph_vk_hybrid_compiler_can_submit_async(&compiler, 1, 1));
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 1, 1),
                    ==, PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL);
    g_assert_cmpint(submit_async(&compiler, 1, 2, "x", "y", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL);
    g_assert_true(take_result(&compiler, &result));
    pgraph_vk_hybrid_compile_result_destroy(&result);
    g_assert_true(pgraph_vk_hybrid_compiler_can_submit_async(&compiler, 8, 8));
    g_assert_cmpint(submit_async(&compiler, 1, 3, "123456789", "12345678",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_BYTE_LIMIT);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_aggregate_byte_pressure_is_retryable(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 2, 16));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "12345678", "1234",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_wait_for_calls(&test, 1);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 4, 4),
                    ==, PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL);
    g_assert_cmpint(submit_async(&compiler, 1, 2, "1234", "1234", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_probe_async(
                        &compiler, 9, 8),
                    ==, PGRAPH_VK_HYBRID_COMPILER_BYTE_LIMIT);

    test_release(&test);
    g_assert_true(take_result(&compiler, &result));
    pgraph_vk_hybrid_compile_result_destroy(&result);
    g_assert_cmpint(submit_async(&compiler, 1, 2, "1234", "1234", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_true(take_result(&compiler, &result));
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_completion_notifies_after_publication(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, false);
    g_assert_true(init_compiler(&compiler, &test, 1, 32));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "shader", "config", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_true(take_result(&compiler, &result));
    for (unsigned int i = 0; i < 1000; i++) {
        qemu_mutex_lock(&test.lock);
        bool notified = test.notifies != 0;
        qemu_mutex_unlock(&test.lock);
        if (notified) {
            break;
        }
        g_usleep(1000);
    }
    qemu_mutex_lock(&test.lock);
    g_assert_cmpuint(test.notifies, ==, 1);
    qemu_mutex_unlock(&test.lock);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_retained_graphics_stages_resume_at_fixed_epoch(void)
{
    static const uint32_t stages[] = { 1, 2, 3 };
    static const char *sources[] = { "vertex", "geometry", "fragment" };
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;
    PGRAPHVkHybridWork work[G_N_ELEMENTS(stages)];
    const uint64_t frozen_epoch = 30;

    test_compiler_init(&test, false);
    g_assert_true(init_compiler(&compiler, &test, 1, 32));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "occupant", "config",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);

    for (size_t i = 0; i < G_N_ELEMENTS(work); i++) {
        g_assert_true(pgraph_vk_hybrid_work_init(&work[i], 3));
        g_assert_true(pgraph_vk_hybrid_note_queue_deferral(
            &work[i], false, 1, frozen_epoch, 8));
        g_assert_false(pgraph_vk_hybrid_rearm_queue_deferral(
            &work[i], pgraph_vk_hybrid_compiler_can_submit_async(
                          &compiler, strlen(sources[i]), strlen("config"))));
    }

    g_assert_true(take_result(&compiler, &result));
    pgraph_vk_hybrid_compile_result_destroy(&result);

    for (size_t i = 0; i < G_N_ELEMENTS(work); i++) {
        PGRAPHVkHybridCompileRequest request = test_request_stage(
            1, i + 2, stages[i], sources[i], "config");
        PGRAPHVkHybridCompileIdentity owner = { 0 };

        g_assert_true(pgraph_vk_hybrid_rearm_queue_deferral(
            &work[i], pgraph_vk_hybrid_compiler_can_submit_async(
                          &compiler, request.glsl_size,
                          request.config_size)));
        g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                            &compiler, &request, &owner),
                        ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
        g_assert_true(pgraph_vk_hybrid_mark_pending(
            &work[i], false, owner.generation, owner.ticket, frozen_epoch));
        g_assert_true(take_result(&compiler, &result));
        g_assert_cmpuint(result.stage, ==, stages[i]);
        pgraph_vk_hybrid_compile_result_destroy(&result);
    }

    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_dedup_keeps_different_immutable_configs_distinct(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, false);
    g_assert_true(init_compiler(&compiler, &test, 2, 64));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "same-source", "config-a",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_cmpint(submit_async(&compiler, 2, 2, "same-source", "config-b",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_true(take_result(&compiler, &result));
    g_assert_cmpuint(result.ticket, ==, 1);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    g_assert_true(take_result(&compiler, &result));
    g_assert_cmpuint(result.ticket, ==, 2);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_async_preserves_stage_identity(void)
{
    static const struct {
        uint64_t generation;
        uint64_t ticket;
        uint32_t stage;
        const char *source;
    } requests[] = {
        { 11, 21, 1, "same-source" },
        { 12, 22, 2, "same-source" },
        { 13, 23, 3, "fragment-source" },
    };
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, false);
    g_assert_true(init_compiler(&compiler, &test, ARRAY_SIZE(requests), 128));
    for (size_t i = 0; i < ARRAY_SIZE(requests); i++) {
        PGRAPHVkHybridCompileRequest request = test_request_stage(
            requests[i].generation, requests[i].ticket, requests[i].stage,
            requests[i].source, "config");
        g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                            &compiler, &request, NULL),
                        ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    }

    for (size_t i = 0; i < ARRAY_SIZE(requests); i++) {
        g_assert_true(take_result(&compiler, &result));
        g_assert_cmpuint(result.generation, ==, requests[i].generation);
        g_assert_cmpuint(result.ticket, ==, requests[i].ticket);
        g_assert_cmpuint(result.stage, ==, requests[i].stage);
        pgraph_vk_hybrid_compile_result_destroy(&result);
    }

    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void *test_submit_blocking(void *opaque)
{
    BlockingSubmit *submit = opaque;

    submit->submitted = pgraph_vk_hybrid_compiler_submit_blocking(
        submit->compiler, &submit->request, &submit->result);
    return NULL;
}

static void test_async_matching_blocking_work_keeps_its_own_result(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    BlockingSubmit blocking = { 0 };
    QemuThread thread;
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 1, 64));
    blocking.compiler = &compiler;
    blocking.request = test_request(3, 4, "same-source", "config");
    qemu_thread_create(&thread, "hybrid-compiler-test", test_submit_blocking,
                       &blocking, QEMU_THREAD_JOINABLE);
    test_wait_for_calls(&test, 1);

    g_assert_cmpint(submit_async(&compiler, 5, 6, "same-source", "config",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_release(&test);
    g_assert_null(qemu_thread_join(&thread));
    g_assert_true(blocking.submitted);
    g_assert_true(blocking.result.success);
    pgraph_vk_hybrid_compile_result_destroy(&blocking.result);
    test_wait_for_calls(&test, 2);

    g_assert_true(take_result(&compiler, &result));
    g_assert_cmpuint(result.generation, ==, 5);
    g_assert_cmpuint(result.ticket, ==, 6);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_blocking_slot_is_reserved_from_async_limits(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    BlockingSubmit blocking = { 0 };
    QemuThread thread;
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 1, 16));
    g_assert_cmpint(submit_async(&compiler, 3, 4, "abcdefgh", "12345678",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_wait_for_calls(&test, 1);

    blocking.compiler = &compiler;
    blocking.request = test_request(5, 6, "abcdefgh", "12345678");
    qemu_thread_create(&thread, "hybrid-compiler-test", test_submit_blocking,
                       &blocking, QEMU_THREAD_JOINABLE);
    test_release(&test);
    g_assert_null(qemu_thread_join(&thread));
    g_assert_true(blocking.submitted);
    g_assert_true(blocking.result.success);
    test_wait_for_calls(&test, 2);
    g_assert_cmpuint(blocking.result.generation, ==, 5);
    g_assert_cmpuint(blocking.result.ticket, ==, 6);
    pgraph_vk_hybrid_compile_result_destroy(&blocking.result);

    g_assert_true(pgraph_vk_hybrid_compiler_has_result(&compiler));
    g_assert_true(take_result(&compiler, &result));
    g_assert_cmpuint(result.ticket, ==, 4);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    g_assert_false(pgraph_vk_hybrid_compiler_has_result(&compiler));
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_required_compile_starts_while_async_is_active(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    BlockingSubmit blocking = { 0 };
    QemuThread thread;
    PGRAPHVkHybridCompileResult result;
    bool started;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 1, 64));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "background", "config",
                                 NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_wait_for_calls(&test, 1);

    blocking.compiler = &compiler;
    blocking.request = test_request(1, 2, "required", "config");
    qemu_thread_create(&thread, "hybrid-compiler-required-test",
                       test_submit_blocking, &blocking, QEMU_THREAD_JOINABLE);

    qemu_mutex_lock(&test.lock);
    while (test.calls < 2 &&
           qemu_cond_timedwait(&test.started, &test.lock, 1000)) {
    }
    started = test.calls >= 2;
    qemu_mutex_unlock(&test.lock);

    test_release(&test);
    g_assert_null(qemu_thread_join(&thread));
    g_assert_true(blocking.submitted);
    g_assert_true(blocking.result.success);
    pgraph_vk_hybrid_compile_result_destroy(&blocking.result);
    g_assert_true(take_result(&compiler, &result));
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
    g_assert_true(started);
}

static void test_compile_failure_is_returned_with_owned_empty_artifact(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, false);
    test.fail = true;
    g_assert_true(init_compiler(&compiler, &test, 1, 64));
    g_assert_cmpint(submit_async(&compiler, 9, 11, "bad", "config", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_true(take_result(&compiler, &result));
    g_assert_false(result.success);
    g_assert_null(result.spirv);
    g_assert_cmpuint(result.spirv_size, ==, 0);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_stop_join_releases_idle_busy_and_full_queues(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler idle = { 0 };
    PGRAPHVkHybridCompiler compiler = { 0 };

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&idle, &test, 1, 64));
    pgraph_vk_hybrid_compiler_stop(&idle);
    pgraph_vk_hybrid_compiler_join(&idle);
    pgraph_vk_hybrid_compiler_destroy(&idle);

    g_assert_true(init_compiler(&compiler, &test, 1, 64));
    g_assert_cmpint(submit_async(&compiler, 1, 1, "busy", "config", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    test_wait_for_calls(&test, 1);
    g_assert_cmpint(submit_async(&compiler, 1, 2, "full", "config", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL);
    pgraph_vk_hybrid_compiler_stop(&compiler);
    g_assert_cmpint(submit_async(&compiler, 1, 3, "stopped", "config", NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_STOPPED);
    test_release(&test);
    pgraph_vk_hybrid_compiler_join(&compiler);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_stop_waits_for_active_required_compile(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    BlockingSubmit blocking = { 0 };
    QemuThread thread;

    test_compiler_init(&test, true);
    g_assert_true(init_compiler(&compiler, &test, 1, 64));
    blocking.compiler = &compiler;
    blocking.request = test_request(1, 2, "required", "config");
    qemu_thread_create(&thread, "hybrid-compiler-stop-test",
                       test_submit_blocking, &blocking, QEMU_THREAD_JOINABLE);
    test_wait_for_calls(&test, 1);

    pgraph_vk_hybrid_compiler_stop(&compiler);
    test_release(&test);
    g_assert_null(qemu_thread_join(&thread));
    g_assert_false(blocking.submitted);
    g_assert_null(blocking.result.spirv);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

static void test_stop_discards_active_and_queued_graphics_stages(void)
{
    static const uint32_t stages[] = { 1, 2, 3 };

    for (size_t active = 0; active < ARRAY_SIZE(stages); active++) {
        TestCompiler test;
        PGRAPHVkHybridCompiler compiler = { 0 };
        PGRAPHVkHybridCompileResult result;

        test_compiler_init(&test, true);
        g_assert_true(init_compiler(&compiler, &test, ARRAY_SIZE(stages),
                                    128));

        PGRAPHVkHybridCompileRequest request = test_request_stage(
            1, 1, stages[active], "active", "config");
        g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                            &compiler, &request, NULL),
                        ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
        test_wait_for_calls(&test, 1);

        for (size_t queued = 1; queued < ARRAY_SIZE(stages); queued++) {
            size_t index = (active + queued) % ARRAY_SIZE(stages);
            request = test_request_stage(1, queued + 1, stages[index],
                                         queued == 1 ? "queued-a" :
                                                       "queued-b",
                                         "config");
            g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                                &compiler, &request, NULL),
                            ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
        }

        pgraph_vk_hybrid_compiler_stop(&compiler);
        for (size_t stage = 0; stage < ARRAY_SIZE(stages); stage++) {
            request = test_request_stage(2, stage + 4, stages[stage],
                                         "after-stop", "config");
            g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                                &compiler, &request, NULL),
                            ==, PGRAPH_VK_HYBRID_COMPILER_STOPPED);
        }

        test_release(&test);
        pgraph_vk_hybrid_compiler_join(&compiler);
        g_assert_cmpuint(test.calls, ==, 1);
        g_assert_false(pgraph_vk_hybrid_compiler_has_result(&compiler));
        g_assert_false(pgraph_vk_hybrid_compiler_take_result(&compiler,
                                                              &result));
        pgraph_vk_hybrid_compiler_destroy(&compiler);
        test_compiler_destroy(&test);
    }
}

static void test_targeted_result_bypasses_unrelated_completion(void)
{
    TestCompiler test;
    PGRAPHVkHybridCompiler compiler = { 0 };
    PGRAPHVkHybridCompileResult result;

    test_compiler_init(&test, false);
    g_assert_true(init_compiler(&compiler, &test, 2, 128));
    PGRAPHVkHybridCompileRequest first = test_request(
        1, 91, "first", "config");
    PGRAPHVkHybridCompileRequest demanded = test_request(
        1, 92, "demanded", "config");
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &first, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);
    g_assert_cmpint(pgraph_vk_hybrid_compiler_submit_async(
                        &compiler, &demanded, NULL),
                    ==, PGRAPH_VK_HYBRID_COMPILER_ACCEPTED);

    bool taken = false;
    for (unsigned int i = 0; i < 5000 && !taken; i++) {
        taken = pgraph_vk_hybrid_compiler_take_result_for(
            &compiler, 1, 92, &result);
        if (!taken) {
            g_usleep(1000);
        }
    }
    g_assert_true(taken);
    g_assert_cmpuint(result.ticket, ==, 92);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    g_assert_true(take_result(&compiler, &result));
    g_assert_cmpuint(result.ticket, ==, 91);
    pgraph_vk_hybrid_compile_result_destroy(&result);
    pgraph_vk_hybrid_compiler_destroy(&compiler);
    test_compiler_destroy(&test);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/hybrid-compiler/dedup-deep-copy",
                    test_async_deduplicates_and_deep_owns_input);
    g_test_add_func("/xbox/vk/hybrid-compiler/priority-order",
                    test_visible_work_overtakes_queued_prewarm);
    g_test_add_func("/xbox/vk/hybrid-compiler/dedup-priority-promotion",
                    test_deduplicated_alias_promotes_one_ticket);
    g_test_add_func("/xbox/vk/hybrid-compiler/async-limits",
                    test_async_limits_release_when_result_is_taken);
    g_test_add_func("/xbox/vk/hybrid-compiler/aggregate-byte-pressure",
                    test_aggregate_byte_pressure_is_retryable);
    g_test_add_func("/xbox/vk/hybrid-compiler/completion-notify",
                    test_completion_notifies_after_publication);
    g_test_add_func("/xbox/vk/hybrid-compiler/targeted-result",
                    test_targeted_result_bypasses_unrelated_completion);
    g_test_add_func("/xbox/vk/hybrid-compiler/fixed-epoch-retained-stages",
                    test_retained_graphics_stages_resume_at_fixed_epoch);
    g_test_add_func("/xbox/vk/hybrid-compiler/exact-config",
                    test_dedup_keeps_different_immutable_configs_distinct);
    g_test_add_func("/xbox/vk/hybrid-compiler/stage-identity",
                    test_async_preserves_stage_identity);
    g_test_add_func("/xbox/vk/hybrid-compiler/async-after-blocking",
                    test_async_matching_blocking_work_keeps_its_own_result);
    g_test_add_func("/xbox/vk/hybrid-compiler/reserved-blocking",
                    test_blocking_slot_is_reserved_from_async_limits);
    g_test_add_func("/xbox/vk/hybrid-compiler/required-starts-before-async-finishes",
                    test_required_compile_starts_while_async_is_active);
    g_test_add_func("/xbox/vk/hybrid-compiler/failure-result",
                    test_compile_failure_is_returned_with_owned_empty_artifact);
    g_test_add_func("/xbox/vk/hybrid-compiler/stop-join",
                    test_stop_join_releases_idle_busy_and_full_queues);
    g_test_add_func("/xbox/vk/hybrid-compiler/stop-active-required",
                    test_stop_waits_for_active_required_compile);
    g_test_add_func("/xbox/vk/hybrid-compiler/stop-all-graphics-stages",
                    test_stop_discards_active_and_queued_graphics_stages);
    return g_test_run();
}

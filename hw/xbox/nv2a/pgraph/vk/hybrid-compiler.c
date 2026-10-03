/*
 * NV2A Vulkan hybrid compiler queue
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/thread.h"

#include "hw/xbox/nv2a/pgraph/vk/hybrid-compiler.h"

typedef struct HybridCompilerJob {
    struct HybridCompilerJob *next;
    PGRAPHVkHybridCompileRequest request;
    uint8_t *glsl;
    uint8_t *config;
    size_t bytes;
    bool blocking;
    bool done;
    bool cancelled;
    bool success;
    uint8_t *spirv;
    size_t spirv_size;
    uint64_t submitted_us;
    uint64_t started_us;
    uint64_t finished_us;
    bool speculative_active_at_start;
} HybridCompilerJob;

typedef struct HybridCompilerState {
    QemuMutex lock;
    QemuCond work_ready;
    QemuCond blocking_ready;
    QemuCond blocking_done;
    QemuThread worker;
    QemuThread blocking_worker;
    PGRAPHVkHybridCompilerConfig config;
    HybridCompilerJob *async_head;
    HybridCompilerJob *async_tail;
    HybridCompilerJob *result_head;
    HybridCompilerJob *result_tail;
    HybridCompilerJob *active;
    HybridCompilerJob *active_blocking;
    HybridCompilerJob *blocking;
    size_t async_jobs;
    size_t async_bytes;
    int result_available;
    bool stopping;
    bool joined;
} HybridCompilerState;

static bool request_valid(const PGRAPHVkHybridCompileRequest *request)
{
    return request && request->ticket && request->glsl && request->glsl_size &&
           (!request->config_size || request->config) &&
           request->glsl_size <= SIZE_MAX - request->config_size;
}

static HybridCompilerJob *job_new(const PGRAPHVkHybridCompileRequest *request,
                                  bool blocking)
{
    HybridCompilerJob *job;

    if (!request_valid(request)) {
        return NULL;
    }
    job = g_new0(HybridCompilerJob, 1);
    job->glsl = g_memdup2(request->glsl, request->glsl_size);
    if (request->config_size) {
        job->config = g_memdup2(request->config, request->config_size);
    }
    if (!job->glsl || (request->config_size && !job->config)) {
        g_free(job->glsl);
        g_free(job->config);
        g_free(job);
        return NULL;
    }
    job->request = *request;
    job->request.glsl = job->glsl;
    job->request.config = job->config;
    job->bytes = request->glsl_size + request->config_size;
    job->blocking = blocking;
    job->submitted_us = g_get_monotonic_time();
    return job;
}

static void job_destroy(HybridCompilerJob *job)
{
    if (!job) {
        return;
    }
    g_free(job->spirv);
    g_free(job->config);
    g_free(job->glsl);
    g_free(job);
}

static bool job_matches_request(const HybridCompilerJob *job,
                                const PGRAPHVkHybridCompileRequest *request)
{
    return job->request.stage == request->stage &&
           job->request.glsl_size == request->glsl_size &&
           job->request.config_size == request->config_size &&
           !memcmp(job->glsl, request->glsl, request->glsl_size) &&
           (!request->config_size ||
            !memcmp(job->config, request->config, request->config_size));
}

static HybridCompilerJob *find_in_list(
    HybridCompilerJob *head, const PGRAPHVkHybridCompileRequest *request)
{
    for (HybridCompilerJob *job = head; job; job = job->next) {
        if (job_matches_request(job, request)) {
            return job;
        }
    }
    return NULL;
}

static HybridCompilerJob *find_matching_async_job(
    HybridCompilerState *state, const PGRAPHVkHybridCompileRequest *request)
{
    if (state->active && !state->active->blocking &&
        job_matches_request(state->active, request)) {
        return state->active;
    }
    HybridCompilerJob *job = find_in_list(state->async_head, request);
    return job ? job : find_in_list(state->result_head, request);
}

static bool async_has_capacity(const HybridCompilerState *state,
                               size_t bytes)
{
    return !state->stopping &&
           state->async_jobs < state->config.max_async_jobs &&
           bytes <= state->config.max_async_bytes - state->async_bytes;
}

static void async_account_release(HybridCompilerState *state,
                                  HybridCompilerJob *job)
{
    assert(!job->blocking);
    assert(state->async_jobs);
    assert(state->async_bytes >= job->bytes);
    state->async_jobs--;
    state->async_bytes -= job->bytes;
}

static void async_list_destroy(HybridCompilerState *state,
                               HybridCompilerJob **head,
                               HybridCompilerJob **tail)
{
    HybridCompilerJob *job = *head;

    while (job) {
        HybridCompilerJob *next = job->next;
        async_account_release(state, job);
        job_destroy(job);
        job = next;
    }
    *head = NULL;
    *tail = NULL;
}

static HybridCompilerJob *list_pop_highest_priority(
    HybridCompilerJob **head, HybridCompilerJob **tail)
{
    HybridCompilerJob *job = *head;

    if (!job) {
        return NULL;
    }

    HybridCompilerJob *best = job;
    HybridCompilerJob *best_prev = NULL;
    HybridCompilerJob *prev = job;
    for (HybridCompilerJob *candidate = job->next; candidate;
         candidate = candidate->next) {
        if (candidate->request.priority > best->request.priority) {
            best = candidate;
            best_prev = prev;
        }
        prev = candidate;
    }
    if (best_prev) {
        best_prev->next = best->next;
    } else {
        *head = best->next;
    }
    if (*tail == best) {
        *tail = best_prev;
    }
    best->next = NULL;
    return best;
}

static HybridCompilerJob *result_take_identity(
    HybridCompilerState *state, uint64_t generation, uint64_t ticket)
{
    HybridCompilerJob *previous = NULL;
    for (HybridCompilerJob *job = state->result_head; job;
         job = job->next) {
        if (job->request.generation != generation ||
            job->request.ticket != ticket) {
            previous = job;
            continue;
        }
        if (previous) {
            previous->next = job->next;
        } else {
            state->result_head = job->next;
        }
        if (state->result_tail == job) {
            state->result_tail = previous;
        }
        job->next = NULL;
        return job;
    }
    return NULL;
}

static void result_append(HybridCompilerState *state, HybridCompilerJob *job)
{
    assert(!job->next);
    if (state->result_tail) {
        state->result_tail->next = job;
    } else {
        state->result_head = job;
    }
    state->result_tail = job;
    qatomic_set(&state->result_available, true);
}

static void *hybrid_compiler_worker(void *opaque)
{
    HybridCompilerState *state = opaque;

    for (;;) {
        HybridCompilerJob *job;
        uint8_t *spirv = NULL;
        size_t spirv_size = 0;
        bool success;

        qemu_mutex_lock(&state->lock);
        while (!state->stopping && !state->async_head) {
            qemu_cond_wait(&state->work_ready, &state->lock);
        }
        if (state->stopping) {
            qemu_mutex_unlock(&state->lock);
            break;
        }
        job = list_pop_highest_priority(&state->async_head,
                                        &state->async_tail);
        state->active = job;
        job->started_us = g_get_monotonic_time();
        qemu_mutex_unlock(&state->lock);

        success = state->config.compile(state->config.opaque, &job->request,
                                        &spirv, &spirv_size);
        if (!success || !spirv || !spirv_size) {
            g_free(spirv);
            spirv = NULL;
            spirv_size = 0;
            success = false;
        }

        qemu_mutex_lock(&state->lock);
        job->finished_us = g_get_monotonic_time();
        state->active = NULL;
        job->success = success;
        job->spirv = spirv;
        job->spirv_size = spirv_size;
        bool notify = false;
        if (state->stopping) {
            async_account_release(state, job);
            job_destroy(job);
        } else {
            result_append(state, job);
            notify = true;
        }
        bool stopping = state->stopping;
        qemu_mutex_unlock(&state->lock);
        if (notify && state->config.notify) {
            state->config.notify(state->config.notify_opaque);
        }
        if (stopping) {
            break;
        }
    }

    return NULL;
}

/* Required draw dependencies must not wait for an active speculative
 * compilation. This lane has no asynchronous work and is independent of
 * the bounded background queue. */
static void *hybrid_compiler_blocking_worker(void *opaque)
{
    HybridCompilerState *state = opaque;

    for (;;) {
        HybridCompilerJob *job;
        uint8_t *spirv = NULL;
        size_t spirv_size = 0;
        bool success;

        qemu_mutex_lock(&state->lock);
        while (!state->stopping && !state->blocking) {
            qemu_cond_wait(&state->blocking_ready, &state->lock);
        }
        if (state->stopping) {
            qemu_mutex_unlock(&state->lock);
            break;
        }
        job = state->blocking;
        state->active_blocking = job;
        job->started_us = g_get_monotonic_time();
        job->speculative_active_at_start = state->active != NULL;
        qemu_mutex_unlock(&state->lock);

        success = state->config.compile(state->config.opaque, &job->request,
                                        &spirv, &spirv_size);
        if (!success || !spirv || !spirv_size) {
            g_free(spirv);
            spirv = NULL;
            spirv_size = 0;
            success = false;
        }

        qemu_mutex_lock(&state->lock);
        job->finished_us = g_get_monotonic_time();
        state->active_blocking = NULL;
        state->blocking = NULL;
        job->success = success;
        job->spirv = spirv;
        job->spirv_size = spirv_size;
        job->done = true;
        qemu_cond_broadcast(&state->blocking_done);
        bool stopping = state->stopping;
        qemu_mutex_unlock(&state->lock);
        if (stopping) {
            break;
        }
    }

    return NULL;
}

bool pgraph_vk_hybrid_compiler_init(
    PGRAPHVkHybridCompiler *compiler,
    const PGRAPHVkHybridCompilerConfig *config)
{
    HybridCompilerState *state;

    if (!compiler || compiler->state || !config || !config->max_async_jobs ||
        !config->max_async_bytes || !config->compile) {
        return false;
    }

    state = g_new0(HybridCompilerState, 1);
    state->config = *config;
    qemu_mutex_init(&state->lock);
    qemu_cond_init(&state->work_ready);
    qemu_cond_init(&state->blocking_ready);
    qemu_cond_init(&state->blocking_done);
    qemu_thread_create(&state->worker, "vk-hybrid-compiler",
                       hybrid_compiler_worker, state, QEMU_THREAD_JOINABLE);
    qemu_thread_create(&state->blocking_worker, "vk-hybrid-required",
                       hybrid_compiler_blocking_worker, state,
                       QEMU_THREAD_JOINABLE);
    compiler->state = state;
    return true;
}

bool pgraph_vk_hybrid_compiler_can_submit_async(
    PGRAPHVkHybridCompiler *compiler, size_t glsl_size, size_t config_size)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    bool result;

    if (!state || !glsl_size || glsl_size > SIZE_MAX - config_size) {
        return false;
    }
    qemu_mutex_lock(&state->lock);
    result = async_has_capacity(state, glsl_size + config_size);
    qemu_mutex_unlock(&state->lock);
    return result;
}

PGRAPHVkHybridCompilerSubmitResult pgraph_vk_hybrid_compiler_probe_async(
    PGRAPHVkHybridCompiler *compiler, size_t glsl_size, size_t config_size)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    PGRAPHVkHybridCompilerSubmitResult result;

    if (!state) {
        return PGRAPH_VK_HYBRID_COMPILER_STOPPED;
    }
    if (!glsl_size || glsl_size > SIZE_MAX - config_size) {
        return PGRAPH_VK_HYBRID_COMPILER_INVALID;
    }

    size_t bytes = glsl_size + config_size;
    qemu_mutex_lock(&state->lock);
    if (state->stopping) {
        result = PGRAPH_VK_HYBRID_COMPILER_STOPPED;
    } else if (bytes > state->config.max_async_bytes) {
        result = PGRAPH_VK_HYBRID_COMPILER_BYTE_LIMIT;
    } else if (state->async_jobs == state->config.max_async_jobs) {
        result = PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL;
    } else if (bytes > state->config.max_async_bytes - state->async_bytes) {
        result = PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL;
    } else {
        result = PGRAPH_VK_HYBRID_COMPILER_ACCEPTED;
    }
    qemu_mutex_unlock(&state->lock);
    return result;
}

PGRAPHVkHybridCompilerSubmitResult pgraph_vk_hybrid_compiler_submit_async(
    PGRAPHVkHybridCompiler *compiler,
    const PGRAPHVkHybridCompileRequest *request,
    PGRAPHVkHybridCompileIdentity *owner)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    HybridCompilerJob *job;
    PGRAPHVkHybridCompilerSubmitResult result;

    if (owner) {
        *owner = (PGRAPHVkHybridCompileIdentity) { 0 };
    }
    if (!state || !request_valid(request)) {
        return PGRAPH_VK_HYBRID_COMPILER_INVALID;
    }
    job = job_new(request, false);
    if (!job) {
        return PGRAPH_VK_HYBRID_COMPILER_INVALID;
    }

    qemu_mutex_lock(&state->lock);
    if (state->stopping) {
        result = PGRAPH_VK_HYBRID_COMPILER_STOPPED;
    } else if (find_matching_async_job(state, request)) {
        result = PGRAPH_VK_HYBRID_COMPILER_DUPLICATE;
        HybridCompilerJob *existing = find_matching_async_job(state, request);
        if (request->priority > existing->request.priority) {
            existing->request.priority = request->priority;
        }
        if (owner) {
            *owner = (PGRAPHVkHybridCompileIdentity) {
                .generation = existing->request.generation,
                .ticket = existing->request.ticket,
            };
        }
    } else if (job->bytes > state->config.max_async_bytes) {
        result = PGRAPH_VK_HYBRID_COMPILER_BYTE_LIMIT;
    } else if (state->async_jobs == state->config.max_async_jobs) {
        result = PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL;
    } else if (job->bytes > state->config.max_async_bytes -
               state->async_bytes) {
        result = PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL;
    } else {
        if (state->async_tail) {
            state->async_tail->next = job;
        } else {
            state->async_head = job;
        }
        state->async_tail = job;
        state->async_jobs++;
        state->async_bytes += job->bytes;
        if (owner) {
            *owner = (PGRAPHVkHybridCompileIdentity) {
                .generation = request->generation,
                .ticket = request->ticket,
            };
        }
        qemu_cond_signal(&state->work_ready);
        qemu_mutex_unlock(&state->lock);
        return PGRAPH_VK_HYBRID_COMPILER_ACCEPTED;
    }
    qemu_mutex_unlock(&state->lock);
    job_destroy(job);
    return result;
}

bool pgraph_vk_hybrid_compiler_promote(
    PGRAPHVkHybridCompiler *compiler, uint64_t generation, uint64_t ticket,
    PGRAPHVkHybridPriority priority)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    if (!state || !ticket) {
        return false;
    }

    bool found = false;
    qemu_mutex_lock(&state->lock);
    HybridCompilerJob *lists[] = {
        state->async_head, state->active, state->result_head,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(lists) && !found; i++) {
        for (HybridCompilerJob *job = lists[i]; job; job = job->next) {
            if (job->request.generation == generation &&
                job->request.ticket == ticket) {
                if (priority > job->request.priority) {
                    job->request.priority = priority;
                }
                found = true;
                break;
            }
        }
    }
    qemu_mutex_unlock(&state->lock);
    return found;
}

bool pgraph_vk_hybrid_compiler_submit_blocking(
    PGRAPHVkHybridCompiler *compiler,
    const PGRAPHVkHybridCompileRequest *request,
    PGRAPHVkHybridCompileResult *result)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    HybridCompilerJob *job;
    bool success;

    if (result) {
        *result = (PGRAPHVkHybridCompileResult) { 0 };
    }
    if (!state || !result || !request_valid(request)) {
        return false;
    }
    job = job_new(request, true);
    if (!job) {
        return false;
    }

    qemu_mutex_lock(&state->lock);
    while (!state->stopping && state->blocking) {
        qemu_cond_wait(&state->blocking_done, &state->lock);
    }
    if (state->stopping) {
        qemu_mutex_unlock(&state->lock);
        job_destroy(job);
        return false;
    }
    state->blocking = job;
    qemu_cond_signal(&state->blocking_ready);
    while (!job->done) {
        qemu_cond_wait(&state->blocking_done, &state->lock);
    }
    success = !job->cancelled && !state->stopping;
    if (success) {
        *result = (PGRAPHVkHybridCompileResult) {
            .generation = job->request.generation,
            .ticket = job->request.ticket,
            .stage = job->request.stage,
            .priority = job->request.priority,
            .success = job->success,
            .spirv = job->spirv,
            .spirv_size = job->spirv_size,
            .submitted_us = job->submitted_us,
            .started_us = job->started_us,
            .finished_us = job->finished_us,
            .caller_return_us = g_get_monotonic_time(),
            .speculative_active_at_start =
                job->speculative_active_at_start,
        };
        job->spirv = NULL;
        job->spirv_size = 0;
    }
    qemu_mutex_unlock(&state->lock);
    job_destroy(job);
    return success;
}

bool pgraph_vk_hybrid_compiler_take_result(
    PGRAPHVkHybridCompiler *compiler,
    PGRAPHVkHybridCompileResult *result)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    HybridCompilerJob *job;

    if (result) {
        *result = (PGRAPHVkHybridCompileResult) { 0 };
    }
    if (!state || !result) {
        return false;
    }

    qemu_mutex_lock(&state->lock);
    job = list_pop_highest_priority(&state->result_head,
                                    &state->result_tail);
    if (!job) {
        qemu_mutex_unlock(&state->lock);
        return false;
    }
    if (!state->result_head) {
        qatomic_set(&state->result_available, false);
    }
    async_account_release(state, job);
    *result = (PGRAPHVkHybridCompileResult) {
        .generation = job->request.generation,
        .ticket = job->request.ticket,
        .stage = job->request.stage,
        .priority = job->request.priority,
        .success = job->success,
        .spirv = job->spirv,
        .spirv_size = job->spirv_size,
        .submitted_us = job->submitted_us,
        .started_us = job->started_us,
        .finished_us = job->finished_us,
    };
    job->spirv = NULL;
    job->spirv_size = 0;
    qemu_mutex_unlock(&state->lock);
    job_destroy(job);
    return true;
}

bool pgraph_vk_hybrid_compiler_take_result_for(
    PGRAPHVkHybridCompiler *compiler, uint64_t generation, uint64_t ticket,
    PGRAPHVkHybridCompileResult *result)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;
    HybridCompilerJob *job;

    if (result) {
        *result = (PGRAPHVkHybridCompileResult) { 0 };
    }
    if (!state || !ticket || !result) {
        return false;
    }

    qemu_mutex_lock(&state->lock);
    job = result_take_identity(state, generation, ticket);
    if (!job) {
        qemu_mutex_unlock(&state->lock);
        return false;
    }
    if (!state->result_head) {
        qatomic_set(&state->result_available, false);
    }
    async_account_release(state, job);
    *result = (PGRAPHVkHybridCompileResult) {
        .generation = job->request.generation,
        .ticket = job->request.ticket,
        .stage = job->request.stage,
        .priority = job->request.priority,
        .success = job->success,
        .spirv = job->spirv,
        .spirv_size = job->spirv_size,
        .submitted_us = job->submitted_us,
        .started_us = job->started_us,
        .finished_us = job->finished_us,
    };
    job->spirv = NULL;
    job->spirv_size = 0;
    qemu_mutex_unlock(&state->lock);
    job_destroy(job);
    return true;
}

bool pgraph_vk_hybrid_compiler_has_result(
    const PGRAPHVkHybridCompiler *compiler)
{
    const HybridCompilerState *state = compiler ? compiler->state : NULL;

    return state && qatomic_read(&state->result_available);
}

void pgraph_vk_hybrid_compile_result_destroy(
    PGRAPHVkHybridCompileResult *result)
{
    if (!result) {
        return;
    }
    g_free(result->spirv);
    *result = (PGRAPHVkHybridCompileResult) { 0 };
}

void pgraph_vk_hybrid_compiler_stop(PGRAPHVkHybridCompiler *compiler)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;

    if (!state) {
        return;
    }
    qemu_mutex_lock(&state->lock);
    if (!state->stopping) {
        state->stopping = true;
        async_list_destroy(state, &state->async_head, &state->async_tail);
        async_list_destroy(state, &state->result_head, &state->result_tail);
        qatomic_set(&state->result_available, false);
        if (state->blocking && state->blocking != state->active_blocking) {
            HybridCompilerJob *job = state->blocking;
            state->blocking = NULL;
            job->cancelled = true;
            job->done = true;
        }
        qemu_cond_broadcast(&state->work_ready);
        qemu_cond_broadcast(&state->blocking_ready);
        qemu_cond_broadcast(&state->blocking_done);
    }
    qemu_mutex_unlock(&state->lock);
}

void pgraph_vk_hybrid_compiler_join(PGRAPHVkHybridCompiler *compiler)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;

    if (!state || state->joined) {
        return;
    }
    pgraph_vk_hybrid_compiler_stop(compiler);
    qemu_thread_join(&state->worker);
    qemu_thread_join(&state->blocking_worker);
    state->joined = true;
}

void pgraph_vk_hybrid_compiler_destroy(PGRAPHVkHybridCompiler *compiler)
{
    HybridCompilerState *state = compiler ? compiler->state : NULL;

    if (!state) {
        return;
    }
    pgraph_vk_hybrid_compiler_join(compiler);
    qemu_mutex_lock(&state->lock);
    async_list_destroy(state, &state->async_head, &state->async_tail);
    async_list_destroy(state, &state->result_head, &state->result_tail);
    assert(!state->active);
    assert(!state->active_blocking);
    assert(!state->blocking);
    qemu_mutex_unlock(&state->lock);
    qemu_cond_destroy(&state->blocking_done);
    qemu_cond_destroy(&state->blocking_ready);
    qemu_cond_destroy(&state->work_ready);
    qemu_mutex_destroy(&state->lock);
    g_free(state);
    compiler->state = NULL;
}

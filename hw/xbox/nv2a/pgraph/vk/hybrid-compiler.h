/*
 * NV2A Vulkan hybrid compiler queue
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_COMPILER_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum PGRAPHVkHybridPriority {
    PGRAPH_VK_HYBRID_PRIORITY_PREWARM = 0,
    PGRAPH_VK_HYBRID_PRIORITY_VISIBLE = 1,
    PGRAPH_VK_HYBRID_PRIORITY_REQUIRED = 2,
} PGRAPHVkHybridPriority;

typedef void (*PGRAPHVkHybridCompletionNotifyFunc)(void *opaque);

/*
 * This queue owns only immutable source/configuration bytes and compiler
 * artifacts. It deliberately knows nothing about PGRAPH, Vulkan, caches, or
 * renderer-owned state.
 */
typedef struct PGRAPHVkHybridCompileRequest {
    uint64_t generation;
    uint64_t ticket;
    uint32_t stage;
    PGRAPHVkHybridPriority priority;
    const void *glsl;
    size_t glsl_size;
    const void *config;
    size_t config_size;
} PGRAPHVkHybridCompileRequest;

typedef struct PGRAPHVkHybridCompileResult {
    uint64_t generation;
    uint64_t ticket;
    uint32_t stage;
    PGRAPHVkHybridPriority priority;
    bool success;
    uint8_t *spirv;
    size_t spirv_size;
    uint64_t submitted_us;
    uint64_t started_us;
    uint64_t finished_us;
    uint64_t caller_return_us;
    bool speculative_active_at_start;
} PGRAPHVkHybridCompileResult;

typedef struct PGRAPHVkHybridCompileIdentity {
    uint64_t generation;
    uint64_t ticket;
} PGRAPHVkHybridCompileIdentity;

/*
 * The callback executes on a compiler worker. It receives deep-owned
 * immutable input valid for the duration of the call and transfers an
 * allocator-compatible artifact to the queue on success.
 */
typedef bool (*PGRAPHVkHybridCompileFunc)(
    void *opaque, const PGRAPHVkHybridCompileRequest *request,
    uint8_t **spirv, size_t *spirv_size);

typedef struct PGRAPHVkHybridCompilerConfig {
    size_t max_async_jobs;
    size_t max_async_bytes;
    PGRAPHVkHybridCompileFunc compile;
    void *opaque;
    PGRAPHVkHybridCompletionNotifyFunc notify;
    void *notify_opaque;
} PGRAPHVkHybridCompilerConfig;

typedef enum PGRAPHVkHybridCompilerSubmitResult {
    PGRAPH_VK_HYBRID_COMPILER_ACCEPTED,
    PGRAPH_VK_HYBRID_COMPILER_DUPLICATE,
    PGRAPH_VK_HYBRID_COMPILER_QUEUE_FULL,
    PGRAPH_VK_HYBRID_COMPILER_BYTE_LIMIT,
    PGRAPH_VK_HYBRID_COMPILER_STOPPED,
    PGRAPH_VK_HYBRID_COMPILER_INVALID,
} PGRAPHVkHybridCompilerSubmitResult;

typedef struct PGRAPHVkHybridCompiler {
    void *state;
} PGRAPHVkHybridCompiler;

bool pgraph_vk_hybrid_compiler_init(
    PGRAPHVkHybridCompiler *compiler,
    const PGRAPHVkHybridCompilerConfig *config);

bool pgraph_vk_hybrid_compiler_can_submit_async(
    PGRAPHVkHybridCompiler *compiler, size_t glsl_size, size_t config_size);
PGRAPHVkHybridCompilerSubmitResult pgraph_vk_hybrid_compiler_probe_async(
    PGRAPHVkHybridCompiler *compiler, size_t glsl_size, size_t config_size);

/*
 * A duplicate is identified by stage plus exact GLSL/config bytes, not by
 * generation or ticket. On ACCEPTED or DUPLICATE, owner identifies the one
 * result that an integration must associate with every matching recipe.
 */
PGRAPHVkHybridCompilerSubmitResult pgraph_vk_hybrid_compiler_submit_async(
    PGRAPHVkHybridCompiler *compiler,
    const PGRAPHVkHybridCompileRequest *request,
    PGRAPHVkHybridCompileIdentity *owner);
bool pgraph_vk_hybrid_compiler_promote(
    PGRAPHVkHybridCompiler *compiler, uint64_t generation, uint64_t ticket,
    PGRAPHVkHybridPriority priority);

/*
 * Blocking work uses one dedicated slot and worker. It is admitted
 * independently of the asynchronous count and byte caps.
 */
bool pgraph_vk_hybrid_compiler_submit_blocking(
    PGRAPHVkHybridCompiler *compiler,
    const PGRAPHVkHybridCompileRequest *request,
    PGRAPHVkHybridCompileResult *result);

/* Transfer one completed asynchronous artifact to the caller, if available. */
bool pgraph_vk_hybrid_compiler_take_result(
    PGRAPHVkHybridCompiler *compiler,
    PGRAPHVkHybridCompileResult *result);
bool pgraph_vk_hybrid_compiler_take_result_for(
    PGRAPHVkHybridCompiler *compiler, uint64_t generation, uint64_t ticket,
    PGRAPHVkHybridCompileResult *result);
bool pgraph_vk_hybrid_compiler_has_result(
    const PGRAPHVkHybridCompiler *compiler);
void pgraph_vk_hybrid_compile_result_destroy(
    PGRAPHVkHybridCompileResult *result);

/* Stop intake, discard unpublished async work, then join both workers. */
void pgraph_vk_hybrid_compiler_stop(PGRAPHVkHybridCompiler *compiler);
void pgraph_vk_hybrid_compiler_join(PGRAPHVkHybridCompiler *compiler);
void pgraph_vk_hybrid_compiler_destroy(PGRAPHVkHybridCompiler *compiler);

#endif

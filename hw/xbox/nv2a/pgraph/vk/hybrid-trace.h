/*
 * Opt-in, slow-frame-only Vulkan hybrid attribution.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_TRACE_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_TRACE_H

#include <stdint.h>

typedef struct PGRAPHVkHybridTrace PGRAPHVkHybridTrace;

typedef enum PGRAPHVkHybridTraceType {
    VK_HYBRID_TRACE_REQUIRED_COMPILE = 1,
    VK_HYBRID_TRACE_SPECULATIVE_COMPILE,
    VK_HYBRID_TRACE_COMPLETION,
    VK_HYBRID_TRACE_COMPLETION_BATCH,
    VK_HYBRID_TRACE_SHADER_BINDING_PROBE,
    VK_HYBRID_TRACE_PIPELINE_PROBE,
    VK_HYBRID_TRACE_PIPELINE_CREATE,
    VK_HYBRID_TRACE_ROUTE_TRANSITION,
    VK_HYBRID_TRACE_FINISH,
    VK_HYBRID_TRACE_RESOURCE_SHORTAGE,
    VK_HYBRID_TRACE_UNCOVERED,
    VK_HYBRID_TRACE_PIPELINE_SUBMIT,
    VK_HYBRID_TRACE_PIPELINE_ADOPT,
    VK_HYBRID_TRACE_MODULE_MATERIALIZE,
    VK_HYBRID_TRACE_LAYOUT_CREATE,
    VK_HYBRID_TRACE_RENDER_PASS_LOOKUP,
} PGRAPHVkHybridTraceType;

typedef enum PGRAPHVkHybridResourceShortage {
    VK_HYBRID_SHORTAGE_PIPELINE_CACHE = 1,
    VK_HYBRID_SHORTAGE_DESCRIPTOR_SET,
    VK_HYBRID_SHORTAGE_UNIFORM_STAGING,
    VK_HYBRID_SHORTAGE_FRAMEBUFFER,
    VK_HYBRID_SHORTAGE_BUFFER,
} PGRAPHVkHybridResourceShortage;

/* One bounded ring is retained per active guest frame. Only slow frames are
 * written to disk. All timestamps use the same monotonic clock, in us. */
PGRAPHVkHybridTrace *pgraph_vk_hybrid_trace_open(const char *path,
                                                 uint64_t threshold_us);
void pgraph_vk_hybrid_trace_close(PGRAPHVkHybridTrace *trace);
void pgraph_vk_hybrid_trace_draw(PGRAPHVkHybridTrace *trace);
void pgraph_vk_hybrid_trace_record(PGRAPHVkHybridTrace *trace,
                                   PGRAPHVkHybridTraceType type,
                                   uint32_t route, uint64_t pipeline_hash,
                                   uint64_t shader_hash, uint64_t ticket,
                                   uint64_t a, uint64_t b, uint64_t c,
                                   uint64_t d);
void pgraph_vk_hybrid_trace_frame(PGRAPHVkHybridTrace *trace);

#endif

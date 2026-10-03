/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Checked source ranges for strided NV2A vertex attributes.
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VERTEX_FETCH_SPAN_H
#define HW_XBOX_NV2A_PGRAPH_VERTEX_FETCH_SPAN_H

#include <stdbool.h>
#include <stdint.h>

static inline bool pgraph_vertex_fetch_span(uint64_t first_vertex,
                                             uint64_t last_vertex,
                                             uint64_t stride,
                                             uint64_t element_bytes,
                                             uint64_t *relative_start,
                                             uint64_t *span_bytes)
{
    if (last_vertex < first_vertex || !element_bytes ||
        (stride && first_vertex > UINT64_MAX / stride)) {
        return false;
    }

    uint64_t vertices_after_first = last_vertex - first_vertex;
    if (stride && vertices_after_first >
                      (UINT64_MAX - element_bytes) / stride) {
        return false;
    }

    *relative_start = first_vertex * stride;
    *span_bytes = vertices_after_first * stride + element_bytes;
    return true;
}

/* DMA object limits are inclusive; VRAM sizes are exclusive. */
static inline bool pgraph_vertex_span_fits_dma(uint64_t start,
                                                uint64_t span_bytes,
                                                uint64_t inclusive_limit)
{
    return span_bytes && start <= inclusive_limit &&
           span_bytes - 1 <= inclusive_limit - start;
}

static inline bool pgraph_vertex_span_fits_vram(uint64_t start,
                                                 uint64_t span_bytes,
                                                 uint64_t vram_size)
{
    return span_bytes && start <= vram_size &&
           span_bytes <= vram_size - start;
}

/*
 * Compare half-open ranges without forming potentially overflowing ends.
 * Callers normally pass validated ranges; malformed overflowing ranges are
 * treated as non-overlapping rather than wrapped into an apparent match.
 */
static inline bool pgraph_ranges_overlap(uint64_t first_start,
                                         uint64_t first_size,
                                         uint64_t second_start,
                                         uint64_t second_size)
{
    if (!first_size || !second_size) {
        return false;
    }
    if (first_size - 1 > UINT64_MAX - first_start ||
        second_size - 1 > UINT64_MAX - second_start) {
        return false;
    }

    if (first_start <= second_start) {
        return second_start - first_start < first_size;
    }
    return first_start - second_start < second_size;
}

typedef struct PGRAPHVertexFetchRange {
    uint64_t attribute_base;
    uint64_t fetch_start;
    uint64_t fetch_size;
} PGRAPHVertexFetchRange;

/* Resolve a strided fetch from DMA-relative coordinates into absolute VRAM.
 * dma_limit is inclusive; vram_size is exclusive. */
static inline bool pgraph_vertex_resolve_fetch_range(
    uint64_t dma_base, uint64_t dma_limit, uint64_t attribute_offset,
    uint64_t vram_size, uint64_t first_vertex, uint64_t last_vertex,
    uint64_t stride, uint64_t element_bytes,
    PGRAPHVertexFetchRange *range)
{
    uint64_t relative_start, fetch_size;
    if (!pgraph_vertex_fetch_span(first_vertex, last_vertex, stride,
                                  element_bytes, &relative_start,
                                  &fetch_size) ||
        attribute_offset > dma_limit ||
        relative_start > dma_limit - attribute_offset) {
        return false;
    }

    uint64_t dma_fetch_start = attribute_offset + relative_start;
    if (!pgraph_vertex_span_fits_dma(dma_fetch_start, fetch_size,
                                     dma_limit) ||
        dma_base > vram_size ||
        attribute_offset > vram_size - dma_base) {
        return false;
    }

    uint64_t attribute_base = dma_base + attribute_offset;
    if (relative_start > vram_size - attribute_base) {
        return false;
    }

    uint64_t fetch_start = attribute_base + relative_start;
    if (!pgraph_vertex_span_fits_vram(fetch_start, fetch_size, vram_size)) {
        return false;
    }

    *range = (PGRAPHVertexFetchRange) {
        .attribute_base = attribute_base,
        .fetch_start = fetch_start,
        .fetch_size = fetch_size,
    };
    return true;
}

#endif

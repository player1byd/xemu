/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Bounds used before copying a vertex attribute into an immutable draw slice.
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VK_VERTEX_VERSION_POLICY_H
#define HW_XBOX_NV2A_PGRAPH_VK_VERTEX_VERSION_POLICY_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vertex-fetch-span.h"

/* Versioned draws are intentionally bounded. The copy budget leaves enough
 * room for worst-case per-attribute alignment (16 attributes at 16 bytes),
 * independently of the maximum supported vertex span. */
#define PGRAPH_VK_VERTEX_VERSION_SCRATCH_SIZE (64U * 1024U)
#define PGRAPH_VK_VERTEX_VERSION_ALIGNMENT_SLACK (16U * 16U)
#define PGRAPH_VK_VERTEX_VERSION_COPY_BUDGET \
    (PGRAPH_VK_VERTEX_VERSION_SCRATCH_SIZE - \
     PGRAPH_VK_VERTEX_VERSION_ALIGNMENT_SLACK)
#define PGRAPH_VK_VERTEX_VERSION_MAX_VERTICES 256U

static inline bool pgraph_vk_vertex_version_source_fits(
    uint64_t vertices, uint64_t stride, uint64_t element_bytes,
    uint64_t dma_offset, uint64_t dma_inclusive_limit,
    uint64_t vram_addr, uint64_t vram_size)
{
    uint64_t relative_start, span_bytes;
    if (!vertices || stride < element_bytes ||
        !pgraph_vertex_fetch_span(0, vertices - 1, stride,
                                  element_bytes, &relative_start,
                                  &span_bytes)) {
        return false;
    }
    return dma_offset <= dma_inclusive_limit &&
           relative_start <= dma_inclusive_limit - dma_offset &&
           vram_addr <= vram_size &&
           relative_start <= vram_size - vram_addr &&
           pgraph_vertex_span_fits_dma(
               dma_offset + relative_start, span_bytes,
               dma_inclusive_limit) &&
           pgraph_vertex_span_fits_vram(
               vram_addr + relative_start, span_bytes, vram_size);
}

static inline bool pgraph_vk_vertex_version_copy_fits(
    uint64_t vertices, uint64_t element_bytes, uint64_t remaining_budget)
{
    return vertices && element_bytes &&
           vertices <= remaining_budget / element_bytes;
}

/* A completed full mirror upload repairs every page, including pages whose
 * guest dirty bit was consumed when their draw selected a private version. */
static inline void pgraph_vk_vertex_version_clear_stale(
    uint8_t *pages, size_t page_count, size_t *stale_count)
{
    memset(pages, 0, page_count);
    *stale_count = 0;
}

static inline void pgraph_vk_vertex_version_set_stale(
    uint8_t *pages, size_t total_pages, size_t *stale_count,
    size_t first_page, size_t page_count, bool stale)
{
    assert(first_page <= total_pages);
    assert(page_count <= total_pages - first_page);
    for (size_t page = first_page; page < first_page + page_count; page++) {
        if (pages[page] == stale) {
            continue;
        }
        pages[page] = stale;
        if (stale) {
            ++*stale_count;
        } else {
            assert(*stale_count);
            --*stale_count;
        }
    }
}

#endif

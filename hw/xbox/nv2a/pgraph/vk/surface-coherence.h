/*
 * NV2A Vulkan surface/guest-memory coherence helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_SURFACE_COHERENCE_H
#define HW_XBOX_NV2A_PGRAPH_VK_SURFACE_COHERENCE_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef bool (*PGRAPHVkSurfaceTakeDirtyPages)(
    void *opaque, uint64_t start, uint64_t size, unsigned long *pages,
    size_t capacity_words);
typedef void (*PGRAPHVkSurfaceVisitDirtyPages)(
    void *opaque, uint64_t start, uint64_t size, uint64_t page_size,
    const unsigned long *pages);
typedef bool (*PGRAPHVkSurfaceRefreshDirtyRange)(
    void *opaque, uint64_t start, uint64_t size);

static inline bool pgraph_vk_surface_range_overlaps(
    uint64_t surface_start, uint64_t surface_size, uint64_t range_start,
    uint64_t range_size)
{
    if (!surface_size || !range_size) {
        return false;
    }
    if (surface_start <= range_start) {
        return range_start - surface_start < surface_size;
    }
    return surface_start - range_start < range_size;
}

/* Query exact observed page bits, without broadening one dirty bit to the
 * whole requested range. The visitor can traverse each cached surface once. */
static inline bool pgraph_vk_surface_consume_dirty_range(
    uint64_t start, uint64_t size, uint64_t page_size,
    unsigned long *pages, size_t capacity_words,
    PGRAPHVkSurfaceTakeDirtyPages take,
    PGRAPHVkSurfaceVisitDirtyPages visit, void *opaque)
{
    assert(page_size && !(page_size & (page_size - 1)));
    assert(start <= UINT64_MAX - (page_size - 1));
    assert(size <= UINT64_MAX - start - (page_size - 1));

    if (!size) {
        return false;
    }
    uint64_t aligned_start = start & ~(page_size - 1);
    uint64_t aligned_end = (start + size + page_size - 1) & ~(page_size - 1);
    uint64_t page_count = (aligned_end - aligned_start) / page_size;
    assert(capacity_words >=
           (page_count + sizeof(unsigned long) * CHAR_BIT - 1) /
               (sizeof(unsigned long) * CHAR_BIT));

    if (!take(opaque, start, size, pages, capacity_words)) {
        return false;
    }
    visit(opaque, aligned_start, aligned_end - aligned_start, page_size,
          pages);
    return true;
}

static inline bool pgraph_vk_surface_dirty_pages_overlap(
    uint64_t surface_start, uint64_t surface_size, uint64_t range_start,
    uint64_t range_size, uint64_t page_size, const unsigned long *pages)
{
    if (!pgraph_vk_surface_range_overlaps(
            surface_start, surface_size, range_start, range_size)) {
        return false;
    }

    uint64_t start = surface_start > range_start ? surface_start : range_start;
    uint64_t surface_end = surface_start + surface_size;
    uint64_t range_end = range_start + range_size;
    uint64_t end = surface_end < range_end ? surface_end : range_end;
    uint64_t first_page = (start - range_start) / page_size;
    uint64_t last_page = (end - 1 - range_start) / page_size;
    const unsigned int word_bits = sizeof(unsigned long) * CHAR_BIT;

    for (uint64_t page = first_page; page <= last_page;) {
        uint64_t word = page / word_bits;
        unsigned int first_bit = page % word_bits;
        uint64_t next_page = (word + 1) * word_bits;
        uint64_t limit = next_page - 1 < last_page ?
                         next_page - 1 : last_page;
        unsigned int count = limit - page + 1;
        unsigned long mask = (~0UL >> (word_bits - count)) << first_bit;

        if (pages[word] & mask) {
            return true;
        }
        page = limit + 1;
    }
    return false;
}

/* Return true only for the transition that first makes an upload necessary. */
static inline bool pgraph_vk_surface_mark_upload_pending(bool *upload_pending)
{
    bool newly_pending = !*upload_pending;
    *upload_pending = true;
    return newly_pending;
}

/*
 * Accelerated guest writes are observed after they reach VRAM.  At that
 * point a cached surface may no longer write its older GPU contents back over
 * the newer guest bytes.  Make the guest copy authoritative and require the
 * cached surface to reload it before the surface is used again.
 */
static inline bool pgraph_vk_surface_resolve_guest_write(
    bool guest_memory_dirty, bool *download_pending, bool *draw_dirty,
    bool *upload_pending, bool *readback_superseded_by_guest)
{
    if (!guest_memory_dirty) {
        return false;
    }

    *download_pending = false;
    *draw_dirty = false;
    pgraph_vk_surface_mark_upload_pending(upload_pending);
    *readback_superseded_by_guest = true;
    return true;
}

/* Retire guest ownership only after the replacement GPU image was prepared. */
static inline void pgraph_vk_surface_upload_complete(
    bool succeeded, bool *upload_pending, bool *readback_superseded_by_guest)
{
    if (succeeded) {
        *upload_pending = false;
        *readback_superseded_by_guest = false;
    }
}

/* A guest write observed before a GPU-to-RAM readback makes that readback
 * stale. Check the entire surface because the readback copies its full extent,
 * even when a caller previously checked a smaller overlapping texture range. */
static inline bool pgraph_vk_surface_readback_preflight(
    bool download_pending, bool force, uint64_t start, uint64_t size,
    bool *readback_superseded_by_guest,
    PGRAPHVkSurfaceRefreshDirtyRange refresh, void *opaque)
{
    if (!(download_pending || force)) {
        return false;
    }
    assert(refresh);
    refresh(opaque, start, size);
    return !*readback_superseded_by_guest;
}

#endif

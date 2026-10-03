/*
 * NV2A Vulkan surface/guest-memory coherence tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hw/xbox/nv2a/pgraph/vk/surface-coherence.h"

static bool test_clean_guest_memory_preserves_surface_state(void)
{
    bool download_pending = true;
    bool draw_dirty = true;
    bool upload_pending = false;
    bool superseded = false;

    if (pgraph_vk_surface_resolve_guest_write(
            false, &download_pending, &draw_dirty, &upload_pending,
            &superseded)) {
        return false;
    }

    return download_pending && draw_dirty && !upload_pending && !superseded;
}

static bool test_guest_write_preempts_stale_surface_download(void)
{
    bool download_pending = true;
    bool draw_dirty = true;
    bool upload_pending = false;
    bool superseded = false;

    if (!pgraph_vk_surface_resolve_guest_write(
            true, &download_pending, &draw_dirty, &upload_pending,
            &superseded)) {
        return false;
    }

    return !download_pending && !draw_dirty && upload_pending && superseded;
}

static bool test_guest_write_keeps_existing_upload_pending(void)
{
    bool download_pending = false;
    bool draw_dirty = false;
    bool upload_pending = true;
    bool superseded = false;

    return pgraph_vk_surface_resolve_guest_write(
               true, &download_pending, &draw_dirty, &upload_pending,
               &superseded) &&
           !download_pending && !draw_dirty && upload_pending && superseded;
}

static bool test_upload_pending_transition_is_counted_once(void)
{
    bool upload_pending = false;

    return pgraph_vk_surface_mark_upload_pending(&upload_pending) &&
           upload_pending &&
           !pgraph_vk_surface_mark_upload_pending(&upload_pending) &&
           upload_pending;
}

typedef struct DirtyPageFixture {
    uint8_t dirty_pages;
    bool surface_stale[4];
    unsigned int tests;
    unsigned int visits;
    uint64_t tested_start;
    uint64_t tested_size;
} DirtyPageFixture;

static bool take_dirty_pages(void *opaque, uint64_t start, uint64_t size,
                             unsigned long *pages, size_t capacity_words)
{
    DirtyPageFixture *fixture = opaque;
    uint8_t requested_pages = 0;
    uint64_t first_page = start / 4096;
    (void)capacity_words;
    for (uint64_t page = start / 4096; page < (start + size + 4095) / 4096;
         page++) {
        requested_pages |= 1u << page;
    }
    uint8_t taken = fixture->dirty_pages & requested_pages;

    pages[0] = taken >> first_page;
    fixture->dirty_pages &= ~requested_pages;
    fixture->tests++;
    fixture->tested_start = start;
    fixture->tested_size = size;
    return taken != 0;
}

static void mark_overlapping_surfaces(void *opaque, uint64_t start,
                                      uint64_t size, uint64_t page_size,
                                      const unsigned long *pages)
{
    DirtyPageFixture *fixture = opaque;
    static const uint64_t surface_start[] = { 0, 256, 4096, 8192 };
    static const uint64_t surface_size[] = { 2048, 1024, 4096, 4096 };

    for (unsigned int i = 0; i < 4; i++) {
        if (pgraph_vk_surface_dirty_pages_overlap(
                surface_start[i], surface_size[i], start, size, page_size,
                pages)) {
            fixture->surface_stale[i] = true;
        }
    }
    fixture->visits++;
}

static bool test_one_dirty_page_reaches_both_overlapping_surfaces(void)
{
    DirtyPageFixture fixture = { .dirty_pages = 1 };
    unsigned long pages[1] = { 0 };

    bool any_dirty = pgraph_vk_surface_consume_dirty_range(
        128, 128, 4096, pages, 1, take_dirty_pages,
        mark_overlapping_surfaces, &fixture);

    return any_dirty && fixture.tests == 1 && fixture.visits == 1 &&
           fixture.tested_start == 128 && fixture.tested_size == 128 &&
           fixture.surface_stale[0] && fixture.surface_stale[1] &&
           !fixture.surface_stale[2] && !fixture.dirty_pages;
}

static bool test_clean_range_does_not_touch_surfaces(void)
{
    DirtyPageFixture fixture = { 0 };
    unsigned long pages[1] = { 0 };

    bool any_dirty = pgraph_vk_surface_consume_dirty_range(
        128, 128, 4096, pages, 1, take_dirty_pages,
        mark_overlapping_surfaces, &fixture);

    return !any_dirty && fixture.tests == 1 && fixture.visits == 0 &&
           !fixture.surface_stale[0] && !fixture.surface_stale[1] &&
           !fixture.surface_stale[2];
}

static bool test_sparse_dirty_page_preserves_clean_neighbor(void)
{
    DirtyPageFixture fixture = { .dirty_pages = 1 };
    unsigned long pages[1] = { 0 };

    bool any_dirty = pgraph_vk_surface_consume_dirty_range(
        0, 8192, 4096, pages, 1, take_dirty_pages,
        mark_overlapping_surfaces, &fixture);

    /* The dirty bit belongs to page zero; surface two covers only page one. */
    return any_dirty && fixture.surface_stale[0] &&
           fixture.surface_stale[1] && !fixture.surface_stale[2];
}

static bool test_sparse_first_and_third_pages(void)
{
    DirtyPageFixture fixture = { .dirty_pages = 0x5 };
    unsigned long pages[1] = { 0 };

    bool any_dirty = pgraph_vk_surface_consume_dirty_range(
        0, 3 * 4096, 4096, pages, 1, take_dirty_pages,
        mark_overlapping_surfaces, &fixture);

    return any_dirty && fixture.visits == 1 &&
           fixture.surface_stale[0] && fixture.surface_stale[1] &&
           !fixture.surface_stale[2] && fixture.surface_stale[3];
}

static bool test_dirty_page_outside_request_remains_pending(void)
{
    DirtyPageFixture fixture = { .dirty_pages = 0x4 };
    unsigned long pages[1] = { 0 };

    bool any_dirty = pgraph_vk_surface_consume_dirty_range(
        0, 2 * 4096, 4096, pages, 1, take_dirty_pages,
        mark_overlapping_surfaces, &fixture);

    return !any_dirty && fixture.dirty_pages == 0x4 &&
           fixture.visits == 0 && !fixture.surface_stale[3];
}

typedef struct ReadbackFixture {
    bool guest_wrote;
    bool download_pending;
    bool draw_dirty;
    bool upload_pending;
    bool superseded;
    unsigned int probes;
} ReadbackFixture;

static bool refresh_readback_guest_writes(void *opaque, uint64_t start,
                                          uint64_t size)
{
    ReadbackFixture *fixture = opaque;

    if (start != 0x37d0000 || size != 65536) {
        fixture->probes = UINT32_MAX;
        return false;
    }
    fixture->probes++;
    pgraph_vk_surface_resolve_guest_write(
        fixture->guest_wrote, &fixture->download_pending,
        &fixture->draw_dirty, &fixture->upload_pending,
        &fixture->superseded);
    return fixture->guest_wrote;
}

static bool test_guest_write_blocks_forced_readback(void)
{
    ReadbackFixture fixture = {
        .guest_wrote = true,
        .draw_dirty = true,
    };
    bool should_readback = pgraph_vk_surface_readback_preflight(
        fixture.download_pending, true,
        0x37d0000, 65536, &fixture.superseded,
        refresh_readback_guest_writes, &fixture);

    return !should_readback && fixture.probes == 1 &&
           !fixture.download_pending && !fixture.draw_dirty &&
           fixture.upload_pending;
}

static bool test_clean_surface_still_reads_back(void)
{
    ReadbackFixture fixture = { .draw_dirty = true };
    bool should_readback = pgraph_vk_surface_readback_preflight(
        fixture.download_pending, true,
        0x37d0000, 65536, &fixture.superseded,
        refresh_readback_guest_writes, &fixture);

    return should_readback && fixture.probes == 1 &&
           fixture.draw_dirty && !fixture.upload_pending;
}

static bool test_guest_write_blocks_forced_clean_surface_readback(void)
{
    ReadbackFixture fixture = { .guest_wrote = true };
    bool should_readback = pgraph_vk_surface_readback_preflight(
        fixture.download_pending, true,
        0x37d0000, 65536, &fixture.superseded,
        refresh_readback_guest_writes, &fixture);

    return !should_readback && fixture.probes == 1 &&
           fixture.upload_pending;
}

static bool test_prewrite_upload_intent_does_not_block_readback(void)
{
    ReadbackFixture fixture = {
        .draw_dirty = true,
        .upload_pending = true,
    };
    bool should_readback = pgraph_vk_surface_readback_preflight(
        false, true, 0x37d0000, 65536, &fixture.superseded,
        refresh_readback_guest_writes, &fixture);

    return should_readback && fixture.probes == 1 &&
           fixture.upload_pending && !fixture.superseded;
}

static bool test_consumed_guest_write_still_blocks_forced_readback(void)
{
    ReadbackFixture fixture = {
        .guest_wrote = true,
        .draw_dirty = true,
    };

    bool first = pgraph_vk_surface_readback_preflight(
        false, true, 0x37d0000, 65536,
        &fixture.superseded,
        refresh_readback_guest_writes, &fixture);
    fixture.guest_wrote = false;
    bool second = pgraph_vk_surface_readback_preflight(
        false, true, 0x37d0000, 65536,
        &fixture.superseded,
        refresh_readback_guest_writes, &fixture);

    return !first && !second && fixture.upload_pending &&
           fixture.probes == 2;
}

static bool test_upload_retires_guest_write_veto_only_on_success(void)
{
    ReadbackFixture fixture = {
        .upload_pending = true,
        .superseded = true,
    };

    pgraph_vk_surface_upload_complete(false, &fixture.upload_pending,
                                      &fixture.superseded);
    if (!fixture.upload_pending || !fixture.superseded) {
        return false;
    }

    pgraph_vk_surface_upload_complete(true, &fixture.upload_pending,
                                      &fixture.superseded);
    return !fixture.upload_pending && !fixture.superseded &&
           pgraph_vk_surface_readback_preflight(
               false, true, 0x37d0000, 65536, &fixture.superseded,
               refresh_readback_guest_writes, &fixture);
}

int main(void)
{
    bool clean = test_clean_guest_memory_preserves_surface_state();
    bool preempt = test_guest_write_preempts_stale_surface_download();
    bool upload = test_guest_write_keeps_existing_upload_pending();
    bool transition = test_upload_pending_transition_is_counted_once();
    bool overlap = test_one_dirty_page_reaches_both_overlapping_surfaces();
    bool clean_range = test_clean_range_does_not_touch_surfaces();
    bool sparse = test_sparse_dirty_page_preserves_clean_neighbor();
    bool sparse_first_third = test_sparse_first_and_third_pages();
    bool outside_request = test_dirty_page_outside_request_remains_pending();
    bool guest_write_readback = test_guest_write_blocks_forced_readback();
    bool clean_readback = test_clean_surface_still_reads_back();
    bool forced_clean_readback =
        test_guest_write_blocks_forced_clean_surface_readback();
    bool prewrite_readback =
        test_prewrite_upload_intent_does_not_block_readback();
    bool consumed_readback =
        test_consumed_guest_write_still_blocks_forced_readback();
    bool upload_retires_veto =
        test_upload_retires_guest_write_veto_only_on_success();

    puts("TAP version 13");
    puts("1..15");
    printf("%s 1 - clean guest memory preserves surface state\n",
           clean ? "ok" : "not ok");
    printf("%s 2 - guest write preempts stale surface download\n",
           preempt ? "ok" : "not ok");
    printf("%s 3 - guest write keeps upload pending\n",
           upload ? "ok" : "not ok");
    printf("%s 4 - upload pending transition is counted once\n",
           transition ? "ok" : "not ok");
    printf("%s 5 - one dirty page reaches both overlapping surfaces\n",
           overlap ? "ok" : "not ok");
    printf("%s 6 - clean range preserves cached surfaces\n",
           clean_range ? "ok" : "not ok");
    printf("%s 7 - guest write blocks forced stale readback\n",
           guest_write_readback ? "ok" : "not ok");
    printf("%s 8 - clean drawn surface still reads back\n",
           clean_readback ? "ok" : "not ok");
    printf("%s 9 - guest write blocks forced clean-surface readback\n",
           forced_clean_readback ? "ok" : "not ok");
    printf("%s 10 - sparse page preserves clean neighbor\n",
           sparse ? "ok" : "not ok");
    printf("%s 11 - consumed write still vetoes forced readback\n",
           consumed_readback ? "ok" : "not ok");
    printf("%s 12 - successful upload retires guest-write veto\n",
           upload_retires_veto ? "ok" : "not ok");
    printf("%s 13 - sparse first and third pages skip clean middle\n",
           sparse_first_third ? "ok" : "not ok");
    printf("%s 14 - dirty page outside request remains pending\n",
           outside_request ? "ok" : "not ok");
    printf("%s 15 - prewrite upload intent permits ordered readback\n",
           prewrite_readback ? "ok" : "not ok");

    return clean && preempt && upload && transition && overlap &&
           clean_range && guest_write_readback && clean_readback &&
           forced_clean_readback && sparse && consumed_readback &&
           upload_retires_veto && sparse_first_third && outside_request &&
           prewrite_readback ? 0 : 1;
}

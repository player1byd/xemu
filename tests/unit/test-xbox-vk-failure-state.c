/*
 * NV2A Vulkan preparation failure-state tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hw/xbox/nv2a/pgraph/vk/failure-state.h"

static void count_unmap(void *opaque)
{
    unsigned int *count = opaque;

    (*count)++;
}

static void count_release(void *opaque)
{
    unsigned int *count = opaque;

    (*count)++;
}

static bool test_map_failure_does_not_unmap(void)
{
    PGRAPHVkMappedMemory mapping = { 0 };
    unsigned int unmaps = 0;

    if (pgraph_vk_mapped_memory_begin(&mapping, false, count_unmap, &unmaps)) {
        return false;
    }
    pgraph_vk_mapped_memory_cleanup(&mapping);
    return unmaps == 0;
}

static bool test_post_map_failure_unmaps_once(void)
{
    PGRAPHVkMappedMemory mapping = { 0 };
    unsigned int unmaps = 0;

    if (!pgraph_vk_mapped_memory_begin(&mapping, true, count_unmap, &unmaps)) {
        return false;
    }
    pgraph_vk_mapped_memory_cleanup(&mapping);
    pgraph_vk_mapped_memory_cleanup(&mapping);
    return unmaps == 1;
}

static bool test_failed_surface_download_keeps_guest_data_unavailable(void)
{
    bool download_pending = true;
    bool draw_dirty = true;

    return !pgraph_vk_surface_download_complete(
               false, &download_pending, &draw_dirty) &&
           download_pending && draw_dirty;
}

static bool test_successful_surface_download_retires_guest_data_pending(void)
{
    bool download_pending = true;
    bool draw_dirty = true;

    return pgraph_vk_surface_download_complete(
               true, &download_pending, &draw_dirty) &&
           !download_pending && !draw_dirty;
}

static bool test_existing_download_batch_preserves_failure_outcome(void)
{
    bool succeeded = false;

    return !pgraph_vk_download_batch_begin(true, &succeeded) && !succeeded;
}

static bool test_post_traversal_enrollment_starts_a_new_batch(void)
{
    bool succeeded = false;

    return pgraph_vk_download_batch_enroll(false, true, &succeeded) &&
           succeeded;
}

static bool test_cached_texture_retry_preserves_old_hash(void)
{
    uint64_t hash = UINT64_C(0x1234);
    bool possibly_dirty = true;

    return !pgraph_vk_texture_upload_complete(
               false, UINT64_C(0x5678), &hash, &possibly_dirty) &&
           hash == UINT64_C(0x1234) && possibly_dirty;
}

static bool test_initial_texture_retry_and_palette_share_the_same_contract(void)
{
    uint64_t initial_hash = UINT64_C(0);
    uint64_t palette_hash = UINT64_C(0xaaaa);
    bool initial_dirty = true;
    bool palette_dirty = true;

    if (pgraph_vk_texture_upload_complete(false, UINT64_C(0x1111),
                                          &initial_hash, &initial_dirty) ||
        pgraph_vk_texture_upload_complete(false, UINT64_C(0xbbbb),
                                          &palette_hash, &palette_dirty)) {
        return false;
    }

    return initial_hash == 0 && initial_dirty &&
           palette_hash == UINT64_C(0xaaaa) && palette_dirty &&
           pgraph_vk_texture_upload_complete(true, UINT64_C(0x1111),
                                              &initial_hash, &initial_dirty) &&
           initial_hash == UINT64_C(0x1111) && !initial_dirty;
}

static bool test_owned_texture_payload_is_released_once(void)
{
    unsigned int releases = 0;
    void *data = &releases;

    pgraph_vk_owned_payload_cleanup(true, &data, count_release);
    pgraph_vk_owned_payload_cleanup(true, &data, count_release);
    return releases == 1 && data == NULL;
}

static bool test_borrowed_texture_payload_is_preserved(void)
{
    unsigned int releases = 0;
    void *borrowed = &releases;
    void *data = borrowed;

    pgraph_vk_owned_payload_cleanup(false, &data, count_release);
    return releases == 0 && data == borrowed;
}

int main(void)
{
    bool map_failure = test_map_failure_does_not_unmap();
    bool post_map_failure = test_post_map_failure_unmaps_once();
    bool surface_failure = test_failed_surface_download_keeps_guest_data_unavailable();
    bool surface_success = test_successful_surface_download_retires_guest_data_pending();
    bool batch_join = test_existing_download_batch_preserves_failure_outcome();
    bool post_traversal_enrollment =
        test_post_traversal_enrollment_starts_a_new_batch();
    bool cached_retry = test_cached_texture_retry_preserves_old_hash();
    bool initial_palette_retry =
        test_initial_texture_retry_and_palette_share_the_same_contract();
    bool owned_payload = test_owned_texture_payload_is_released_once();
    bool borrowed_payload = test_borrowed_texture_payload_is_preserved();

    puts("TAP version 13");
    puts("1..10");
    printf("%s 1 - map failure does not unmap\n", map_failure ? "ok" : "not ok");
    printf("%s 2 - post-map failure unmaps once\n", post_map_failure ? "ok" : "not ok");
    printf("%s 3 - failed surface download remains pending\n", surface_failure ? "ok" : "not ok");
    printf("%s 4 - successful surface download retires pending state\n", surface_success ? "ok" : "not ok");
    printf("%s 5 - existing download batch preserves failure outcome\n", batch_join ? "ok" : "not ok");
    printf("%s 6 - post-traversal enrollment starts a new batch\n", post_traversal_enrollment ? "ok" : "not ok");
    printf("%s 7 - cached texture retry preserves old hash\n", cached_retry ? "ok" : "not ok");
    printf("%s 8 - initial and palette retries preserve state\n", initial_palette_retry ? "ok" : "not ok");
    printf("%s 9 - owned texture payload is released once\n",
           owned_payload ? "ok" : "not ok");
    printf("%s 10 - borrowed texture payload is preserved\n",
           borrowed_payload ? "ok" : "not ok");

    return map_failure && post_map_failure && surface_failure && surface_success &&
           batch_join && post_traversal_enrollment && cached_retry &&
           initial_palette_retry && owned_payload && borrowed_payload ? 0 : 1;
}

/*
 * NV2A Vulkan hybrid family history tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/pgraph/vk/hybrid-family-history.h"

typedef struct PublishFixture {
    GByteArray *written;
    unsigned int replacements;
    unsigned int removals;
    bool replace_success;
} PublishFixture;

static bool fixture_write(void *opaque, const char *path,
                          const uint8_t *data, size_t size)
{
    PublishFixture *fixture = opaque;

    g_assert_cmpstr(path, ==, "families.tmp");
    g_byte_array_append(fixture->written, data, size);
    return true;
}

static bool fixture_replace(void *opaque, const char *temporary,
                            const char *published)
{
    PublishFixture *fixture = opaque;

    g_assert_cmpstr(temporary, ==, "families.tmp");
    g_assert_cmpstr(published, ==, "families.bin");
    fixture->replacements++;
    return fixture->replace_success;
}

static void fixture_remove(void *opaque, const char *path)
{
    PublishFixture *fixture = opaque;

    g_assert_cmpstr(path, ==, "families.tmp");
    fixture->removals++;
}

static void test_rank_and_session_attempts(void)
{
    static const uint8_t family_a[] = { 1, 2, 3 };
    static const uint8_t family_b[] = { 4, 5 };
    PGRAPHVkFamilyHistory history;

    g_assert_true(pgraph_vk_family_history_init(&history, 4));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_a, sizeof(family_a)));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_b, sizeof(family_b)));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_a, sizeof(family_a)));
    g_assert_true(pgraph_vk_family_history_note_cold_miss(
        &history, family_b, sizeof(family_b), 250));

    const PGRAPHVkFamilyHistoryRecord *record =
        pgraph_vk_family_history_next_unattempted(&history);
    g_assert_nonnull(record);
    g_assert_cmpmem(record->payload, record->payload_size,
                    family_b, sizeof(family_b));
    g_assert_cmpuint(record->cold_misses, ==, 1);
    g_assert_cmpuint(record->synchronous_create_us, ==, 250);
    pgraph_vk_family_history_mark_attempted(&history, record);

    record = pgraph_vk_family_history_next_unattempted(&history);
    g_assert_nonnull(record);
    g_assert_cmpmem(record->payload, record->payload_size,
                    family_a, sizeof(family_a));
    g_assert_cmpuint(record->uses, ==, 2);
    pgraph_vk_family_history_mark_attempted(&history, record);
    g_assert_null(pgraph_vk_family_history_next_unattempted(&history));

    pgraph_vk_family_history_reset_attempts(&history);
    g_assert_nonnull(pgraph_vk_family_history_next_unattempted(&history));
    pgraph_vk_family_history_destroy(&history);
}

static void test_bounded_history_evicts_least_useful(void)
{
    static const uint8_t family_a[] = { 1 };
    static const uint8_t family_b[] = { 2 };
    static const uint8_t family_c[] = { 3 };
    PGRAPHVkFamilyHistory history;

    g_assert_true(pgraph_vk_family_history_init(&history, 2));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_a, sizeof(family_a)));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_a, sizeof(family_a)));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_b, sizeof(family_b)));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family_c, sizeof(family_c)));

    g_assert_nonnull(pgraph_vk_family_history_find(
        &history, family_a, sizeof(family_a)));
    g_assert_null(pgraph_vk_family_history_find(
        &history, family_b, sizeof(family_b)));
    g_assert_nonnull(pgraph_vk_family_history_find(
        &history, family_c, sizeof(family_c)));
    g_assert_cmpuint(history.count, ==, 2);
    pgraph_vk_family_history_destroy(&history);
}

static void test_round_trip_and_corruption(void)
{
    static const uint8_t family_a[] = { 0x10, 0x20, 0x30, 0x40 };
    static const uint8_t family_b[] = { 0xaa, 0xbb, 0xcc };
    PGRAPHVkFamilyHistory source;
    PGRAPHVkFamilyHistory loaded;
    PGRAPHVkFamilyHistoryBlob blob = { 0 };

    g_assert_true(pgraph_vk_family_history_init(&source, 4));
    g_assert_true(pgraph_vk_family_history_init(&loaded, 4));
    g_assert_true(pgraph_vk_family_history_note(
        &source, family_a, sizeof(family_a)));
    g_assert_true(pgraph_vk_family_history_note(
        &source, family_b, sizeof(family_b)));
    g_assert_true(pgraph_vk_family_history_note(
        &source, family_a, sizeof(family_a)));
    g_assert_true(pgraph_vk_family_history_serialize(&source, &blob));

    g_assert_cmpint(pgraph_vk_family_history_load(
                        &loaded, blob.data, blob.size),
                    ==, PGRAPH_VK_FAMILY_HISTORY_LOAD_OK);
    const PGRAPHVkFamilyHistoryRecord *record =
        pgraph_vk_family_history_find(
            &loaded, family_a, sizeof(family_a));
    g_assert_nonnull(record);
    g_assert_cmpuint(record->uses, ==, 2);
    g_assert_false(loaded.dirty);

    uint8_t saved = blob.data[blob.size - 1];
    blob.data[blob.size - 1] ^= 0xff;
    g_assert_cmpint(pgraph_vk_family_history_load(
                        &loaded, blob.data, blob.size),
                    ==, PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID);
    g_assert_cmpuint(loaded.count, ==, 2);
    blob.data[blob.size - 1] = saved;
    g_assert_cmpint(pgraph_vk_family_history_load(
                        &loaded, blob.data, blob.size - 1),
                    ==, PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID);
    g_assert_cmpuint(loaded.count, ==, 2);

    pgraph_vk_family_history_blob_destroy(&blob);
    pgraph_vk_family_history_destroy(&loaded);
    pgraph_vk_family_history_destroy(&source);
}

static void test_fallback_sessions_retain_prior_history(void)
{
    static const uint8_t family_a[] = { 0xa1 };
    static const uint8_t family_b[] = { 0xb2 };
    PGRAPHVkFamilyHistory first;
    PGRAPHVkFamilyHistory second;
    PGRAPHVkFamilyHistory third;
    PGRAPHVkFamilyHistoryBlob saved = { 0 };
    PGRAPHVkFamilyHistoryBlob updated = { 0 };

    g_assert_true(pgraph_vk_family_history_should_load(true, true));
    g_assert_false(pgraph_vk_family_history_should_load(false, true));
    g_assert_false(pgraph_vk_family_history_should_load(true, false));
    g_assert_true(pgraph_vk_family_history_init(&first, 4));
    g_assert_true(pgraph_vk_family_history_init(&second, 4));
    g_assert_true(pgraph_vk_family_history_init(&third, 4));
    g_assert_true(pgraph_vk_family_history_note(&first, family_a, 1));
    g_assert_true(pgraph_vk_family_history_serialize(&first, &saved));

    /* A second Fallback session loads metadata without scheduling it. */
    g_assert_cmpint(pgraph_vk_family_history_load(
                        &second, saved.data, saved.size),
                    ==, PGRAPH_VK_FAMILY_HISTORY_LOAD_OK);
    g_assert_true(pgraph_vk_family_history_note(&second, family_b, 1));
    g_assert_true(pgraph_vk_family_history_serialize(&second, &updated));
    g_assert_cmpint(pgraph_vk_family_history_load(
                        &third, updated.data, updated.size),
                    ==, PGRAPH_VK_FAMILY_HISTORY_LOAD_OK);
    g_assert_nonnull(pgraph_vk_family_history_find(&third, family_a, 1));
    g_assert_nonnull(pgraph_vk_family_history_find(&third, family_b, 1));

    pgraph_vk_family_history_blob_destroy(&updated);
    pgraph_vk_family_history_blob_destroy(&saved);
    pgraph_vk_family_history_destroy(&third);
    pgraph_vk_family_history_destroy(&second);
    pgraph_vk_family_history_destroy(&first);
}

static void test_publish_is_atomic_and_clears_dirty(void)
{
    static const uint8_t family[] = { 9, 8, 7 };
    PGRAPHVkFamilyHistory history;
    PublishFixture fixture = {
        .written = g_byte_array_new(),
        .replace_success = true,
    };
    const PGRAPHVkFamilyHistoryFileOps ops = {
        .write = fixture_write,
        .replace = fixture_replace,
        .remove = fixture_remove,
    };

    g_assert_true(pgraph_vk_family_history_init(&history, 4));
    g_assert_true(pgraph_vk_family_history_note(
        &history, family, sizeof(family)));
    g_assert_true(history.dirty);
    g_assert_true(pgraph_vk_family_history_publish(
        &history, "families.tmp", "families.bin", &ops, &fixture));
    g_assert_cmpuint(fixture.written->len, >, 0);
    g_assert_cmpuint(fixture.replacements, ==, 1);
    g_assert_cmpuint(fixture.removals, ==, 0);
    g_assert_false(history.dirty);

    g_assert_true(pgraph_vk_family_history_note(
        &history, family, sizeof(family)));
    fixture.replace_success = false;
    g_assert_false(pgraph_vk_family_history_publish(
        &history, "families.tmp", "families.bin", &ops, &fixture));
    g_assert_cmpuint(fixture.replacements, ==, 2);
    g_assert_cmpuint(fixture.removals, ==, 1);
    g_assert_true(history.dirty);

    g_byte_array_unref(fixture.written);
    pgraph_vk_family_history_destroy(&history);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/vk/family-history/rank-attempts",
                    test_rank_and_session_attempts);
    g_test_add_func("/nv2a/vk/family-history/bounded-eviction",
                    test_bounded_history_evicts_least_useful);
    g_test_add_func("/nv2a/vk/family-history/round-trip-corruption",
                    test_round_trip_and_corruption);
    g_test_add_func("/nv2a/vk/family-history/atomic-publish",
                    test_publish_is_atomic_and_clears_dirty);
    g_test_add_func("/nv2a/vk/family-history/fallback-sessions",
                    test_fallback_sessions_retain_prior_history);
    return g_test_run();
}

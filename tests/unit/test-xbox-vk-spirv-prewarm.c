/*
 * NV2A Vulkan SPIR-V prewarm cache tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw/xbox/nv2a/pgraph/vk/spirv-prewarm.h"

static const char test_flavor[] = "glslang-test";
static const char vertex_source[] = "#version 460\nvoid main(){}";
static const char fragment_source[] = "#version 460\nvoid main(){ }";
static const uint32_t test_spirv[] = {
    0x07230203, 0x00010600, 0, 1, 0,
    0x00010000, /* OpNop: structurally complete one-word instruction. */
};

static PGRAPHVkSpirvCachePolicy test_policy(void)
{
    return (PGRAPHVkSpirvCachePolicy) {
        .generator_abi = 3,
        .compiler_major = 15,
        .compiler_minor = 1,
        .compiler_patch = 0,
        .client_target = 0x00403000,
        .spirv_target = 0x00010600,
        .compiler_flags = 0x37,
        .compiler_flavor = test_flavor,
        .compiler_flavor_size = sizeof(test_flavor) - 1,
    };
}

static void add_vertex(PGRAPHVkSpirvCache *cache)
{
    assert(pgraph_vk_spirv_cache_add(
        cache, 1, vertex_source, sizeof(vertex_source) - 1, test_spirv,
        sizeof(test_spirv)));
}

static PGRAPHVkSpirvCacheBlob serialize_vertex_cache(void)
{
    PGRAPHVkSpirvCache cache = { 0 };
    PGRAPHVkSpirvCacheBlob blob = { 0 };
    PGRAPHVkSpirvCachePolicy policy = test_policy();

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    add_vertex(&cache);
    assert(pgraph_vk_spirv_cache_serialize(&cache, &blob));
    pgraph_vk_spirv_cache_destroy(&cache);
    return blob;
}

static void test_round_trip_and_exact_identity(void)
{
    PGRAPHVkSpirvCacheBlob blob = serialize_vertex_cache();
    PGRAPHVkSpirvCache cache = { 0 };
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    const uint8_t *spirv = NULL;
    size_t spirv_size = 0;

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(pgraph_vk_spirv_cache_load(&cache, blob.data, blob.size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_OK);
    assert(!pgraph_vk_spirv_cache_is_dirty(&cache));
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, vertex_source, sizeof(vertex_source) - 1, &spirv,
               &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);
    assert(pgraph_vk_spirv_cache_is_dirty(&cache));
    assert(spirv_size == sizeof(test_spirv));
    assert(!memcmp(spirv, test_spirv, sizeof(test_spirv)));

    /* Hash selection never substitutes for exact stage/source comparison. */
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 16, vertex_source, sizeof(vertex_source) - 1, &spirv,
               &spirv_size) == PGRAPH_VK_SPIRV_CACHE_MISS);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, fragment_source, sizeof(fragment_source) - 1,
               &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_MISS);

    const PGRAPHVkSpirvCacheStats *stats =
        pgraph_vk_spirv_cache_stats(&cache);
    assert(stats->hits == 1 && stats->misses == 2);
    assert(stats->records == 1);
    assert(stats->source_bytes == sizeof(vertex_source) - 1);
    assert(stats->spirv_bytes == sizeof(test_spirv));

    pgraph_vk_spirv_cache_destroy(&cache);
    free(blob.data);
}

static void test_policy_mismatch_is_rejected_transactionally(void)
{
    static const char other_flavor[] = "glslang-alt!";
    PGRAPHVkSpirvCacheBlob blob = serialize_vertex_cache();
    PGRAPHVkSpirvCachePolicy expected = test_policy();
    PGRAPHVkSpirvCachePolicy variants[9];
    for (size_t i = 0; i < 9; i++) {
        variants[i] = expected;
    }
    variants[0].generator_abi++;
    variants[1].compiler_major++;
    variants[2].compiler_minor++;
    variants[3].compiler_patch++;
    variants[4].client_target++;
    variants[5].spirv_target++;
    variants[6].compiler_flags ^= 1;
    variants[7].compiler_flavor_size--;
    variants[8].compiler_flavor = other_flavor;

    for (size_t i = 0; i < 9; i++) {
        PGRAPHVkSpirvCache cache = { 0 };
        const uint8_t *spirv = (const uint8_t *)0x1;
        size_t spirv_size = 123;
        assert(pgraph_vk_spirv_cache_init(&cache, &variants[i]));
        assert(pgraph_vk_spirv_cache_load(&cache, blob.data, blob.size) ==
               PGRAPH_VK_SPIRV_CACHE_LOAD_INCOMPATIBLE);
        assert(pgraph_vk_spirv_cache_lookup(
                   &cache, 1, vertex_source, sizeof(vertex_source) - 1,
                   &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_MISS);
        assert(spirv == (const uint8_t *)0x1 && spirv_size == 123);
        assert(pgraph_vk_spirv_cache_stats(&cache)->rejections == 1);
        pgraph_vk_spirv_cache_destroy(&cache);
    }
    free(blob.data);
}

static void store_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void store_u64_le(uint8_t *dst, uint64_t value)
{
    store_u32_le(dst, (uint32_t)value);
    store_u32_le(dst + 4, (uint32_t)(value >> 32));
}

/* Independent file-format checksum, never the loader's private helper. */
static uint64_t fixture_hash(const uint8_t *data, size_t size)
{
    uint64_t value = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < size; i++) {
        value ^= data[i];
        value *= UINT64_C(0x100000001b3);
    }
    return value;
}

static void refresh_payload_checksum(uint8_t *data, size_t size)
{
    assert(size >= PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE);
    store_u64_le(data + 64,
                 fixture_hash(data + PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE,
                              size - PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE));
}

static uint32_t fixture_load_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | (uint32_t)src[1] << 8 |
           (uint32_t)src[2] << 16 | (uint32_t)src[3] << 24;
}

static size_t first_record_offset(const uint8_t *data)
{
    return PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE +
           fixture_load_u32_le(data + 44);
}

static size_t first_spirv_offset(const uint8_t *data)
{
    size_t record = first_record_offset(data);
    return record + 32 + fixture_load_u32_le(data + record + 4);
}

static void refresh_first_spirv_checksums(uint8_t *data, size_t size)
{
    size_t record = first_record_offset(data);
    size_t spirv = first_spirv_offset(data);
    uint32_t spirv_size = fixture_load_u32_le(data + record + 8);

    assert(spirv <= size && spirv_size <= size - spirv);
    store_u64_le(data + record + 24,
                 fixture_hash(data + spirv, spirv_size));
    refresh_payload_checksum(data, size);
}

static void test_spirv_preflight_rejects_unsafe_modules(void)
{
    enum {
        ZERO_ID_BOUND,
        OVERSIZED_ID_BOUND,
        NONZERO_SCHEMA,
        ZERO_INSTRUCTION_WORD_COUNT,
        OVERRUNNING_INSTRUCTION,
        POLICY_INCOMPATIBLE_VERSION,
        NUM_VARIANTS,
    };
    PGRAPHVkSpirvCacheBlob valid = serialize_vertex_cache();
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    size_t spirv = first_spirv_offset(valid.data);

    for (unsigned int variant = 0; variant < NUM_VARIANTS; variant++) {
        PGRAPHVkSpirvCache cache = { 0 };
        uint8_t *data = malloc(valid.size);
        assert(data);
        memcpy(data, valid.data, valid.size);

        switch (variant) {
        case ZERO_ID_BOUND:
            store_u32_le(data + spirv + 12, 0);
            break;
        case OVERSIZED_ID_BOUND:
            store_u32_le(data + spirv + 12,
                         PGRAPH_VK_SPIRV_CACHE_MAX_ID_BOUND + 1);
            break;
        case NONZERO_SCHEMA:
            store_u32_le(data + spirv + 16, 1);
            break;
        case ZERO_INSTRUCTION_WORD_COUNT:
            store_u32_le(data + spirv + 20, 0);
            break;
        case OVERRUNNING_INSTRUCTION:
            store_u32_le(data + spirv + 20, 0xffff0000U);
            break;
        case POLICY_INCOMPATIBLE_VERSION:
            store_u32_le(data + spirv + 4, 0x00010500U);
            break;
        default:
            assert(false);
        }
        refresh_first_spirv_checksums(data, valid.size);

        assert(pgraph_vk_spirv_cache_init(&cache, &policy));
        assert(pgraph_vk_spirv_cache_load(&cache, data, valid.size) ==
               PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
        assert(pgraph_vk_spirv_cache_stats(&cache)->records == 0);
        pgraph_vk_spirv_cache_destroy(&cache);
        free(data);
    }

    /* The largest accepted bound still caps SPIRV-Reflect's ID allocation. */
    PGRAPHVkSpirvCache cache = { 0 };
    store_u32_le(valid.data + spirv + 12,
                 PGRAPH_VK_SPIRV_CACHE_MAX_ID_BOUND);
    refresh_first_spirv_checksums(valid.data, valid.size);
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(pgraph_vk_spirv_cache_load(&cache, valid.data, valid.size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_OK);
    pgraph_vk_spirv_cache_destroy(&cache);
    free(valid.data);
}

static void test_corruption_truncation_version_and_bounds_reject(void)
{
    PGRAPHVkSpirvCacheBlob valid = serialize_vertex_cache();
    PGRAPHVkSpirvCachePolicy policy = test_policy();

    for (unsigned int variant = 0; variant < 7; variant++) {
        PGRAPHVkSpirvCache cache = { 0 };
        uint8_t *data = malloc(valid.size);
        memcpy(data, valid.data, valid.size);
        size_t size = valid.size;

        switch (variant) {
        case 0:
            data[size - 1] ^= 0x80;
            break;
        case 1:
            size--;
            break;
        case 2:
            store_u32_le(data + 4, PGRAPH_VK_SPIRV_CACHE_ABI + 1);
            break;
        case 3:
            store_u32_le(data + 12,
                         PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS + 1);
            break;
        case 4:
            store_u32_le(data + PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE +
                                    sizeof(test_flavor) - 1 + 4,
                         PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE + 1);
            refresh_payload_checksum(data, size);
            break;
        case 5:
            store_u32_le(data + 48,
                         PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES + 1);
            break;
        case 6:
            store_u32_le(data + 56,
                         PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_BYTES + 1);
            break;
        default:
            assert(false);
        }

        assert(pgraph_vk_spirv_cache_init(&cache, &policy));
        assert(pgraph_vk_spirv_cache_load(&cache, data, size) ==
               PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
        assert(pgraph_vk_spirv_cache_stats(&cache)->records == 0);
        assert(pgraph_vk_spirv_cache_stats(&cache)->rejections == 1);
        pgraph_vk_spirv_cache_destroy(&cache);
        free(data);
    }
    free(valid.data);
}

static void test_structural_rejection_after_valid_outer_checksum(void)
{
    enum {
        RESERVED_FIELD, SPIRV_SIZE_LIMIT, RECORD_HEADER_EXTENT,
        RECORD_PAYLOAD_EXTENT, SOURCE_TOTAL, SPIRV_TOTAL,
        SOURCE_CHECKSUM, SPIRV_CHECKSUM, SPIRV_MAGIC, EMPTY_SOURCE,
        NUM_CASES,
    };
    PGRAPHVkSpirvCacheBlob valid = serialize_vertex_cache();
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    const size_t record = PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE +
                          sizeof(test_flavor) - 1;
    const size_t spv = record + 32 + sizeof(vertex_source) - 1;

    assert(fixture_hash((const uint8_t *)"a", 1) ==
           UINT64_C(0xaf63dc4c8601ec8c));
    /* Recomputed checksums must also admit the untouched positive control. */
    refresh_payload_checksum(valid.data, valid.size);
    PGRAPHVkSpirvCache control = { 0 };
    assert(pgraph_vk_spirv_cache_init(&control, &policy));
    assert(pgraph_vk_spirv_cache_load(&control, valid.data, valid.size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_OK);
    pgraph_vk_spirv_cache_destroy(&control);

    for (unsigned int variant = 0; variant < NUM_CASES; variant++) {
        PGRAPHVkSpirvCache cache = { 0 };
        uint8_t *data = malloc(valid.size);
        assert(data);
        memcpy(data, valid.data, valid.size);
        size_t size = valid.size;
        switch (variant) {
        case RESERVED_FIELD:
            store_u32_le(data + record + 12, 1);
            break;
        case SPIRV_SIZE_LIMIT:
            store_u32_le(data + record + 8,
                         PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_SIZE + 4);
            break;
        case RECORD_HEADER_EXTENT:
            size = record + 31;
            break;
        case RECORD_PAYLOAD_EXTENT:
            size--;
            break;
        case SOURCE_TOTAL:
            store_u64_le(data + 48, sizeof(vertex_source) - 2);
            break;
        case SPIRV_TOTAL:
            store_u64_le(data + 56, sizeof(test_spirv) - 4);
            break;
        case SOURCE_CHECKSUM:
            data[record + 16] ^= 1;
            break;
        case SPIRV_CHECKSUM:
            data[record + 24] ^= 1;
            break;
        case SPIRV_MAGIC:
            store_u32_le(data + spv, 0);
            store_u64_le(data + record + 24,
                         fixture_hash(data + spv, sizeof(test_spirv)));
            break;
        case EMPTY_SOURCE:
            store_u32_le(data + record + 4, 0);
            break;
        default:
            assert(false);
        }
        refresh_payload_checksum(data, size);

        assert(pgraph_vk_spirv_cache_init(&cache, &policy));
        assert(pgraph_vk_spirv_cache_add(
            &cache, 16, fragment_source, sizeof(fragment_source) - 1,
            test_spirv, sizeof(test_spirv)));
        assert(pgraph_vk_spirv_cache_load(&cache, data, size) ==
               PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
        assert(pgraph_vk_spirv_cache_stats(&cache)->records == 1);
        assert(pgraph_vk_spirv_cache_stats(&cache)->rejections == 1);
        const uint8_t *spirv = NULL;
        size_t spirv_size = 0;
        assert(pgraph_vk_spirv_cache_lookup(
                   &cache, 16, fragment_source, sizeof(fragment_source) - 1,
                   &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);
        assert(spirv_size == sizeof(test_spirv));
        assert(!memcmp(spirv, test_spirv, sizeof(test_spirv)));
        pgraph_vk_spirv_cache_destroy(&cache);
        free(data);
    }
    free(valid.data);
}

static void test_failed_load_keeps_existing_store_visible(void)
{
    PGRAPHVkSpirvCacheBlob invalid = serialize_vertex_cache();
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    const uint8_t *spirv = NULL;
    size_t spirv_size = 0;

    invalid.data[invalid.size - 1] ^= 0x40;
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(pgraph_vk_spirv_cache_add(
        &cache, 16, fragment_source, sizeof(fragment_source) - 1,
        test_spirv, sizeof(test_spirv)));
    assert(pgraph_vk_spirv_cache_load(&cache, invalid.data, invalid.size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 16, fragment_source, sizeof(fragment_source) - 1,
               &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);
    assert(spirv_size == sizeof(test_spirv));

    pgraph_vk_spirv_cache_destroy(&cache);
    free(invalid.data);
}

static void test_spirv_and_aggregate_bounds(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    uint32_t bad_spirv[5];

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    memcpy(bad_spirv, test_spirv, sizeof(bad_spirv));
    bad_spirv[0] = 0;
    assert(!pgraph_vk_spirv_cache_add(
        &cache, 1, vertex_source, sizeof(vertex_source) - 1, bad_spirv,
        sizeof(bad_spirv)));
    assert(!pgraph_vk_spirv_cache_add(
        &cache, 1, vertex_source, sizeof(vertex_source) - 1, test_spirv,
        sizeof(test_spirv) - 1));
    assert(!pgraph_vk_spirv_cache_add(
        &cache, 1, vertex_source, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE + 1,
        test_spirv, sizeof(test_spirv)));
    assert(!pgraph_vk_spirv_cache_add(
        &cache, 1, vertex_source, sizeof(vertex_source) - 1, test_spirv,
        PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_SIZE + 4));
    assert(pgraph_vk_spirv_cache_stats(&cache)->rejections == 4);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 0);
    pgraph_vk_spirv_cache_destroy(&cache);
}

static void test_rejected_hit_falls_back_and_can_be_replaced(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    add_vertex(&cache);
    assert(pgraph_vk_spirv_cache_reject_hit(
        &cache, 1, vertex_source, sizeof(vertex_source) - 1));
    assert(pgraph_vk_spirv_cache_stats(&cache)->fallbacks == 1);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 0);
    add_vertex(&cache);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 1);
    pgraph_vk_spirv_cache_destroy(&cache);
}

typedef struct AdoptFixture {
    unsigned int calls;
    PGRAPHVkSpirvCacheArtifactResult result;
} AdoptFixture;

static PGRAPHVkSpirvCacheArtifactResult adopt_cached_spirv(
    void *opaque, const uint8_t *spirv, size_t spirv_size)
{
    AdoptFixture *fixture = opaque;

    fixture->calls++;
    assert(spirv_size == sizeof(test_spirv));
    assert(!memcmp(spirv, test_spirv, sizeof(test_spirv)));
    return fixture->result;
}

static void test_cached_artifact_adoption_controls_compile_fallback(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    AdoptFixture fixture = {
        .result = PGRAPH_VK_SPIRV_CACHE_ARTIFACT_ACCEPTED,
    };

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    add_vertex(&cache);
    assert(pgraph_vk_spirv_cache_adopt_hit(
               &cache, 1, vertex_source, sizeof(vertex_source) - 1,
               adopt_cached_spirv, &fixture) ==
           PGRAPH_VK_SPIRV_CACHE_ADOPTED);
    assert(!pgraph_vk_spirv_cache_adoption_needs_compile(
        PGRAPH_VK_SPIRV_CACHE_ADOPTED));
    assert(fixture.calls == 1);
    assert(pgraph_vk_spirv_cache_stats(&cache)->fallbacks == 0);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 1);

    fixture.result = PGRAPH_VK_SPIRV_CACHE_ARTIFACT_DEFERRED;
    assert(pgraph_vk_spirv_cache_adopt_hit(
               &cache, 1, vertex_source, sizeof(vertex_source) - 1,
               adopt_cached_spirv, &fixture) ==
           PGRAPH_VK_SPIRV_CACHE_DEFERRED);
    assert(!pgraph_vk_spirv_cache_adoption_needs_compile(
        PGRAPH_VK_SPIRV_CACHE_DEFERRED));
    assert(fixture.calls == 2);
    assert(pgraph_vk_spirv_cache_stats(&cache)->fallbacks == 0);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 1);

    fixture.result = PGRAPH_VK_SPIRV_CACHE_ARTIFACT_REJECTED;
    assert(pgraph_vk_spirv_cache_adopt_hit(
               &cache, 1, vertex_source, sizeof(vertex_source) - 1,
               adopt_cached_spirv, &fixture) ==
           PGRAPH_VK_SPIRV_CACHE_REJECTED);
    assert(pgraph_vk_spirv_cache_adoption_needs_compile(
        PGRAPH_VK_SPIRV_CACHE_REJECTED));
    assert(fixture.calls == 3);
    assert(pgraph_vk_spirv_cache_stats(&cache)->fallbacks == 1);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 0);

    assert(pgraph_vk_spirv_cache_adopt_hit(
               &cache, 1, vertex_source, sizeof(vertex_source) - 1,
               adopt_cached_spirv, &fixture) ==
           PGRAPH_VK_SPIRV_CACHE_NOT_FOUND);
    assert(pgraph_vk_spirv_cache_adoption_needs_compile(
        PGRAPH_VK_SPIRV_CACHE_NOT_FOUND));
    assert(fixture.calls == 3);
    pgraph_vk_spirv_cache_destroy(&cache);
}

static void make_source(char *source, size_t size, const char *prefix,
                        uint32_t index)
{
    int written = snprintf(source, size, "%s-%08" PRIu32, prefix, index);
    assert(written > 0 && (size_t)written < size);
}

static void test_persistence_accepts_only_graphics_stages(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    const uint32_t graphics_stages[] = { 1, 8, 16 };

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    for (size_t i = 0;
         i < sizeof(graphics_stages) / sizeof(graphics_stages[0]); i++) {
        char source[32];
        make_source(source, sizeof(source), "graphics", graphics_stages[i]);
        assert(pgraph_vk_spirv_cache_add(
            &cache, graphics_stages[i], source, strlen(source), test_spirv,
            sizeof(test_spirv)));
    }
    assert(!pgraph_vk_spirv_cache_add(
        &cache, 32, vertex_source, sizeof(vertex_source) - 1, test_spirv,
        sizeof(test_spirv)));
    assert(!pgraph_vk_spirv_cache_add(
        &cache, 0, vertex_source, sizeof(vertex_source) - 1, test_spirv,
        sizeof(test_spirv)));
    assert(pgraph_vk_spirv_cache_stats(&cache)->records ==
           sizeof(graphics_stages) / sizeof(graphics_stages[0]));
    pgraph_vk_spirv_cache_destroy(&cache);

    PGRAPHVkSpirvCacheBlob blob = serialize_vertex_cache();
    size_t record = first_record_offset(blob.data);
    store_u32_le(blob.data + record, 32);
    refresh_payload_checksum(blob.data, blob.size);
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(pgraph_vk_spirv_cache_load(&cache, blob.data, blob.size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 0);
    pgraph_vk_spirv_cache_destroy(&cache);
    free(blob.data);
}

static void test_hash_index_collision_keeps_exact_sources_distinct(void)
{
    uint32_t first_by_bucket[PGRAPH_VK_SPIRV_CACHE_INDEX_BUCKETS];
    char first_source[32];
    char second_source[32];
    bool found = false;

    for (size_t i = 0; i < PGRAPH_VK_SPIRV_CACHE_INDEX_BUCKETS; i++) {
        first_by_bucket[i] = UINT32_MAX;
    }
    for (uint32_t i = 0; i < 100000 && !found; i++) {
        char source[32];
        make_source(source, sizeof(source), "collision", i);
        uint32_t bucket = (uint32_t)(
            fixture_hash((const uint8_t *)source, strlen(source)) &
            (PGRAPH_VK_SPIRV_CACHE_INDEX_BUCKETS - 1));
        if (first_by_bucket[bucket] != UINT32_MAX) {
            make_source(first_source, sizeof(first_source), "collision",
                        first_by_bucket[bucket]);
            memcpy(second_source, source, strlen(source) + 1);
            found = true;
        } else {
            first_by_bucket[bucket] = i;
        }
    }
    assert(found && strcmp(first_source, second_source));

    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    const uint8_t *spirv = NULL;
    size_t spirv_size = 0;
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(pgraph_vk_spirv_cache_add(
        &cache, 1, first_source, strlen(first_source), test_spirv,
        sizeof(test_spirv)));
    assert(pgraph_vk_spirv_cache_add(
        &cache, 1, second_source, strlen(second_source), test_spirv,
        sizeof(test_spirv)));
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, first_source, strlen(first_source), &spirv,
               &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, second_source, strlen(second_source), &spirv,
               &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == 2);
    pgraph_vk_spirv_cache_destroy(&cache);
}

static void test_full_cache_evicts_lru_and_learns_new_titles(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    const uint8_t *spirv = NULL;
    size_t spirv_size = 0;
    char source[32];

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    for (uint32_t i = 0; i < PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS; i++) {
        make_source(source, sizeof(source), "old-title", i);
        assert(pgraph_vk_spirv_cache_add(
            &cache, 1, source, strlen(source), test_spirv,
            sizeof(test_spirv)));
    }

    make_source(source, sizeof(source), "old-title", 0);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, strlen(source), &spirv, &spirv_size) ==
           PGRAPH_VK_SPIRV_CACHE_HIT);
    static const char new_title_source[] = "new-title-first-source";
    assert(pgraph_vk_spirv_cache_add(
        &cache, 16, new_title_source, sizeof(new_title_source) - 1, test_spirv,
        sizeof(test_spirv)));

    make_source(source, sizeof(source), "old-title", 1);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, strlen(source), &spirv, &spirv_size) ==
           PGRAPH_VK_SPIRV_CACHE_MISS);
    make_source(source, sizeof(source), "old-title", 0);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, strlen(source), &spirv, &spirv_size) ==
           PGRAPH_VK_SPIRV_CACHE_HIT);
    assert(spirv_size == sizeof(test_spirv));
    assert(pgraph_vk_spirv_cache_stats(&cache)->records ==
           PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS);

    PGRAPHVkSpirvCacheBlob blob = { 0 };
    PGRAPHVkSpirvCacheBlob duplicate = { 0 };
    assert(pgraph_vk_spirv_cache_serialize(&cache, &blob));
    assert(pgraph_vk_spirv_cache_serialize(&cache, &duplicate));
    assert(blob.size == duplicate.size);
    assert(!memcmp(blob.data, duplicate.data, blob.size));
    pgraph_vk_spirv_cache_destroy(&cache);

    /* Persisted recency must still admit a later title after restart. */
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(pgraph_vk_spirv_cache_load(&cache, blob.data, blob.size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_OK);
    PGRAPHVkSpirvCacheBlob restarted = { 0 };
    assert(pgraph_vk_spirv_cache_serialize(&cache, &restarted));
    assert(restarted.size == blob.size);
    assert(!memcmp(restarted.data, blob.data, blob.size));
    free(restarted.data);
    static const char next_title_source[] = "new-title-after-restart";
    assert(pgraph_vk_spirv_cache_add(
        &cache, 8, next_title_source, sizeof(next_title_source) - 1,
        test_spirv, sizeof(test_spirv)));
    make_source(source, sizeof(source), "old-title", 2);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, strlen(source), &spirv, &spirv_size) ==
           PGRAPH_VK_SPIRV_CACHE_MISS);
    make_source(source, sizeof(source), "old-title", 0);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, strlen(source), &spirv, &spirv_size) ==
           PGRAPH_VK_SPIRV_CACHE_HIT);
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 16, new_title_source, sizeof(new_title_source) - 1,
               &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);
    pgraph_vk_spirv_cache_destroy(&cache);
    free(blob.data);
    free(duplicate.data);
}

static void test_source_byte_saturation_evicts_lru(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    uint8_t *source = malloc(PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE);
    const uint8_t *spirv = NULL;
    size_t spirv_size = 0;
    const uint32_t source_count =
        PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES /
        PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE;

    assert(source);
    memset(source, 0x5a, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE);
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    for (uint32_t i = 0; i < source_count; i++) {
        memcpy(source, &i, sizeof(i));
        assert(pgraph_vk_spirv_cache_add(
            &cache, 1, source, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE,
            test_spirv, sizeof(test_spirv)));
    }

    uint32_t identity = 0;
    memcpy(source, &identity, sizeof(identity));
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE,
               &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);

    identity = source_count;
    memcpy(source, &identity, sizeof(identity));
    assert(pgraph_vk_spirv_cache_add(
        &cache, 1, source, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE, test_spirv,
        sizeof(test_spirv)));
    assert(pgraph_vk_spirv_cache_stats(&cache)->records == source_count);
    assert(pgraph_vk_spirv_cache_stats(&cache)->source_bytes ==
           PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES);

    identity = 1;
    memcpy(source, &identity, sizeof(identity));
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE,
               &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_MISS);
    identity = 0;
    memcpy(source, &identity, sizeof(identity));
    assert(pgraph_vk_spirv_cache_lookup(
               &cache, 1, source, PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE,
               &spirv, &spirv_size) == PGRAPH_VK_SPIRV_CACHE_HIT);

    pgraph_vk_spirv_cache_destroy(&cache);
    free(source);
}

typedef struct MockFiles {
    uint8_t *temporary;
    size_t temporary_size;
    uint8_t *published;
    size_t published_size;
    bool fail_write;
    bool fail_replace;
    bool removed;
    unsigned int write_attempts;
    unsigned int replace_attempts;
} MockFiles;

static bool mock_write(void *opaque, const char *path, const uint8_t *data,
                       size_t size)
{
    MockFiles *files = opaque;
    assert(!strcmp(path, "cache.tmp"));
    files->write_attempts++;
    if (files->fail_write) {
        return false;
    }
    free(files->temporary);
    files->temporary = malloc(size);
    memcpy(files->temporary, data, size);
    files->temporary_size = size;
    return true;
}

static bool mock_replace(void *opaque, const char *temporary,
                         const char *published)
{
    MockFiles *files = opaque;
    assert(!strcmp(temporary, "cache.tmp"));
    assert(!strcmp(published, "cache.bin"));
    files->replace_attempts++;
    if (files->fail_replace) {
        return false;
    }
    free(files->published);
    files->published = files->temporary;
    files->published_size = files->temporary_size;
    files->temporary = NULL;
    files->temporary_size = 0;
    return true;
}

static void mock_remove(void *opaque, const char *path)
{
    MockFiles *files = opaque;
    assert(!strcmp(path, "cache.tmp"));
    free(files->temporary);
    files->temporary = NULL;
    files->temporary_size = 0;
    files->removed = true;
}

static void test_failed_publish_preserves_previous_cache(void)
{
    static const uint8_t old_cache[] = "previous-valid-cache";
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };
    MockFiles files = { 0 };
    PGRAPHVkSpirvCacheFileOps ops = {
        .write = mock_write,
        .replace = mock_replace,
        .remove = mock_remove,
    };

    files.published = malloc(sizeof(old_cache));
    memcpy(files.published, old_cache, sizeof(old_cache));
    files.published_size = sizeof(old_cache);
    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    add_vertex(&cache);

    files.fail_write = true;
    assert(!pgraph_vk_spirv_cache_publish(
        &cache, "cache.tmp", "cache.bin", &ops, &files));
    assert(files.published_size == sizeof(old_cache));
    assert(!memcmp(files.published, old_cache, sizeof(old_cache)));
    assert(pgraph_vk_spirv_cache_is_dirty(&cache));
    assert(files.write_attempts == 1);
    assert(files.replace_attempts == 0);

    files.fail_write = false;
    files.fail_replace = true;
    assert(!pgraph_vk_spirv_cache_publish(
        &cache, "cache.tmp", "cache.bin", &ops, &files));
    assert(files.removed);
    assert(files.published_size == sizeof(old_cache));
    assert(!memcmp(files.published, old_cache, sizeof(old_cache)));
    assert(pgraph_vk_spirv_cache_is_dirty(&cache));
    assert(files.write_attempts == 2);
    assert(files.replace_attempts == 1);

    files.fail_replace = false;
    assert(pgraph_vk_spirv_cache_publish(
        &cache, "cache.tmp", "cache.bin", &ops, &files));
    assert(files.published_size > sizeof(old_cache));
    assert(!pgraph_vk_spirv_cache_is_dirty(&cache));
    assert(files.write_attempts == 3);
    assert(files.replace_attempts == 2);

    PGRAPHVkSpirvCache loaded = { 0 };
    assert(pgraph_vk_spirv_cache_init(&loaded, &policy));
    assert(pgraph_vk_spirv_cache_load(
               &loaded, files.published, files.published_size) ==
           PGRAPH_VK_SPIRV_CACHE_LOAD_OK);
    assert(pgraph_vk_spirv_cache_stats(&loaded)->records == 1);
    pgraph_vk_spirv_cache_destroy(&loaded);

    free(files.temporary);
    free(files.published);
    pgraph_vk_spirv_cache_destroy(&cache);
}

static void test_session_eligibility_and_live_toggle_control_cache_use(void)
{
    PGRAPHVkSpirvCachePolicy policy = test_policy();
    PGRAPHVkSpirvCache cache = { 0 };

    assert(pgraph_vk_spirv_cache_init(&cache, &policy));
    assert(!pgraph_vk_spirv_cache_is_active(&cache, false, false));
    assert(!pgraph_vk_spirv_cache_is_active(&cache, false, true));
    assert(pgraph_vk_spirv_cache_is_active(&cache, true, true));
    assert(!pgraph_vk_spirv_cache_is_active(&cache, true, false));
    assert(pgraph_vk_spirv_cache_is_active(&cache, true, true));
    pgraph_vk_spirv_cache_destroy(&cache);
    assert(!pgraph_vk_spirv_cache_is_active(&cache, true, true));
}

static void test_reflected_stage_must_match_expected_stage(void)
{
    assert(pgraph_vk_spirv_stage_matches(1, 1));
    assert(pgraph_vk_spirv_stage_matches(8, 8));
    assert(pgraph_vk_spirv_stage_matches(16, 16));
    assert(pgraph_vk_spirv_stage_matches(32, 32));
    assert(!pgraph_vk_spirv_stage_matches(1, 16));
    assert(!pgraph_vk_spirv_stage_matches(0, 0));
    assert(!pgraph_vk_spirv_stage_matches(17, 17));
    assert(!pgraph_vk_spirv_stage_matches(2, 2));
}

static void test_zero_member_block_still_claims_layout(void)
{
    bool seen = false;

    assert(pgraph_vk_spirv_layout_claim_block(&seen, 0, 0));
    assert(seen);
    assert(!pgraph_vk_spirv_layout_claim_block(&seen, 0, 0));

    seen = false;
    assert(!pgraph_vk_spirv_layout_claim_block(
        &seen, PGRAPH_VK_SPIRV_LAYOUT_MAX_BLOCK_SIZE + 1, 0));
    assert(!pgraph_vk_spirv_layout_claim_block(
        &seen, 0, PGRAPH_VK_SPIRV_LAYOUT_MAX_MEMBERS + 1));
}

static void test_reflected_member_arithmetic_is_bounded(void)
{
    PGRAPHVkSpirvLayoutMember member = {
        .offset = 16,
        .size = 64,
        .array_dims_count = 1,
        .array_elements = 4,
        .array_stride = 16,
        .matrix_columns = 0,
        .matrix_rows = 0,
        .matrix_stride = 0,
        .vector_components = 4,
        .scalar_width = 32,
    };
    PGRAPHVkSpirvLayoutDimensions dimensions = { 0 };

    assert(pgraph_vk_spirv_layout_validate_member(
        128, &member, &dimensions));
    assert(dimensions.vector_components == 4);
    assert(dimensions.element_count == 4);
    assert(dimensions.stride == 16);

    member.size = 48;
    assert(!pgraph_vk_spirv_layout_validate_member(
        128, &member, &dimensions));
    member.size = 64;

    member.offset = 80;
    assert(!pgraph_vk_spirv_layout_validate_member(
        128, &member, &dimensions));
    member.offset = 16;

    member.array_dims_count = 2;
    assert(!pgraph_vk_spirv_layout_validate_member(
        128, &member, &dimensions));
    member.array_dims_count = 1;

    member.array_elements = PGRAPH_VK_SPIRV_LAYOUT_MAX_ELEMENTS;
    member.matrix_columns = 4;
    member.matrix_rows = 4;
    assert(!pgraph_vk_spirv_layout_validate_member(
        PGRAPH_VK_SPIRV_LAYOUT_MAX_BLOCK_SIZE, &member, &dimensions));

    member.array_elements = 4;
    member.matrix_columns = 4;
    member.array_stride = 65;
    assert(!pgraph_vk_spirv_layout_validate_member(
        1024, &member, &dimensions));

    member.array_stride = 64;
    member.matrix_rows = 3;
    assert(!pgraph_vk_spirv_layout_validate_member(
        1024, &member, &dimensions));
    member.matrix_rows = 4;
    member.scalar_width = 64;
    assert(!pgraph_vk_spirv_layout_validate_member(
        1024, &member, &dimensions));
    member.scalar_width = 32;

    member.offset = 0;
    member.size = 128;
    member.array_elements = 2;
    member.array_stride = 64;
    assert(pgraph_vk_spirv_layout_validate_member(
        256, &member, &dimensions));
    member.size = 112;
    assert(!pgraph_vk_spirv_layout_validate_member(
        256, &member, &dimensions));

    member.array_stride = UINT32_MAX - 3;
    member.matrix_columns = 0;
    member.matrix_rows = 0;
    member.offset = UINT32_MAX - 7;
    member.size = 8;
    assert(!pgraph_vk_spirv_layout_validate_member(
        PGRAPH_VK_SPIRV_LAYOUT_MAX_BLOCK_SIZE, &member, &dimensions));
}

int main(void)
{
    test_round_trip_and_exact_identity();
    test_policy_mismatch_is_rejected_transactionally();
    test_spirv_preflight_rejects_unsafe_modules();
    test_corruption_truncation_version_and_bounds_reject();
    test_structural_rejection_after_valid_outer_checksum();
    test_failed_load_keeps_existing_store_visible();
    test_spirv_and_aggregate_bounds();
    test_rejected_hit_falls_back_and_can_be_replaced();
    test_cached_artifact_adoption_controls_compile_fallback();
    test_persistence_accepts_only_graphics_stages();
    test_hash_index_collision_keeps_exact_sources_distinct();
    test_full_cache_evicts_lru_and_learns_new_titles();
    test_source_byte_saturation_evicts_lru();
    test_failed_publish_preserves_previous_cache();
    test_session_eligibility_and_live_toggle_control_cache_use();
    test_reflected_stage_must_match_expected_stage();
    test_zero_member_block_still_claims_layout();
    test_reflected_member_arithmetic_is_bounded();
    return 0;
}

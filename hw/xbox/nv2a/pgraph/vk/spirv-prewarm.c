/*
 * Geforce NV2A PGRAPH Vulkan SPIR-V prewarm cache
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/spirv-prewarm.h"

#include <stdlib.h>
#include <string.h>

#define SPIRV_CACHE_MAGIC       0x56505358U /* XSPV */
#define SPIRV_CACHE_RECORD_SIZE 32U
#define SPIRV_MAGIC_WORD        0x07230203U
#define SPIRV_MIN_MODULE_SIZE   20U
#define SPIRV_CACHE_INDEX_NONE UINT32_MAX

enum {
    HEADER_MAGIC = 0,
    HEADER_ABI = 4,
    HEADER_SIZE = 8,
    HEADER_RECORD_COUNT = 12,
    HEADER_COMPILER_MAJOR = 16,
    HEADER_COMPILER_MINOR = 20,
    HEADER_COMPILER_PATCH = 24,
    HEADER_CLIENT_TARGET = 28,
    HEADER_SPIRV_TARGET = 32,
    HEADER_COMPILER_FLAGS = 36,
    HEADER_GENERATOR_ABI = 40,
    HEADER_FLAVOR_SIZE = 44,
    HEADER_SOURCE_BYTES = 48,
    HEADER_SPIRV_BYTES = 56,
    HEADER_PAYLOAD_HASH = 64,
};

typedef struct PGRAPHVkSpirvCacheEntry {
    uint64_t source_hash;
    uint64_t last_used;
    uint32_t stage;
    uint32_t index_next;
    size_t source_size;
    size_t spirv_size;
    uint8_t *source;
    uint8_t *spirv;
} PGRAPHVkSpirvCacheEntry;

static uint32_t index_bucket(const PGRAPHVkSpirvCache *cache,
                             uint64_t source_hash, uint32_t stage)
{
    uint64_t mixed =
        source_hash ^ ((uint64_t)stage * UINT64_C(0x9e3779b97f4a7c15));
    return (uint32_t)(mixed & (cache->index_bucket_count - 1));
}

static void rebuild_index(PGRAPHVkSpirvCache *cache)
{
    uint32_t *buckets = cache->index_buckets;
    PGRAPHVkSpirvCacheEntry *entries = cache->entries;

    memset(buckets, 0xff, cache->index_bucket_count * sizeof(*buckets));
    for (uint32_t i = 0; i < cache->stats.records; i++) {
        uint32_t bucket = index_bucket(cache, entries[i].source_hash,
                                       entries[i].stage);
        entries[i].index_next = buckets[bucket];
        buckets[bucket] = i;
    }
}

static uint64_t next_access(PGRAPHVkSpirvCache *cache)
{
    if (cache->access_clock == UINT64_MAX) {
        PGRAPHVkSpirvCacheEntry *entries = cache->entries;
        for (size_t i = 0; i < cache->stats.records; i++) {
            entries[i].last_used = i + 1;
        }
        cache->access_clock = cache->stats.records;
    }
    return ++cache->access_clock;
}

static uint32_t load_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | (uint32_t)src[1] << 8 |
           (uint32_t)src[2] << 16 | (uint32_t)src[3] << 24;
}

static uint64_t load_u64_le(const uint8_t *src)
{
    return (uint64_t)load_u32_le(src) |
           (uint64_t)load_u32_le(src + 4) << 32;
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

static uint64_t cache_hash(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool policy_valid(const PGRAPHVkSpirvCachePolicy *policy)
{
    return policy && policy->generator_abi && policy->compiler_flavor &&
           policy->compiler_flavor_size &&
           policy->compiler_flavor_size <=
               PGRAPH_VK_SPIRV_CACHE_MAX_FLAVOR_SIZE;
}

static bool persistent_stage_valid(uint32_t stage)
{
    return stage == 1U ||  /* VK_SHADER_STAGE_VERTEX_BIT */
           stage == 8U ||  /* VK_SHADER_STAGE_GEOMETRY_BIT */
           stage == 16U;   /* VK_SHADER_STAGE_FRAGMENT_BIT */
}

static bool spirv_valid(const PGRAPHVkSpirvCachePolicy *policy,
                        const void *data, size_t size)
{
    if (!data || size < SPIRV_MIN_MODULE_SIZE ||
        size > PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_SIZE ||
        size % sizeof(uint32_t)) {
        return false;
    }

    const uint8_t *bytes = data;
    size_t word_count = size / sizeof(uint32_t);
    uint32_t version = load_u32_le(bytes + sizeof(uint32_t));
    uint32_t id_bound = load_u32_le(bytes + 3 * sizeof(uint32_t));
    if (!policy || load_u32_le(bytes) != SPIRV_MAGIC_WORD ||
        version < 0x00010000U || version > 0x00010600U ||
        version != policy->spirv_target || !id_bound ||
        id_bound > PGRAPH_VK_SPIRV_CACHE_MAX_ID_BOUND ||
        load_u32_le(bytes + 4 * sizeof(uint32_t)) != 0) {
        return false;
    }

    for (size_t word = 5; word < word_count;) {
        uint32_t instruction = load_u32_le(bytes + word * sizeof(uint32_t));
        uint32_t instruction_words = instruction >> 16;
        if (!instruction_words || instruction_words > word_count - word) {
            return false;
        }
        word += instruction_words;
    }
    return true;
}

static bool source_valid(const void *data, size_t size)
{
    return data && size && size <= PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE;
}

static void free_entries(PGRAPHVkSpirvCacheEntry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(entries[i].source);
        free(entries[i].spirv);
    }
    free(entries);
}

static bool reserve_entry(PGRAPHVkSpirvCache *cache)
{
    if (cache->stats.records == cache->capacity) {
        size_t capacity = cache->capacity ? cache->capacity * 2 : 16;
        if (capacity > PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS) {
            capacity = PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS;
        }
        if (capacity == cache->capacity ||
            capacity > SIZE_MAX / sizeof(PGRAPHVkSpirvCacheEntry)) {
            return false;
        }
        void *entries = realloc(cache->entries,
                                capacity * sizeof(PGRAPHVkSpirvCacheEntry));
        if (!entries) {
            return false;
        }
        cache->entries = entries;
        cache->capacity = capacity;
    }
    return true;
}

static bool append_entry(PGRAPHVkSpirvCache *cache, uint32_t stage,
                         const void *source, size_t source_size,
                         const void *spirv, size_t spirv_size)
{
    if (!reserve_entry(cache)) {
        return false;
    }

    uint8_t *source_copy = malloc(source_size);
    uint8_t *spirv_copy = malloc(spirv_size);
    if (!source_copy || !spirv_copy) {
        free(source_copy);
        free(spirv_copy);
        return false;
    }
    memcpy(source_copy, source, source_size);
    memcpy(spirv_copy, spirv, spirv_size);

    PGRAPHVkSpirvCacheEntry *entries = cache->entries;
    entries[cache->stats.records++] = (PGRAPHVkSpirvCacheEntry) {
        .source_hash = cache_hash(source, source_size),
        .last_used = next_access(cache),
        .stage = stage,
        .index_next = SPIRV_CACHE_INDEX_NONE,
        .source_size = source_size,
        .spirv_size = spirv_size,
        .source = source_copy,
        .spirv = spirv_copy,
    };
    cache->stats.source_bytes += source_size;
    cache->stats.spirv_bytes += spirv_size;
    uint32_t index = (uint32_t)(cache->stats.records - 1);
    uint32_t bucket = index_bucket(cache, entries[index].source_hash, stage);
    uint32_t *buckets = cache->index_buckets;
    entries[index].index_next = buckets[bucket];
    buckets[bucket] = index;
    return true;
}

static PGRAPHVkSpirvCacheEntry *find_entry(PGRAPHVkSpirvCache *cache,
                                           uint32_t stage,
                                           const void *source,
                                           size_t source_size, bool touch)
{
    if (!persistent_stage_valid(stage) || !source_valid(source, source_size) ||
        !cache->index_buckets || !cache->index_bucket_count) {
        return NULL;
    }
    uint64_t hash = cache_hash(source, source_size);
    PGRAPHVkSpirvCacheEntry *entries = cache->entries;
    uint32_t *buckets = cache->index_buckets;
    uint32_t index = buckets[index_bucket(cache, hash, stage)];
    while (index != SPIRV_CACHE_INDEX_NONE) {
        if (index >= cache->stats.records) {
            return NULL;
        }
        PGRAPHVkSpirvCacheEntry *entry = &entries[index];
        if (entry->source_hash == hash && entry->stage == stage &&
            entry->source_size == source_size &&
            !memcmp(entry->source, source, source_size)) {
            if (touch) {
                entry->last_used = next_access(cache);
                cache->dirty = true;
            }
            return entry;
        }
        index = entry->index_next;
    }
    return NULL;
}

bool pgraph_vk_spirv_cache_init(PGRAPHVkSpirvCache *cache,
                                const PGRAPHVkSpirvCachePolicy *policy)
{
    if (!cache || !policy_valid(policy)) {
        return false;
    }
    void *flavor = malloc(policy->compiler_flavor_size);
    uint32_t *buckets =
        malloc(PGRAPH_VK_SPIRV_CACHE_INDEX_BUCKETS * sizeof(*buckets));
    if (!flavor || !buckets) {
        free(flavor);
        free(buckets);
        return false;
    }
    memcpy(flavor, policy->compiler_flavor, policy->compiler_flavor_size);
    *cache = (PGRAPHVkSpirvCache) {
        .policy = *policy,
        .compiler_flavor = flavor,
        .index_buckets = buckets,
        .index_bucket_count = PGRAPH_VK_SPIRV_CACHE_INDEX_BUCKETS,
    };
    cache->policy.compiler_flavor = flavor;
    rebuild_index(cache);
    return true;
}

void pgraph_vk_spirv_cache_destroy(PGRAPHVkSpirvCache *cache)
{
    if (!cache) {
        return;
    }
    free_entries(cache->entries, cache->stats.records);
    free(cache->index_buckets);
    free(cache->compiler_flavor);
    *cache = (PGRAPHVkSpirvCache) { 0 };
}

static bool policy_matches(const PGRAPHVkSpirvCache *cache,
                           const uint8_t *data, uint32_t flavor_size)
{
    const PGRAPHVkSpirvCachePolicy *policy = &cache->policy;
    return load_u32_le(data + HEADER_COMPILER_MAJOR) ==
               policy->compiler_major &&
           load_u32_le(data + HEADER_COMPILER_MINOR) ==
               policy->compiler_minor &&
           load_u32_le(data + HEADER_COMPILER_PATCH) ==
               policy->compiler_patch &&
           load_u32_le(data + HEADER_CLIENT_TARGET) == policy->client_target &&
           load_u32_le(data + HEADER_SPIRV_TARGET) == policy->spirv_target &&
           load_u32_le(data + HEADER_COMPILER_FLAGS) ==
               policy->compiler_flags &&
           load_u32_le(data + HEADER_GENERATOR_ABI) == policy->generator_abi &&
           flavor_size == policy->compiler_flavor_size &&
           !memcmp(data + PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE,
                   policy->compiler_flavor, flavor_size);
}

static PGRAPHVkSpirvCacheLoadResult load_rejected(
    PGRAPHVkSpirvCache *cache, PGRAPHVkSpirvCacheLoadResult result)
{
    cache->stats.rejections++;
    return result;
}

PGRAPHVkSpirvCacheLoadResult pgraph_vk_spirv_cache_load(
    PGRAPHVkSpirvCache *cache, const uint8_t *data, size_t size)
{
    if (!cache || !cache->compiler_flavor || !data ||
        size < PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE ||
        size > PGRAPH_VK_SPIRV_CACHE_MAX_FILE_SIZE ||
        load_u32_le(data + HEADER_MAGIC) != SPIRV_CACHE_MAGIC ||
        load_u32_le(data + HEADER_ABI) != PGRAPH_VK_SPIRV_CACHE_ABI ||
        load_u32_le(data + HEADER_SIZE) !=
            PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE ||
        load_u32_le(data + HEADER_RECORD_COUNT) >
            PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS) {
        return cache ? load_rejected(
                           cache, PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID) :
                       PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID;
    }

    uint32_t record_count = load_u32_le(data + HEADER_RECORD_COUNT);
    uint32_t flavor_size = load_u32_le(data + HEADER_FLAVOR_SIZE);
    uint64_t source_bytes = load_u64_le(data + HEADER_SOURCE_BYTES);
    uint64_t spirv_bytes = load_u64_le(data + HEADER_SPIRV_BYTES);
    if (!flavor_size || flavor_size > PGRAPH_VK_SPIRV_CACHE_MAX_FLAVOR_SIZE ||
        source_bytes > PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES ||
        spirv_bytes > PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_BYTES ||
        PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE + flavor_size > size) {
        return load_rejected(cache, PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
    }

    const uint8_t *payload = data + PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE;
    size_t payload_size = size - PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE;
    if (load_u64_le(data + HEADER_PAYLOAD_HASH) !=
        cache_hash(payload, payload_size)) {
        return load_rejected(cache, PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
    }
    if (!policy_matches(cache, data, flavor_size)) {
        return load_rejected(cache,
                             PGRAPH_VK_SPIRV_CACHE_LOAD_INCOMPATIBLE);
    }

    PGRAPHVkSpirvCache temporary = { 0 };
    PGRAPHVkSpirvCachePolicy policy = cache->policy;
    if (!pgraph_vk_spirv_cache_init(&temporary, &policy)) {
        return load_rejected(cache, PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
    }

    size_t offset = PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE + flavor_size;
    bool valid = true;
    for (uint32_t i = 0; i < record_count && valid; i++) {
        if (offset > size || size - offset < SPIRV_CACHE_RECORD_SIZE) {
            valid = false;
            break;
        }
        const uint8_t *record = data + offset;
        uint32_t stage = load_u32_le(record);
        uint32_t record_source_size = load_u32_le(record + 4);
        uint32_t record_spirv_size = load_u32_le(record + 8);
        if (!record_source_size ||
            record_source_size > PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE ||
            record_spirv_size > PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_SIZE ||
            load_u32_le(record + 12) != 0) {
            valid = false;
            break;
        }
        offset += SPIRV_CACHE_RECORD_SIZE;
        size_t record_payload_size =
            (size_t)record_source_size + record_spirv_size;
        if (offset > size || record_payload_size > size - offset) {
            valid = false;
            break;
        }
        const uint8_t *source = data + offset;
        const uint8_t *spirv = source + record_source_size;
        if (load_u64_le(record + 16) !=
                cache_hash(source, record_source_size) ||
            load_u64_le(record + 24) !=
                cache_hash(spirv, record_spirv_size) ||
            !persistent_stage_valid(stage) ||
            !spirv_valid(&temporary.policy, spirv, record_spirv_size) ||
            temporary.stats.source_bytes + record_source_size >
                PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES ||
            temporary.stats.spirv_bytes + record_spirv_size >
                PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_BYTES ||
            find_entry(&temporary, stage, source, record_source_size, false) ||
            !append_entry(&temporary, stage, source, record_source_size, spirv,
                          record_spirv_size)) {
            valid = false;
            break;
        }
        offset += record_payload_size;
    }

    valid = valid && offset == size &&
            temporary.stats.records == record_count &&
            temporary.stats.source_bytes == source_bytes &&
            temporary.stats.spirv_bytes == spirv_bytes;
    if (!valid) {
        pgraph_vk_spirv_cache_destroy(&temporary);
        return load_rejected(cache, PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID);
    }

    free_entries(cache->entries, cache->stats.records);
    free(cache->index_buckets);
    cache->entries = temporary.entries;
    cache->capacity = temporary.capacity;
    cache->index_buckets = temporary.index_buckets;
    cache->index_bucket_count = temporary.index_bucket_count;
    cache->access_clock = temporary.access_clock;
    cache->stats.records = temporary.stats.records;
    cache->stats.source_bytes = temporary.stats.source_bytes;
    cache->stats.spirv_bytes = temporary.stats.spirv_bytes;
    cache->stats.loaded_bytes = size;
    temporary.entries = NULL;
    temporary.index_buckets = NULL;
    temporary.stats.records = 0;
    pgraph_vk_spirv_cache_destroy(&temporary);
    return PGRAPH_VK_SPIRV_CACHE_LOAD_OK;
}

PGRAPHVkSpirvCacheLookupResult pgraph_vk_spirv_cache_lookup(
    PGRAPHVkSpirvCache *cache, uint32_t stage, const void *source,
    size_t source_size, const uint8_t **spirv, size_t *spirv_size)
{
    PGRAPHVkSpirvCacheEntry *entry =
        cache ? find_entry(cache, stage, source, source_size, true) : NULL;
    if (!entry) {
        if (cache) {
            cache->stats.misses++;
        }
        return PGRAPH_VK_SPIRV_CACHE_MISS;
    }
    cache->stats.hits++;
    if (spirv) {
        *spirv = entry->spirv;
    }
    if (spirv_size) {
        *spirv_size = entry->spirv_size;
    }
    return PGRAPH_VK_SPIRV_CACHE_HIT;
}

PGRAPHVkSpirvCacheAdoptResult pgraph_vk_spirv_cache_adopt_hit(
    PGRAPHVkSpirvCache *cache, uint32_t stage, const void *source,
    size_t source_size, PGRAPHVkSpirvCacheAdoptFunc adopt, void *opaque)
{
    const uint8_t *spirv = NULL;
    size_t spirv_size = 0;

    if (!adopt || pgraph_vk_spirv_cache_lookup(
                      cache, stage, source, source_size, &spirv,
                      &spirv_size) != PGRAPH_VK_SPIRV_CACHE_HIT) {
        return PGRAPH_VK_SPIRV_CACHE_NOT_FOUND;
    }
    PGRAPHVkSpirvCacheArtifactResult result =
        adopt(opaque, spirv, spirv_size);
    if (result == PGRAPH_VK_SPIRV_CACHE_ARTIFACT_ACCEPTED) {
        return PGRAPH_VK_SPIRV_CACHE_ADOPTED;
    }
    if (result == PGRAPH_VK_SPIRV_CACHE_ARTIFACT_DEFERRED) {
        return PGRAPH_VK_SPIRV_CACHE_DEFERRED;
    }
    pgraph_vk_spirv_cache_reject_hit(cache, stage, source, source_size);
    return PGRAPH_VK_SPIRV_CACHE_REJECTED;
}

bool pgraph_vk_spirv_cache_add(PGRAPHVkSpirvCache *cache, uint32_t stage,
                               const void *source, size_t source_size,
                               const void *spirv, size_t spirv_size)
{
    if (!cache || !cache->compiler_flavor || !cache->index_buckets ||
        !cache->index_bucket_count || !persistent_stage_valid(stage) ||
        !source_valid(source, source_size) ||
        !spirv_valid(&cache->policy, spirv, spirv_size)) {
        if (cache) {
            cache->stats.rejections++;
        }
        return false;
    }
    if (find_entry(cache, stage, source, source_size, true)) {
        return true;
    }
    while (cache->stats.records >= PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS ||
           cache->stats.source_bytes + source_size >
               PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES ||
           cache->stats.spirv_bytes + spirv_size >
               PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_BYTES) {
        PGRAPHVkSpirvCacheEntry *entries = cache->entries;
        size_t victim = 0;
        for (size_t i = 1; i < cache->stats.records; i++) {
            if (entries[i].last_used < entries[victim].last_used) {
                victim = i;
            }
        }
        cache->stats.source_bytes -= entries[victim].source_size;
        cache->stats.spirv_bytes -= entries[victim].spirv_size;
        free(entries[victim].source);
        free(entries[victim].spirv);
        if (victim + 1 < cache->stats.records) {
            memmove(&entries[victim], &entries[victim + 1],
                    (cache->stats.records - victim - 1) * sizeof(*entries));
        }
        cache->stats.records--;
        cache->dirty = true;
        rebuild_index(cache);
    }
    if (!append_entry(cache, stage, source, source_size, spirv, spirv_size)) {
        cache->stats.rejections++;
        return false;
    }
    cache->stats.queued_bytes += source_size + spirv_size;
    cache->dirty = true;
    return true;
}

bool pgraph_vk_spirv_cache_reject_hit(PGRAPHVkSpirvCache *cache,
                                      uint32_t stage, const void *source,
                                      size_t source_size)
{
    if (!cache) {
        return false;
    }
    PGRAPHVkSpirvCacheEntry *entry =
        find_entry(cache, stage, source, source_size, false);
    if (!entry) {
        return false;
    }
    PGRAPHVkSpirvCacheEntry *entries = cache->entries;
    size_t index = (size_t)(entry - entries);
    cache->stats.source_bytes -= entry->source_size;
    cache->stats.spirv_bytes -= entry->spirv_size;
    free(entry->source);
    free(entry->spirv);
    if (index + 1 < cache->stats.records) {
        memmove(entry, entry + 1,
                (cache->stats.records - index - 1) * sizeof(*entry));
    }
    cache->stats.records--;
    cache->stats.rejections++;
    cache->stats.fallbacks++;
    cache->dirty = true;
    rebuild_index(cache);
    return true;
}

static int compare_entry_recency(const void *left, const void *right)
{
    const PGRAPHVkSpirvCacheEntry *const *a = left;
    const PGRAPHVkSpirvCacheEntry *const *b = right;

    if ((*a)->last_used < (*b)->last_used) {
        return -1;
    }
    if ((*a)->last_used > (*b)->last_used) {
        return 1;
    }
    if ((*a)->stage < (*b)->stage) {
        return -1;
    }
    if ((*a)->stage > (*b)->stage) {
        return 1;
    }
    if ((*a)->source_hash < (*b)->source_hash) {
        return -1;
    }
    if ((*a)->source_hash > (*b)->source_hash) {
        return 1;
    }
    if ((*a)->source_size < (*b)->source_size) {
        return -1;
    }
    if ((*a)->source_size > (*b)->source_size) {
        return 1;
    }
    return memcmp((*a)->source, (*b)->source, (*a)->source_size);
}

bool pgraph_vk_spirv_cache_serialize(const PGRAPHVkSpirvCache *cache,
                                     PGRAPHVkSpirvCacheBlob *blob)
{
    if (!cache || !cache->compiler_flavor || !blob ||
        cache->stats.records > PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS) {
        return false;
    }
    size_t size = PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE +
                  cache->policy.compiler_flavor_size +
                  cache->stats.records * SPIRV_CACHE_RECORD_SIZE +
                  cache->stats.source_bytes + cache->stats.spirv_bytes;
    if (size > PGRAPH_VK_SPIRV_CACHE_MAX_FILE_SIZE) {
        return false;
    }
    uint8_t *data = calloc(1, size);
    if (!data) {
        return false;
    }
    const PGRAPHVkSpirvCacheEntry **ordered = NULL;
    if (cache->stats.records) {
        ordered = malloc(cache->stats.records * sizeof(*ordered));
        if (!ordered) {
            free(data);
            return false;
        }
        const PGRAPHVkSpirvCacheEntry *entries = cache->entries;
        for (size_t i = 0; i < cache->stats.records; i++) {
            ordered[i] = &entries[i];
        }
        qsort(ordered, cache->stats.records, sizeof(*ordered),
              compare_entry_recency);
    }

    store_u32_le(data + HEADER_MAGIC, SPIRV_CACHE_MAGIC);
    store_u32_le(data + HEADER_ABI, PGRAPH_VK_SPIRV_CACHE_ABI);
    store_u32_le(data + HEADER_SIZE, PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE);
    store_u32_le(data + HEADER_RECORD_COUNT, (uint32_t)cache->stats.records);
    store_u32_le(data + HEADER_COMPILER_MAJOR, cache->policy.compiler_major);
    store_u32_le(data + HEADER_COMPILER_MINOR, cache->policy.compiler_minor);
    store_u32_le(data + HEADER_COMPILER_PATCH, cache->policy.compiler_patch);
    store_u32_le(data + HEADER_CLIENT_TARGET, cache->policy.client_target);
    store_u32_le(data + HEADER_SPIRV_TARGET, cache->policy.spirv_target);
    store_u32_le(data + HEADER_COMPILER_FLAGS, cache->policy.compiler_flags);
    store_u32_le(data + HEADER_GENERATOR_ABI, cache->policy.generator_abi);
    store_u32_le(data + HEADER_FLAVOR_SIZE,
                 (uint32_t)cache->policy.compiler_flavor_size);
    store_u64_le(data + HEADER_SOURCE_BYTES, cache->stats.source_bytes);
    store_u64_le(data + HEADER_SPIRV_BYTES, cache->stats.spirv_bytes);

    size_t offset = PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE;
    memcpy(data + offset, cache->policy.compiler_flavor,
           cache->policy.compiler_flavor_size);
    offset += cache->policy.compiler_flavor_size;
    for (size_t i = 0; i < cache->stats.records; i++) {
        const PGRAPHVkSpirvCacheEntry *entry = ordered[i];
        uint8_t *record = data + offset;
        store_u32_le(record, entry->stage);
        store_u32_le(record + 4, (uint32_t)entry->source_size);
        store_u32_le(record + 8, (uint32_t)entry->spirv_size);
        store_u64_le(record + 16, entry->source_hash);
        store_u64_le(record + 24,
                     cache_hash(entry->spirv, entry->spirv_size));
        offset += SPIRV_CACHE_RECORD_SIZE;
        memcpy(data + offset, entry->source, entry->source_size);
        offset += entry->source_size;
        memcpy(data + offset, entry->spirv, entry->spirv_size);
        offset += entry->spirv_size;
    }
    store_u64_le(data + HEADER_PAYLOAD_HASH,
                 cache_hash(data + PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE,
                            size - PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE));
    blob->data = data;
    blob->size = size;
    free(ordered);
    return true;
}

bool pgraph_vk_spirv_cache_publish(PGRAPHVkSpirvCache *cache,
                                   const char *temporary,
                                   const char *published,
                                   const PGRAPHVkSpirvCacheFileOps *ops,
                                   void *opaque)
{
    if (!cache || !temporary || !published || !ops || !ops->write ||
        !ops->replace || !ops->remove) {
        return false;
    }
    if (!cache->dirty) {
        return true;
    }
    PGRAPHVkSpirvCacheBlob blob = { 0 };
    if (!pgraph_vk_spirv_cache_serialize(cache, &blob)) {
        return false;
    }
    bool written = ops->write(opaque, temporary, blob.data, blob.size);
    bool replaced = written && ops->replace(opaque, temporary, published);
    if (!replaced) {
        ops->remove(opaque, temporary);
    } else {
        cache->dirty = false;
    }
    free(blob.data);
    return replaced;
}

bool pgraph_vk_spirv_cache_is_dirty(const PGRAPHVkSpirvCache *cache)
{
    return cache && cache->dirty;
}

bool pgraph_vk_spirv_cache_is_active(const PGRAPHVkSpirvCache *cache,
                                     bool session_eligible,
                                     bool setting_enabled)
{
    return session_eligible && setting_enabled && cache &&
           cache->compiler_flavor;
}

bool pgraph_vk_spirv_stage_matches(uint32_t expected_stage,
                                   uint32_t reflected_stage)
{
    switch (expected_stage) {
    case 1U:  /* VK_SHADER_STAGE_VERTEX_BIT */
    case 8U:  /* VK_SHADER_STAGE_GEOMETRY_BIT */
    case 16U: /* VK_SHADER_STAGE_FRAGMENT_BIT */
    case 32U: /* VK_SHADER_STAGE_COMPUTE_BIT */
        return reflected_stage == expected_stage;
    default:
        return false;
    }
}

bool pgraph_vk_spirv_layout_claim_block(bool *seen, uint32_t block_size,
                                        uint32_t member_count)
{
    if (!seen || *seen ||
        block_size > PGRAPH_VK_SPIRV_LAYOUT_MAX_BLOCK_SIZE ||
        member_count > PGRAPH_VK_SPIRV_LAYOUT_MAX_MEMBERS) {
        return false;
    }
    *seen = true;
    return true;
}

bool pgraph_vk_spirv_layout_validate_member(
    uint32_t block_size, const PGRAPHVkSpirvLayoutMember *member,
    PGRAPHVkSpirvLayoutDimensions *dimensions)
{
    if (!member || !dimensions ||
        block_size > PGRAPH_VK_SPIRV_LAYOUT_MAX_BLOCK_SIZE ||
        !member->size || member->array_dims_count > 1 ||
        member->vector_components > 4 || member->matrix_columns > 4 ||
        member->matrix_rows > 4 || member->scalar_width != 32 ||
        (!!member->matrix_columns != !!member->matrix_rows) ||
        (member->matrix_columns &&
         member->matrix_rows != member->vector_components) ||
        member->offset > block_size ||
        member->size > block_size - member->offset) {
        return false;
    }

    uint64_t array_elements = 1;
    if (member->array_dims_count) {
        if (!member->array_elements ||
            member->array_elements > PGRAPH_VK_SPIRV_LAYOUT_MAX_ELEMENTS) {
            return false;
        }
        array_elements = member->array_elements;
    }
    uint64_t matrix_columns =
        member->matrix_columns ? member->matrix_columns : 1;
    uint64_t element_count = array_elements * matrix_columns;
    if (!element_count || element_count > PGRAPH_VK_SPIRV_LAYOUT_MAX_ELEMENTS) {
        return false;
    }

    uint64_t stride = member->array_stride > member->matrix_stride ?
                          member->array_stride : member->matrix_stride;
    if (member->matrix_columns && member->array_stride) {
        if (member->array_stride % member->matrix_columns) {
            return false;
        }
        stride = member->array_stride / member->matrix_columns;
    }
    uint64_t vector_components =
        member->vector_components ? member->vector_components : 1;
    uint64_t element_size = vector_components * sizeof(uint32_t);
    if (element_count > 1 && (!stride || stride < element_size)) {
        return false;
    }
    uint64_t last_end = member->offset + element_size;
    if (element_count > 1) {
        last_end += (element_count - 1) * stride;
    }
    uint64_t member_end = (uint64_t)member->offset + member->size;
    if (last_end > member_end || last_end > block_size) {
        return false;
    }

    *dimensions = (PGRAPHVkSpirvLayoutDimensions) {
        .vector_components = (size_t)vector_components,
        .element_count = (size_t)element_count,
        .stride = (size_t)stride,
    };
    return true;
}

void pgraph_vk_spirv_cache_note_rejection(PGRAPHVkSpirvCache *cache)
{
    if (cache) {
        cache->stats.rejections++;
    }
}

const PGRAPHVkSpirvCacheStats *pgraph_vk_spirv_cache_stats(
    const PGRAPHVkSpirvCache *cache)
{
    return cache ? &cache->stats : NULL;
}

/*
 * Geforce NV2A PGRAPH Vulkan SPIR-V prewarm cache
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_SPIRV_PREWARM_H
#define HW_XBOX_NV2A_PGRAPH_VK_SPIRV_PREWARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGRAPH_VK_SPIRV_CACHE_ABI              1U
#define PGRAPH_VK_SPIRV_CACHE_HEADER_SIZE       72U
#define PGRAPH_VK_SPIRV_CACHE_MAX_RECORDS       4096U
#define PGRAPH_VK_SPIRV_CACHE_INDEX_BUCKETS      8192U
#define PGRAPH_VK_SPIRV_CACHE_MAX_FLAVOR_SIZE   256U
#define PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_SIZE   (1024U * 1024U)
#define PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_SIZE    (1024U * 1024U)
#define PGRAPH_VK_SPIRV_CACHE_MAX_ID_BOUND      \
    (PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_SIZE / sizeof(uint32_t))
#define PGRAPH_VK_SPIRV_CACHE_MAX_SOURCE_BYTES  (16U * 1024U * 1024U)
#define PGRAPH_VK_SPIRV_CACHE_MAX_SPIRV_BYTES   (32U * 1024U * 1024U)
#define PGRAPH_VK_SPIRV_CACHE_MAX_FILE_SIZE      (64U * 1024U * 1024U)

#define PGRAPH_VK_SPIRV_LAYOUT_MAX_BLOCK_SIZE    (1024U * 1024U)
#define PGRAPH_VK_SPIRV_LAYOUT_MAX_MEMBERS       4096U
#define PGRAPH_VK_SPIRV_LAYOUT_MAX_ELEMENTS      65536U
#define PGRAPH_VK_SPIRV_LAYOUT_MAX_NAME_SIZE     4096U
#define PGRAPH_VK_SPIRV_LAYOUT_MAX_DESCRIPTOR_SETS 32U
#define PGRAPH_VK_SPIRV_LAYOUT_MAX_BINDINGS      4096U

typedef struct PGRAPHVkSpirvCachePolicy {
    uint32_t generator_abi;
    uint32_t compiler_major;
    uint32_t compiler_minor;
    uint32_t compiler_patch;
    uint32_t client_target;
    uint32_t spirv_target;
    uint32_t compiler_flags;
    const void *compiler_flavor;
    size_t compiler_flavor_size;
} PGRAPHVkSpirvCachePolicy;

typedef struct PGRAPHVkSpirvCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t rejections;
    uint64_t fallbacks;
    uint64_t loaded_bytes;
    uint64_t queued_bytes;
    size_t records;
    size_t source_bytes;
    size_t spirv_bytes;
} PGRAPHVkSpirvCacheStats;

typedef struct PGRAPHVkSpirvCache {
    void *entries;
    size_t capacity;
    void *index_buckets;
    size_t index_bucket_count;
    uint64_t access_clock;
    PGRAPHVkSpirvCachePolicy policy;
    void *compiler_flavor;
    PGRAPHVkSpirvCacheStats stats;
    bool dirty;
} PGRAPHVkSpirvCache;

typedef struct PGRAPHVkSpirvCacheBlob {
    uint8_t *data;
    size_t size;
} PGRAPHVkSpirvCacheBlob;

typedef enum PGRAPHVkSpirvCacheLoadResult {
    PGRAPH_VK_SPIRV_CACHE_LOAD_OK,
    PGRAPH_VK_SPIRV_CACHE_LOAD_INCOMPATIBLE,
    PGRAPH_VK_SPIRV_CACHE_LOAD_INVALID,
} PGRAPHVkSpirvCacheLoadResult;

typedef enum PGRAPHVkSpirvCacheLookupResult {
    PGRAPH_VK_SPIRV_CACHE_HIT,
    PGRAPH_VK_SPIRV_CACHE_MISS,
} PGRAPHVkSpirvCacheLookupResult;

typedef struct PGRAPHVkSpirvCacheFileOps {
    bool (*write)(void *opaque, const char *path, const uint8_t *data,
                  size_t size);
    bool (*replace)(void *opaque, const char *temporary,
                    const char *published);
    void (*remove)(void *opaque, const char *path);
} PGRAPHVkSpirvCacheFileOps;

typedef struct PGRAPHVkSpirvLayoutMember {
    uint32_t offset;
    uint32_t size;
    uint32_t array_dims_count;
    uint32_t array_elements;
    uint32_t array_stride;
    uint32_t matrix_columns;
    uint32_t matrix_rows;
    uint32_t matrix_stride;
    uint32_t vector_components;
    uint32_t scalar_width;
} PGRAPHVkSpirvLayoutMember;

typedef struct PGRAPHVkSpirvLayoutDimensions {
    size_t vector_components;
    size_t element_count;
    size_t stride;
} PGRAPHVkSpirvLayoutDimensions;

typedef enum PGRAPHVkSpirvCacheArtifactResult {
    PGRAPH_VK_SPIRV_CACHE_ARTIFACT_ACCEPTED,
    PGRAPH_VK_SPIRV_CACHE_ARTIFACT_DEFERRED,
    PGRAPH_VK_SPIRV_CACHE_ARTIFACT_REJECTED,
} PGRAPHVkSpirvCacheArtifactResult;

typedef PGRAPHVkSpirvCacheArtifactResult (*PGRAPHVkSpirvCacheAdoptFunc)(
    void *opaque, const uint8_t *spirv, size_t spirv_size);

typedef enum PGRAPHVkSpirvCacheAdoptResult {
    PGRAPH_VK_SPIRV_CACHE_NOT_FOUND,
    PGRAPH_VK_SPIRV_CACHE_ADOPTED,
    PGRAPH_VK_SPIRV_CACHE_DEFERRED,
    PGRAPH_VK_SPIRV_CACHE_REJECTED,
} PGRAPHVkSpirvCacheAdoptResult;

static inline bool pgraph_vk_spirv_cache_adoption_needs_compile(
    PGRAPHVkSpirvCacheAdoptResult result)
{
    return result == PGRAPH_VK_SPIRV_CACHE_NOT_FOUND ||
           result == PGRAPH_VK_SPIRV_CACHE_REJECTED;
}

bool pgraph_vk_spirv_cache_init(PGRAPHVkSpirvCache *cache,
                                const PGRAPHVkSpirvCachePolicy *policy);
void pgraph_vk_spirv_cache_destroy(PGRAPHVkSpirvCache *cache);
PGRAPHVkSpirvCacheLoadResult pgraph_vk_spirv_cache_load(
    PGRAPHVkSpirvCache *cache, const uint8_t *data, size_t size);
PGRAPHVkSpirvCacheLookupResult pgraph_vk_spirv_cache_lookup(
    PGRAPHVkSpirvCache *cache, uint32_t stage, const void *source,
    size_t source_size, const uint8_t **spirv, size_t *spirv_size);
PGRAPHVkSpirvCacheAdoptResult pgraph_vk_spirv_cache_adopt_hit(
    PGRAPHVkSpirvCache *cache, uint32_t stage, const void *source,
    size_t source_size, PGRAPHVkSpirvCacheAdoptFunc adopt, void *opaque);
bool pgraph_vk_spirv_cache_add(PGRAPHVkSpirvCache *cache, uint32_t stage,
                               const void *source, size_t source_size,
                               const void *spirv, size_t spirv_size);
bool pgraph_vk_spirv_cache_reject_hit(PGRAPHVkSpirvCache *cache,
                                      uint32_t stage, const void *source,
                                      size_t source_size);
bool pgraph_vk_spirv_cache_serialize(const PGRAPHVkSpirvCache *cache,
                                     PGRAPHVkSpirvCacheBlob *blob);
bool pgraph_vk_spirv_cache_publish(PGRAPHVkSpirvCache *cache,
                                   const char *temporary,
                                   const char *published,
                                   const PGRAPHVkSpirvCacheFileOps *ops,
                                   void *opaque);
bool pgraph_vk_spirv_cache_is_dirty(const PGRAPHVkSpirvCache *cache);
bool pgraph_vk_spirv_cache_is_active(const PGRAPHVkSpirvCache *cache,
                                     bool session_eligible,
                                     bool setting_enabled);
bool pgraph_vk_spirv_stage_matches(uint32_t expected_stage,
                                   uint32_t reflected_stage);
bool pgraph_vk_spirv_layout_claim_block(bool *seen, uint32_t block_size,
                                        uint32_t member_count);
bool pgraph_vk_spirv_layout_validate_member(
    uint32_t block_size, const PGRAPHVkSpirvLayoutMember *member,
    PGRAPHVkSpirvLayoutDimensions *dimensions);
void pgraph_vk_spirv_cache_note_rejection(PGRAPHVkSpirvCache *cache);
const PGRAPHVkSpirvCacheStats *pgraph_vk_spirv_cache_stats(
    const PGRAPHVkSpirvCache *cache);

#endif

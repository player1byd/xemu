/*
 * Geforce NV2A PGRAPH Vulkan hybrid family history
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_HYBRID_FAMILY_HISTORY_H
#define HW_XBOX_NV2A_PGRAPH_VK_HYBRID_FAMILY_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGRAPH_VK_FAMILY_HISTORY_ABI 1U
#define PGRAPH_VK_FAMILY_HISTORY_MAX_RECORDS 1024U
#define PGRAPH_VK_FAMILY_HISTORY_MAX_PAYLOAD (64U * 1024U)
#define PGRAPH_VK_FAMILY_HISTORY_MAX_FILE_SIZE (8U * 1024U * 1024U)

typedef struct PGRAPHVkFamilyHistoryRecord {
    uint8_t *payload;
    size_t payload_size;
    uint64_t uses;
    uint64_t cold_misses;
    uint64_t synchronous_create_us;
    uint64_t last_used;
    bool attempted;
    /* Session-only prewarm scheduling; never serialized. */
    bool prewarm_considered;
    uint8_t prewarm_defer_count;
    uint64_t prewarm_retry_after_service;
} PGRAPHVkFamilyHistoryRecord;

typedef struct PGRAPHVkFamilyHistory {
    PGRAPHVkFamilyHistoryRecord *records;
    size_t count;
    size_t capacity;
    uint64_t access_clock;
    bool dirty;
} PGRAPHVkFamilyHistory;

typedef struct PGRAPHVkFamilyHistoryBlob {
    uint8_t *data;
    size_t size;
} PGRAPHVkFamilyHistoryBlob;

typedef enum PGRAPHVkFamilyHistoryLoadResult {
    PGRAPH_VK_FAMILY_HISTORY_LOAD_OK,
    PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID,
} PGRAPHVkFamilyHistoryLoadResult;

typedef struct PGRAPHVkFamilyHistoryFileOps {
    bool (*write)(void *opaque, const char *path, const uint8_t *data,
                  size_t size);
    bool (*replace)(void *opaque, const char *temporary,
                    const char *published);
    void (*remove)(void *opaque, const char *path);
} PGRAPHVkFamilyHistoryFileOps;

bool pgraph_vk_family_history_init(PGRAPHVkFamilyHistory *history,
                                   size_t capacity);
bool pgraph_vk_family_history_should_load(bool persistent_cache_eligible,
                                          bool hybrid_enabled);
void pgraph_vk_family_history_destroy(PGRAPHVkFamilyHistory *history);
bool pgraph_vk_family_history_note(PGRAPHVkFamilyHistory *history,
                                   const void *payload, size_t payload_size);
bool pgraph_vk_family_history_note_cold_miss(
    PGRAPHVkFamilyHistory *history, const void *payload,
    size_t payload_size, uint64_t synchronous_create_us);
const PGRAPHVkFamilyHistoryRecord *pgraph_vk_family_history_find(
    const PGRAPHVkFamilyHistory *history, const void *payload,
    size_t payload_size);
const PGRAPHVkFamilyHistoryRecord *pgraph_vk_family_history_next_unattempted(
    const PGRAPHVkFamilyHistory *history);
const PGRAPHVkFamilyHistoryRecord *pgraph_vk_family_history_next_eligible(
    const PGRAPHVkFamilyHistory *history, uint64_t service_id,
    bool allow_new);
bool pgraph_vk_family_history_mark_considered(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record);
bool pgraph_vk_family_history_defer(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record, uint64_t service_id);
void pgraph_vk_family_history_mark_attempted(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record);
void pgraph_vk_family_history_reset_attempts(PGRAPHVkFamilyHistory *history);
bool pgraph_vk_family_history_serialize(
    const PGRAPHVkFamilyHistory *history,
    PGRAPHVkFamilyHistoryBlob *blob);
PGRAPHVkFamilyHistoryLoadResult pgraph_vk_family_history_load(
    PGRAPHVkFamilyHistory *history, const uint8_t *data, size_t size);
void pgraph_vk_family_history_blob_destroy(PGRAPHVkFamilyHistoryBlob *blob);
bool pgraph_vk_family_history_publish(
    PGRAPHVkFamilyHistory *history, const char *temporary,
    const char *published, const PGRAPHVkFamilyHistoryFileOps *ops,
    void *opaque);

#endif

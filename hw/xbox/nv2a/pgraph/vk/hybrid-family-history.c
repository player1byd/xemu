/*
 * Geforce NV2A PGRAPH Vulkan hybrid family history
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/hybrid-family-history.h"

#include <stdlib.h>
#include <string.h>

#define FAMILY_HISTORY_MAGIC 0x48465358U /* XSFH */
#define FAMILY_HISTORY_HEADER_SIZE 32U
#define FAMILY_HISTORY_RECORD_SIZE 40U

enum {
    HEADER_MAGIC = 0,
    HEADER_ABI = 4,
    HEADER_SIZE = 8,
    HEADER_RECORD_COUNT = 12,
    HEADER_ACCESS_CLOCK = 16,
    HEADER_PAYLOAD_HASH = 24,
};

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
    dst[0] = value;
    dst[1] = value >> 8;
    dst[2] = value >> 16;
    dst[3] = value >> 24;
}

static void store_u64_le(uint8_t *dst, uint64_t value)
{
    store_u32_le(dst, value);
    store_u32_le(dst + 4, value >> 32);
}

static uint64_t history_hash(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t next_access(PGRAPHVkFamilyHistory *history)
{
    if (history->access_clock == UINT64_MAX) {
        for (size_t i = 0; i < history->count; i++) {
            history->records[i].last_used = i + 1;
        }
        history->access_clock = history->count;
    }
    return ++history->access_clock;
}

static bool payload_valid(const void *payload, size_t payload_size)
{
    return payload && payload_size &&
           payload_size <= PGRAPH_VK_FAMILY_HISTORY_MAX_PAYLOAD;
}

bool pgraph_vk_family_history_should_load(bool persistent_cache_eligible,
                                          bool hybrid_enabled)
{
    return persistent_cache_eligible && hybrid_enabled;
}

bool pgraph_vk_family_history_init(PGRAPHVkFamilyHistory *history,
                                   size_t capacity)
{
    if (!history || !capacity ||
        capacity > PGRAPH_VK_FAMILY_HISTORY_MAX_RECORDS ||
        capacity > SIZE_MAX / sizeof(*history->records)) {
        return false;
    }

    PGRAPHVkFamilyHistoryRecord *records =
        calloc(capacity, sizeof(*records));
    if (!records) {
        return false;
    }
    *history = (PGRAPHVkFamilyHistory) {
        .records = records,
        .capacity = capacity,
    };
    return true;
}

void pgraph_vk_family_history_destroy(PGRAPHVkFamilyHistory *history)
{
    if (!history) {
        return;
    }
    for (size_t i = 0; i < history->count; i++) {
        free(history->records[i].payload);
    }
    free(history->records);
    *history = (PGRAPHVkFamilyHistory) { 0 };
}

static PGRAPHVkFamilyHistoryRecord *find_mutable(
    PGRAPHVkFamilyHistory *history, const void *payload, size_t payload_size)
{
    if (!history || !payload_valid(payload, payload_size)) {
        return NULL;
    }
    for (size_t i = 0; i < history->count; i++) {
        PGRAPHVkFamilyHistoryRecord *record = &history->records[i];
        if (record->payload_size == payload_size &&
            !memcmp(record->payload, payload, payload_size)) {
            return record;
        }
    }
    return NULL;
}

const PGRAPHVkFamilyHistoryRecord *pgraph_vk_family_history_find(
    const PGRAPHVkFamilyHistory *history, const void *payload,
    size_t payload_size)
{
    return find_mutable((PGRAPHVkFamilyHistory *)history, payload,
                        payload_size);
}

static size_t eviction_index(const PGRAPHVkFamilyHistory *history)
{
    size_t selected = 0;
    for (size_t i = 1; i < history->count; i++) {
        const PGRAPHVkFamilyHistoryRecord *candidate = &history->records[i];
        const PGRAPHVkFamilyHistoryRecord *current = &history->records[selected];
        if (candidate->cold_misses < current->cold_misses ||
            (candidate->cold_misses == current->cold_misses &&
             candidate->synchronous_create_us <
                 current->synchronous_create_us) ||
            (candidate->cold_misses == current->cold_misses &&
             candidate->synchronous_create_us ==
                 current->synchronous_create_us &&
             candidate->uses < current->uses) ||
            (candidate->cold_misses == current->cold_misses &&
             candidate->synchronous_create_us ==
                 current->synchronous_create_us &&
             candidate->uses == current->uses &&
             candidate->last_used < current->last_used)) {
            selected = i;
        }
    }
    return selected;
}

bool pgraph_vk_family_history_note(PGRAPHVkFamilyHistory *history,
                                   const void *payload, size_t payload_size)
{
    if (!history || !history->records ||
        !payload_valid(payload, payload_size)) {
        return false;
    }

    PGRAPHVkFamilyHistoryRecord *record =
        find_mutable(history, payload, payload_size);
    if (record) {
        if (record->uses != UINT64_MAX) {
            record->uses++;
        }
        record->last_used = next_access(history);
        history->dirty = true;
        return true;
    }

    uint8_t *copy = malloc(payload_size);
    if (!copy) {
        return false;
    }
    memcpy(copy, payload, payload_size);

    size_t index;
    if (history->count < history->capacity) {
        index = history->count++;
    } else {
        index = eviction_index(history);
        free(history->records[index].payload);
    }
    history->records[index] = (PGRAPHVkFamilyHistoryRecord) {
        .payload = copy,
        .payload_size = payload_size,
        .uses = 1,
        .last_used = next_access(history),
    };
    history->dirty = true;
    return true;
}

static uint64_t add_saturating(uint64_t value, uint64_t increment)
{
    return increment > UINT64_MAX - value ? UINT64_MAX : value + increment;
}

bool pgraph_vk_family_history_note_cold_miss(
    PGRAPHVkFamilyHistory *history, const void *payload,
    size_t payload_size, uint64_t synchronous_create_us)
{
    if (!pgraph_vk_family_history_note(history, payload, payload_size)) {
        return false;
    }
    PGRAPHVkFamilyHistoryRecord *record =
        find_mutable(history, payload, payload_size);
    record->cold_misses = add_saturating(record->cold_misses, 1);
    record->synchronous_create_us = add_saturating(
        record->synchronous_create_us, synchronous_create_us);
    return true;
}

const PGRAPHVkFamilyHistoryRecord *pgraph_vk_family_history_next_eligible(
    const PGRAPHVkFamilyHistory *history, uint64_t service_id,
    bool allow_new)
{
    if (!history) {
        return NULL;
    }
    const PGRAPHVkFamilyHistoryRecord *selected = NULL;
    for (size_t i = 0; i < history->count; i++) {
        const PGRAPHVkFamilyHistoryRecord *candidate = &history->records[i];
        if (candidate->attempted ||
            (!allow_new && !candidate->prewarm_considered) ||
            candidate->prewarm_retry_after_service > service_id) {
            continue;
        }
        if (!selected || candidate->cold_misses > selected->cold_misses ||
            (candidate->cold_misses == selected->cold_misses &&
             candidate->synchronous_create_us >
                 selected->synchronous_create_us) ||
            (candidate->cold_misses == selected->cold_misses &&
             candidate->synchronous_create_us ==
                 selected->synchronous_create_us &&
             candidate->uses > selected->uses) ||
            (candidate->cold_misses == selected->cold_misses &&
             candidate->synchronous_create_us ==
                 selected->synchronous_create_us &&
             candidate->uses == selected->uses &&
             candidate->last_used > selected->last_used) ||
            (candidate->cold_misses == selected->cold_misses &&
             candidate->synchronous_create_us ==
                 selected->synchronous_create_us &&
             candidate->uses == selected->uses &&
             candidate->last_used == selected->last_used &&
             (candidate->payload_size < selected->payload_size ||
              (candidate->payload_size == selected->payload_size &&
               memcmp(candidate->payload, selected->payload,
                      candidate->payload_size) < 0)))) {
            selected = candidate;
        }
    }
    return selected;
}

const PGRAPHVkFamilyHistoryRecord *pgraph_vk_family_history_next_unattempted(
    const PGRAPHVkFamilyHistory *history)
{
    return pgraph_vk_family_history_next_eligible(history, UINT64_MAX, true);
}

static PGRAPHVkFamilyHistoryRecord *mutable_record(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record)
{
    if (!history || !record || !history->records) {
        return NULL;
    }
    for (size_t i = 0; i < history->count; i++) {
        if (record == &history->records[i]) {
            return &history->records[i];
        }
    }
    return NULL;
}

bool pgraph_vk_family_history_mark_considered(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record)
{
    PGRAPHVkFamilyHistoryRecord *mutable = mutable_record(history, record);
    if (!mutable || mutable->prewarm_considered) {
        return false;
    }
    mutable->prewarm_considered = true;
    return true;
}

bool pgraph_vk_family_history_defer(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record, uint64_t service_id)
{
    PGRAPHVkFamilyHistoryRecord *mutable = mutable_record(history, record);
    if (!mutable || !mutable->prewarm_considered || mutable->attempted) {
        return false;
    }
    if (++mutable->prewarm_defer_count >= 3) {
        mutable->attempted = true;
        return false;
    }
    uint64_t delay = 8 * mutable->prewarm_defer_count;
    mutable->prewarm_retry_after_service =
        service_id > UINT64_MAX - delay ? UINT64_MAX : service_id + delay;
    return true;
}

void pgraph_vk_family_history_mark_attempted(
    PGRAPHVkFamilyHistory *history,
    const PGRAPHVkFamilyHistoryRecord *record)
{
    if (!history || !record || !history->records) {
        return;
    }
    for (size_t i = 0; i < history->count; i++) {
        if (record == &history->records[i]) {
            history->records[i].attempted = true;
            return;
        }
    }
}

void pgraph_vk_family_history_reset_attempts(PGRAPHVkFamilyHistory *history)
{
    if (!history) {
        return;
    }
    for (size_t i = 0; i < history->count; i++) {
        history->records[i].attempted = false;
        history->records[i].prewarm_considered = false;
        history->records[i].prewarm_defer_count = 0;
        history->records[i].prewarm_retry_after_service = 0;
    }
}

bool pgraph_vk_family_history_serialize(
    const PGRAPHVkFamilyHistory *history,
    PGRAPHVkFamilyHistoryBlob *blob)
{
    if (!history || !blob || history->count > UINT32_MAX) {
        return false;
    }
    size_t size = FAMILY_HISTORY_HEADER_SIZE;
    for (size_t i = 0; i < history->count; i++) {
        size_t payload_size = history->records[i].payload_size;
        if (!payload_valid(history->records[i].payload, payload_size) ||
            payload_size > SIZE_MAX - FAMILY_HISTORY_RECORD_SIZE ||
            size > SIZE_MAX - FAMILY_HISTORY_RECORD_SIZE - payload_size) {
            return false;
        }
        size += FAMILY_HISTORY_RECORD_SIZE + payload_size;
    }
    if (size > PGRAPH_VK_FAMILY_HISTORY_MAX_FILE_SIZE) {
        return false;
    }

    uint8_t *data = calloc(1, size);
    if (!data) {
        return false;
    }
    store_u32_le(data + HEADER_MAGIC, FAMILY_HISTORY_MAGIC);
    store_u32_le(data + HEADER_ABI, PGRAPH_VK_FAMILY_HISTORY_ABI);
    store_u32_le(data + HEADER_SIZE, FAMILY_HISTORY_HEADER_SIZE);
    store_u32_le(data + HEADER_RECORD_COUNT, history->count);
    store_u64_le(data + HEADER_ACCESS_CLOCK, history->access_clock);

    size_t offset = FAMILY_HISTORY_HEADER_SIZE;
    for (size_t i = 0; i < history->count; i++) {
        const PGRAPHVkFamilyHistoryRecord *record = &history->records[i];
        store_u32_le(data + offset, record->payload_size);
        store_u32_le(data + offset + 4, 0);
        store_u64_le(data + offset + 8, record->uses);
        store_u64_le(data + offset + 16, record->cold_misses);
        store_u64_le(data + offset + 24, record->synchronous_create_us);
        store_u64_le(data + offset + 32, record->last_used);
        offset += FAMILY_HISTORY_RECORD_SIZE;
        memcpy(data + offset, record->payload, record->payload_size);
        offset += record->payload_size;
    }
    store_u64_le(data + HEADER_PAYLOAD_HASH,
                 history_hash(data + FAMILY_HISTORY_HEADER_SIZE,
                              size - FAMILY_HISTORY_HEADER_SIZE));
    *blob = (PGRAPHVkFamilyHistoryBlob) {
        .data = data,
        .size = size,
    };
    return true;
}

PGRAPHVkFamilyHistoryLoadResult pgraph_vk_family_history_load(
    PGRAPHVkFamilyHistory *history, const uint8_t *data, size_t size)
{
    if (!history || !history->records || !data ||
        size < FAMILY_HISTORY_HEADER_SIZE ||
        size > PGRAPH_VK_FAMILY_HISTORY_MAX_FILE_SIZE ||
        load_u32_le(data + HEADER_MAGIC) != FAMILY_HISTORY_MAGIC ||
        load_u32_le(data + HEADER_ABI) != PGRAPH_VK_FAMILY_HISTORY_ABI ||
        load_u32_le(data + HEADER_SIZE) != FAMILY_HISTORY_HEADER_SIZE) {
        return PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID;
    }
    uint32_t count = load_u32_le(data + HEADER_RECORD_COUNT);
    if (count > history->capacity ||
        load_u64_le(data + HEADER_PAYLOAD_HASH) !=
            history_hash(data + FAMILY_HISTORY_HEADER_SIZE,
                         size - FAMILY_HISTORY_HEADER_SIZE)) {
        return PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID;
    }

    PGRAPHVkFamilyHistory replacement;
    if (!pgraph_vk_family_history_init(&replacement, history->capacity)) {
        return PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID;
    }
    replacement.access_clock = load_u64_le(data + HEADER_ACCESS_CLOCK);

    size_t offset = FAMILY_HISTORY_HEADER_SIZE;
    bool valid = true;
    for (uint32_t i = 0; i < count; i++) {
        if (size - offset < FAMILY_HISTORY_RECORD_SIZE) {
            valid = false;
            break;
        }
        uint32_t payload_size = load_u32_le(data + offset);
        uint32_t reserved = load_u32_le(data + offset + 4);
        uint64_t uses = load_u64_le(data + offset + 8);
        uint64_t cold_misses = load_u64_le(data + offset + 16);
        uint64_t synchronous_create_us = load_u64_le(data + offset + 24);
        uint64_t last_used = load_u64_le(data + offset + 32);
        offset += FAMILY_HISTORY_RECORD_SIZE;
        if (reserved || !payload_size ||
            payload_size > PGRAPH_VK_FAMILY_HISTORY_MAX_PAYLOAD ||
            payload_size > size - offset || !uses ||
            last_used > replacement.access_clock) {
            valid = false;
            break;
        }
        uint8_t *payload = malloc(payload_size);
        if (!payload) {
            valid = false;
            break;
        }
        memcpy(payload, data + offset, payload_size);
        offset += payload_size;
        replacement.records[replacement.count++] =
            (PGRAPHVkFamilyHistoryRecord) {
                .payload = payload,
                .payload_size = payload_size,
                .uses = uses,
                .cold_misses = cold_misses,
                .synchronous_create_us = synchronous_create_us,
                .last_used = last_used,
            };
    }
    if (offset != size) {
        valid = false;
    }
    if (!valid) {
        pgraph_vk_family_history_destroy(&replacement);
        return PGRAPH_VK_FAMILY_HISTORY_LOAD_INVALID;
    }

    pgraph_vk_family_history_destroy(history);
    *history = replacement;
    return PGRAPH_VK_FAMILY_HISTORY_LOAD_OK;
}

void pgraph_vk_family_history_blob_destroy(PGRAPHVkFamilyHistoryBlob *blob)
{
    if (!blob) {
        return;
    }
    free(blob->data);
    *blob = (PGRAPHVkFamilyHistoryBlob) { 0 };
}

bool pgraph_vk_family_history_publish(
    PGRAPHVkFamilyHistory *history, const char *temporary,
    const char *published, const PGRAPHVkFamilyHistoryFileOps *ops,
    void *opaque)
{
    if (!history || !temporary || !published || !ops || !ops->write ||
        !ops->replace || !ops->remove) {
        return false;
    }
    if (!history->dirty) {
        return true;
    }

    PGRAPHVkFamilyHistoryBlob blob = { 0 };
    if (!pgraph_vk_family_history_serialize(history, &blob)) {
        return false;
    }
    bool written = ops->write(opaque, temporary, blob.data, blob.size);
    bool replaced = written && ops->replace(opaque, temporary, published);
    if (!replaced) {
        ops->remove(opaque, temporary);
    } else {
        history->dirty = false;
    }
    pgraph_vk_family_history_blob_destroy(&blob);
    return replaced;
}

/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qemu/lru.h"

#define TEST_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct TestEntry {
    LruNode node;
    uint64_t key;
    unsigned int index;
} TestEntry;

typedef struct VisitResult {
    unsigned int count;
    uint32_t mask;
} VisitResult;

typedef struct CallbackCounts {
    unsigned int init;
    unsigned int pre_evict;
    unsigned int post_evict;
} CallbackCounts;

static CallbackCounts callback_counts;

static void entry_init(Lru *lru, LruNode *node, const void *key)
{
    TestEntry *entry = TEST_CONTAINER_OF(node, TestEntry, node);

    (void)lru;
    callback_counts.init++;
    entry->key = *(const uint64_t *)key;
}

static bool entry_compare(Lru *lru, LruNode *node, const void *key)
{
    TestEntry *entry = TEST_CONTAINER_OF(node, TestEntry, node);

    (void)lru;
    return entry->key != *(const uint64_t *)key;
}

static bool entry_pre_evict(Lru *lru, LruNode *node)
{
    (void)lru;
    (void)node;
    callback_counts.pre_evict++;
    return true;
}

static void entry_post_evict(Lru *lru, LruNode *node)
{
    (void)lru;
    (void)node;
    callback_counts.post_evict++;
}

static size_t snapshot_global_order(Lru *lru, LruNode **nodes,
                                    size_t capacity)
{
    LruNode *node;
    size_t count = 0;

    QTAILQ_FOREACH(node, &lru->global, next_global) {
        if (count < capacity) {
            nodes[count] = node;
        }
        count++;
    }

    return count;
}

static bool test_find_existing_is_exact(void)
{
    static Lru lru;
    TestEntry entries[3] = {
        { .index = 0 },
        { .index = 1 },
        { .index = 2 },
    };
    uint64_t key0 = 10;
    uint64_t key1 = 20;
    uint64_t missing_key = 30;
    const uint64_t colliding_hash = 0x1234;
    LruNode *node0;
    LruNode *node1;

    lru_init(&lru);
    lru.init_node = entry_init;
    lru.compare_nodes = entry_compare;
    for (size_t i = 0; i < 3; i++) {
        lru_add_free(&lru, &entries[i].node);
    }

    node0 = lru_lookup(&lru, colliding_hash, &key0);
    node1 = lru_lookup(&lru, colliding_hash, &key1);

    return lru_find_existing(&lru, colliding_hash, &key0) == node0 &&
           lru_find_existing(&lru, colliding_hash, &key1) == node1 &&
           lru_find_existing(&lru, colliding_hash, &missing_key) == NULL &&
           lru_find_existing(&lru, colliding_hash + 1, &key0) == NULL;
}

static bool test_find_existing_does_not_mutate(void)
{
    static Lru lru;
    TestEntry entries[3] = {
        { .index = 0 },
        { .index = 1 },
        { .index = 2 },
    };
    LruNode *before[3];
    LruNode *after[3];
    uint64_t key0 = 10;
    uint64_t key1 = 20;
    uint64_t missing_key = 30;
    const uint64_t colliding_hash = 0x1234;
    int num_used;
    int num_free;
    size_t before_count;
    size_t after_count;

    lru_init(&lru);
    lru.init_node = entry_init;
    lru.compare_nodes = entry_compare;
    lru.pre_node_evict = entry_pre_evict;
    lru.post_node_evict = entry_post_evict;
    for (size_t i = 0; i < 3; i++) {
        lru_add_free(&lru, &entries[i].node);
    }

    lru_lookup(&lru, colliding_hash, &key0);
    lru_lookup(&lru, colliding_hash, &key1);
    callback_counts = (CallbackCounts) { 0 };
    num_used = lru.num_used;
    num_free = lru.num_free;
    before_count = snapshot_global_order(&lru, before, 3);

    lru_find_existing(&lru, colliding_hash, &key0);
    lru_find_existing(&lru, colliding_hash, &missing_key);
    lru_find_existing(&lru, colliding_hash + 1, &key0);

    after_count = snapshot_global_order(&lru, after, 3);
    return before_count == 3 && after_count == before_count &&
           memcmp(before, after, sizeof(before)) == 0 &&
           lru.num_used == num_used && lru.num_free == num_free &&
           callback_counts.init == 0 &&
           callback_counts.pre_evict == 0 &&
           callback_counts.post_evict == 0;
}

static bool test_touch_existing_updates_recency_without_lookup(void)
{
    static Lru lru;
    TestEntry entries[3] = { 0 };
    uint64_t keys[3] = { 10, 20, 30 };
    const uint64_t hash = 0x1234;
    LruNode *nodes[3];
    LruNode *borrowed;

    lru_init(&lru);
    lru.init_node = entry_init;
    lru.compare_nodes = entry_compare;
    lru.pre_node_evict = entry_pre_evict;
    lru.post_node_evict = entry_post_evict;
    for (size_t i = 0; i < 3; i++) {
        lru_add_free(&lru, &entries[i].node);
        nodes[i] = lru_lookup(&lru, hash, &keys[i]);
    }

    borrowed = lru_find_existing(&lru, hash, &keys[0]);
    if (borrowed != nodes[0]) {
        return false;
    }
    callback_counts = (CallbackCounts) { 0 };
    lru_touch_existing(&lru, borrowed);
    lru_touch_existing(&lru, borrowed);

    if (QTAILQ_FIRST(&lru.global) != nodes[0] ||
        QTAILQ_NEXT(nodes[0], next_global) != nodes[2] ||
        QTAILQ_NEXT(nodes[2], next_global) != nodes[1] ||
        QTAILQ_FIRST(&lru.bins[lru_hash_to_bin(&lru, hash)]) != nodes[0] ||
        QTAILQ_NEXT(nodes[0], next_bin) != nodes[2] ||
        QTAILQ_NEXT(nodes[2], next_bin) != nodes[1] ||
        lru.num_used != 3 || lru.num_free != 0 ||
        callback_counts.init != 0 || callback_counts.pre_evict != 0 ||
        callback_counts.post_evict != 0) {
        return false;
    }

    /* The untouched oldest entry, not the borrowed hit, is evicted. */
    return lru_try_evict_one(&lru) == nodes[1] &&
           callback_counts.pre_evict == 1 &&
           callback_counts.post_evict == 1;
}

static void record_visit(Lru *lru, LruNode *node, void *opaque)
{
    TestEntry *entry = TEST_CONTAINER_OF(node, TestEntry, node);
    VisitResult *result = opaque;

    (void)lru;
    result->count++;
    result->mask |= 1U << entry->index;
}

int main(void)
{
    static Lru lru;
    TestEntry entries[3] = {
        { .index = 0 },
        { .index = 1 },
        { .index = 2 },
    };
    VisitResult active = { 0 };
    VisitResult flushed = { 0 };
    LruNode *node0;
    LruNode *node1;
    uint32_t expected_mask;
    uint64_t key0 = 1;
    uint64_t key1 = 2;

    lru_init(&lru);
    lru.init_node = entry_init;
    lru.compare_nodes = entry_compare;
    for (size_t i = 0; i < 3; i++) {
        lru_add_free(&lru, &entries[i].node);
    }

    node0 = lru_lookup(&lru, key0, &key0);
    node1 = lru_lookup(&lru, key1, &key1);
    expected_mask =
        1U << TEST_CONTAINER_OF(node0, TestEntry, node)->index |
        1U << TEST_CONTAINER_OF(node1, TestEntry, node)->index;
    lru_visit_active(&lru, record_visit, &active);
    lru_flush(&lru);
    lru_visit_active(&lru, record_visit, &flushed);

    puts("TAP version 13");
    puts("1..5");
    printf("%s 1 - visit includes every active node and no free node\n",
           active.count == 2 && active.mask == expected_mask ?
           "ok" : "not ok");
    printf("%s 2 - visit excludes nodes after flush\n",
           flushed.count == 0 && flushed.mask == 0 ? "ok" : "not ok");

    bool exact = test_find_existing_is_exact();
    bool immutable = test_find_existing_does_not_mutate();
    bool touched = test_touch_existing_updates_recency_without_lookup();
    printf("%s 3 - noncreating lookup resolves exact keys through collisions\n",
           exact ? "ok" : "not ok");
    printf("%s 4 - noncreating lookup preserves LRU state and callbacks\n",
           immutable ? "ok" : "not ok");
    printf("%s 5 - touching a borrowed hit updates recency without eviction\n",
           touched ? "ok" : "not ok");

    return active.count == 2 && active.mask == expected_mask &&
           flushed.count == 0 && flushed.mask == 0 && exact && immutable &&
           touched ?
           0 : 1;
}

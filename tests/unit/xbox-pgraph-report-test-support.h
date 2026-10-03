/*
 * Owned-memory support for NV2A report integration tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef XBOX_PGRAPH_REPORT_TEST_SUPPORT_H
#define XBOX_PGRAPH_REPORT_TEST_SUPPORT_H

#include "qemu/osdep.h"
#include "system/memory.h"

typedef struct ReportTestGuardedBuffer {
    uint8_t *mapping;
    uint8_t *guard;
    size_t page_size;
} ReportTestGuardedBuffer;

bool report_test_guarded_buffer_init(ReportTestGuardedBuffer *buffer);
void report_test_guarded_buffer_destroy(ReportTestGuardedBuffer *buffer);
void report_test_memory_region_set_size(MemoryRegion *region, uint64_t size);
unsigned int report_test_dma_load_count(void);
void report_test_reset_dma_load_count(void);

#endif

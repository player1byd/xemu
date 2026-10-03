/*
 * NV2A PGRAPH report wrapper/decoder integration tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "xbox-pgraph-report-test-support.h"

#define VRAM_SIZE 64
#define REPORT_SIZE 16
#define CANARY 0xa5

static bool buffer_is_value(const uint8_t *buffer, size_t size, uint8_t value)
{
    for (size_t i = 0; i < size; i++) {
        if (buffer[i] != value) {
            return false;
        }
    }
    return true;
}

static bool report_matches(const uint8_t *report, uint32_t result)
{
    static const uint8_t timestamp[] = {
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    uint8_t expected[REPORT_SIZE] = { 0 };

    memcpy(expected, timestamp, sizeof(timestamp));
    stl_le_p(expected + 8, result);
    return !memcmp(report, expected, sizeof(expected));
}

static void write_dma_descriptor(uint8_t *ramin, uint32_t base, uint32_t limit)
{
    uint32_t flags = 0;

    SET_MASK(flags, NV_DMA_ADJUST, base & 0xfff);
    stl_le_p(ramin, flags);
    stl_le_p(ramin + 4, limit);
    stl_le_p(ramin + 8, base & NV_DMA_ADDRESS);
}

static bool test_incomplete_descriptor_stops_before_guarded_read(void)
{
    ReportTestGuardedBuffer guarded = { 0 };
    MemoryRegion vram_region;
    g_autofree NV2AState *d = g_new0(NV2AState, 1);
    uint8_t vram[VRAM_SIZE];
    bool pass;

    if (!report_test_guarded_buffer_init(&guarded)) {
        return false;
    }
    d->ramin_ptr = guarded.guard - 8;
    memset(d->ramin_ptr, 0, 8);
    report_test_memory_region_set_size(&d->ramin, 8);
    report_test_memory_region_set_size(&vram_region, sizeof(vram));
    d->vram = &vram_region;
    d->vram_ptr = vram;
    memset(vram, CANARY, sizeof(vram));
    report_test_reset_dma_load_count();

    pgraph_write_zpass_pixel_cnt_report(d, 0, 0, 1);
    pass = report_test_dma_load_count() == 0 &&
           buffer_is_value(vram, sizeof(vram), CANARY);
    report_test_guarded_buffer_destroy(&guarded);
    return pass;
}

static bool test_exact_descriptor_decodes_and_serializes(void)
{
    ReportTestGuardedBuffer guarded = { 0 };
    MemoryRegion vram_region;
    g_autofree NV2AState *d = g_new0(NV2AState, 1);
    uint8_t vram[VRAM_SIZE];
    uint8_t *ramin;
    bool pass;

    if (!report_test_guarded_buffer_init(&guarded)) {
        return false;
    }
    ramin = guarded.guard - 12;
    write_dma_descriptor(ramin, VRAM_SIZE - REPORT_SIZE, REPORT_SIZE - 1);
    d->ramin_ptr = ramin;
    report_test_memory_region_set_size(&d->ramin, 12);
    report_test_memory_region_set_size(&vram_region, sizeof(vram));
    d->vram = &vram_region;
    d->vram_ptr = vram;
    memset(vram, CANARY, sizeof(vram));
    report_test_reset_dma_load_count();

    pgraph_write_zpass_pixel_cnt_report(d, 0, 0, UINT32_C(0x78563412));
    pass = report_test_dma_load_count() == 1 &&
           buffer_is_value(vram, VRAM_SIZE - REPORT_SIZE, CANARY) &&
           report_matches(vram + VRAM_SIZE - REPORT_SIZE, UINT32_C(0x78563412));
    report_test_guarded_buffer_destroy(&guarded);
    return pass;
}

int main(void)
{
    bool incomplete = test_incomplete_descriptor_stops_before_guarded_read();
    bool exact = test_exact_descriptor_decodes_and_serializes();

    puts("TAP version 13");
    puts("1..2");
    printf("%s 1 - incomplete descriptor stops before guarded read\n",
           incomplete ? "ok" : "not ok");
    printf("%s 2 - exact descriptor decodes and serializes\n",
           exact ? "ok" : "not ok");
    return incomplete && exact ? 0 : 1;
}

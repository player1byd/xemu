/*
 * NV2A PGRAPH report write tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hw/xbox/nv2a/pgraph/reports.h"

#define REPORT_SIZE 16
#define BUFFER_SIZE 64
#define CANARY 0xa5

static bool buffer_is_canary(const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (buffer[i] != CANARY) {
            return false;
        }
    }

    return true;
}

static bool buffer_is_canary_range(const uint8_t *buffer, size_t start,
                                   size_t end)
{
    return buffer_is_canary(buffer + start, end - start);
}

static bool report_matches(const uint8_t *report, uint32_t result)
{
    static const uint8_t timestamp[] = {
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    uint8_t expected[REPORT_SIZE] = { 0 };

    memcpy(expected, timestamp, sizeof(timestamp));
    expected[8] = result;
    expected[9] = result >> 8;
    expected[10] = result >> 16;
    expected[11] = result >> 24;
    return !memcmp(report, expected, sizeof(expected));
}

static bool test_exact_dma_fit_writes_complete_report(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    if (!pgraph_zpass_report_write(buffer, 4, 15, 0, sizeof(buffer),
                                   UINT32_C(0x78563412))) {
        return false;
    }

    return buffer_is_canary_range(buffer, 0, 4) &&
           report_matches(&buffer[4], UINT32_C(0x78563412)) &&
           buffer_is_canary_range(buffer, 20, sizeof(buffer));
}

static bool test_incomplete_dma_span_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 0, 14, 0, sizeof(buffer), 1) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_nonzero_offset_exact_fit_writes_complete_report(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    if (!pgraph_zpass_report_write(buffer, 3, 31, 16, sizeof(buffer), 2)) {
        return false;
    }

    return buffer_is_canary_range(buffer, 0, 19) &&
           report_matches(&buffer[19], 2) &&
           buffer_is_canary_range(buffer, 35, sizeof(buffer));
}

static bool test_one_byte_short_dma_span_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 0, 30, 16, sizeof(buffer), 3) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_original_nonzero_offset_case_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 0, 17, 16, sizeof(buffer), 4) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_offset_beyond_limit_rejects_without_underflow(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 0, 15, 16, sizeof(buffer), 5) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_exact_vram_fit_writes_complete_report(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    if (!pgraph_zpass_report_write(buffer, sizeof(buffer) - REPORT_SIZE,
                                   UINT64_MAX, 0, sizeof(buffer), 6)) {
        return false;
    }

    return buffer_is_canary_range(buffer, 0, BUFFER_SIZE - REPORT_SIZE) &&
           report_matches(&buffer[BUFFER_SIZE - REPORT_SIZE], 6);
}

static bool test_incomplete_vram_span_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, sizeof(buffer) - REPORT_SIZE + 1,
                                      UINT64_MAX, 0, sizeof(buffer), 7) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_nonzero_base_and_offset_exact_vram_fit(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    if (!pgraph_zpass_report_write(buffer, 8, UINT64_MAX,
                                   sizeof(buffer) - 8 - REPORT_SIZE,
                                   sizeof(buffer), 8)) {
        return false;
    }

    return buffer_is_canary_range(buffer, 0, BUFFER_SIZE - REPORT_SIZE) &&
           report_matches(&buffer[BUFFER_SIZE - REPORT_SIZE], 8);
}

static bool test_base_beyond_vram_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 65, UINT64_MAX, 0, sizeof(buffer),
                                      9) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_base_at_vram_end_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 64, UINT64_MAX, 0, sizeof(buffer),
                                      10) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_base_plus_offset_beyond_vram_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 8, UINT64_MAX, 57, sizeof(buffer),
                                      11) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_report_one_byte_past_vram_rejects_without_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 17, UINT64_MAX, 32,
                                      sizeof(buffer), 12) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_max_base_rejects_without_overflow_or_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, UINT64_MAX, UINT64_MAX, 0,
                                      sizeof(buffer), 13) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_max_offset_rejects_without_overflow_or_stores(void)
{
    uint8_t buffer[BUFFER_SIZE];

    memset(buffer, CANARY, sizeof(buffer));
    return !pgraph_zpass_report_write(buffer, 0, UINT64_MAX, UINT64_MAX,
                                      sizeof(buffer), 14) &&
           buffer_is_canary(buffer, sizeof(buffer));
}

static bool test_ramin_descriptor_requires_all_three_words(void)
{
    return pgraph_zpass_report_descriptor_fits(12, 0) &&
           pgraph_zpass_report_descriptor_fits(64, 52) &&
           !pgraph_zpass_report_descriptor_fits(11, 0) &&
           !pgraph_zpass_report_descriptor_fits(64, 53) &&
           !pgraph_zpass_report_descriptor_fits(64, UINT64_MAX);
}

static void print_tap_result(unsigned int number, const char *name, bool pass)
{
    printf("%s %u - %s\n", pass ? "ok" : "not ok", number, name);
}

int main(void)
{
    bool exact_dma = test_exact_dma_fit_writes_complete_report();
    bool short_dma = test_incomplete_dma_span_rejects_without_stores();
    bool nonzero_offset =
        test_nonzero_offset_exact_fit_writes_complete_report();
    bool one_byte_short = test_one_byte_short_dma_span_rejects_without_stores();
    bool original_case =
        test_original_nonzero_offset_case_rejects_without_stores();
    bool beyond_limit = test_offset_beyond_limit_rejects_without_underflow();
    bool exact_vram = test_exact_vram_fit_writes_complete_report();
    bool short_vram = test_incomplete_vram_span_rejects_without_stores();
    bool nonzero_end = test_nonzero_base_and_offset_exact_vram_fit();
    bool descriptor = test_ramin_descriptor_requires_all_three_words();
    bool base_beyond = test_base_beyond_vram_rejects_without_stores();
    bool base_at_end = test_base_at_vram_end_rejects_without_stores();
    bool offset_beyond =
        test_base_plus_offset_beyond_vram_rejects_without_stores();
    bool report_past_end =
        test_report_one_byte_past_vram_rejects_without_stores();
    bool max_base = test_max_base_rejects_without_overflow_or_stores();
    bool max_offset = test_max_offset_rejects_without_overflow_or_stores();

    puts("TAP version 13");
    puts("1..16");
    print_tap_result(1, "exact DMA fit writes a complete report", exact_dma);
    print_tap_result(2, "incomplete DMA span performs no stores", short_dma);
    print_tap_result(3, "exact nonzero offset fit writes a complete report",
                     nonzero_offset);
    print_tap_result(4, "one-byte-short DMA span performs no stores",
                     one_byte_short);
    print_tap_result(5, "original incomplete span performs no stores",
                     original_case);
    print_tap_result(6, "offset beyond DMA limit performs no stores",
                     beyond_limit);
    print_tap_result(7, "exact VRAM fit writes a complete report", exact_vram);
    print_tap_result(8, "incomplete VRAM span performs no stores", short_vram);
    print_tap_result(9,
                     "exact nonzero base and offset VRAM fit writes a report",
                     nonzero_end);
    print_tap_result(10, "RAMIN descriptor requires all three words",
                     descriptor);
    print_tap_result(11, "base beyond VRAM performs no stores", base_beyond);
    print_tap_result(12, "base at VRAM end performs no stores", base_at_end);
    print_tap_result(13, "base plus offset beyond VRAM performs no stores",
                     offset_beyond);
    print_tap_result(14, "report one byte past VRAM performs no stores",
                     report_past_end);
    print_tap_result(15, "maximum base performs no stores", max_base);
    print_tap_result(16, "maximum offset performs no stores", max_offset);

    return exact_dma && short_dma && nonzero_offset && one_byte_short &&
                   original_case && beyond_limit && exact_vram && short_vram &&
                   nonzero_end && descriptor && base_beyond && base_at_end &&
                   offset_beyond && report_past_end && max_base && max_offset ?
               0 :
               1;
}

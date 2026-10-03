/*
 * NV2A PGRAPH report helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_REPORTS_H
#define HW_XBOX_NV2A_PGRAPH_REPORTS_H

#include <stdbool.h>
#include <stdint.h>

bool pgraph_zpass_report_descriptor_fits(uint64_t ramin_size,
                                         uint64_t dma_report);
bool pgraph_zpass_report_span_fits(uint64_t base, uint64_t inclusive_limit,
                                   uint64_t offset, uint64_t vram_size);
bool pgraph_zpass_report_write(uint8_t *vram, uint64_t base,
                               uint64_t inclusive_limit, uint64_t offset,
                               uint64_t vram_size, uint32_t result);

#endif

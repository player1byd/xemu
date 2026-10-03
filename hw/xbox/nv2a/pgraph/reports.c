/*
 * NV2A PGRAPH report handling
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "reports.h"

enum {
    ZPASS_REPORT_SIZE = 16,
    /* nv_dma_load currently reads flags, limit, and frame. */
    DMA_DESCRIPTOR_SIZE = 3 * sizeof(uint32_t),
};

bool pgraph_zpass_report_descriptor_fits(uint64_t ramin_size,
                                         uint64_t dma_report)
{
    return dma_report <= ramin_size &&
           DMA_DESCRIPTOR_SIZE <= ramin_size - dma_report;
}

bool pgraph_zpass_report_span_fits(uint64_t base, uint64_t inclusive_limit,
                                   uint64_t offset, uint64_t vram_size)
{
    return offset <= inclusive_limit &&
           ZPASS_REPORT_SIZE - 1 <= inclusive_limit - offset &&
           base <= vram_size && offset <= vram_size - base &&
           ZPASS_REPORT_SIZE <= vram_size - base - offset;
}

bool pgraph_zpass_report_write(uint8_t *vram, uint64_t base,
                               uint64_t inclusive_limit, uint64_t offset,
                               uint64_t vram_size, uint32_t result)
{
    if (!pgraph_zpass_report_span_fits(base, inclusive_limit, offset,
                                       vram_size)) {
        return false;
    }

    uint8_t *report = vram + base + offset;
    const uint64_t timestamp = UINT64_C(0x0011223344556677);

    stq_le_p(report, timestamp);
    stl_le_p(report + 8, result);
    stl_le_p(report + 12, 0);
    return true;
}

void pgraph_write_zpass_pixel_cnt_report(NV2AState *d, hwaddr dma_report,
                                         uint32_t parameter, uint32_t result)
{
    const hwaddr ramin_size = memory_region_size(&d->ramin);
    const hwaddr vram_size = memory_region_size(d->vram);
    const hwaddr offset = GET_MASK(parameter, NV097_GET_REPORT_OFFSET);

    if (!pgraph_zpass_report_descriptor_fits(ramin_size, dma_report)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PGRAPH: rejected report DMA descriptor @%" HWADDR_PRIx
                      "\n",
                      dma_report);
        return;
    }

    DMAObject dma = nv_dma_load(d, dma_report);
    trace_nv2a_dma_map(dma_report, dma.dma_class, dma.dma_target, dma.address,
                       dma.limit);

    const hwaddr base = dma.address & 0x07FFFFFF;
    if (!pgraph_zpass_report_write(d->vram_ptr, base, dma.limit, offset,
                                   vram_size, result)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PGRAPH: rejected report span: dma=%" HWADDR_PRIx
                      " base=%" HWADDR_PRIx " offset=%" HWADDR_PRIx
                      " limit=%" HWADDR_PRIx " vram_size=%" HWADDR_PRIx "\n",
                      dma_report, base, offset, dma.limit, vram_size);
        return;
    }

    NV2A_DPRINTF("Report result %d @%" HWADDR_PRIx, result, offset);
}

/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_POLYGON_OFFSET_H
#define HW_XBOX_NV2A_PGRAPH_POLYGON_OFFSET_H

#include <stdbool.h>
#include <stdint.h>

#include "hw/xbox/nv2a/nv2a_regs.h"

typedef struct PGRAPHPolygonOffsetUniformKey {
    uint32_t offset_bits;
    uint32_t factor_bits;
} PGRAPHPolygonOffsetUniformKey;

static inline bool pgraph_polygon_offset_is_enabled(uint32_t primitive_mode,
                                                    uint32_t setup_raster)
{
    if (primitive_mode < NV097_SET_BEGIN_END_OP_TRIANGLES) {
        return false;
    }

    uint32_t polygon_mode =
        setup_raster & NV_PGRAPH_SETUPRASTER_FRONTFACEMODE;

    switch (polygon_mode) {
    case NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL:
        return setup_raster & NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE;
    case NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE:
        return setup_raster & NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE;
    case NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT:
        return setup_raster & NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE;
    default:
        return false;
    }
}

static inline PGRAPHPolygonOffsetUniformKey pgraph_polygon_offset_uniform_key(
    uint32_t primitive_mode, uint32_t setup_raster, uint32_t offset_bits,
    uint32_t factor_bits)
{
    if (!pgraph_polygon_offset_is_enabled(primitive_mode, setup_raster)) {
        return (PGRAPHPolygonOffsetUniformKey){ 0 };
    }

    return (PGRAPHPolygonOffsetUniformKey){
        .offset_bits = offset_bits,
        .factor_bits = factor_bits,
    };
}

static inline bool pgraph_polygon_offset_uniform_key_equal(
    PGRAPHPolygonOffsetUniformKey a, PGRAPHPolygonOffsetUniformKey b)
{
    return a.offset_bits == b.offset_bits && a.factor_bits == b.factor_bits;
}

static inline bool pgraph_polygon_offset_uniform_key_changed(
    bool cached_valid, PGRAPHPolygonOffsetUniformKey cached,
    PGRAPHPolygonOffsetUniformKey current)
{
    return !cached_valid ||
           !pgraph_polygon_offset_uniform_key_equal(cached, current);
}

#endif

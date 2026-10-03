/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_UNIFORM_SOURCE_H
#define HW_XBOX_NV2A_PGRAPH_UNIFORM_SOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hw/xbox/nv2a/nv2a_regs.h"

typedef enum PGRAPHUniformStage {
    PGRAPH_UNIFORM_STAGE_VSH,
    PGRAPH_UNIFORM_STAGE_PSH,
    PGRAPH_UNIFORM_STAGE_COUNT,
} PGRAPHUniformStage;

typedef enum PGRAPHUniformStageMask {
    PGRAPH_UNIFORM_STAGE_MASK_NONE = 0,
    PGRAPH_UNIFORM_STAGE_MASK_VSH = 1 << PGRAPH_UNIFORM_STAGE_VSH,
    PGRAPH_UNIFORM_STAGE_MASK_PSH = 1 << PGRAPH_UNIFORM_STAGE_PSH,
    PGRAPH_UNIFORM_STAGE_MASK_BOTH = PGRAPH_UNIFORM_STAGE_MASK_VSH |
                                     PGRAPH_UNIFORM_STAGE_MASK_PSH,
} PGRAPHUniformStageMask;

typedef struct PGRAPHUniformSourceEpochs {
    uint64_t total;
    uint64_t stage[PGRAPH_UNIFORM_STAGE_COUNT];
    uint64_t unclassified;
} PGRAPHUniformSourceEpochs;

static inline void pgraph_uniform_source_touch(
    PGRAPHUniformSourceEpochs *epochs, PGRAPHUniformStageMask stages)
{
    epochs->total++;
    for (unsigned int stage = 0; stage < PGRAPH_UNIFORM_STAGE_COUNT; stage++) {
        if (stages & (1U << stage)) {
            epochs->stage[stage]++;
        }
    }
}

static inline void pgraph_uniform_source_touch_unclassified(
    PGRAPHUniformSourceEpochs *epochs)
{
    pgraph_uniform_source_touch(epochs, PGRAPH_UNIFORM_STAGE_MASK_BOTH);
    epochs->unclassified++;
}

static inline bool pgraph_uniform_float_bits_update(
    float *value, uint32_t new_bits, PGRAPHUniformSourceEpochs *epochs,
    PGRAPHUniformStageMask stages)
{
    uint32_t old_bits;

    _Static_assert(sizeof(*value) == sizeof(new_bits),
                   "uniform float must be 32-bit");
    memcpy(&old_bits, value, sizeof(old_bits));
    if (old_bits == new_bits) {
        return false;
    }

    memcpy(value, &new_bits, sizeof(new_bits));
    pgraph_uniform_source_touch(epochs, stages);
    return true;
}

static inline bool pgraph_uniform_source_stage_changed(
    const PGRAPHUniformSourceEpochs *current,
    const PGRAPHUniformSourceEpochs *previous, PGRAPHUniformStage stage)
{
    return current->stage[stage] != previous->stage[stage];
}

/* Uniform values read directly from PGRAPH registers. */
static inline PGRAPHUniformStageMask
pgraph_reg_uniform_stage_mask(unsigned int reg, uint32_t changed_bits)
{
    if ((reg >= NV_PGRAPH_COMBINEFACTOR0 &&
         reg < NV_PGRAPH_COMBINEFACTOR0 + 8 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_COMBINEFACTOR1 &&
         reg < NV_PGRAPH_COMBINEFACTOR1 + 8 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_WINDOWCLIPX0 &&
         reg < NV_PGRAPH_WINDOWCLIPX0 + 8 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_WINDOWCLIPY0 &&
         reg < NV_PGRAPH_WINDOWCLIPY0 + 8 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_BUMPMAT00 &&
         reg < NV_PGRAPH_BUMPMAT00 + 3 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_BUMPMAT01 &&
         reg < NV_PGRAPH_BUMPMAT01 + 3 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_BUMPMAT10 &&
         reg < NV_PGRAPH_BUMPMAT10 + 3 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_BUMPMAT11 &&
         reg < NV_PGRAPH_BUMPMAT11 + 3 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_BUMPOFFSET1 &&
         reg < NV_PGRAPH_BUMPOFFSET1 + 3 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_BUMPSCALE1 &&
         reg < NV_PGRAPH_BUMPSCALE1 + 3 * sizeof(uint32_t)) ||
        (reg >= NV_PGRAPH_TEXFMT0 &&
         reg < NV_PGRAPH_TEXFMT0 + 4 * sizeof(uint32_t))) {
        return PGRAPH_UNIFORM_STAGE_MASK_PSH;
    }

    switch (reg) {
    case NV_PGRAPH_FOGPARAM0:
    case NV_PGRAPH_FOGPARAM1:
        return PGRAPH_UNIFORM_STAGE_MASK_VSH;
    case NV_PGRAPH_ZCLIPMIN:
    case NV_PGRAPH_ZCLIPMAX:
        return PGRAPH_UNIFORM_STAGE_MASK_BOTH;
    case NV_PGRAPH_COLORKEYCOLOR0:
    case NV_PGRAPH_COLORKEYCOLOR1:
    case NV_PGRAPH_COLORKEYCOLOR2:
    case NV_PGRAPH_COLORKEYCOLOR3:
    case NV_PGRAPH_CONTROL_0:
    case NV_PGRAPH_FOGCOLOR:
    case NV_PGRAPH_SPECFOGFACTOR0:
    case NV_PGRAPH_SPECFOGFACTOR1:
    case NV_PGRAPH_ZOFFSETBIAS:
    case NV_PGRAPH_ZOFFSETFACTOR:
        return PGRAPH_UNIFORM_STAGE_MASK_PSH;
    case NV_PGRAPH_SETUPRASTER:
        return PGRAPH_UNIFORM_STAGE_MASK_PSH |
               ((changed_bits & NV_PGRAPH_SETUPRASTER_Z_FORMAT) ?
                    PGRAPH_UNIFORM_STAGE_MASK_VSH :
                    PGRAPH_UNIFORM_STAGE_MASK_NONE);
    default:
        return PGRAPH_UNIFORM_STAGE_MASK_NONE;
    }
}

#endif

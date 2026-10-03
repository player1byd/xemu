/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_UNIFORM_STAGE_UPDATE_H
#define HW_XBOX_NV2A_PGRAPH_UNIFORM_STAGE_UPDATE_H

#include <stdbool.h>

#include "uniform-source.h"

typedef struct PGRAPHUniformStageUpdateInputs {
    bool source_changed[PGRAPH_UNIFORM_STAGE_COUNT];
    bool layout_changed[PGRAPH_UNIFORM_STAGE_COUNT];
    bool texture_bindings_changed;
    bool psh_effective_inputs_changed;
    bool inline_values_in_vsh_ubo;
    bool vsh_rows_dirty;
    bool force_full_update;
} PGRAPHUniformStageUpdateInputs;

static inline void pgraph_uniform_stage_update_needs(
    const PGRAPHUniformStageUpdateInputs *inputs,
    bool update_stage[PGRAPH_UNIFORM_STAGE_COUNT])
{
    for (unsigned int stage = 0; stage < PGRAPH_UNIFORM_STAGE_COUNT; stage++) {
        update_stage[stage] = inputs->source_changed[stage] ||
                              inputs->layout_changed[stage] ||
                              inputs->force_full_update;
    }

    update_stage[PGRAPH_UNIFORM_STAGE_VSH] |=
        inputs->inline_values_in_vsh_ubo || inputs->vsh_rows_dirty;
    update_stage[PGRAPH_UNIFORM_STAGE_PSH] |=
        inputs->texture_bindings_changed ||
        inputs->psh_effective_inputs_changed;
}

#endif

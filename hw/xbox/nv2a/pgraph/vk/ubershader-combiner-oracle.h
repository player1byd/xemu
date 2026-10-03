/*
 * Geforce NV2A PGRAPH Vulkan ubershader combiner oracle
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_UBERSHADER_COMBINER_ORACLE_H
#define HW_XBOX_NV2A_PGRAPH_VK_UBERSHADER_COMBINER_ORACLE_H

#include <stdbool.h>

#include "hw/xbox/nv2a/pgraph/vk/ubershader-controls.h"

typedef struct PGRAPHUberCombinerInputs {
    float fog[4];
    float v[2][4];
    float t[4][4];
    /*
     * The specialized shell starts R0.rgb/R1 at zero. Its texture shell
     * supplies R0.a from T0.a when stage zero is active, otherwise one.
     */
    float initial_r0_alpha;
} PGRAPHUberCombinerInputs;

typedef struct PGRAPHUberCombinerResult {
    float color[4];
    float v[2][4];
    float t[4][4];
    float r[2][4];
} PGRAPHUberCombinerResult;

/*
 * Evaluate only the combiner subset admitted by the Stage 1 packer. Stage 1
 * transport remains bit-preserving. This synchronous CPU numerical oracle
 * accepts only finite constants, shell inputs, intermediates, and results.
 * NaN/Inf and signed-zero equivalence belong to the generated-GLSL/GPU oracle.
 * This is shader-development evidence, not a render path.
 */
bool pgraph_vk_eval_ubershader_combiner(
    const PGRAPHUberControls *packet,
    const PGRAPHUberCombinerInputs *inputs,
    PGRAPHUberCombinerResult *result);

#endif

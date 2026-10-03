/*
 * Geforce NV2A PGRAPH Vulkan ubershader control ABI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_UBERSHADER_CONTROLS_H
#define HW_XBOX_NV2A_PGRAPH_VK_UBERSHADER_CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGRAPH_UBER_CONTROL_ABI 1U
#define PGRAPH_UBER_STAGE_COUNT 8U
#define PGRAPH_UBER_CONSTANT_COUNT 18U

/* Decoded combiner semantics supplied by the existing PshState path. */
typedef struct PGRAPHUberControlSource {
    uint32_t combiner_control;
    uint32_t rgb_inputs[PGRAPH_UBER_STAGE_COUNT];
    uint32_t alpha_inputs[PGRAPH_UBER_STAGE_COUNT];
    uint32_t rgb_outputs[PGRAPH_UBER_STAGE_COUNT];
    uint32_t alpha_outputs[PGRAPH_UBER_STAGE_COUNT];
    uint32_t final_inputs_0;
    uint32_t final_inputs_1;
    float constants[PGRAPH_UBER_CONSTANT_COUNT][4];
} PGRAPHUberControlSource;

typedef struct PGRAPHUberControls {
    /* ABI, active stage count, raw combiner control, reserved zero. */
    _Alignas(16) uint32_t header[4];

    /* RGB input, alpha input, RGB output, alpha output per active stage. */
    uint32_t stage[PGRAPH_UBER_STAGE_COUNT][4];

    /* Final input words 0/1 and two reserved zero words. */
    uint32_t final_words[4];
    float constants[PGRAPH_UBER_CONSTANT_COUNT][4];
} PGRAPHUberControls;

_Static_assert(sizeof(float) == 4, "32-bit float required");
_Static_assert(_Alignof(PGRAPHUberControls) == 16, "Control alignment");
_Static_assert(offsetof(PGRAPHUberControls, stage) == 16, "Stage offset");
_Static_assert(offsetof(PGRAPHUberControls, final_words) == 144,
               "Final offset");
_Static_assert(offsetof(PGRAPHUberControls, constants) == 160,
               "Constants offset");
_Static_assert(sizeof(PGRAPHUberControls) == 448, "Packet size");

typedef enum PGRAPHUberControlRejectReason {
    PGRAPH_UBER_CONTROL_REJECT_NONE,
    PGRAPH_UBER_CONTROL_REJECT_NULL_OUTPUT,
    PGRAPH_UBER_CONTROL_REJECT_NULL_SOURCE,
    PGRAPH_UBER_CONTROL_REJECT_STAGE_COUNT,
    PGRAPH_UBER_CONTROL_REJECT_LSB_MUX,
    PGRAPH_UBER_CONTROL_REJECT_CONTROL_BITS,
    PGRAPH_UBER_CONTROL_REJECT_RGB_INPUT,
    PGRAPH_UBER_CONTROL_REJECT_ALPHA_INPUT,
    PGRAPH_UBER_CONTROL_REJECT_RGB_OUTPUT,
    PGRAPH_UBER_CONTROL_REJECT_ALPHA_OUTPUT,
    PGRAPH_UBER_CONTROL_REJECT_FINAL_DISABLED,
    PGRAPH_UBER_CONTROL_REJECT_FINAL_INPUT,
    PGRAPH_UBER_CONTROL_REJECT_FINAL_FLAGS,
} PGRAPHUberControlRejectReason;

bool pgraph_vk_pack_ubershader_controls(
    PGRAPHUberControls *packet, const PGRAPHUberControlSource *source,
    PGRAPHUberControlRejectReason *reason);

#endif

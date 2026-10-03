/*
 * Geforce NV2A PGRAPH Vulkan ubershader control ABI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/ubershader-controls.h"

#include <string.h>

#include "hw/xbox/nv2a/pgraph/psh_regs.h"

#define COMBINER_COUNT_MASK UINT32_C(0xff)
#define COMBINER_FLAG_SHIFT 8U
#define COMBINER_ALLOWED_FLAGS                                           \
    (PS_COMBINERCOUNT_MUX_MSB | PS_COMBINERCOUNT_UNIQUE_C0 |           \
     PS_COMBINERCOUNT_UNIQUE_C1)

static bool reject(PGRAPHUberControlRejectReason *reason,
                   PGRAPHUberControlRejectReason value)
{
    if (reason) {
        *reason = value;
    }
    return false;
}

static bool readable_stage_register(uint8_t input)
{
    switch (input & 0xf) {
    case PS_REGISTER_ZERO:
    case PS_REGISTER_C0:
    case PS_REGISTER_C1:
    case PS_REGISTER_FOG:
    case PS_REGISTER_V0:
    case PS_REGISTER_V1:
    case PS_REGISTER_T0:
    case PS_REGISTER_T1:
    case PS_REGISTER_T2:
    case PS_REGISTER_T3:
    case PS_REGISTER_R0:
    case PS_REGISTER_R1:
        return true;
    default:
        return false;
    }
}

static bool readable_final_register(uint8_t input)
{
    if (readable_stage_register(input)) {
        return true;
    }
    return (input & 0xf) == PS_REGISTER_V1R0_SUM ||
           (input & 0xf) == PS_REGISTER_EF_PROD;
}

static bool valid_stage_inputs(uint32_t inputs)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        if (!readable_stage_register(inputs >> shift)) {
            return false;
        }
    }
    return true;
}

static bool writable_register(uint8_t reg)
{
    switch (reg & 0xf) {
    case PS_REGISTER_DISCARD:
    case PS_REGISTER_V0:
    case PS_REGISTER_V1:
    case PS_REGISTER_T0:
    case PS_REGISTER_T1:
    case PS_REGISTER_T2:
    case PS_REGISTER_T3:
    case PS_REGISTER_R0:
    case PS_REGISTER_R1:
        return true;
    default:
        return false;
    }
}

static bool valid_output_mapping(uint8_t flags)
{
    switch (flags & 0x38) {
    case PS_COMBINEROUTPUT_IDENTITY:
    case PS_COMBINEROUTPUT_BIAS:
    case PS_COMBINEROUTPUT_SHIFTLEFT_1:
    case PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS:
    case PS_COMBINEROUTPUT_SHIFTLEFT_2:
    case PS_COMBINEROUTPUT_SHIFTRIGHT_1:
        return true;
    default:
        return false;
    }
}

static bool valid_stage_output(uint32_t output, bool alpha)
{
    uint8_t flags;

    if (output & UINT32_C(0xfff00000)) {
        return false;
    }
    if (!writable_register(output) ||
        !writable_register(output >> 4) ||
        !writable_register(output >> 8)) {
        return false;
    }

    flags = output >> 12;
    if (!valid_output_mapping(flags)) {
        return false;
    }

    /* Dot products and blue-to-alpha writes are RGB-lane operations. */
    return !alpha || !(flags & (PS_COMBINEROUTPUT_AB_DOT_PRODUCT |
                                PS_COMBINEROUTPUT_CD_DOT_PRODUCT |
                                PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA |
                                PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA));
}

static bool valid_final_input(uint8_t input)
{
    uint8_t mapping = input & 0xe0;

    if (!readable_final_register(input)) {
        return false;
    }
    return mapping == PS_INPUTMAPPING_UNSIGNED_IDENTITY ||
           mapping == PS_INPUTMAPPING_UNSIGNED_INVERT;
}

static bool valid_final_inputs(uint32_t final_0, uint32_t final_1)
{
    uint8_t e = final_1 >> 24;
    uint8_t f = final_1 >> 16;
    uint8_t g = final_1 >> 8;

    for (unsigned int shift = 0; shift < 32; shift += 8) {
        if (!valid_final_input(final_0 >> shift)) {
            return false;
        }
    }
    if (!valid_final_input(e) || !valid_final_input(f) ||
        !valid_final_input(g)) {
        return false;
    }

    /* E and F define EF_PROD, so neither can recursively read it. */
    return (e & 0xf) != PS_REGISTER_EF_PROD &&
           (f & 0xf) != PS_REGISTER_EF_PROD;
}

bool pgraph_vk_pack_ubershader_controls(
    PGRAPHUberControls *packet, const PGRAPHUberControlSource *source,
    PGRAPHUberControlRejectReason *reason)
{
    unsigned int stages;
    uint32_t flags;
    bool uses_mux = false;
    const uint32_t mux_mask =
        (uint32_t)PS_COMBINEROUTPUT_AB_CD_MUX << 12;

    if (!packet) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_NULL_OUTPUT);
    }
    memset(packet, 0, sizeof(*packet));
    if (!source) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_NULL_SOURCE);
    }

    stages = source->combiner_control & COMBINER_COUNT_MASK;
    flags = source->combiner_control >> COMBINER_FLAG_SHIFT;
    if (stages > PGRAPH_UBER_STAGE_COUNT) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_STAGE_COUNT);
    }
    if (flags & ~COMBINER_ALLOWED_FLAGS) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_CONTROL_BITS);
    }

    for (unsigned int i = 0; i < stages; i++) {
        if (!valid_stage_inputs(source->rgb_inputs[i])) {
            return reject(reason, PGRAPH_UBER_CONTROL_REJECT_RGB_INPUT);
        }
        if (!valid_stage_inputs(source->alpha_inputs[i])) {
            return reject(reason, PGRAPH_UBER_CONTROL_REJECT_ALPHA_INPUT);
        }
        if (!valid_stage_output(source->rgb_outputs[i], false)) {
            return reject(reason, PGRAPH_UBER_CONTROL_REJECT_RGB_OUTPUT);
        }
        if (!valid_stage_output(source->alpha_outputs[i], true)) {
            return reject(reason, PGRAPH_UBER_CONTROL_REJECT_ALPHA_OUTPUT);
        }
        uses_mux |= ((source->rgb_outputs[i] |
                      source->alpha_outputs[i]) & mux_mask) != 0;
    }
    if (uses_mux && !(flags & PS_COMBINERCOUNT_MUX_MSB)) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_LSB_MUX);
    }

    if (!source->final_inputs_0 && !source->final_inputs_1) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_FINAL_DISABLED);
    }
    if (source->final_inputs_1 & UINT32_C(0x1f)) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_FINAL_FLAGS);
    }
    if (!valid_final_inputs(source->final_inputs_0,
                            source->final_inputs_1)) {
        return reject(reason, PGRAPH_UBER_CONTROL_REJECT_FINAL_INPUT);
    }

    packet->header[0] = PGRAPH_UBER_CONTROL_ABI;
    packet->header[1] = stages;
    packet->header[2] = source->combiner_control;
    for (unsigned int i = 0; i < stages; i++) {
        packet->stage[i][0] = source->rgb_inputs[i];
        packet->stage[i][1] = source->alpha_inputs[i];
        packet->stage[i][2] = source->rgb_outputs[i];
        packet->stage[i][3] = source->alpha_outputs[i];
    }
    packet->final_words[0] = source->final_inputs_0;
    packet->final_words[1] = source->final_inputs_1;
    memcpy(packet->constants, source->constants, sizeof(packet->constants));

    if (reason) {
        *reason = PGRAPH_UBER_CONTROL_REJECT_NONE;
    }
    return true;
}

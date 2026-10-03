/*
 * NV2A Vulkan ubershader control ABI tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hw/xbox/nv2a/pgraph/vk/ubershader-controls.h"
#include "hw/xbox/nv2a/pgraph/psh_regs.h"

static uint32_t input_word(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return (uint32_t)a << 24 | (uint32_t)b << 16 |
           (uint32_t)c << 8 | d;
}

static uint32_t output_word(uint8_t ab, uint8_t cd, uint8_t muxsum,
                            uint8_t flags)
{
    return (uint32_t)flags << 12 | (uint32_t)muxsum << 8 |
           (uint32_t)ab << 4 | cd;
}

static PGRAPHUberControlSource valid_state(void)
{
    PGRAPHUberControlSource state = { 0 };

    state.combiner_control = 2 |
        (PS_COMBINERCOUNT_MUX_MSB << 8) |
        (PS_COMBINERCOUNT_UNIQUE_C0 << 8) |
        (PS_COMBINERCOUNT_UNIQUE_C1 << 8);
    state.rgb_inputs[0] = input_word(
        PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1 | PS_INPUTMAPPING_UNSIGNED_INVERT,
        PS_REGISTER_T0 | PS_INPUTMAPPING_EXPAND_NORMAL,
        PS_REGISTER_R0 | PS_INPUTMAPPING_SIGNED_IDENTITY);
    state.alpha_inputs[0] = input_word(
        PS_REGISTER_C0 | PS_CHANNEL_ALPHA,
        PS_REGISTER_C1 | PS_CHANNEL_ALPHA,
        PS_REGISTER_FOG | PS_CHANNEL_BLUE,
        PS_REGISTER_R1 | PS_CHANNEL_ALPHA);
    state.rgb_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_V0,
        PS_COMBINEROUTPUT_AB_DOT_PRODUCT |
        PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA |
        PS_COMBINEROUTPUT_SHIFTLEFT_1);
    state.alpha_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD,
        PS_COMBINEROUTPUT_AB_CD_MUX | PS_COMBINEROUTPUT_BIAS);

    state.rgb_inputs[1] = input_word(
        PS_REGISTER_T1, PS_REGISTER_T2, PS_REGISTER_T3, PS_REGISTER_R0);
    state.alpha_inputs[1] = input_word(
        PS_REGISTER_T1 | PS_CHANNEL_ALPHA,
        PS_REGISTER_T2 | PS_CHANNEL_ALPHA,
        PS_REGISTER_T3 | PS_CHANNEL_ALPHA,
        PS_REGISTER_R0 | PS_CHANNEL_ALPHA);
    state.rgb_outputs[1] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD,
        PS_COMBINEROUTPUT_SHIFTRIGHT_1);
    state.alpha_outputs[1] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD,
        PS_COMBINEROUTPUT_SHIFTLEFT_2);

    state.final_inputs_0 = input_word(
        PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1 | PS_INPUTMAPPING_UNSIGNED_INVERT,
        PS_REGISTER_V1R0_SUM | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_EF_PROD | PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    state.final_inputs_1 = input_word(
        PS_REGISTER_C0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_C1 | PS_INPUTMAPPING_UNSIGNED_INVERT,
        PS_REGISTER_R0 | PS_CHANNEL_ALPHA,
        PS_FINALCOMBINERSETTING_CLAMP_SUM |
        PS_FINALCOMBINERSETTING_COMPLEMENT_R0);
    return state;
}

static void fill_constants(PGRAPHUberControlSource *source,
                           uint32_t bits[PGRAPH_UBER_CONSTANT_COUNT][4])
{
    static const uint32_t special[] = {
        UINT32_C(0x00000000), UINT32_C(0x80000000),
        UINT32_C(0x7fc01234), UINT32_C(0xff800000),
    };

    for (unsigned int row = 0; row < PGRAPH_UBER_CONSTANT_COUNT; row++) {
        for (unsigned int lane = 0; lane < 4; lane++) {
            bits[row][lane] = row == 0 ? special[lane] :
                UINT32_C(0x5a000000) | row << 8 | lane;
            memcpy(&source->constants[row][lane], &bits[row][lane],
                   sizeof(uint32_t));
        }
    }
}

static bool packet_is_zero(const PGRAPHUberControls *packet)
{
    const uint8_t *bytes = (const uint8_t *)packet;

    for (size_t i = 0; i < sizeof(*packet); i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

static void test_packet_layout_and_active_state(void)
{
    PGRAPHUberControlSource state = valid_state();
    PGRAPHUberControls packet;
    PGRAPHUberControlRejectReason reason =
        PGRAPH_UBER_CONTROL_REJECT_NULL_SOURCE;
    uint32_t constant_bits[PGRAPH_UBER_CONSTANT_COUNT][4];

    fill_constants(&state, constant_bits);
    state.rgb_inputs[2] = UINT32_MAX;
    state.alpha_inputs[2] = UINT32_MAX;
    state.rgb_outputs[2] = UINT32_MAX;
    state.alpha_outputs[2] = UINT32_MAX;
    memset(&packet, 0xa5, sizeof(packet));
    assert(pgraph_vk_pack_ubershader_controls(&packet, &state, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NONE);
    assert(packet.header[0] == PGRAPH_UBER_CONTROL_ABI);
    assert(packet.header[1] == 2);
    assert(packet.header[2] == state.combiner_control);
    assert(packet.header[3] == 0);
    assert(!memcmp(packet.stage[0],
                   (uint32_t[]) { state.rgb_inputs[0],
                                  state.alpha_inputs[0],
                                  state.rgb_outputs[0],
                                  state.alpha_outputs[0] },
                   sizeof(packet.stage[0])));
    assert(!memcmp(packet.stage[1],
                   (uint32_t[]) { state.rgb_inputs[1],
                                  state.alpha_inputs[1],
                                  state.rgb_outputs[1],
                                  state.alpha_outputs[1] },
                   sizeof(packet.stage[1])));
    for (unsigned int i = 2; i < PGRAPH_UBER_STAGE_COUNT; i++) {
        assert(!memcmp(packet.stage[i], (uint32_t[4]) { 0 },
                       sizeof(packet.stage[i])));
    }
    assert(packet.final_words[0] == state.final_inputs_0);
    assert(packet.final_words[1] == state.final_inputs_1);
    assert(packet.final_words[2] == 0 && packet.final_words[3] == 0);
    assert(!memcmp(packet.constants, constant_bits, sizeof(constant_bits)));
}

static void test_zero_stage_count_and_inactive_rows_are_supported(void)
{
    PGRAPHUberControlSource state = valid_state();
    PGRAPHUberControls packet;
    PGRAPHUberControlRejectReason reason;
    state.combiner_control &=
        ~(UINT32_C(0xff) | (PS_COMBINERCOUNT_MUX_MSB << 8));
    for (unsigned int i = 0; i < PGRAPH_UBER_STAGE_COUNT; i++) {
        state.rgb_inputs[i] = UINT32_MAX;
        state.alpha_inputs[i] = UINT32_MAX;
        state.rgb_outputs[i] = UINT32_MAX;
        state.alpha_outputs[i] = UINT32_MAX;
    }
    assert(pgraph_vk_pack_ubershader_controls(&packet, &state, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NONE);
    assert(packet.header[1] == 0);
    for (unsigned int i = 0; i < PGRAPH_UBER_STAGE_COUNT; i++) {
        assert(!memcmp(packet.stage[i], (uint32_t[4]) { 0 },
                       sizeof(packet.stage[i])));
    }
}

static void test_lsb_selector_is_irrelevant_without_active_mux(void)
{
    PGRAPHUberControlSource state = valid_state();
    PGRAPHUberControls packet;
    PGRAPHUberControlRejectReason reason;

    state.combiner_control &= ~(PS_COMBINERCOUNT_MUX_MSB << 8);
    state.alpha_outputs[0] &=
        ~((uint32_t)PS_COMBINEROUTPUT_AB_CD_MUX << 12);
    assert(pgraph_vk_pack_ubershader_controls(&packet, &state, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NONE);
    assert(packet.header[2] == state.combiner_control);
}

static void test_maximum_stage_count_is_supported(void)
{
    PGRAPHUberControlSource state = valid_state();
    PGRAPHUberControls packet;
    PGRAPHUberControlRejectReason reason;

    state.combiner_control =
        (state.combiner_control & ~UINT32_C(0xff)) | PGRAPH_UBER_STAGE_COUNT;
    for (unsigned int i = 2; i < PGRAPH_UBER_STAGE_COUNT; i++) {
        state.rgb_inputs[i] = state.rgb_inputs[0];
        state.alpha_inputs[i] = state.alpha_inputs[0];
        state.rgb_outputs[i] = state.rgb_outputs[0];
        state.alpha_outputs[i] = state.alpha_outputs[0];
    }
    assert(pgraph_vk_pack_ubershader_controls(&packet, &state, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NONE);
    assert(packet.header[1] == PGRAPH_UBER_STAGE_COUNT);
    assert(packet.stage[PGRAPH_UBER_STAGE_COUNT - 1][0] ==
           state.rgb_inputs[PGRAPH_UBER_STAGE_COUNT - 1]);
}

typedef void (*MutateState)(PGRAPHUberControlSource *state);

static void too_many_stages(PGRAPHUberControlSource *state)
{
    state->combiner_control =
        (state->combiner_control & ~UINT32_C(0xff)) | 9;
}

static void lsb_mux(PGRAPHUberControlSource *state)
{
    state->combiner_control &= ~(PS_COMBINERCOUNT_MUX_MSB << 8);
}

static void reserved_control_bit(PGRAPHUberControlSource *state)
{
    state->combiner_control |= UINT32_C(1) << 9;
}

static void invalid_rgb_input(PGRAPHUberControlSource *state)
{
    state->rgb_inputs[0] = input_word(6, PS_REGISTER_V0, PS_REGISTER_V1,
                                      PS_REGISTER_R0);
}

static void invalid_alpha_input(PGRAPHUberControlSource *state)
{
    state->alpha_inputs[0] = input_word(
        PS_REGISTER_EF_PROD, PS_REGISTER_V0, PS_REGISTER_V1, PS_REGISTER_R0);
}

static void invalid_rgb_output(PGRAPHUberControlSource *state)
{
    state->rgb_outputs[0] = output_word(PS_REGISTER_C0, PS_REGISTER_R0,
                                        PS_REGISTER_R1, 0);
}

static void invalid_alpha_output(PGRAPHUberControlSource *state)
{
    state->alpha_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD,
        PS_COMBINEROUTPUT_AB_DOT_PRODUCT);
}

static void invalid_output_mapping(PGRAPHUberControlSource *state)
{
    state->rgb_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, 0x28);
}

static void reserved_output_bit(PGRAPHUberControlSource *state)
{
    state->rgb_outputs[0] |= UINT32_C(1) << 20;
}

static void disabled_final(PGRAPHUberControlSource *state)
{
    state->final_inputs_0 = 0;
    state->final_inputs_1 = 0;
}

static void invalid_final_input(PGRAPHUberControlSource *state)
{
    state->final_inputs_0 = input_word(
        PS_REGISTER_V0 | PS_INPUTMAPPING_EXPAND_NORMAL,
        PS_REGISTER_V1, PS_REGISTER_R0, PS_REGISTER_R1);
}

static void invalid_final_flags(PGRAPHUberControlSource *state)
{
    state->final_inputs_1 |= 1;
}

static void recursive_final_ef(PGRAPHUberControlSource *state)
{
    state->final_inputs_1 = input_word(
        PS_REGISTER_EF_PROD, PS_REGISTER_C1, PS_REGISTER_R0,
        PS_FINALCOMBINERSETTING_CLAMP_SUM);
}

static void test_rejections_zero_the_entire_packet(void)
{
    static const struct {
        MutateState mutate;
        PGRAPHUberControlRejectReason reason;
    } cases[] = {
        { too_many_stages, PGRAPH_UBER_CONTROL_REJECT_STAGE_COUNT },
        { lsb_mux, PGRAPH_UBER_CONTROL_REJECT_LSB_MUX },
        { reserved_control_bit, PGRAPH_UBER_CONTROL_REJECT_CONTROL_BITS },
        { invalid_rgb_input, PGRAPH_UBER_CONTROL_REJECT_RGB_INPUT },
        { invalid_alpha_input, PGRAPH_UBER_CONTROL_REJECT_ALPHA_INPUT },
        { invalid_rgb_output, PGRAPH_UBER_CONTROL_REJECT_RGB_OUTPUT },
        { invalid_alpha_output, PGRAPH_UBER_CONTROL_REJECT_ALPHA_OUTPUT },
        { invalid_output_mapping, PGRAPH_UBER_CONTROL_REJECT_RGB_OUTPUT },
        { reserved_output_bit, PGRAPH_UBER_CONTROL_REJECT_RGB_OUTPUT },
        { disabled_final, PGRAPH_UBER_CONTROL_REJECT_FINAL_DISABLED },
        { invalid_final_input, PGRAPH_UBER_CONTROL_REJECT_FINAL_INPUT },
        { invalid_final_flags, PGRAPH_UBER_CONTROL_REJECT_FINAL_FLAGS },
        { recursive_final_ef, PGRAPH_UBER_CONTROL_REJECT_FINAL_INPUT },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHUberControlSource state = valid_state();
        PGRAPHUberControls packet;
        PGRAPHUberControlRejectReason reason = PGRAPH_UBER_CONTROL_REJECT_NONE;

        cases[i].mutate(&state);
        memset(&packet, 0xa5, sizeof(packet));
        assert(!pgraph_vk_pack_ubershader_controls(&packet, &state, &reason));
        assert(reason == cases[i].reason);
        assert(packet_is_zero(&packet));
    }
}

static void test_null_inputs_are_reported_and_output_is_zero(void)
{
    PGRAPHUberControlSource state = valid_state();
    PGRAPHUberControls packet;
    PGRAPHUberControlRejectReason reason;
    memset(&packet, 0xa5, sizeof(packet));
    assert(!pgraph_vk_pack_ubershader_controls(&packet, NULL, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NULL_SOURCE);
    assert(packet_is_zero(&packet));

    assert(!pgraph_vk_pack_ubershader_controls(NULL, &state, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NULL_OUTPUT);
}

int main(void)
{
    test_packet_layout_and_active_state();
    test_zero_stage_count_and_inactive_rows_are_supported();
    test_lsb_selector_is_irrelevant_without_active_mux();
    test_maximum_stage_count_is_supported();
    test_rejections_zero_the_entire_packet();
    test_null_inputs_are_reported_and_output_is_zero();
    puts("ubershader control ABI tests passed");
    return 0;
}

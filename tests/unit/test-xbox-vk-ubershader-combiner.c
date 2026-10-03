/*
 * NV2A Vulkan ubershader combiner oracle tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hw/xbox/nv2a/pgraph/psh_regs.h"
#include "hw/xbox/nv2a/pgraph/vk/ubershader-combiner-oracle.h"

typedef struct RefVec {
    float lane[4];
} RefVec;

typedef struct RefMachine {
    RefVec fog;
    RefVec v[2];
    RefVec t[4];
    RefVec r[2];
} RefMachine;

static uint32_t inputs_word(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
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

static uint8_t input_code(uint8_t reg, uint8_t channel, uint8_t mapping)
{
    return reg | channel | mapping;
}

static void assert_close(float actual, float expected)
{
    if (fabsf(actual - expected) > 0.00001f) {
        fprintf(stderr, "actual=%g expected=%g\n", actual, expected);
    }
    assert(fabsf(actual - expected) <= 0.00001f);
}

static void assert_vec(const float actual[4], const float expected[4])
{
    for (unsigned int i = 0; i < 4; i++) {
        assert_close(actual[i], expected[i]);
    }
}

static PGRAPHUberControlSource base_source(unsigned int stages)
{
    PGRAPHUberControlSource source = { 0 };
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);

    source.combiner_control = stages | (PS_COMBINERCOUNT_MUX_MSB << 8);
    for (unsigned int i = 0; i < stages; i++) {
        source.rgb_inputs[i] = inputs_word(zero, zero, zero, zero);
        source.alpha_inputs[i] = inputs_word(zero, zero, zero, zero);
        source.rgb_outputs[i] = output_word(PS_REGISTER_DISCARD,
                                             PS_REGISTER_DISCARD,
                                             PS_REGISTER_DISCARD, 0);
        source.alpha_outputs[i] = source.rgb_outputs[i];
    }
    source.final_inputs_0 = inputs_word(zero, zero, zero, zero);
    source.final_inputs_1 = inputs_word(zero, zero, one, 0);
    return source;
}

static PGRAPHUberCombinerInputs base_inputs(void)
{
    PGRAPHUberCombinerInputs inputs = { 0 };

    inputs.fog[0] = 0.125f;
    inputs.fog[1] = 0.25f;
    inputs.fog[2] = 0.5f;
    inputs.fog[3] = 0.75f;
    for (unsigned int i = 0; i < 2; i++) {
        for (unsigned int lane = 0; lane < 4; lane++) {
            inputs.v[i][lane] = 0.0625f * (float)(1 + i * 4 + lane);
        }
    }
    for (unsigned int i = 0; i < 4; i++) {
        for (unsigned int lane = 0; lane < 4; lane++) {
            inputs.t[i][lane] = 0.03125f * (float)(1 + i * 4 + lane);
        }
    }
    inputs.initial_r0_alpha = 0.75f;
    return inputs;
}

static PGRAPHUberControls pack(const PGRAPHUberControlSource *source)
{
    PGRAPHUberControls packet;
    PGRAPHUberControlRejectReason reason;

    assert(pgraph_vk_pack_ubershader_controls(&packet, source, &reason));
    assert(reason == PGRAPH_UBER_CONTROL_REJECT_NONE);
    return packet;
}

static float ref_clamp(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static RefVec ref_register(const RefMachine *machine,
                           const PGRAPHUberControlSource *source,
                           unsigned int stage, uint8_t code,
                           const RefVec *ef)
{
    unsigned int reg = code & 0xf;
    unsigned int index;
    RefVec value = { 0 };

    switch (reg) {
    case PS_REGISTER_ZERO:
        break;
    case PS_REGISTER_C0:
        index = stage == 8 ||
                (source->combiner_control >> 8 & PS_COMBINERCOUNT_UNIQUE_C0) ?
                stage * 2 : 0;
        memcpy(value.lane, source->constants[index], sizeof(value.lane));
        break;
    case PS_REGISTER_C1:
        index = stage == 8 ||
                (source->combiner_control >> 8 & PS_COMBINERCOUNT_UNIQUE_C1) ?
                stage * 2 + 1 : 1;
        memcpy(value.lane, source->constants[index], sizeof(value.lane));
        break;
    case PS_REGISTER_FOG:
        value = machine->fog;
        break;
    case PS_REGISTER_V0:
    case PS_REGISTER_V1:
        value = machine->v[reg - PS_REGISTER_V0];
        break;
    case PS_REGISTER_T0:
    case PS_REGISTER_T1:
    case PS_REGISTER_T2:
    case PS_REGISTER_T3:
        value = machine->t[reg - PS_REGISTER_T0];
        break;
    case PS_REGISTER_R0:
    case PS_REGISTER_R1:
        value = machine->r[reg - PS_REGISTER_R0];
        break;
    case PS_REGISTER_V1R0_SUM:
        for (unsigned int i = 0; i < 3; i++) {
            float v1 = machine->v[1].lane[i];
            float r0 = machine->r[0].lane[i];
            uint8_t flags = source->final_inputs_1;
            if (flags & PS_FINALCOMBINERSETTING_COMPLEMENT_V1) {
                v1 = 1.0f - v1;
            }
            if (flags & PS_FINALCOMBINERSETTING_COMPLEMENT_R0) {
                r0 = 1.0f - r0;
            }
            value.lane[i] = v1 + r0;
            if (flags & PS_FINALCOMBINERSETTING_CLAMP_SUM) {
                value.lane[i] = ref_clamp(value.lane[i], 0.0f, 1.0f);
            }
        }
        break;
    case PS_REGISTER_EF_PROD:
        assert(ef);
        value = *ef;
        break;
    default:
        assert(!"invalid reference register");
    }
    return value;
}

static float ref_map(float value, uint8_t mapping)
{
    switch (mapping & 0xe0) {
    case PS_INPUTMAPPING_UNSIGNED_IDENTITY:
        return fmaxf(value, 0.0f);
    case PS_INPUTMAPPING_UNSIGNED_INVERT:
        return 1.0f - ref_clamp(value, 0.0f, 1.0f);
    case PS_INPUTMAPPING_EXPAND_NORMAL:
        return 2.0f * fmaxf(value, 0.0f) - 1.0f;
    case PS_INPUTMAPPING_EXPAND_NEGATE:
        return 1.0f - 2.0f * fmaxf(value, 0.0f);
    case PS_INPUTMAPPING_HALFBIAS_NORMAL:
        return fmaxf(value, 0.0f) - 0.5f;
    case PS_INPUTMAPPING_HALFBIAS_NEGATE:
        return 0.5f - fmaxf(value, 0.0f);
    case PS_INPUTMAPPING_SIGNED_IDENTITY:
        return value;
    case PS_INPUTMAPPING_SIGNED_NEGATE:
        return -value;
    default:
        assert(!"invalid input mapping");
        return 0.0f;
    }
}

static RefVec ref_input(const RefMachine *machine,
                        const PGRAPHUberControlSource *source,
                        unsigned int stage, uint8_t code, bool alpha,
                        const RefVec *ef)
{
    RefVec raw = ref_register(machine, source, stage, code, ef);
    RefVec result;

    if (alpha) {
        unsigned int lane = code & PS_CHANNEL_ALPHA ? 3 : 2;
        result.lane[0] = ref_map(raw.lane[lane], code);
        result.lane[1] = result.lane[2] = result.lane[3] = result.lane[0];
    } else {
        for (unsigned int i = 0; i < 3; i++) {
            unsigned int lane = code & PS_CHANNEL_ALPHA ? 3 : i;
            result.lane[i] = ref_map(raw.lane[lane], code);
        }
        result.lane[3] = 0.0f;
    }
    return result;
}

static float ref_output_map(float value, uint8_t mapping)
{
    switch (mapping & 0x38) {
    case PS_COMBINEROUTPUT_IDENTITY:
        return value;
    case PS_COMBINEROUTPUT_BIAS:
        return value - 0.5f;
    case PS_COMBINEROUTPUT_SHIFTLEFT_1:
        return value * 2.0f;
    case PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS:
        return (value - 0.5f) * 2.0f;
    case PS_COMBINEROUTPUT_SHIFTLEFT_2:
        return value * 4.0f;
    case PS_COMBINEROUTPUT_SHIFTRIGHT_1:
        return value * 0.5f;
    default:
        assert(!"invalid output mapping");
        return 0.0f;
    }
}

static RefVec *ref_writable(RefMachine *machine, uint8_t reg)
{
    switch (reg & 0xf) {
    case PS_REGISTER_DISCARD:
        return NULL;
    case PS_REGISTER_V0:
    case PS_REGISTER_V1:
        return &machine->v[(reg & 0xf) - PS_REGISTER_V0];
    case PS_REGISTER_T0:
    case PS_REGISTER_T1:
    case PS_REGISTER_T2:
    case PS_REGISTER_T3:
        return &machine->t[(reg & 0xf) - PS_REGISTER_T0];
    case PS_REGISTER_R0:
    case PS_REGISTER_R1:
        return &machine->r[(reg & 0xf) - PS_REGISTER_R0];
    default:
        assert(!"invalid reference destination");
        return NULL;
    }
}

typedef struct RefStageValues {
    float ab[4];
    float cd[4];
    float muxsum[4];
    uint8_t ab_reg;
    uint8_t cd_reg;
    uint8_t muxsum_reg;
    uint8_t flags;
    bool alpha;
} RefStageValues;

static RefStageValues ref_calculate_lane(
    const RefMachine *snapshot, const PGRAPHUberControlSource *source,
    unsigned int stage, uint32_t input_word_value,
    uint32_t output_word_value, bool alpha)
{
    RefStageValues values = { .alpha = alpha };
    RefVec input[4];
    unsigned int lanes = alpha ? 1 : 3;

    for (unsigned int i = 0; i < 4; i++) {
        uint8_t code = input_word_value >> (24 - i * 8);
        input[i] = ref_input(snapshot, source, stage, code, alpha, NULL);
    }
    values.cd_reg = output_word_value & 0xf;
    values.ab_reg = output_word_value >> 4 & 0xf;
    values.muxsum_reg = output_word_value >> 8 & 0xf;
    values.flags = output_word_value >> 12;

    for (unsigned int lane = 0; lane < lanes; lane++) {
        float ab;
        float cd;
        if (!alpha && (values.flags & PS_COMBINEROUTPUT_AB_DOT_PRODUCT)) {
            ab = input[0].lane[0] * input[1].lane[0] +
                 input[0].lane[1] * input[1].lane[1] +
                 input[0].lane[2] * input[1].lane[2];
        } else {
            ab = input[0].lane[lane] * input[1].lane[lane];
        }
        if (!alpha && (values.flags & PS_COMBINEROUTPUT_CD_DOT_PRODUCT)) {
            cd = input[2].lane[0] * input[3].lane[0] +
                 input[2].lane[1] * input[3].lane[1] +
                 input[2].lane[2] * input[3].lane[2];
        } else {
            cd = input[2].lane[lane] * input[3].lane[lane];
        }
        values.ab[lane] = ref_clamp(ref_output_map(ab, values.flags),
                                    -1.0f, 1.0f);
        values.cd[lane] = ref_clamp(ref_output_map(cd, values.flags),
                                    -1.0f, 1.0f);
        if (values.flags & PS_COMBINEROUTPUT_AB_CD_MUX) {
            values.muxsum[lane] = snapshot->r[0].lane[3] >= 0.5f ? cd : ab;
        } else {
            values.muxsum[lane] = ab + cd;
        }
        values.muxsum[lane] = ref_clamp(
            ref_output_map(values.muxsum[lane], values.flags), -1.0f, 1.0f);
    }
    return values;
}

static void ref_commit_lane(RefMachine *machine,
                            const RefStageValues *values)
{
    RefVec *destinations[3] = {
        ref_writable(machine, values->ab_reg),
        ref_writable(machine, values->cd_reg),
        ref_writable(machine, values->muxsum_reg),
    };
    const float *sources[3] = { values->ab, values->cd, values->muxsum };

    for (unsigned int output = 0; output < 3; output++) {
        if (!destinations[output]) {
            continue;
        }
        if (values->alpha) {
            destinations[output]->lane[3] = sources[output][0];
        } else {
            memcpy(destinations[output]->lane, sources[output],
                   3 * sizeof(float));
            if ((output == 0 &&
                 (values->flags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA)) ||
                (output == 1 &&
                 (values->flags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA))) {
                destinations[output]->lane[3] = sources[output][2];
            }
        }
    }
}

static PGRAPHUberCombinerResult reference_eval(
    const PGRAPHUberControlSource *source,
    const PGRAPHUberCombinerInputs *inputs)
{
    RefMachine machine = { 0 };
    PGRAPHUberCombinerResult result = { 0 };
    unsigned int stages = source->combiner_control & 0xff;

    memcpy(machine.fog.lane, inputs->fog, sizeof(machine.fog.lane));
    memcpy(machine.v, inputs->v, sizeof(machine.v));
    memcpy(machine.t, inputs->t, sizeof(machine.t));
    machine.r[0].lane[3] = inputs->initial_r0_alpha;

    for (unsigned int stage = 0; stage < stages; stage++) {
        RefMachine snapshot = machine;
        RefStageValues rgb = ref_calculate_lane(
            &snapshot, source, stage, source->rgb_inputs[stage],
            source->rgb_outputs[stage], false);
        RefStageValues alpha = ref_calculate_lane(
            &snapshot, source, stage, source->alpha_inputs[stage],
            source->alpha_outputs[stage], true);
        ref_commit_lane(&machine, &rgb);
        ref_commit_lane(&machine, &alpha);
    }

    RefVec ef;
    RefVec e = ref_input(&machine, source, 8,
                         source->final_inputs_1 >> 24, false, NULL);
    RefVec f = ref_input(&machine, source, 8,
                         source->final_inputs_1 >> 16, false, NULL);
    for (unsigned int i = 0; i < 3; i++) {
        ef.lane[i] = e.lane[i] * f.lane[i];
    }
    ef.lane[3] = 0.0f;

    RefVec final[5];
    for (unsigned int i = 0; i < 4; i++) {
        final[i] = ref_input(&machine, source, 8,
                             source->final_inputs_0 >> (24 - i * 8),
                             false, &ef);
    }
    final[4] = ref_input(&machine, source, 8,
                         source->final_inputs_1 >> 8, true, &ef);
    for (unsigned int i = 0; i < 3; i++) {
        result.color[i] = final[3].lane[i] + final[2].lane[i] *
                          (1.0f - final[0].lane[i]) +
                          final[1].lane[i] * final[0].lane[i];
    }
    result.color[3] = final[4].lane[0];
    memcpy(result.v, machine.v, sizeof(result.v));
    memcpy(result.t, machine.t, sizeof(result.t));
    memcpy(result.r, machine.r, sizeof(result.r));
    return result;
}

static void assert_results_equal(const PGRAPHUberCombinerResult *actual,
                                 const PGRAPHUberCombinerResult *expected)
{
    assert_vec(actual->color, expected->color);
    for (unsigned int i = 0; i < 2; i++) {
        assert_vec(actual->v[i], expected->v[i]);
        assert_vec(actual->r[i], expected->r[i]);
    }
    for (unsigned int i = 0; i < 4; i++) {
        assert_vec(actual->t[i], expected->t[i]);
    }
}

static void test_zero_stage_hand_calculated_final(void)
{
    PGRAPHUberControlSource source = base_source(0);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberControls packet;
    PGRAPHUberCombinerResult result;
    uint8_t v0 = input_code(PS_REGISTER_V0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t v1 = input_code(PS_REGISTER_V1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t half = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t g = input_code(PS_REGISTER_V1, PS_CHANNEL_ALPHA,
                           PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    const float expected[4] = { 0.25f, 0.375f, 0.5f, 0.5f };

    for (unsigned int i = 0; i < 4; i++) {
        source.constants[16][i] = 0.5f;
    }
    source.combiner_control &= ~(PS_COMBINERCOUNT_MUX_MSB << 8);
    source.final_inputs_0 = inputs_word(half, v1, v0, v0);
    source.final_inputs_1 = inputs_word(v0, v1, g, 0);
    packet = pack(&source);
    assert(pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert_vec(result.color, expected);
}

static void test_delayed_commit_alias_and_blue_alpha(void)
{
    PGRAPHUberControlSource source = base_source(1);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberControls packet;
    PGRAPHUberCombinerResult result;
    uint8_t v0 = input_code(PS_REGISTER_V0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t v1 = input_code(PS_REGISTER_V1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t r0a = input_code(PS_REGISTER_R0, PS_CHANNEL_ALPHA,
                             PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_BLUE,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    const float expected_r0[4] = { 0.375f, 0.5f, 0.625f, 0.75f };

    source.rgb_inputs[0] = inputs_word(v0, one, v1, one);
    source.rgb_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R0, PS_REGISTER_R0,
        PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA);
    source.alpha_inputs[0] = inputs_word(r0a, one, zero, zero);
    source.alpha_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
    packet = pack(&source);
    assert(pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert_vec(result.r[0], expected_r0);
}

static void test_final_virtual_registers_and_no_clamp(void)
{
    PGRAPHUberControlSource source = base_source(0);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberControls packet;
    PGRAPHUberCombinerResult result;
    uint8_t sum = input_code(PS_REGISTER_V1R0_SUM, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t ef = input_code(PS_REGISTER_EF_PROD, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t c0 = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t c1 = input_code(PS_REGISTER_C1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t g = input_code(PS_REGISTER_EF_PROD, PS_CHANNEL_ALPHA,
                           PS_INPUTMAPPING_UNSIGNED_INVERT);
    const float expected[4] = { 2.0f, 2.0f, 2.0f, 1.0f };

    inputs.v[1][0] = inputs.v[1][1] = inputs.v[1][2] = 0.75f;
    inputs.initial_r0_alpha = 0.25f;
    source.constants[16][0] = source.constants[16][1] =
        source.constants[16][2] = 0.5f;
    source.constants[17][0] = source.constants[17][1] =
        source.constants[17][2] = 0.5f;
    source.final_inputs_0 = inputs_word(c0, sum, ef, sum);
    source.final_inputs_1 = inputs_word(c0, c1, g,
        PS_FINALCOMBINERSETTING_COMPLEMENT_V1 |
        PS_FINALCOMBINERSETTING_COMPLEMENT_R0);
    packet = pack(&source);
    assert(pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert_vec(result.color, expected);
}

static void test_msb_mux_arms_and_threshold_goldens(void)
{
    static const struct {
        float r0_alpha;
        float expected[4];
    } cases[] = {
        { 0.25f, { 0.25f, 0.5f, 0.75f, 0.125f } },
        { 0.5f, { 0.75f, 0.25f, 0.5f, 0.125f } },
        { 0.75f, { 0.75f, 0.25f, 0.5f, 0.125f } },
    };
    uint8_t v0 = input_code(PS_REGISTER_V0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t v1 = input_code(PS_REGISTER_V1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHUberControlSource source = base_source(1);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult result;

        memcpy(inputs.v[0],
               (float[4]) { 0.25f, 0.5f, 0.75f, 0.125f },
               sizeof(inputs.v[0]));
        memcpy(inputs.v[1],
               (float[4]) { 0.75f, 0.25f, 0.5f, 0.875f },
               sizeof(inputs.v[1]));
        inputs.initial_r0_alpha = cases[i].r0_alpha;
        source.rgb_inputs[0] = inputs_word(v0, one, v1, one);
        source.rgb_outputs[0] = output_word(
            PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_T0,
            PS_COMBINEROUTPUT_AB_CD_MUX);

        PGRAPHUberControls packet = pack(&source);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &result));
        assert_vec(result.t[0], cases[i].expected);
    }
}

static void test_rgb_dot_broadcast_goldens(void)
{
    PGRAPHUberControlSource source = base_source(1);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberCombinerResult result;
    uint8_t v0 = input_code(PS_REGISTER_V0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t v1 = input_code(PS_REGISTER_V1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t t0 = input_code(PS_REGISTER_T0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t t1 = input_code(PS_REGISTER_T1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    const float expected_r0[4] = {
        -0.1875f, -0.1875f, -0.1875f, 0.75f,
    };
    const float expected_r1[4] = {
        0.0625f, 0.0625f, 0.0625f, 0.0f,
    };

    memcpy(inputs.v[0], (float[4]) { 0.25f, 0.5f, -0.25f, 0.0f },
           sizeof(inputs.v[0]));
    memcpy(inputs.v[1], (float[4]) { 0.5f, -0.5f, 0.25f, 0.0f },
           sizeof(inputs.v[1]));
    memcpy(inputs.t[0], (float[4]) { 0.5f, 0.25f, 0.75f, 0.0f },
           sizeof(inputs.t[0]));
    memcpy(inputs.t[1], (float[4]) { 0.25f, 0.5f, -0.25f, 0.0f },
           sizeof(inputs.t[1]));
    source.rgb_inputs[0] = inputs_word(v0, v1, t0, t1);
    source.rgb_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD,
        PS_COMBINEROUTPUT_AB_DOT_PRODUCT |
        PS_COMBINEROUTPUT_CD_DOT_PRODUCT);

    PGRAPHUberControls packet = pack(&source);
    assert(pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert_vec(result.r[0], expected_r0);
    assert_vec(result.r[1], expected_r1);
}

static void test_output_mapping_and_clamp_goldens(void)
{
    static const struct {
        uint8_t mapping;
        float input;
        float expected[4];
    } cases[] = {
        { PS_COMBINEROUTPUT_IDENTITY, 0.75f,
          { 0.75f, 0.75f, 0.75f, 0.75f } },
        { PS_COMBINEROUTPUT_BIAS, 0.75f,
          { 0.25f, 0.25f, 0.25f, 0.75f } },
        { PS_COMBINEROUTPUT_SHIFTLEFT_1, 0.75f,
          { 1.0f, 1.0f, 1.0f, 0.75f } },
        { PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS, 0.75f,
          { 0.5f, 0.5f, 0.5f, 0.75f } },
        { PS_COMBINEROUTPUT_SHIFTLEFT_2, 0.75f,
          { 1.0f, 1.0f, 1.0f, 0.75f } },
        { PS_COMBINEROUTPUT_SHIFTRIGHT_1, 0.75f,
          { 0.375f, 0.375f, 0.375f, 0.75f } },
        { PS_COMBINEROUTPUT_BIAS, -0.75f,
          { -1.0f, -1.0f, -1.0f, 0.75f } },
    };
    uint8_t v0 = input_code(PS_REGISTER_V0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHUberControlSource source = base_source(1);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult result;
        inputs.v[0][0] = inputs.v[0][1] = inputs.v[0][2] = cases[i].input;
        source.rgb_inputs[0] = inputs_word(v0, one, zero, zero);
        source.rgb_outputs[0] = output_word(
            PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD,
            cases[i].mapping);

        PGRAPHUberControls packet = pack(&source);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &result));
        assert_vec(result.r[0], cases[i].expected);
    }
}

static void test_same_unique_and_final_constant_goldens(void)
{
    uint8_t c0 = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t c1 = input_code(PS_REGISTER_C1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);

    for (unsigned int unique = 0; unique < 2; unique++) {
        PGRAPHUberControlSource source = base_source(2);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult result;
        const float expected_same_r0[4] = {
            0.125f, 0.125f, 0.125f, 0.75f,
        };
        const float expected_same_r1[4] = {
            0.25f, 0.25f, 0.25f, 0.0f,
        };
        const float expected_unique_r0[4] = {
            0.5f, 0.5f, 0.5f, 0.75f,
        };
        const float expected_unique_r1[4] = {
            0.75f, 0.75f, 0.75f, 0.0f,
        };
        const float expected_stage0_c0[4] = {
            0.125f, 0.125f, 0.125f, 0.125f,
        };
        const float expected_stage0_c1[4] = {
            0.25f, 0.25f, 0.25f, 0.25f,
        };

        if (unique) {
            source.combiner_control |=
                (PS_COMBINERCOUNT_UNIQUE_C0 |
                 PS_COMBINERCOUNT_UNIQUE_C1) << 8;
        }
        for (unsigned int lane = 0; lane < 4; lane++) {
            source.constants[0][lane] = 0.125f;
            source.constants[1][lane] = 0.25f;
            source.constants[2][lane] = 0.5f;
            source.constants[3][lane] = 0.75f;
        }
        source.rgb_inputs[0] = inputs_word(c0, one, c1, one);
        source.rgb_outputs[0] = output_word(
            PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_DISCARD, 0);
        source.rgb_inputs[1] = source.rgb_inputs[0];
        source.rgb_outputs[1] = output_word(
            PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, 0);

        PGRAPHUberControls packet = pack(&source);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &result));
        assert_vec(result.r[0], unique ? expected_unique_r0 :
                                        expected_same_r0);
        assert_vec(result.r[1], unique ? expected_unique_r1 :
                                        expected_same_r1);
        assert_vec(result.t[0], expected_stage0_c0);
        assert_vec(result.t[1], expected_stage0_c1);
    }

    for (unsigned int unique = 0; unique < 2; unique++) {
        PGRAPHUberControlSource source = base_source(0);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult result;
        uint8_t final_c0 = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                                     PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        uint8_t final_c1_alpha = input_code(
            PS_REGISTER_C1, PS_CHANNEL_ALPHA,
            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        const float expected[4] = { 0.625f, 0.625f, 0.625f, 0.875f };

        if (unique) {
            source.combiner_control |=
                (PS_COMBINERCOUNT_UNIQUE_C0 |
                 PS_COMBINERCOUNT_UNIQUE_C1) << 8;
        }
        source.constants[16][0] = source.constants[16][1] =
            source.constants[16][2] = source.constants[16][3] = 0.625f;
        source.constants[17][0] = source.constants[17][1] =
            source.constants[17][2] = source.constants[17][3] = 0.875f;
        source.final_inputs_0 = inputs_word(zero, zero, zero, final_c0);
        source.final_inputs_1 = inputs_word(final_c0, final_c0,
                                            final_c1_alpha, 0);

        PGRAPHUberControls packet = pack(&source);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &result));
        assert_vec(result.color, expected);
    }
}

static void test_v1r0_sum_flag_goldens(void)
{
    static const struct {
        uint8_t flags;
        float expected[4];
    } cases[] = {
        { 0, { 0.375f, 0.75f, 1.25f, 1.0f } },
        { PS_FINALCOMBINERSETTING_CLAMP_SUM,
          { 0.375f, 0.75f, 1.0f, 1.0f } },
        { PS_FINALCOMBINERSETTING_COMPLEMENT_V1,
          { 1.125f, 1.25f, 1.25f, 1.0f } },
        { PS_FINALCOMBINERSETTING_COMPLEMENT_V1 |
          PS_FINALCOMBINERSETTING_CLAMP_SUM,
          { 1.0f, 1.0f, 1.0f, 1.0f } },
        { PS_FINALCOMBINERSETTING_COMPLEMENT_R0,
          { 0.875f, 0.75f, 0.75f, 1.0f } },
        { PS_FINALCOMBINERSETTING_COMPLEMENT_R0 |
          PS_FINALCOMBINERSETTING_CLAMP_SUM,
          { 0.875f, 0.75f, 0.75f, 1.0f } },
        { PS_FINALCOMBINERSETTING_COMPLEMENT_V1 |
          PS_FINALCOMBINERSETTING_COMPLEMENT_R0,
          { 1.625f, 1.25f, 0.75f, 1.0f } },
        { PS_FINALCOMBINERSETTING_COMPLEMENT_V1 |
          PS_FINALCOMBINERSETTING_COMPLEMENT_R0 |
          PS_FINALCOMBINERSETTING_CLAMP_SUM,
          { 1.0f, 1.0f, 0.75f, 1.0f } },
    };
    uint8_t t0 = input_code(PS_REGISTER_T0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t sum = input_code(PS_REGISTER_V1R0_SUM, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_IDENTITY);

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHUberControlSource source = base_source(1);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult result;

        memcpy(inputs.t[0], (float[4]) { 0.25f, 0.5f, 0.75f, 0.0f },
               sizeof(inputs.t[0]));
        memcpy(inputs.v[1], (float[4]) { 0.125f, 0.25f, 0.5f, 0.0f },
               sizeof(inputs.v[1]));
        source.rgb_inputs[0] = inputs_word(t0, one, zero, zero);
        source.rgb_outputs[0] = output_word(
            PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
        source.final_inputs_0 = inputs_word(zero, zero, zero, sum);
        source.final_inputs_1 = inputs_word(zero, zero, one,
                                            cases[i].flags);

        PGRAPHUberControls packet = pack(&source);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &result));
        assert_vec(result.color, cases[i].expected);
    }
}

static void test_input_channel_goldens(void)
{
    static const struct {
        uint8_t rgb_channel;
        uint8_t alpha_channel;
        float expected_r0[4];
        float expected_r1[4];
    } cases[] = {
        { PS_CHANNEL_RGB, PS_CHANNEL_BLUE,
          { 0.125f, 0.25f, 0.5f, 0.75f },
          { 0.0f, 0.0f, 0.0f, 0.375f } },
        { PS_CHANNEL_ALPHA, PS_CHANNEL_ALPHA,
          { 0.875f, 0.875f, 0.875f, 0.75f },
          { 0.0f, 0.0f, 0.0f, 0.625f } },
    };
    uint8_t one_rgb = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                                 PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t one_alpha = input_code(PS_REGISTER_ZERO, PS_CHANNEL_ALPHA,
                                   PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHUberControlSource source = base_source(1);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult result;
        uint8_t v0 = input_code(PS_REGISTER_V0, cases[i].rgb_channel,
                                PS_INPUTMAPPING_SIGNED_IDENTITY);
        uint8_t v1 = input_code(PS_REGISTER_V1, cases[i].alpha_channel,
                                PS_INPUTMAPPING_SIGNED_IDENTITY);

        memcpy(inputs.v[0],
               (float[4]) { 0.125f, 0.25f, 0.5f, 0.875f },
               sizeof(inputs.v[0]));
        memcpy(inputs.v[1],
               (float[4]) { 0.125f, 0.25f, 0.375f, 0.625f },
               sizeof(inputs.v[1]));
        source.rgb_inputs[0] = inputs_word(v0, one_rgb, zero, zero);
        source.rgb_outputs[0] = output_word(
            PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
        source.alpha_inputs[0] = inputs_word(v1, one_alpha, zero, zero);
        source.alpha_outputs[0] = output_word(
            PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);

        PGRAPHUberControls packet = pack(&source);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &result));
        assert_vec(result.r[0], cases[i].expected_r0);
        assert_vec(result.r[1], cases[i].expected_r1);
    }
}

static void test_two_stage_delayed_propagation_golden(void)
{
    PGRAPHUberControlSource source = base_source(2);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberCombinerResult result;
    uint8_t v0 = input_code(PS_REGISTER_V0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t v1 = input_code(PS_REGISTER_V1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t v0a = input_code(PS_REGISTER_V0, PS_CHANNEL_ALPHA,
                             PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t v1a = input_code(PS_REGISTER_V1, PS_CHANNEL_ALPHA,
                             PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t r0 = input_code(PS_REGISTER_R0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t final_r0 = input_code(PS_REGISTER_R0, PS_CHANNEL_RGB,
                                  PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    uint8_t r1 = input_code(PS_REGISTER_R1, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t r0a = input_code(PS_REGISTER_R0, PS_CHANNEL_ALPHA,
                             PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t r1a = input_code(PS_REGISTER_R1, PS_CHANNEL_ALPHA,
                             PS_INPUTMAPPING_SIGNED_IDENTITY);
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t onea = input_code(PS_REGISTER_ZERO, PS_CHANNEL_ALPHA,
                              PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                              PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    const float expected_t0[4] = { 0.75f, 0.75f, 0.875f, 0.125f };
    const float expected_r0[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
    const float expected_r1[4] = { 0.5f, 0.25f, 0.125f, 0.875f };

    memcpy(inputs.v[0],
           (float[4]) { 0.25f, 0.5f, 0.75f, 0.125f },
           sizeof(inputs.v[0]));
    memcpy(inputs.v[1],
           (float[4]) { 0.5f, 0.25f, 0.125f, 0.875f },
           sizeof(inputs.v[1]));
    source.rgb_inputs[0] = inputs_word(v0, one, v1, one);
    source.rgb_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, 0);
    source.alpha_inputs[0] = inputs_word(v0a, onea, v1a, onea);
    source.alpha_outputs[0] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, 0);
    source.rgb_inputs[1] = inputs_word(r0, one, r1, one);
    source.rgb_outputs[1] = output_word(
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_T0, 0);
    source.alpha_inputs[1] = inputs_word(r0a, onea, r1a, onea);
    source.alpha_outputs[1] = output_word(
        PS_REGISTER_T0, PS_REGISTER_DISCARD, PS_REGISTER_V0, 0);
    source.final_inputs_0 = inputs_word(zero, zero, zero, final_r0);

    PGRAPHUberControls packet = pack(&source);
    assert(pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert_vec(result.t[0], expected_t0);
    assert_vec(result.r[0], expected_r0);
    assert_vec(result.r[1], expected_r1);
    assert_close(result.v[0][3], 1.0f);
}

static void run_differential_matrix(void)
{
    static const uint8_t mappings[] = {
        PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_INPUTMAPPING_UNSIGNED_INVERT,
        PS_INPUTMAPPING_EXPAND_NORMAL,
        PS_INPUTMAPPING_EXPAND_NEGATE,
        PS_INPUTMAPPING_HALFBIAS_NORMAL,
        PS_INPUTMAPPING_HALFBIAS_NEGATE,
        PS_INPUTMAPPING_SIGNED_IDENTITY,
        PS_INPUTMAPPING_SIGNED_NEGATE,
    };
    static const uint8_t output_mappings[] = {
        PS_COMBINEROUTPUT_IDENTITY,
        PS_COMBINEROUTPUT_BIAS,
        PS_COMBINEROUTPUT_SHIFTLEFT_1,
        PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS,
        PS_COMBINEROUTPUT_SHIFTLEFT_2,
        PS_COMBINEROUTPUT_SHIFTRIGHT_1,
    };

    for (unsigned int seed = 0; seed < 96; seed++) {
        unsigned int stages = seed % 9;
        PGRAPHUberControlSource source = base_source(stages);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult actual;
        PGRAPHUberCombinerResult expected;
        uint8_t mapping = mappings[seed % 8];
        uint8_t out_mapping = output_mappings[seed % 6];

        inputs.initial_r0_alpha = seed & 1 ? 0.25f : 0.75f;

        if (seed & 1) {
            source.combiner_control |= PS_COMBINERCOUNT_UNIQUE_C0 << 8;
        }
        if (seed & 2) {
            source.combiner_control |= PS_COMBINERCOUNT_UNIQUE_C1 << 8;
        }
        for (unsigned int i = 0; i < PGRAPH_UBER_CONSTANT_COUNT; i++) {
            for (unsigned int lane = 0; lane < 4; lane++) {
                source.constants[i][lane] =
                    ((int)((i * 5 + lane * 3 + seed) % 17) - 5) / 8.0f;
            }
        }
        for (unsigned int stage = 0; stage < stages; stage++) {
            uint8_t a = input_code(PS_REGISTER_V0 + (stage & 1),
                                   seed & 4 ? PS_CHANNEL_ALPHA :
                                              PS_CHANNEL_RGB,
                                   mapping);
            uint8_t b = input_code(PS_REGISTER_C0 + (stage & 1),
                                   PS_CHANNEL_RGB, mappings[(seed + 1) % 8]);
            uint8_t c = input_code(PS_REGISTER_T0 + stage % 4,
                                   PS_CHANNEL_RGB, mappings[(seed + 2) % 8]);
            uint8_t d = input_code(PS_REGISTER_R0 + (stage & 1),
                                   PS_CHANNEL_RGB, mappings[(seed + 3) % 8]);
            uint8_t flags = out_mapping;
            if (seed & 8) {
                flags |= PS_COMBINEROUTPUT_AB_DOT_PRODUCT;
            }
            if (seed & 16) {
                flags |= PS_COMBINEROUTPUT_CD_DOT_PRODUCT;
            }
            if (seed & 32) {
                flags |= PS_COMBINEROUTPUT_AB_CD_MUX;
            }
            if (seed & 64) {
                flags |= PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA |
                         PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA;
            }
            source.rgb_inputs[stage] = inputs_word(a, b, c, d);
            source.rgb_outputs[stage] = output_word(
                PS_REGISTER_R0 + (stage & 1),
                PS_REGISTER_T0 + stage % 4,
                PS_REGISTER_V0 + (stage & 1), flags);
            source.alpha_inputs[stage] = inputs_word(
                a | PS_CHANNEL_ALPHA, b | PS_CHANNEL_ALPHA,
                c | PS_CHANNEL_ALPHA, d | PS_CHANNEL_ALPHA);
            source.alpha_outputs[stage] = output_word(
                PS_REGISTER_R1 - (stage & 1), PS_REGISTER_T0 + stage % 4,
                PS_REGISTER_V1 - (stage & 1), out_mapping |
                    (seed & 32 ? PS_COMBINEROUTPUT_AB_CD_MUX : 0));
        }

        uint8_t c0 = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                                PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        uint8_t c1 = input_code(PS_REGISTER_C1, PS_CHANNEL_RGB,
                                PS_INPUTMAPPING_UNSIGNED_INVERT);
        uint8_t sum = input_code(PS_REGISTER_V1R0_SUM, PS_CHANNEL_RGB,
                                 seed & 1 ? PS_INPUTMAPPING_UNSIGNED_INVERT :
                                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        uint8_t ef = input_code(PS_REGISTER_EF_PROD, PS_CHANNEL_RGB,
                                PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        uint8_t r0a = input_code(PS_REGISTER_R0, PS_CHANNEL_ALPHA,
                                 PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        source.final_inputs_0 = inputs_word(c0, c1, sum, ef);
        source.final_inputs_1 = inputs_word(c0, c1, r0a,
            (seed & 1 ? PS_FINALCOMBINERSETTING_CLAMP_SUM : 0) |
            (seed & 2 ? PS_FINALCOMBINERSETTING_COMPLEMENT_V1 : 0) |
            (seed & 4 ? PS_FINALCOMBINERSETTING_COMPLEMENT_R0 : 0));

        PGRAPHUberControls packet = pack(&source);
        expected = reference_eval(&source, &inputs);
        assert(pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &actual));
        assert_results_equal(&actual, &expected);
    }
}

static void test_all_admitted_registers_and_destinations(void)
{
    static const uint8_t readable[] = {
        PS_REGISTER_ZERO, PS_REGISTER_C0, PS_REGISTER_C1,
        PS_REGISTER_FOG, PS_REGISTER_V0, PS_REGISTER_V1,
        PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_T2, PS_REGISTER_T3,
        PS_REGISTER_R0, PS_REGISTER_R1,
    };
    static const uint8_t writable[] = {
        PS_REGISTER_DISCARD, PS_REGISTER_V0, PS_REGISTER_V1,
        PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_T2, PS_REGISTER_T3,
        PS_REGISTER_R0, PS_REGISTER_R1,
    };

    for (unsigned int read = 0; read < sizeof(readable); read++) {
        for (unsigned int write = 0; write < sizeof(writable); write++) {
            PGRAPHUberControlSource source = base_source(1);
            PGRAPHUberCombinerInputs inputs = base_inputs();
            PGRAPHUberCombinerResult actual;
            PGRAPHUberCombinerResult expected;
            uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                                     PS_INPUTMAPPING_UNSIGNED_INVERT);
            uint8_t zero = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                                      PS_INPUTMAPPING_UNSIGNED_IDENTITY);
            uint8_t input = input_code(readable[read], PS_CHANNEL_RGB,
                                      PS_INPUTMAPPING_SIGNED_IDENTITY);

            for (unsigned int i = 0; i < PGRAPH_UBER_CONSTANT_COUNT; i++) {
                for (unsigned int lane = 0; lane < 4; lane++) {
                    source.constants[i][lane] =
                        (float)(i + lane + 1) / 32.0f;
                }
            }
            source.rgb_inputs[0] = inputs_word(input, one, zero, zero);
            source.rgb_outputs[0] = output_word(
                writable[write], PS_REGISTER_DISCARD,
                PS_REGISTER_DISCARD, 0);
            source.alpha_inputs[0] = inputs_word(
                input | PS_CHANNEL_ALPHA, one | PS_CHANNEL_ALPHA,
                zero, zero);
            source.alpha_outputs[0] = output_word(
                writable[write], PS_REGISTER_DISCARD,
                PS_REGISTER_DISCARD, 0);

            PGRAPHUberControls packet = pack(&source);
            expected = reference_eval(&source, &inputs);
            assert(pgraph_vk_eval_ubershader_combiner(
                &packet, &inputs, &actual));
            assert_results_equal(&actual, &expected);
        }
    }
}

static void test_all_admitted_final_registers(void)
{
    static const uint8_t readable[] = {
        PS_REGISTER_ZERO, PS_REGISTER_C0, PS_REGISTER_C1,
        PS_REGISTER_FOG, PS_REGISTER_V0, PS_REGISTER_V1,
        PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_T2, PS_REGISTER_T3,
        PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_V1R0_SUM,
        PS_REGISTER_EF_PROD,
    };

    for (unsigned int i = 0; i < sizeof(readable); i++) {
        PGRAPHUberControlSource source = base_source(0);
        PGRAPHUberCombinerInputs inputs = base_inputs();
        PGRAPHUberCombinerResult actual;
        PGRAPHUberCombinerResult expected;
        uint8_t mapping = i & 1 ? PS_INPUTMAPPING_UNSIGNED_INVERT :
                                  PS_INPUTMAPPING_UNSIGNED_IDENTITY;
        uint8_t selected = input_code(readable[i], PS_CHANNEL_RGB, mapping);
        uint8_t c0 = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                                PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        uint8_t c1 = input_code(PS_REGISTER_C1, PS_CHANNEL_RGB,
                                PS_INPUTMAPPING_UNSIGNED_IDENTITY);
        uint8_t g = input_code(readable[i], PS_CHANNEL_ALPHA, mapping);
        uint8_t e = readable[i] == PS_REGISTER_EF_PROD ? c0 : selected;
        uint8_t f = readable[i] == PS_REGISTER_EF_PROD ? c1 : selected;

        for (unsigned int constant = 0;
             constant < PGRAPH_UBER_CONSTANT_COUNT; constant++) {
            for (unsigned int lane = 0; lane < 4; lane++) {
                source.constants[constant][lane] =
                    (float)(constant + lane + 1) / 64.0f;
            }
        }
        source.final_inputs_0 = inputs_word(selected, c1, c0, selected);
        source.final_inputs_1 = inputs_word(e, f, g,
            i & 2 ? PS_FINALCOMBINERSETTING_CLAMP_SUM : 0);

        PGRAPHUberControls packet = pack(&source);
        expected = reference_eval(&source, &inputs);
        assert(pgraph_vk_eval_ubershader_combiner(
            &packet, &inputs, &actual));
        assert_results_equal(&actual, &expected);
    }
}

static void test_rejects_invalid_packet_without_partial_result(void)
{
    PGRAPHUberControlSource source = base_source(0);
    PGRAPHUberControlSource mux_source;
    PGRAPHUberControls packet = pack(&source);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberCombinerResult result;
    PGRAPHUberCombinerResult sentinel;

    memset(&sentinel, 0xa5, sizeof(sentinel));
    result = sentinel;
    packet.header[0]++;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    packet = pack(&source);
    packet.header[1] = 9;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    mux_source = base_source(1);
    mux_source.alpha_outputs[0] =
        output_word(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD,
                    PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_AB_CD_MUX);
    packet = pack(&mux_source);
    packet.header[2] &= ~(PS_COMBINERCOUNT_MUX_MSB << 8);
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    packet = pack(&source);
    packet.stage[0][0] = UINT32_MAX;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    packet = pack(&source);
    packet.final_words[1] |= 1;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    assert(!pgraph_vk_eval_ubershader_combiner(NULL, &inputs, &result));
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, NULL, &result));
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, NULL));
}

static void test_finite_domain_rejection_preserves_result(void)
{
    PGRAPHUberControlSource source = base_source(0);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberControls packet = pack(&source);
    PGRAPHUberCombinerResult result;
    PGRAPHUberCombinerResult sentinel;

    memset(&sentinel, 0x5a, sizeof(sentinel));
    result = sentinel;
    packet.constants[0][0] = INFINITY;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    packet = pack(&source);
    packet.constants[17][3] = NAN;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    packet = pack(&source);
    inputs.v[0][0] = -INFINITY;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));

    inputs = base_inputs();
    inputs.initial_r0_alpha = NAN;
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));
}

static void test_finite_overflow_rejection_preserves_result(void)
{
    PGRAPHUberControlSource source = base_source(0);
    PGRAPHUberCombinerInputs inputs = base_inputs();
    PGRAPHUberCombinerResult result;
    PGRAPHUberCombinerResult sentinel;
    uint8_t one = input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                             PS_INPUTMAPPING_UNSIGNED_INVERT);
    uint8_t c0 = input_code(PS_REGISTER_C0, PS_CHANNEL_RGB,
                            PS_INPUTMAPPING_UNSIGNED_IDENTITY);

    memset(&sentinel, 0x3c, sizeof(sentinel));
    result = sentinel;
    for (unsigned int lane = 0; lane < 4; lane++) {
        source.constants[16][lane] = FLT_MAX;
    }
    source.final_inputs_0 = inputs_word(one, c0, one, c0);
    source.final_inputs_1 = inputs_word(
        input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                   PS_INPUTMAPPING_UNSIGNED_IDENTITY),
        input_code(PS_REGISTER_ZERO, PS_CHANNEL_RGB,
                   PS_INPUTMAPPING_UNSIGNED_IDENTITY), one, 0);

    PGRAPHUberControls packet = pack(&source);
    assert(!pgraph_vk_eval_ubershader_combiner(&packet, &inputs, &result));
    assert(!memcmp(&result, &sentinel, sizeof(result)));
}

int main(void)
{
    test_zero_stage_hand_calculated_final();
    test_delayed_commit_alias_and_blue_alpha();
    test_final_virtual_registers_and_no_clamp();
    test_msb_mux_arms_and_threshold_goldens();
    test_rgb_dot_broadcast_goldens();
    test_output_mapping_and_clamp_goldens();
    test_same_unique_and_final_constant_goldens();
    test_v1r0_sum_flag_goldens();
    test_input_channel_goldens();
    test_two_stage_delayed_propagation_golden();
    run_differential_matrix();
    test_all_admitted_registers_and_destinations();
    test_all_admitted_final_registers();
    test_rejects_invalid_packet_without_partial_result();
    test_finite_domain_rejection_preserves_result();
    test_finite_overflow_rejection_preserves_result();
    puts("ubershader combiner oracle tests: PASS");
    return 0;
}

/*
 * Geforce NV2A PGRAPH Vulkan ubershader combiner oracle
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/ubershader-combiner-oracle.h"

#include <math.h>
#include <string.h>

#include "hw/xbox/nv2a/pgraph/psh_regs.h"

typedef struct Vec4 {
    float lane[4];
} Vec4;

typedef struct RegisterFile {
    Vec4 fog;
    Vec4 v[2];
    Vec4 t[4];
    Vec4 r[2];
} RegisterFile;

typedef struct StageResult {
    float ab[3];
    float cd[3];
    float muxsum[3];
    uint8_t ab_register;
    uint8_t cd_register;
    uint8_t muxsum_register;
    uint8_t flags;
    bool alpha;
} StageResult;

static bool is_canonical_packet(const PGRAPHUberControls *packet)
{
    PGRAPHUberControlSource source = { 0 };
    PGRAPHUberControls canonical;

    source.combiner_control = packet->header[2];
    for (unsigned int i = 0; i < PGRAPH_UBER_STAGE_COUNT; i++) {
        source.rgb_inputs[i] = packet->stage[i][0];
        source.alpha_inputs[i] = packet->stage[i][1];
        source.rgb_outputs[i] = packet->stage[i][2];
        source.alpha_outputs[i] = packet->stage[i][3];
    }
    source.final_inputs_0 = packet->final_words[0];
    source.final_inputs_1 = packet->final_words[1];
    memcpy(source.constants, packet->constants, sizeof(source.constants));

    return pgraph_vk_pack_ubershader_controls(&canonical, &source, NULL) &&
           !memcmp(&canonical, packet, sizeof(canonical));
}

static bool finite_packet_constants(const PGRAPHUberControls *packet)
{
    for (unsigned int constant = 0;
         constant < PGRAPH_UBER_CONSTANT_COUNT; constant++) {
        for (unsigned int lane = 0; lane < 4; lane++) {
            if (!isfinite(packet->constants[constant][lane])) {
                return false;
            }
        }
    }
    return true;
}

static bool finite_shell_inputs(const PGRAPHUberCombinerInputs *inputs)
{
    for (unsigned int lane = 0; lane < 4; lane++) {
        if (!isfinite(inputs->fog[lane])) {
            return false;
        }
        for (unsigned int i = 0; i < 2; i++) {
            if (!isfinite(inputs->v[i][lane])) {
                return false;
            }
        }
        for (unsigned int i = 0; i < 4; i++) {
            if (!isfinite(inputs->t[i][lane])) {
                return false;
            }
        }
    }
    return isfinite(inputs->initial_r0_alpha);
}

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static float map_input(float value, uint8_t input)
{
    switch (input & 0xe0) {
    case PS_INPUTMAPPING_UNSIGNED_IDENTITY:
        return fmaxf(value, 0.0f);
    case PS_INPUTMAPPING_UNSIGNED_INVERT:
        return 1.0f - clampf(value, 0.0f, 1.0f);
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
        return 0.0f;
    }
}

static float map_output(float value, uint8_t flags)
{
    switch (flags & 0x38) {
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
        return 0.0f;
    }
}

static bool read_register(const PGRAPHUberControls *packet,
                          const RegisterFile *registers,
                          unsigned int stage, uint8_t input,
                          const Vec4 *ef_product, Vec4 *value)
{
    unsigned int index;
    uint8_t reg = input & 0xf;

    memset(value, 0, sizeof(*value));
    switch (reg) {
    case PS_REGISTER_ZERO:
        return true;
    case PS_REGISTER_C0:
        index = stage == PGRAPH_UBER_STAGE_COUNT ||
                (packet->header[2] >> 8 & PS_COMBINERCOUNT_UNIQUE_C0) ?
                stage * 2 : 0;
        memcpy(value->lane, packet->constants[index], sizeof(value->lane));
        return true;
    case PS_REGISTER_C1:
        index = stage == PGRAPH_UBER_STAGE_COUNT ||
                (packet->header[2] >> 8 & PS_COMBINERCOUNT_UNIQUE_C1) ?
                stage * 2 + 1 : 1;
        memcpy(value->lane, packet->constants[index], sizeof(value->lane));
        return true;
    case PS_REGISTER_FOG:
        *value = registers->fog;
        return true;
    case PS_REGISTER_V0:
    case PS_REGISTER_V1:
        *value = registers->v[reg - PS_REGISTER_V0];
        return true;
    case PS_REGISTER_T0:
    case PS_REGISTER_T1:
    case PS_REGISTER_T2:
    case PS_REGISTER_T3:
        *value = registers->t[reg - PS_REGISTER_T0];
        return true;
    case PS_REGISTER_R0:
    case PS_REGISTER_R1:
        *value = registers->r[reg - PS_REGISTER_R0];
        return true;
    case PS_REGISTER_V1R0_SUM:
        if (stage != PGRAPH_UBER_STAGE_COUNT) {
            return false;
        }
        for (unsigned int lane = 0; lane < 3; lane++) {
            float v1 = registers->v[1].lane[lane];
            float r0 = registers->r[0].lane[lane];
            uint8_t flags = packet->final_words[1];

            if (flags & PS_FINALCOMBINERSETTING_COMPLEMENT_V1) {
                v1 = 1.0f - v1;
            }
            if (flags & PS_FINALCOMBINERSETTING_COMPLEMENT_R0) {
                r0 = 1.0f - r0;
            }
            value->lane[lane] = v1 + r0;
            if (!isfinite(value->lane[lane])) {
                return false;
            }
            if (flags & PS_FINALCOMBINERSETTING_CLAMP_SUM) {
                value->lane[lane] = clampf(value->lane[lane], 0.0f, 1.0f);
            }
        }
        return true;
    case PS_REGISTER_EF_PROD:
        if (stage != PGRAPH_UBER_STAGE_COUNT || !ef_product) {
            return false;
        }
        *value = *ef_product;
        return true;
    default:
        return false;
    }
}

static bool read_input(const PGRAPHUberControls *packet,
                       const RegisterFile *registers,
                       unsigned int stage, uint8_t input, bool alpha,
                       const Vec4 *ef_product, Vec4 *value)
{
    Vec4 raw;

    if (!read_register(packet, registers, stage, input, ef_product, &raw)) {
        return false;
    }
    if (alpha) {
        unsigned int source_lane = input & PS_CHANNEL_ALPHA ? 3 : 2;
        value->lane[0] = map_input(raw.lane[source_lane], input);
        value->lane[1] = value->lane[2] = value->lane[3] = value->lane[0];
    } else {
        for (unsigned int lane = 0; lane < 3; lane++) {
            unsigned int source_lane = input & PS_CHANNEL_ALPHA ? 3 : lane;
            value->lane[lane] = map_input(raw.lane[source_lane], input);
        }
        value->lane[3] = 0.0f;
    }
    for (unsigned int lane = 0; lane < 4; lane++) {
        if (!isfinite(value->lane[lane])) {
            return false;
        }
    }
    return true;
}

static bool calculate_stage_result(const PGRAPHUberControls *packet,
                                   const RegisterFile *snapshot,
                                   unsigned int stage, uint32_t input_word,
                                   uint32_t output_word, bool alpha,
                                   StageResult *result)
{
    Vec4 input[4];
    unsigned int lanes = alpha ? 1 : 3;

    memset(result, 0, sizeof(*result));
    result->alpha = alpha;
    result->cd_register = output_word & 0xf;
    result->ab_register = output_word >> 4 & 0xf;
    result->muxsum_register = output_word >> 8 & 0xf;
    result->flags = output_word >> 12;

    for (unsigned int i = 0; i < 4; i++) {
        if (!read_input(packet, snapshot, stage,
                        input_word >> (24 - i * 8), alpha, NULL, &input[i])) {
            return false;
        }
    }

    float dot_ab = 0.0f;
    float dot_cd = 0.0f;

    if (!alpha && (result->flags & PS_COMBINEROUTPUT_AB_DOT_PRODUCT)) {
        dot_ab = input[0].lane[0] * input[1].lane[0] +
                 input[0].lane[1] * input[1].lane[1] +
                 input[0].lane[2] * input[1].lane[2];
        if (!isfinite(dot_ab)) {
            return false;
        }
    }
    if (!alpha && (result->flags & PS_COMBINEROUTPUT_CD_DOT_PRODUCT)) {
        dot_cd = input[2].lane[0] * input[3].lane[0] +
                 input[2].lane[1] * input[3].lane[1] +
                 input[2].lane[2] * input[3].lane[2];
        if (!isfinite(dot_cd)) {
            return false;
        }
    }

    for (unsigned int lane = 0; lane < lanes; lane++) {
        float ab = !alpha &&
                   (result->flags & PS_COMBINEROUTPUT_AB_DOT_PRODUCT) ?
                   dot_ab : input[0].lane[lane] * input[1].lane[lane];
        float cd = !alpha &&
                   (result->flags & PS_COMBINEROUTPUT_CD_DOT_PRODUCT) ?
                   dot_cd : input[2].lane[lane] * input[3].lane[lane];
        float muxsum = result->flags & PS_COMBINEROUTPUT_AB_CD_MUX ?
                       (snapshot->r[0].lane[3] >= 0.5f ? cd : ab) :
                       ab + cd;

        if (!isfinite(ab) || !isfinite(cd) || !isfinite(muxsum)) {
            return false;
        }

        float mapped_ab = map_output(ab, result->flags);
        float mapped_cd = map_output(cd, result->flags);
        float mapped_muxsum = map_output(muxsum, result->flags);

        if (!isfinite(mapped_ab) || !isfinite(mapped_cd) ||
            !isfinite(mapped_muxsum)) {
            return false;
        }
        result->ab[lane] = clampf(mapped_ab, -1.0f, 1.0f);
        result->cd[lane] = clampf(mapped_cd, -1.0f, 1.0f);
        result->muxsum[lane] = clampf(mapped_muxsum, -1.0f, 1.0f);
    }
    return true;
}

static Vec4 *writable_register(RegisterFile *registers, uint8_t reg)
{
    switch (reg & 0xf) {
    case PS_REGISTER_DISCARD:
        return NULL;
    case PS_REGISTER_V0:
    case PS_REGISTER_V1:
        return &registers->v[(reg & 0xf) - PS_REGISTER_V0];
    case PS_REGISTER_T0:
    case PS_REGISTER_T1:
    case PS_REGISTER_T2:
    case PS_REGISTER_T3:
        return &registers->t[(reg & 0xf) - PS_REGISTER_T0];
    case PS_REGISTER_R0:
    case PS_REGISTER_R1:
        return &registers->r[(reg & 0xf) - PS_REGISTER_R0];
    default:
        return NULL;
    }
}

static void commit_stage_result(RegisterFile *registers,
                                const StageResult *result)
{
    Vec4 *destinations[3] = {
        writable_register(registers, result->ab_register),
        writable_register(registers, result->cd_register),
        writable_register(registers, result->muxsum_register),
    };
    const float *sources[3] = { result->ab, result->cd, result->muxsum };

    for (unsigned int output = 0; output < 3; output++) {
        if (!destinations[output]) {
            continue;
        }
        if (result->alpha) {
            destinations[output]->lane[3] = sources[output][0];
        } else {
            memcpy(destinations[output]->lane, sources[output],
                   3 * sizeof(float));
            if ((output == 0 &&
                 (result->flags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA)) ||
                (output == 1 &&
                 (result->flags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA))) {
                destinations[output]->lane[3] = sources[output][2];
            }
        }
    }
}

static bool evaluate_final(const PGRAPHUberControls *packet,
                           const RegisterFile *registers, float color[4])
{
    Vec4 e;
    Vec4 f;
    Vec4 ef = { 0 };
    Vec4 final[5];

    if (!read_input(packet, registers, PGRAPH_UBER_STAGE_COUNT,
                    packet->final_words[1] >> 24, false, NULL, &e) ||
        !read_input(packet, registers, PGRAPH_UBER_STAGE_COUNT,
                    packet->final_words[1] >> 16, false, NULL, &f)) {
        return false;
    }
    for (unsigned int lane = 0; lane < 3; lane++) {
        ef.lane[lane] = e.lane[lane] * f.lane[lane];
        if (!isfinite(ef.lane[lane])) {
            return false;
        }
    }

    for (unsigned int i = 0; i < 4; i++) {
        if (!read_input(packet, registers, PGRAPH_UBER_STAGE_COUNT,
                        packet->final_words[0] >> (24 - i * 8), false,
                        &ef, &final[i])) {
            return false;
        }
    }
    if (!read_input(packet, registers, PGRAPH_UBER_STAGE_COUNT,
                    packet->final_words[1] >> 8, true, &ef, &final[4])) {
        return false;
    }

    for (unsigned int lane = 0; lane < 3; lane++) {
        float c_term = final[2].lane[lane] *
                       (1.0f - final[0].lane[lane]);
        float b_term = final[1].lane[lane] * final[0].lane[lane];

        if (!isfinite(c_term) || !isfinite(b_term)) {
            return false;
        }
        color[lane] = final[3].lane[lane] + c_term + b_term;
        if (!isfinite(color[lane])) {
            return false;
        }
    }
    color[3] = final[4].lane[0];
    return isfinite(color[3]);
}

bool pgraph_vk_eval_ubershader_combiner(
    const PGRAPHUberControls *packet,
    const PGRAPHUberCombinerInputs *inputs,
    PGRAPHUberCombinerResult *result)
{
    RegisterFile registers = { 0 };
    PGRAPHUberCombinerResult candidate = { 0 };
    unsigned int stages;

    if (!packet || !inputs || !result || !is_canonical_packet(packet) ||
        !finite_packet_constants(packet) || !finite_shell_inputs(inputs)) {
        return false;
    }
    stages = packet->header[1];
    if (stages > PGRAPH_UBER_STAGE_COUNT ||
        stages != (packet->header[2] & 0xff)) {
        return false;
    }

    memcpy(registers.fog.lane, inputs->fog, sizeof(registers.fog.lane));
    memcpy(registers.v, inputs->v, sizeof(registers.v));
    memcpy(registers.t, inputs->t, sizeof(registers.t));
    registers.r[0].lane[3] = inputs->initial_r0_alpha;

    for (unsigned int stage = 0; stage < stages; stage++) {
        RegisterFile snapshot = registers;
        StageResult rgb;
        StageResult alpha;

        if (!calculate_stage_result(packet, &snapshot, stage,
                                    packet->stage[stage][0],
                                    packet->stage[stage][2], false, &rgb) ||
            !calculate_stage_result(packet, &snapshot, stage,
                                    packet->stage[stage][1],
                                    packet->stage[stage][3], true, &alpha)) {
            return false;
        }
        commit_stage_result(&registers, &rgb);
        commit_stage_result(&registers, &alpha);
    }

    if (!evaluate_final(packet, &registers, candidate.color)) {
        return false;
    }
    memcpy(candidate.v, registers.v, sizeof(candidate.v));
    memcpy(candidate.t, registers.t, sizeof(candidate.t));
    memcpy(candidate.r, registers.r, sizeof(candidate.r));
    *result = candidate;
    return true;
}

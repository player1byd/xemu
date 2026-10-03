/*
 * Geforce NV2A PGRAPH Vulkan hybrid family key codec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/hybrid-family-codec.h"

#include <stdlib.h>
#include <string.h>

#define FAMILY_KEY_MAGIC 0x4b465358U /* XSFK */
#define FAMILY_KEY_HEADER_SIZE 24U

enum {
    HEADER_MAGIC = 0,
    HEADER_ABI = 4,
    HEADER_SIZE = 8,
    HEADER_RESERVED = 12,
    HEADER_PAYLOAD_HASH = 16,
};

typedef struct FamilyKeyCodec {
    uint8_t *data;
    size_t size;
    size_t offset;
    bool reading;
} FamilyKeyCodec;

static uint32_t load_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | (uint32_t)src[1] << 8 |
           (uint32_t)src[2] << 16 | (uint32_t)src[3] << 24;
}

static uint64_t load_u64_le(const uint8_t *src)
{
    return (uint64_t)load_u32_le(src) |
           (uint64_t)load_u32_le(src + 4) << 32;
}

static void store_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = value;
    dst[1] = value >> 8;
    dst[2] = value >> 16;
    dst[3] = value >> 24;
}

static void store_u64_le(uint8_t *dst, uint64_t value)
{
    store_u32_le(dst, value);
    store_u32_le(dst + 4, value >> 32);
}

static uint64_t family_key_hash(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool codec_u32(FamilyKeyCodec *codec, uint32_t *value)
{
    if (codec->size - codec->offset < sizeof(*value)) {
        return false;
    }
    if (codec->reading) {
        *value = load_u32_le(codec->data + codec->offset);
    } else {
        store_u32_le(codec->data + codec->offset, *value);
    }
    codec->offset += sizeof(*value);
    return true;
}

#define FIELD_U32(field)                                                   \
    do {                                                                   \
        uint32_t value_ = codec->reading ? 0 : (uint32_t)(field);          \
        if (!codec_u32(codec, &value_)) {                                  \
            return false;                                                  \
        }                                                                  \
        if (codec->reading) {                                              \
            (field) = value_;                                              \
        }                                                                  \
    } while (0)

#define FIELD_U16(field)                                                   \
    do {                                                                   \
        uint32_t value_ = codec->reading ? 0 : (field);                    \
        if (!codec_u32(codec, &value_) || value_ > UINT16_MAX) {          \
            return false;                                                  \
        }                                                                  \
        if (codec->reading) {                                              \
            (field) = value_;                                              \
        }                                                                  \
    } while (0)

#define FIELD_I32(field)                                                   \
    do {                                                                   \
        uint32_t value_ = codec->reading ? 0 : (uint32_t)(int32_t)(field); \
        if (!codec_u32(codec, &value_)) {                                  \
            return false;                                                  \
        }                                                                  \
        if (codec->reading) {                                              \
            (field) = (int32_t)value_;                                     \
        }                                                                  \
    } while (0)

#define FIELD_BOOL(field)                                                  \
    do {                                                                   \
        uint32_t value_ = codec->reading ? 0 : !!(field);                  \
        if (!codec_u32(codec, &value_) || value_ > 1) {                    \
            return false;                                                  \
        }                                                                  \
        if (codec->reading) {                                              \
            (field) = value_;                                              \
        }                                                                  \
    } while (0)

#define FIELD_FLOAT(field)                                                 \
    do {                                                                   \
        uint32_t value_ = 0;                                               \
        if (!codec->reading) {                                             \
            memcpy(&value_, &(field), sizeof(value_));                     \
        }                                                                  \
        if (!codec_u32(codec, &value_)) {                                  \
            return false;                                                  \
        }                                                                  \
        if (codec->reading) {                                              \
            memcpy(&(field), &value_, sizeof(value_));                     \
        }                                                                  \
    } while (0)

static bool visit_vsh(FamilyKeyCodec *codec, VshState *state)
{
    FIELD_U32(state->surface_scale_factor);
    FIELD_U16(state->compressed_attrs);
    FIELD_U16(state->uniform_attrs);
    FIELD_U16(state->swizzle_attrs);
    FIELD_BOOL(state->fog_enable);
    FIELD_I32(state->fog_mode);
    FIELD_BOOL(state->specular_enable);
    FIELD_BOOL(state->separate_specular);
    FIELD_BOOL(state->ignore_specular_alpha);
    FIELD_FLOAT(state->specular_power);
    FIELD_FLOAT(state->specular_power_back);
    FIELD_BOOL(state->point_params_enable);
    FIELD_FLOAT(state->point_size);
    for (size_t i = 0; i < ARRAY_SIZE(state->point_params); i++) {
        FIELD_FLOAT(state->point_params[i]);
    }
    FIELD_BOOL(state->smooth_shading);
    FIELD_BOOL(state->z_perspective);
    FIELD_BOOL(state->is_fixed_function);

    FixedFunctionVshState *fixed = &state->fixed_function;
    FIELD_BOOL(fixed->normalization);
    for (size_t i = 0; i < ARRAY_SIZE(fixed->texture_matrix_enable); i++) {
        FIELD_BOOL(fixed->texture_matrix_enable[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(fixed->texgen); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(fixed->texgen[i]); j++) {
            FIELD_I32(fixed->texgen[i][j]);
        }
    }
    FIELD_I32(fixed->foggen);
    FIELD_I32(fixed->skinning);
    FIELD_BOOL(fixed->lighting);
    for (size_t i = 0; i < ARRAY_SIZE(fixed->light); i++) {
        FIELD_I32(fixed->light[i]);
    }
    FIELD_I32(fixed->emission_src);
    FIELD_I32(fixed->ambient_src);
    FIELD_I32(fixed->diffuse_src);
    FIELD_I32(fixed->specular_src);
    FIELD_BOOL(fixed->local_eye);

    ProgrammableVshState *programmable = &state->programmable;
    for (size_t i = 0; i < ARRAY_SIZE(programmable->program_data); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(programmable->program_data[i]); j++) {
            FIELD_U32(programmable->program_data[i][j]);
        }
    }
    FIELD_I32(programmable->program_length);
    return true;
}

static bool visit_geom(FamilyKeyCodec *codec, GeomState *state)
{
    FIELD_I32(state->primitive_mode);
    FIELD_I32(state->polygon_front_mode);
    FIELD_I32(state->polygon_back_mode);
    FIELD_BOOL(state->smooth_shading);
    FIELD_BOOL(state->first_vertex_is_provoking);
    FIELD_BOOL(state->z_perspective);
    FIELD_I32(state->tri_rot0);
    FIELD_I32(state->tri_rot1);
    return true;
}

static bool visit_psh(FamilyKeyCodec *codec, PshState *state)
{
    FIELD_U32(state->combiner_control);
    FIELD_U32(state->shader_stage_program);
    FIELD_U32(state->other_stage_input);
    FIELD_U32(state->final_inputs_0);
    FIELD_U32(state->final_inputs_1);
    for (size_t i = 0; i < ARRAY_SIZE(state->rgb_inputs); i++) {
        FIELD_U32(state->rgb_inputs[i]);
        FIELD_U32(state->rgb_outputs[i]);
        FIELD_U32(state->alpha_inputs[i]);
        FIELD_U32(state->alpha_outputs[i]);
    }
    FIELD_BOOL(state->point_sprite);
    for (size_t i = 0; i < ARRAY_SIZE(state->rect_tex); i++) {
        FIELD_BOOL(state->rect_tex[i]);
        FIELD_BOOL(state->snorm_tex[i]);
        for (size_t j = 0; j < ARRAY_SIZE(state->compare_mode[i]); j++) {
            FIELD_BOOL(state->compare_mode[i][j]);
        }
        FIELD_BOOL(state->alphakill[i]);
        FIELD_I32(state->colorkey_mode[i]);
        FIELD_I32(state->conv_tex[i]);
        FIELD_BOOL(state->tex_x8y24[i]);
        FIELD_I32(state->dim_tex[i]);
        FIELD_BOOL(state->tex_cubemap[i]);
        for (size_t j = 0; j < ARRAY_SIZE(state->border_logical_size[i]); j++) {
            FIELD_FLOAT(state->border_logical_size[i][j]);
            FIELD_FLOAT(state->border_inv_real_size[i][j]);
        }
        FIELD_BOOL(state->shadow_map[i]);
    }
    FIELD_I32(state->shadow_depth_func);
    FIELD_BOOL(state->alpha_test);
    FIELD_I32(state->alpha_func);
    FIELD_BOOL(state->window_clip_exclusive);
    FIELD_BOOL(state->smooth_shading);
    FIELD_BOOL(state->depth_clipping);
    FIELD_BOOL(state->z_perspective);
    FIELD_U32(state->surface_zeta_format);
    FIELD_I32(state->depth_format);
    return true;
}

static bool visit_pipeline_key(FamilyKeyCodec *codec, PipelineKey *key)
{
    FIELD_BOOL(key->clear);
    FIELD_I32(key->fragment_route);
    FIELD_I32(key->render_pass_state.color_format);
    FIELD_I32(key->render_pass_state.zeta_format);
    if (!visit_vsh(codec, &key->shader_state.vsh) ||
        !visit_geom(codec, &key->shader_state.geom) ||
        !visit_psh(codec, &key->shader_state.psh)) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(key->regs); i++) {
        FIELD_U32(key->regs[i]);
    }
    FIELD_U32(key->binding_description_count);
    FIELD_U32(key->attribute_description_count);
    if (key->binding_description_count > ARRAY_SIZE(key->binding_descriptions) ||
        key->attribute_description_count >
            ARRAY_SIZE(key->attribute_descriptions)) {
        return false;
    }
    for (size_t i = 0; i < key->binding_description_count; i++) {
        FIELD_U32(key->binding_descriptions[i].binding);
        FIELD_U32(key->binding_descriptions[i].stride);
        FIELD_I32(key->binding_descriptions[i].inputRate);
    }
    for (size_t i = 0; i < key->attribute_description_count; i++) {
        FIELD_U32(key->attribute_descriptions[i].location);
        FIELD_U32(key->attribute_descriptions[i].binding);
        FIELD_I32(key->attribute_descriptions[i].format);
        FIELD_U32(key->attribute_descriptions[i].offset);
    }
    return !key->clear &&
           (key->fragment_route == PGRAPH_VK_FRAGMENT_SPECIALIZED ||
            key->fragment_route == PGRAPH_VK_FRAGMENT_UBERSHADER);
}

static bool vsh_program_replay_safe(const ProgrammableVshState *program)
{
    if (program->program_length < 1 ||
        program->program_length > ARRAY_SIZE(program->program_data)) {
        return false;
    }
    for (int i = 0; i < program->program_length; i++) {
        const uint32_t *token = program->program_data[i];
        /* These fields match the instruction mapping in glsl/vsh-prog.c.
         * Decode only the values that index opcode tables or trigger an
         * assertion there, before a saved token reaches that generator. */
        unsigned int mac = (token[1] >> 21) & 15;
        unsigned int ilu = (token[1] >> 25) & 7;
        unsigned int a_mux = (token[2] >> 26) & 3;
        unsigned int b_mux = (token[2] >> 11) & 3;
        unsigned int c_mux = (token[3] >> 28) & 3;
        unsigned int output_mask = (token[3] >> 12) & 15;
        unsigned int output_mux = (token[3] >> 2) & 1;
        unsigned int output_is_const = !((token[3] >> 11) & 1);
        if (mac > MAC_ARL || (mac && !a_mux) ||
            ((mac == MAC_MUL || mac == MAC_MAD || mac == MAC_DP3 ||
              mac == MAC_DPH || mac == MAC_DP4 || mac == MAC_DST ||
              mac == MAC_MIN || mac == MAC_MAX || mac == MAC_SLT ||
              mac == MAC_SGE) && !b_mux) ||
            ((mac || ilu) && !c_mux) ||
            (output_mask && output_is_const &&
             ((output_mux == OMUX_MAC && mac) ||
              (output_mux == OMUX_ILU && ilu))) ||
            ((token[3] & 1) != (i == program->program_length - 1))) {
            return false;
        }
    }
    return true;
}

static bool family_vertex_format_valid(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16B16_SNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16_SSCALED:
    case VK_FORMAT_R16G16_SSCALED:
    case VK_FORMAT_R16G16B16_SSCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED:
    case VK_FORMAT_R32_SINT:
        return true;
    default:
        return false;
    }
}

static bool family_render_formats_valid(const RenderPassState *state)
{
    switch (state->color_format) {
    case VK_FORMAT_UNDEFINED:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
        break;
    default:
        return false;
    }
    switch (state->zeta_format) {
    case VK_FORMAT_UNDEFINED:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        break;
    default:
        return false;
    }
    return state->color_format != VK_FORMAT_UNDEFINED ||
           state->zeta_format != VK_FORMAT_UNDEFINED;
}

static bool family_texture_modes_valid(const PshState *psh)
{
    if (psh->point_sprite && psh->rect_tex[3]) {
        return false;
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        unsigned int mode = (psh->shader_stage_program >> (i * 5)) & 0x1f;
        unsigned int input = i < 2 ? 0 :
            (psh->other_stage_input >> (i == 2 ? 16 : 20)) & 0xf;
        if (mode > PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST ||
            (i >= 2 && input >= NV2A_MAX_TEXTURES) ||
            (i > 0 && ((psh->other_stage_input >> ((i - 1) * 4)) &
                       0xf) >= 8) ||
            (psh->shadow_map[i] && psh->dim_tex[i] != 2 &&
             (mode == PS_TEXTUREMODES_PROJECT2D ||
              mode == PS_TEXTUREMODES_PROJECT3D))) {
            return false;
        }
        switch (mode) {
        case PS_TEXTUREMODES_PROJECT2D:
            if (psh->dim_tex[i] != 2 && psh->dim_tex[i] != 3) {
                return false;
            }
            if (psh->conv_tex[i] != CONVOLUTION_FILTER_DISABLED &&
                psh->dim_tex[i] != 2) {
                return false;
            }
            break;
        case PS_TEXTUREMODES_BUMPENVMAP:
        case PS_TEXTUREMODES_BUMPENVMAP_LUM:
        case PS_TEXTUREMODES_DPNDNT_AR:
        case PS_TEXTUREMODES_DPNDNT_GB:
            if (i == 0 || psh->rect_tex[i]) {
                return false;
            }
            if (psh->shadow_map[i] ||
                (mode == PS_TEXTUREMODES_BUMPENVMAP ||
                 mode == PS_TEXTUREMODES_BUMPENVMAP_LUM ?
                     (psh->dim_tex[i] != 2 && psh->dim_tex[i] != 3) :
                     psh->dim_tex[i] != 2)) {
                return false;
            }
            break;
        case PS_TEXTUREMODES_BRDF:
        case PS_TEXTUREMODES_DOT_ST:
        case PS_TEXTUREMODES_DOT_ZW:
            if (i < 2) {
                return false;
            }
            if (mode == PS_TEXTUREMODES_DOT_ST &&
                (psh->shadow_map[i] || psh->dim_tex[i] != 2)) {
                return false;
            }
            break;
        case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
            if (i != 2) {
                return false;
            }
            break;
        case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
        case PS_TEXTUREMODES_DOT_STR_3D:
        case PS_TEXTUREMODES_DOT_STR_CUBE:
        case PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST:
            if (i != 3) {
                return false;
            }
            break;
        case PS_TEXTUREMODES_CUBEMAP:
            if (psh->shadow_map[i] || psh->dim_tex[i] != 2) {
                return false;
            }
            break;
        case PS_TEXTUREMODES_DOTPRODUCT:
            if (i != 1 && i != 2) {
                return false;
            }
            break;
        default:
            break;
        }
        if (mode == PS_TEXTUREMODES_PASSTHRU &&
            psh->border_logical_size[i][0] != 0.0f) {
            return false;
        }
        if ((mode == PS_TEXTUREMODES_DOT_RFLCT_DIFF ||
             mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC ||
             mode == PS_TEXTUREMODES_DOT_STR_CUBE) &&
            (psh->shadow_map[i] || psh->dim_tex[i] != 2)) {
            return false;
        }
    }
    return true;
}

static bool family_pipeline_registers_valid(const PipelineKey *key)
{
    uint32_t blend = key->regs[0];
    uint32_t control_2 = key->regs[3];
    if ((blend & NV_PGRAPH_BLEND_EN) &&
        GET_MASK(blend, NV_PGRAPH_BLEND_EQN) >= 7) {
        return false;
    }
    return GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL) < 9 &&
           GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL) < 9 &&
           GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS) < 9;
}

/* A checksum proves transport integrity, not that the saved values are safe
 * inputs to the shader generators or Vulkan recipe builder. Keep this gate
 * before the first replay-side generator call. New consumed fields or enum
 * interpretations require a matching admission update (and an ABI bump if
 * their serialized identity changes). */
static bool family_key_replay_safe(const PipelineKey *key)
{
    const VshState *vsh = &key->shader_state.vsh;
    const GeomState *geom = &key->shader_state.geom;
    const PshState *psh = &key->shader_state.psh;

    if (key->fragment_route != PGRAPH_VK_FRAGMENT_UBERSHADER ||
        key->clear ||
        !family_render_formats_valid(&key->render_pass_state) ||
        !family_pipeline_registers_valid(key) ||
        (vsh->uniform_attrs &
         (vsh->compressed_attrs | vsh->swizzle_attrs)) ||
        (!vsh->is_fixed_function &&
         !vsh_program_replay_safe(&vsh->programmable)) ||
        (vsh->fog_enable &&
         (vsh->fog_mode < FOG_MODE_LINEAR ||
          vsh->fog_mode > FOG_MODE_EXP2_ABS ||
          vsh->fog_mode == FOG_MODE_ERROR2 ||
          vsh->fog_mode == FOG_MODE_ERROR6)) ||
        geom->primitive_mode < PRIM_TYPE_POINTS ||
        geom->primitive_mode > PRIM_TYPE_POLYGON ||
        geom->polygon_front_mode != geom->polygon_back_mode ||
        geom->polygon_front_mode < POLY_MODE_FILL ||
        geom->polygon_front_mode > POLY_MODE_LINE ||
        (geom->primitive_mode == PRIM_TYPE_POLYGON &&
         geom->polygon_front_mode == POLY_MODE_POINT) ||
        psh->alpha_func < ALPHA_FUNC_NEVER ||
        psh->alpha_func > ALPHA_FUNC_ALWAYS ||
        psh->shadow_depth_func < SHADOW_DEPTH_FUNC_NEVER ||
        psh->shadow_depth_func > SHADOW_DEPTH_FUNC_ALWAYS ||
        psh->depth_format < DEPTH_FORMAT_D24 ||
        psh->depth_format > DEPTH_FORMAT_F16 ||
        (psh->surface_zeta_format != NV097_SET_SURFACE_FORMAT_ZETA_Z16 &&
         psh->surface_zeta_format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) ||
        !family_texture_modes_valid(psh) ||
        psh->combiner_control || psh->final_inputs_0 ||
        psh->final_inputs_1) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(psh->rgb_inputs); i++) {
        if (psh->rgb_inputs[i] || psh->rgb_outputs[i] ||
            psh->alpha_inputs[i] || psh->alpha_outputs[i]) {
            return false;
        }
    }
    for (size_t i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (psh->colorkey_mode[i] < COLOR_KEY_NONE ||
            psh->colorkey_mode[i] > COLOR_KEY_DISCARD ||
            psh->conv_tex[i] < CONVOLUTION_FILTER_DISABLED ||
            psh->conv_tex[i] > CONVOLUTION_FILTER_GAUSSIAN ||
            psh->dim_tex[i] < 0 || psh->dim_tex[i] > 3) {
            return false;
        }
    }

    if (vsh->is_fixed_function) {
        const FixedFunctionVshState *fixed = &vsh->fixed_function;
        if (fixed->skinning < SKINNING_OFF ||
            fixed->skinning > SKINNING_4WEIGHTS4MATRICES ||
            fixed->foggen < FOGGEN_SPEC_ALPHA ||
            fixed->foggen > FOGGEN_FOG_X ||
            fixed->emission_src < MATERIAL_COLOR_SRC_MATERIAL ||
            fixed->emission_src > MATERIAL_COLOR_SRC_SPECULAR ||
            fixed->ambient_src < MATERIAL_COLOR_SRC_MATERIAL ||
            fixed->ambient_src > MATERIAL_COLOR_SRC_SPECULAR ||
            fixed->diffuse_src < MATERIAL_COLOR_SRC_MATERIAL ||
            fixed->diffuse_src > MATERIAL_COLOR_SRC_SPECULAR ||
            fixed->specular_src < MATERIAL_COLOR_SRC_MATERIAL ||
            fixed->specular_src > MATERIAL_COLOR_SRC_SPECULAR) {
            return false;
        }
        for (size_t i = 0; i < ARRAY_SIZE(fixed->texgen); i++) {
            for (size_t j = 0; j < ARRAY_SIZE(fixed->texgen[i]); j++) {
                enum VshTexgen mode = fixed->texgen[i][j];
                if (mode < TEXGEN_DISABLE || mode > TEXGEN_REFLECTION_MAP ||
                    (j >= 2 && mode == TEXGEN_SPHERE_MAP) ||
                    (j >= 3 && (mode == TEXGEN_NORMAL_MAP ||
                                mode == TEXGEN_REFLECTION_MAP))) {
                    return false;
                }
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(fixed->light); i++) {
            if (fixed->light[i] < LIGHT_OFF ||
                fixed->light[i] > LIGHT_SPOT) {
                return false;
            }
        }
    }

    bool bindings[NV2A_VERTEXSHADER_ATTRIBUTES] = { 0 };
    bool locations[NV2A_VERTEXSHADER_ATTRIBUTES] = { 0 };
    for (size_t i = 0; i < key->binding_description_count; i++) {
        const VkVertexInputBindingDescription *binding =
            &key->binding_descriptions[i];
        if (binding->binding >= ARRAY_SIZE(bindings) ||
            bindings[binding->binding] ||
            (binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX &&
             binding->inputRate != VK_VERTEX_INPUT_RATE_INSTANCE)) {
            return false;
        }
        bindings[binding->binding] = true;
    }
    for (size_t i = 0; i < key->attribute_description_count; i++) {
        const VkVertexInputAttributeDescription *attribute =
            &key->attribute_descriptions[i];
        if (attribute->location >= ARRAY_SIZE(locations) ||
            locations[attribute->location] ||
            attribute->binding >= ARRAY_SIZE(bindings) ||
            !bindings[attribute->binding] ||
            !family_vertex_format_valid(attribute->format)) {
            return false;
        }
        locations[attribute->location] = true;
    }
    return true;
}

bool pgraph_vk_family_key_encode(const PipelineKey *key,
                                 PGRAPHVkFamilyKeyBlob *blob)
{
    if (!key || !blob) {
        return false;
    }
    uint8_t *data = calloc(1, PGRAPH_VK_FAMILY_KEY_MAX_SIZE);
    if (!data) {
        return false;
    }
    PipelineKey copy = *key;
    FamilyKeyCodec codec = {
        .data = data,
        .size = PGRAPH_VK_FAMILY_KEY_MAX_SIZE,
        .offset = FAMILY_KEY_HEADER_SIZE,
    };
    if (!visit_pipeline_key(&codec, &copy) || codec.offset > UINT32_MAX) {
        free(data);
        return false;
    }
    store_u32_le(data + HEADER_MAGIC, FAMILY_KEY_MAGIC);
    store_u32_le(data + HEADER_ABI, PGRAPH_VK_FAMILY_KEY_ABI);
    store_u32_le(data + HEADER_SIZE, codec.offset);
    store_u32_le(data + HEADER_RESERVED, 0);
    store_u64_le(data + HEADER_PAYLOAD_HASH,
                 family_key_hash(data + FAMILY_KEY_HEADER_SIZE,
                                 codec.offset - FAMILY_KEY_HEADER_SIZE));
    uint8_t *compact = realloc(data, codec.offset);
    *blob = (PGRAPHVkFamilyKeyBlob) {
        .data = compact ? compact : data,
        .size = codec.offset,
    };
    return true;
}

bool pgraph_vk_family_key_decode(const uint8_t *data, size_t size,
                                 PipelineKey *key)
{
    if (!data || !key || size < FAMILY_KEY_HEADER_SIZE ||
        size > PGRAPH_VK_FAMILY_KEY_MAX_SIZE ||
        load_u32_le(data + HEADER_MAGIC) != FAMILY_KEY_MAGIC ||
        load_u32_le(data + HEADER_ABI) != PGRAPH_VK_FAMILY_KEY_ABI ||
        load_u32_le(data + HEADER_SIZE) != size ||
        load_u32_le(data + HEADER_RESERVED) != 0 ||
        load_u64_le(data + HEADER_PAYLOAD_HASH) !=
            family_key_hash(data + FAMILY_KEY_HEADER_SIZE,
                            size - FAMILY_KEY_HEADER_SIZE)) {
        return false;
    }
    PipelineKey decoded = { 0 };
    FamilyKeyCodec codec = {
        .data = (uint8_t *)data,
        .size = size,
        .offset = FAMILY_KEY_HEADER_SIZE,
        .reading = true,
    };
    if (!visit_pipeline_key(&codec, &decoded) || codec.offset != size ||
        !family_key_replay_safe(&decoded)) {
        return false;
    }
    *key = decoded;
    return true;
}

void pgraph_vk_family_key_blob_destroy(PGRAPHVkFamilyKeyBlob *blob)
{
    if (!blob) {
        return;
    }
    free(blob->data);
    *blob = (PGRAPHVkFamilyKeyBlob) { 0 };
}

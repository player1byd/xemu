/*
 * NV2A Vulkan ubershader compiler and reflection integration test
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/pgraph/glsl/geom.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh-prog.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"
#include "hw/xbox/nv2a/pgraph/vk/hybrid-ready.h"
#include "ui/xemu-settings.h"

struct config g_config;

/* This compiler test does not exercise texture-format classification. */
const BasicColorFormatInfo kelvin_color_format_info_map[66] = { 0 };

static uint64_t fake_shader_module_next;
static uint64_t fake_shader_module_destroy_count;

static VKAPI_ATTR VkResult VKAPI_CALL fake_vk_create_shader_module(
    VkDevice device, const VkShaderModuleCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkShaderModule *module)
{
    (void)device;
    (void)create_info;
    (void)allocator;
    *module = (VkShaderModule)(uintptr_t)++fake_shader_module_next;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_vk_destroy_shader_module(
    VkDevice device, VkShaderModule module,
    const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)module;
    (void)allocator;
    fake_shader_module_destroy_count++;
}

void nv2a_profile_log_event_once(NV2AProfileEvent event)
{
    (void)event;
}

void pgraph_vk_hybrid_trace_record(PGRAPHVkHybridTrace *trace,
                                   PGRAPHVkHybridTraceType type,
                                   uint32_t route, uint64_t pipeline_hash,
                                   uint64_t shader_hash, uint64_t ticket,
                                   uint64_t a, uint64_t b, uint64_t c,
                                   uint64_t d)
{
    (void)trace;
    (void)type;
    (void)route;
    (void)pipeline_hash;
    (void)shader_hash;
    (void)ticket;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
}

static PshState base_state(void)
{
    PshState state = { 0 };

    state.combiner_control = 1 | (PS_COMBINERCOUNT_MUX_MSB << 8);
    state.final_inputs_0 = 0x20202020;
    state.final_inputs_1 = 0x20202000;
    state.smooth_shading = true;
    state.depth_format = DEPTH_FORMAT_D24;
    return state;
}

static const SpvReflectDescriptorBinding *find_binding(
    const ShaderModuleInfo *info, uint32_t set, uint32_t binding)
{
    for (uint32_t i = 0; i < info->reflect_module.descriptor_set_count; i++) {
        const SpvReflectDescriptorSet *descriptor_set =
            info->descriptor_sets[i];

        if (descriptor_set->set != set) {
            continue;
        }
        for (uint32_t j = 0; j < descriptor_set->binding_count; j++) {
            if (descriptor_set->bindings[j]->binding == binding) {
                return descriptor_set->bindings[j];
            }
        }
    }
    return NULL;
}

static void test_real_compiler_and_reflection_accept_uber_abi(void)
{
    PshState state = base_state();
    GenPshGlslOptions opts = {
        .vulkan = true,
        .ubo_binding = 1,
        .tex_binding = 2,
        .ubershader = true,
        .uber_binding = PGRAPH_VK_PSH_UBER_UBO_BINDING,
    };
    PGRAPHVkGlslCompileConfig compile_config = {
        .api_version = VK_API_VERSION_1_1,
        .debug_shaders = false,
    };
    ShaderModuleInfo info = { 0 };
    MString *source = pgraph_glsl_gen_psh(&state, opts);
    g_assert_nonnull(strstr(mstring_get_str(source),
        "uint uberStageCount = min(uberHeader.y, 8u);"));

    pgraph_vk_init_glsl_compiler();
    info.spirv = pgraph_vk_compile_glsl_to_spv_config(
        &compile_config, GLSLANG_STAGE_FRAGMENT, mstring_get_str(source));
    g_assert_nonnull(info.spirv);
    g_assert_true(pgraph_vk_init_shader_module_layout_from_spv(
        &info, VK_SHADER_STAGE_FRAGMENT_BIT));
    g_assert_true(info.uses_uber_controls);

    const SpvReflectDescriptorBinding *binding = find_binding(
        &info, 0, PGRAPH_VK_PSH_UBER_UBO_BINDING);
    g_assert_nonnull(binding);
    g_assert_true(pgraph_vk_uber_controls_block_matches_abi(&binding->block));
    g_assert_cmpuint(binding->block.size, ==, sizeof(PGRAPHUberControls));
    g_assert_cmpuint(binding->block.member_count, ==, 4);
    g_assert_cmpuint(binding->block.members[0].offset, ==,
                     offsetof(PGRAPHUberControls, header));
    g_assert_cmpuint(binding->block.members[1].offset, ==,
                     offsetof(PGRAPHUberControls, stage));
    g_assert_cmpuint(binding->block.members[2].offset, ==,
                     offsetof(PGRAPHUberControls, final_words));
    g_assert_cmpuint(binding->block.members[3].offset, ==,
                     offsetof(PGRAPHUberControls, constants));

    SpvReflectBlockVariable bad_block = binding->block;
    g_autofree SpvReflectBlockVariable *bad_members = g_memdup2(
        binding->block.members,
        binding->block.member_count * sizeof(*binding->block.members));
    bad_block.members = bad_members;
    bad_block.members[1].offset++;
    g_assert_false(pgraph_vk_uber_controls_block_matches_abi(&bad_block));

    pgraph_vk_clear_shader_module_layout(&info);
    g_byte_array_unref(info.spirv);
    pgraph_vk_finalize_glsl_compiler();
    mstring_unref(source);
}

static void test_invalid_glsl_returns_failure(void)
{
    PGRAPHVkGlslCompileConfig config = {
        .api_version = VK_API_VERSION_1_1,
    };

    pgraph_vk_init_glsl_compiler();
    GByteArray *spirv = pgraph_vk_compile_glsl_to_spv_config(
        &config, GLSLANG_STAGE_FRAGMENT,
        "#version 460\nvoid main() { this is invalid GLSL; }\n");
    g_assert_null(spirv);
    pgraph_vk_finalize_glsl_compiler();
}

static void test_depth_replace_compiles_for_both_fragment_routes(void)
{
    PGRAPHVkGlslCompileConfig config = { .api_version = VK_API_VERSION_1_1 };
    pgraph_vk_init_glsl_compiler();
    for (int stage = 2; stage <= 3; stage++) {
        for (int uber = 0; uber <= 1; uber++) {
            for (int format = DEPTH_FORMAT_D24; format <= DEPTH_FORMAT_D16;
                 format++) {
                PshState state = base_state();
                state.shader_stage_program =
                    PS_TEXTUREMODES_PASSTHRU |
                    (PS_TEXTUREMODES_DOTPRODUCT << ((stage - 1) * 5)) |
                    (PS_TEXTUREMODES_DOT_ZW << (stage * 5));
                state.depth_clipping = true;
                state.depth_format = format;
                MString *source = pgraph_glsl_gen_psh(
                    &state, (GenPshGlslOptions){
                                .vulkan = true,
                                .ubo_binding = 1,
                                .tex_binding = 2,
                                .ubershader = uber,
                                .uber_binding = PGRAPH_VK_PSH_UBER_UBO_BINDING,
                            });
                g_assert_nonnull(strstr(mstring_get_str(source),
                                        "zvalue = dot"));
                g_assert_null(strstr(mstring_get_str(source),
                                     "precise float bc0"));
                g_assert_null(strstr(mstring_get_str(source),
                                     "zvalue += depthOffset;"));
                GByteArray *spirv = pgraph_vk_compile_glsl_to_spv_config(
                    &config, GLSLANG_STAGE_FRAGMENT, mstring_get_str(source));
                g_assert_nonnull(spirv);
                g_byte_array_unref(spirv);
                mstring_unref(source);
            }
        }
    }
    PshState invalid = base_state();
    invalid.shader_stage_program = PS_TEXTUREMODES_PASSTHRU |
        (PS_TEXTUREMODES_DOTPRODUCT << 10) |
        (PS_TEXTUREMODES_DOT_ZW << 15);
    invalid.other_stage_input =
        (invalid.other_stage_input & ~(0xFu << 20)) | (2u << 20);
    MString *ordinary = pgraph_glsl_gen_psh(
        &invalid, (GenPshGlslOptions){ .vulkan = true,
                                       .ubo_binding = 1,
                                       .tex_binding = 2 });
    g_assert_null(strstr(mstring_get_str(ordinary), "zvalue = dot"));
    g_assert_nonnull(strstr(mstring_get_str(ordinary),
                            "precise float bc0"));
    g_assert_nonnull(strstr(mstring_get_str(ordinary),
                            "zvalue += depthOffset;"));
    GByteArray *ordinary_spirv = pgraph_vk_compile_glsl_to_spv_config(
        &config, GLSLANG_STAGE_FRAGMENT, mstring_get_str(ordinary));
    g_assert_nonnull(ordinary_spirv);
    g_byte_array_unref(ordinary_spirv);
    mstring_unref(ordinary);
    pgraph_vk_finalize_glsl_compiler();
}

static void test_real_compiler_accepts_nv20_vertex_arithmetic(void)
{
    const uint32_t final_nop[VSH_TOKEN_SIZE] = { 0, 0, 0, 1 };
    MString *header = mstring_from_str(
        "#version 450\n"
        "float clampAwayZeroInf(float value) { return value; }\n"
        "vec4 NaNToOne(vec4 value) { return value; }\n"
        "vec2 roundScreenCoords(vec2 value) { return value; }\n"
        "const vec2 surfaceSize = vec2(640.0, 480.0);\n"
        "const vec4 clipRange = vec4(0.0, 16777215.0, 0.0, 0.0);\n"
        "vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n");
    MString *body = mstring_from_str("void main() {\n");

    pgraph_glsl_gen_vsh_prog(VSH_VERSION_XVS, final_nop, 1, true,
                             header, body);
    mstring_append(body, "  gl_Position = oPos;\n}\n");
    mstring_append(header, mstring_get_str(body));

    g_assert_nonnull(strstr(mstring_get_str(header),
                            "#define NV2A_NV20_ARITHMETIC 1"));
    g_assert_nonnull(strstr(mstring_get_str(header), "nv20_mul_bits"));
    g_assert_nonnull(strstr(mstring_get_str(header), "nv20_sum_bits"));
    g_assert_nonnull(strstr(mstring_get_str(header), "nv20_rcp_bits"));

    PGRAPHVkGlslCompileConfig config = {
        .api_version = VK_API_VERSION_1_1,
    };
    pgraph_vk_init_glsl_compiler();
    GByteArray *spirv = pgraph_vk_compile_glsl_to_spv_config(
        &config, GLSLANG_STAGE_VERTEX, mstring_get_str(header));
    g_assert_nonnull(spirv);
    g_byte_array_unref(spirv);
    pgraph_vk_finalize_glsl_compiler();

    mstring_unref(body);
    mstring_unref(header);
}

static void test_real_compiler_accepts_generated_graphics_stages(void)
{
    GeomState geom = {
        .primitive_mode = PRIM_TYPE_TRIANGLES,
        .polygon_front_mode = POLY_MODE_FILL,
        .polygon_back_mode = POLY_MODE_FILL,
        .smooth_shading = true,
        .z_perspective = true,
    };
    PshState psh = base_state();
    const uint32_t final_nop[VSH_TOKEN_SIZE] = { 0, 0, 0, 1 };
    MString *vertex_source = mstring_from_str(
        "#version 450\n"
        "float clampAwayZeroInf(float value) { return value; }\n"
        "vec4 NaNToOne(vec4 value) { return value; }\n"
        "vec2 roundScreenCoords(vec2 value) { return value; }\n"
        "const vec2 surfaceSize = vec2(640.0, 480.0);\n"
        "const vec4 clipRange = vec4(0.0, 16777215.0, 0.0, 0.0);\n"
        "vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n");
    MString *vertex_body = mstring_from_str("void main() {\n");
    pgraph_glsl_gen_vsh_prog(VSH_VERSION_XVS, final_nop, 1, false,
                             vertex_source, vertex_body);
    mstring_append(vertex_body, "  gl_Position = oPos;\n}\n");
    mstring_append(vertex_source, mstring_get_str(vertex_body));
    mstring_unref(vertex_body);

    MString *sources[] = {
        vertex_source,
        pgraph_glsl_gen_geom(&geom, (GenGeomGlslOptions) {
            .vulkan = true,
        }),
        pgraph_glsl_gen_psh(&psh, (GenPshGlslOptions) {
            .vulkan = true,
            .ubo_binding = 1,
            .tex_binding = 2,
        }),
    };
    static const glslang_stage_t stages[] = {
        GLSLANG_STAGE_VERTEX,
        GLSLANG_STAGE_GEOMETRY,
        GLSLANG_STAGE_FRAGMENT,
    };
    PGRAPHVkGlslCompileConfig config = {
        .api_version = VK_API_VERSION_1_1,
    };

    pgraph_vk_init_glsl_compiler();
    for (size_t i = 0; i < ARRAY_SIZE(sources); i++) {
        g_assert_nonnull(sources[i]);
        GByteArray *spirv = pgraph_vk_compile_glsl_to_spv_config(
            &config, stages[i], mstring_get_str(sources[i]));
        g_assert_nonnull(spirv);
        g_byte_array_unref(spirv);
        mstring_unref(sources[i]);
    }
    pgraph_vk_finalize_glsl_compiler();
}

typedef struct AliasPublicationFixture {
    PGRAPHVkState *renderer;
    PGRAPHVkHybridShaderWork *first_work;
    const char *source;
    GByteArray *spirv;
    ShaderModuleInfo *first;
    ShaderModuleInfo *second;
} AliasPublicationFixture;

typedef struct AliasFailureFixture {
    PGRAPHVkHybridShaderWork *failed_work;
    unsigned int calls;
} AliasFailureFixture;

static bool publish_completion_alias(void *opaque,
                                     PGRAPHVkHybridShaderWork *work)
{
    AliasPublicationFixture *fixture = opaque;
    ShaderModuleInfo *info = pgraph_vk_create_shader_module_from_spirv(
        fixture->renderer, work->module_key.kind, fixture->source,
        fixture->spirv);

    if (work == fixture->first_work) {
        fixture->first = info;
    } else {
        fixture->second = info;
    }
    return info != NULL;
}

static bool publish_completion_alias_with_failure(
    void *opaque, PGRAPHVkHybridShaderWork *work)
{
    AliasFailureFixture *fixture = opaque;

    fixture->calls++;
    return work != fixture->failed_work;
}

static void test_completion_fanout_visits_aliases_after_one_failure(void)
{
    PGRAPHVkHybridShaderWork work[2] = { 0 };

    for (size_t i = 0; i < G_N_ELEMENTS(work); i++) {
        work[i].in_use = true;
        work[i].metadata = (PGRAPHVkHybridWork) {
            .generation = 7,
            .ticket = 11,
            .status = PGRAPH_VK_HYBRID_WORK_PENDING,
        };
    }
    AliasFailureFixture fixture = { .failed_work = &work[0] };
    bool all_published = true;

    g_assert_cmpuint(pgraph_vk_hybrid_completion_fanout(
                         work, G_N_ELEMENTS(work), 7, 11, 7,
                         publish_completion_alias_with_failure, &fixture,
                         &all_published), ==, 2);
    g_assert_false(all_published);
    g_assert_cmpuint(fixture.calls, ==, 2);
}

static void test_deduplicated_completion_publishes_independent_aliases(void)
{
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);
    PGRAPHVkHybridShaderWork work[2] = { 0 };
    PshState state = base_state();
    GenPshGlslOptions opts = {
        .vulkan = true,
        .ubo_binding = 1,
        .tex_binding = 2,
    };
    PGRAPHVkGlslCompileConfig config = {
        .api_version = VK_API_VERSION_1_1,
    };
    MString *source = pgraph_glsl_gen_psh(&state, opts);

    pgraph_vk_init_glsl_compiler();
    GByteArray *spirv = pgraph_vk_compile_glsl_to_spv_config(
        &config, GLSLANG_STAGE_FRAGMENT, mstring_get_str(source));
    g_assert_nonnull(spirv);

    PFN_vkCreateShaderModule saved_create = vkCreateShaderModule;
    PFN_vkDestroyShaderModule saved_destroy = vkDestroyShaderModule;
    vkCreateShaderModule = fake_vk_create_shader_module;
    vkDestroyShaderModule = fake_vk_destroy_shader_module;
    fake_shader_module_next = 0;
    fake_shader_module_destroy_count = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(work); i++) {
        work[i].in_use = true;
        work[i].metadata = (PGRAPHVkHybridWork) {
            .generation = 7,
            .ticket = 11,
            .status = PGRAPH_VK_HYBRID_WORK_PENDING,
        };
        work[i].module_key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    work[0].module_key.psh.state.alpha_func = ALPHA_FUNC_ALWAYS;
    work[1].module_key.psh.state.alpha_func = ALPHA_FUNC_GREATER;
    g_assert_cmpint(memcmp(&work[0].module_key, &work[1].module_key,
                           sizeof(work[0].module_key)), !=, 0);

    AliasPublicationFixture fixture = {
        .renderer = r,
        .first_work = &work[0],
        .source = mstring_get_str(source),
        .spirv = spirv,
    };
    bool all_published = false;
    g_assert_cmpuint(pgraph_vk_hybrid_completion_fanout(
                         work, G_N_ELEMENTS(work), 7, 11, 7,
                         publish_completion_alias, &fixture,
                         &all_published), ==, 2);
    g_assert_true(all_published);
    g_assert_nonnull(fixture.first);
    g_assert_nonnull(fixture.second);
    g_assert_true(fixture.first != fixture.second);
    g_assert_true(fixture.first->module != fixture.second->module);

    pgraph_vk_ref_shader_module(fixture.first);
    pgraph_vk_ref_shader_module(fixture.second);
    VkShaderModule surviving_module = fixture.second->module;
    pgraph_vk_unref_shader_module(r, fixture.first);
    g_assert_cmpuint(fake_shader_module_destroy_count, ==, 1);
    g_assert_true(fixture.second->module == surviving_module);
    g_assert_cmpuint(fixture.second->refcnt, ==, 1);
    pgraph_vk_unref_shader_module(r, fixture.second);
    g_assert_cmpuint(fake_shader_module_destroy_count, ==, 2);

    vkCreateShaderModule = saved_create;
    vkDestroyShaderModule = saved_destroy;
    g_byte_array_unref(spirv);
    pgraph_vk_finalize_glsl_compiler();
    mstring_unref(source);
    g_free(r);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/ubershader/glsl/compile-reflect-abi",
                    test_real_compiler_and_reflection_accept_uber_abi);
    g_test_add_func("/xbox/vk/ubershader/glsl/invalid-source",
                    test_invalid_glsl_returns_failure);
    g_test_add_func("/xbox/vk/psh/depth-replace-compile",
                    test_depth_replace_compiles_for_both_fragment_routes);
    g_test_add_func("/xbox/vk/vsh/nv20-arithmetic-compile",
                    test_real_compiler_accepts_nv20_vertex_arithmetic);
    g_test_add_func("/xbox/vk/ubershader/glsl/generated-graphics-stages",
                    test_real_compiler_accepts_generated_graphics_stages);
    g_test_add_func("/xbox/vk/ubershader/glsl/dedup-alias-ownership",
                    test_deduplicated_completion_publishes_independent_aliases);
    g_test_add_func("/xbox/vk/ubershader/glsl/dedup-alias-partial-failure",
                    test_completion_fanout_visits_aliases_after_one_failure);
    return g_test_run();
}

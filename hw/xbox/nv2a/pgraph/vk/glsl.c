/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/fast-hash.h"
#include "ui/xemu-settings.h"
#include "device-inventory.h"
#include "renderer.h"

#include <assert.h>
#include <glslang/Include/glslang_c_interface.h>
#include <stdio.h>
#include <stdlib.h>

static const glslang_resource_t
    resource_limits = { .max_lights = 32,
                        .max_clip_planes = 6,
                        .max_texture_units = 32,
                        .max_texture_coords = 32,
                        .max_vertex_attribs = 64,
                        .max_vertex_uniform_components = 4096,
                        .max_varying_floats = 64,
                        .max_vertex_texture_image_units = 32,
                        .max_combined_texture_image_units = 80,
                        .max_texture_image_units = 32,
                        .max_fragment_uniform_components = 4096,
                        .max_draw_buffers = 32,
                        .max_vertex_uniform_vectors = 128,
                        .max_varying_vectors = 8,
                        .max_fragment_uniform_vectors = 16,
                        .max_vertex_output_vectors = 16,
                        .max_fragment_input_vectors = 15,
                        .min_program_texel_offset = -8,
                        .max_program_texel_offset = 7,
                        .max_clip_distances = 8,
                        .max_compute_work_group_count_x = 65535,
                        .max_compute_work_group_count_y = 65535,
                        .max_compute_work_group_count_z = 65535,
                        .max_compute_work_group_size_x = 1024,
                        .max_compute_work_group_size_y = 1024,
                        .max_compute_work_group_size_z = 64,
                        .max_compute_uniform_components = 1024,
                        .max_compute_texture_image_units = 16,
                        .max_compute_image_uniforms = 8,
                        .max_compute_atomic_counters = 8,
                        .max_compute_atomic_counter_buffers = 1,
                        .max_varying_components = 60,
                        .max_vertex_output_components = 64,
                        .max_geometry_input_components = 64,
                        .max_geometry_output_components = 128,
                        .max_fragment_input_components = 128,
                        .max_image_units = 8,
                        .max_combined_image_units_and_fragment_outputs = 8,
                        .max_combined_shader_output_resources = 8,
                        .max_image_samples = 0,
                        .max_vertex_image_uniforms = 0,
                        .max_tess_control_image_uniforms = 0,
                        .max_tess_evaluation_image_uniforms = 0,
                        .max_geometry_image_uniforms = 0,
                        .max_fragment_image_uniforms = 8,
                        .max_combined_image_uniforms = 8,
                        .max_geometry_texture_image_units = 16,
                        .max_geometry_output_vertices = 256,
                        .max_geometry_total_output_components = 1024,
                        .max_geometry_uniform_components = 1024,
                        .max_geometry_varying_components = 64,
                        .max_tess_control_input_components = 128,
                        .max_tess_control_output_components = 128,
                        .max_tess_control_texture_image_units = 16,
                        .max_tess_control_uniform_components = 1024,
                        .max_tess_control_total_output_components = 4096,
                        .max_tess_evaluation_input_components = 128,
                        .max_tess_evaluation_output_components = 128,
                        .max_tess_evaluation_texture_image_units = 16,
                        .max_tess_evaluation_uniform_components = 1024,
                        .max_tess_patch_components = 120,
                        .max_patch_vertices = 32,
                        .max_tess_gen_level = 64,
                        .max_viewports = 16,
                        .max_vertex_atomic_counters = 0,
                        .max_tess_control_atomic_counters = 0,
                        .max_tess_evaluation_atomic_counters = 0,
                        .max_geometry_atomic_counters = 0,
                        .max_fragment_atomic_counters = 8,
                        .max_combined_atomic_counters = 8,
                        .max_atomic_counter_bindings = 1,
                        .max_vertex_atomic_counter_buffers = 0,
                        .max_tess_control_atomic_counter_buffers = 0,
                        .max_tess_evaluation_atomic_counter_buffers = 0,
                        .max_geometry_atomic_counter_buffers = 0,
                        .max_fragment_atomic_counter_buffers = 1,
                        .max_combined_atomic_counter_buffers = 1,
                        .max_atomic_counter_buffer_size = 16384,
                        .max_transform_feedback_buffers = 4,
                        .max_transform_feedback_interleaved_components = 64,
                        .max_cull_distances = 8,
                        .max_combined_clip_and_cull_distances = 8,
                        .max_samples = 4,
                        .max_mesh_output_vertices_nv = 256,
                        .max_mesh_output_primitives_nv = 512,
                        .max_mesh_work_group_size_x_nv = 32,
                        .max_mesh_work_group_size_y_nv = 1,
                        .max_mesh_work_group_size_z_nv = 1,
                        .max_task_work_group_size_x_nv = 32,
                        .max_task_work_group_size_y_nv = 1,
                        .max_task_work_group_size_z_nv = 1,
                        .max_mesh_view_count_nv = 4,
                        .maxDualSourceDrawBuffersEXT = 1,
                        .limits = {
                            .non_inductive_for_loops = 1,
                            .while_loops = 1,
                            .do_while_loops = 1,
                            .general_uniform_indexing = 1,
                            .general_attribute_matrix_vector_indexing = 1,
                            .general_varying_indexing = 1,
                            .general_sampler_indexing = 1,
                            .general_variable_indexing = 1,
                            .general_constant_matrix_vector_indexing = 1,
                        } };

void pgraph_vk_init_glsl_compiler(void)
{
    glslang_initialize_process();
}

void pgraph_vk_finalize_glsl_compiler(void)
{
    glslang_finalize_process();
}

void pgraph_vk_glsl_target_versions(
    uint32_t api_version, glslang_target_client_version_t *client_version,
    glslang_target_language_version_t *language_version)
{
    PGRAPHVkShaderTarget shader_target =
        pgraph_vk_shader_target_for_api(api_version);
    switch (shader_target) {
    case PGRAPH_VK_SHADER_TARGET_VULKAN_1_3:
        *client_version = GLSLANG_TARGET_VULKAN_1_3;
        *language_version = GLSLANG_TARGET_SPV_1_6;
        break;
    case PGRAPH_VK_SHADER_TARGET_VULKAN_1_2:
        *client_version = GLSLANG_TARGET_VULKAN_1_2;
        *language_version = GLSLANG_TARGET_SPV_1_5;
        break;
    default:
        *client_version = GLSLANG_TARGET_VULKAN_1_1;
        *language_version = GLSLANG_TARGET_SPV_1_3;
        break;
    }
}

GByteArray *pgraph_vk_compile_glsl_to_spv_config(
    const PGRAPHVkGlslCompileConfig *config, glslang_stage_t stage,
    const char *glsl_source)
{
    g_return_val_if_fail(config != NULL, NULL);
    g_return_val_if_fail(glsl_source != NULL, NULL);

    glslang_target_client_version_t client_version;
    glslang_target_language_version_t language_version;
    pgraph_vk_glsl_target_versions(config->api_version, &client_version,
                                   &language_version);
    const glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = client_version,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = language_version,
        .code = glsl_source,
        .default_version = 460,
        .default_profile = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = &resource_limits,
    };

    glslang_shader_t *shader = glslang_shader_create(&input);
    if (!shader) {
        error_report("nv2a/vk: failed to create GLSL shader compiler object");
        return NULL;
    }

    if (!glslang_shader_preprocess(shader, &input)) {
        error_report("nv2a/vk: GLSL preprocessing failed: %s",
                     glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return NULL;
    }

    if (!glslang_shader_parse(shader, &input)) {
        error_report("nv2a/vk: GLSL parsing failed: %s",
                     glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_t *program = glslang_program_create();
    if (!program) {
        error_report("nv2a/vk: failed to create GLSL program compiler object");
        glslang_shader_delete(shader);
        return NULL;
    }
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                           GLSLANG_MSG_VULKAN_RULES_BIT)) {
        error_report("nv2a/vk: GLSL linking failed: %s",
                     glslang_program_get_info_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_spv_options_t spv_options = {
        .validate = true,
    };

    if (config->debug_shaders) {
        spv_options.disable_optimizer = true;
        spv_options.generate_debug_info = true;
        spv_options.emit_nonsemantic_shader_debug_info = true;
        spv_options.emit_nonsemantic_shader_debug_source = true;

        // XXX: Note emit_nonsemantic_shader_debug_source actually does nothing
        // as of 2024.07.25. To actually get glsl source embedded in spv, we
        // must do the following...
        //
        // ref: https://github.com/KhronosGroup/glslang/issues/3252
        glslang_program_add_source_text(program, input.stage, input.code,
                                        strlen(input.code));
    }
    glslang_program_SPIRV_generate_with_options(program, stage, &spv_options);

    const char *spirv_messages = glslang_program_SPIRV_get_messages(program);
    if (spirv_messages) {
        printf("%s\b", spirv_messages);
    }

    size_t num_program_bytes =
        glslang_program_SPIRV_get_size(program) * sizeof(uint32_t);

    guint8 *data = g_malloc(num_program_bytes);
    glslang_program_SPIRV_get(program, (unsigned int *)data);

    glslang_program_delete(program);
    glslang_shader_delete(shader);

    return g_byte_array_new_take(data, num_program_bytes);
}

GByteArray *pgraph_vk_compile_glsl_to_spv(PGRAPHVkState *r,
                                          glslang_stage_t stage,
                                          const char *glsl_source)
{
    g_return_val_if_fail(r != NULL, NULL);

    PGRAPHVkGlslCompileConfig config = {
        .api_version = r->vk_api_version,
        .debug_shaders = g_config.display.vulkan.debug_shaders,
    };
    if (r->hybrid_compiler_initialized) {
        uint64_t ticket = pgraph_vk_hybrid_allocate_ticket(
            &r->hybrid_ticket_allocator);
        PGRAPHVkHybridCompileResult result = { 0 };
        PGRAPHVkHybridCompileRequest request = {
            .generation = r->hybrid_generation,
            .ticket = ticket,
            .stage = stage,
            .glsl = glsl_source,
            .glsl_size = strlen(glsl_source) + 1,
            .config = &config,
            .config_size = sizeof(config),
        };
        bool submitted = ticket && pgraph_vk_hybrid_compiler_submit_blocking(
            &r->hybrid_compiler, &request, &result);
        if (r->hybrid_trace && submitted) {
            pgraph_vk_hybrid_trace_record(
                r->hybrid_trace, VK_HYBRID_TRACE_REQUIRED_COMPILE,
                result.speculative_active_at_start, stage,
                fast_hash((const uint8_t *)glsl_source,
                          request.glsl_size - 1),
                ticket, result.submitted_us, result.started_us,
                result.finished_us, result.caller_return_us);
        }
        if (!submitted ||
            !result.success || result.generation != request.generation ||
            result.ticket != request.ticket || result.stage != request.stage) {
            pgraph_vk_hybrid_compile_result_destroy(&result);
            return NULL;
        }

        GByteArray *spirv = g_byte_array_new_take(result.spirv,
                                                   result.spirv_size);
        result.spirv = NULL;
        result.spirv_size = 0;
        pgraph_vk_hybrid_compile_result_destroy(&result);
        return spirv;
    }
    return pgraph_vk_compile_glsl_to_spv_config(&config, stage, glsl_source);
}

static bool block_to_uniforms(const SpvReflectBlockVariable *block,
                              ShaderUniformLayout *layout, bool *block_seen)
{
    if (!pgraph_vk_spirv_layout_claim_block(block_seen, block->size,
                                            block->member_count) ||
        (block->member_count && !block->members)) {
        return false;
    }

    for (uint32_t k = 0; k < block->member_count; k++) {
        const SpvReflectBlockVariable *member = &block->members[k];
        if (!member->name ||
            strnlen(member->name,
                    PGRAPH_VK_SPIRV_LAYOUT_MAX_NAME_SIZE + 1) >
                PGRAPH_VK_SPIRV_LAYOUT_MAX_NAME_SIZE) {
            return false;
        }
        PGRAPHVkSpirvLayoutMember reflected_member = {
            .offset = member->offset,
            .size = member->size,
            .array_dims_count = member->array.dims_count,
            .array_elements = member->array.dims_count ?
                                  member->array.dims[0] : 1,
            .array_stride = member->array.stride,
            .matrix_columns = member->numeric.matrix.column_count,
            .matrix_rows = member->numeric.matrix.row_count,
            .matrix_stride = member->numeric.matrix.stride,
            .vector_components = member->numeric.vector.component_count,
            .scalar_width = member->numeric.scalar.width,
        };
        PGRAPHVkSpirvLayoutDimensions dimensions;
        if (!pgraph_vk_spirv_layout_validate_member(
                block->size, &reflected_member, &dimensions)) {
            return false;
        }
    }

    layout->num_uniforms = block->member_count;
    layout->uniforms =
        g_try_malloc0_n(block->member_count, sizeof(ShaderUniform));
    layout->total_size = block->size;
    layout->allocation = g_try_malloc0(block->size);
    if ((block->member_count && !layout->uniforms) ||
        (block->size && !layout->allocation)) {
        shader_uniform_layout_clear(layout);
        return false;
    }

    for (uint32_t k = 0; k < block->member_count; ++k) {
        const SpvReflectBlockVariable *member = &block->members[k];
        PGRAPHVkSpirvLayoutMember reflected_member = {
            .offset = member->offset,
            .size = member->size,
            .array_dims_count = member->array.dims_count,
            .array_elements = member->array.dims_count ?
                                  member->array.dims[0] : 1,
            .array_stride = member->array.stride,
            .matrix_columns = member->numeric.matrix.column_count,
            .matrix_rows = member->numeric.matrix.row_count,
            .matrix_stride = member->numeric.matrix.stride,
            .vector_components = member->numeric.vector.component_count,
            .scalar_width = member->numeric.scalar.width,
        };
        PGRAPHVkSpirvLayoutDimensions dimensions;
        if (!pgraph_vk_spirv_layout_validate_member(
                block->size, &reflected_member, &dimensions)) {
            shader_uniform_layout_clear(layout);
            return false;
        }
        char *name = strdup(member->name);
        if (!name) {
            shader_uniform_layout_clear(layout);
            return false;
        }
        layout->uniforms[k] = (ShaderUniform) {
            .name = name,
            .offset = member->offset,
            .dim_v = dimensions.vector_components,
            .dim_a = dimensions.element_count,
            .stride = dimensions.stride,
        };

        // fprintf(stderr, "<%s offset=%zd dim_v=%zd dim_a=%zd stride=%zd>\n",
        //     layout->uniforms[k].name,
        //     layout->uniforms[k].offset,
        //     layout->uniforms[k].dim_v,
        //     layout->uniforms[k].dim_a,
        //     layout->uniforms[k].stride
        //     );
    }
    // fprintf(stderr, "--\n");
    return true;
}

bool pgraph_vk_uber_controls_block_matches_abi(
    const SpvReflectBlockVariable *block)
{
    static const struct {
        const char *name;
        uint32_t offset;
        uint32_t size;
        uint32_t array_elements;
        uint32_t array_stride;
        uint32_t vector_components;
        SpvReflectTypeFlags scalar_type;
        bool unsigned_integer;
    } expected[] = {
        { "uberHeader", 0, 16, 1, 0, 4, SPV_REFLECT_TYPE_FLAG_INT, true },
        { "uberStage", 16, 128, 8, 16, 4, SPV_REFLECT_TYPE_FLAG_INT, true },
        { "uberFinal", 144, 16, 1, 0, 4, SPV_REFLECT_TYPE_FLAG_INT, true },
        { "uberConstants", 160, 288, 18, 16, 4,
          SPV_REFLECT_TYPE_FLAG_FLOAT, false },
    };

    if (!block || block->size != sizeof(PGRAPHUberControls) ||
        block->member_count != ARRAY_SIZE(expected) || !block->members) {
        return false;
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(expected); i++) {
        const SpvReflectBlockVariable *member = &block->members[i];
        const SpvReflectTypeDescription *type = member->type_description;
        uint32_t array_elements = member->array.dims_count ?
                                      member->array.dims[0] : 1;

        if (!member->name || strcmp(member->name, expected[i].name) ||
            !type ||
            (type->type_flags & (SPV_REFLECT_TYPE_FLAG_INT |
                                 SPV_REFLECT_TYPE_FLAG_FLOAT |
                                 SPV_REFLECT_TYPE_FLAG_VECTOR)) !=
                (expected[i].scalar_type | SPV_REFLECT_TYPE_FLAG_VECTOR) ||
            member->offset != expected[i].offset ||
            member->size != expected[i].size ||
            member->array.dims_count !=
                (expected[i].array_elements == 1 ? 0 : 1) ||
            array_elements != expected[i].array_elements ||
            member->array.stride != expected[i].array_stride ||
            member->numeric.vector.component_count !=
                expected[i].vector_components ||
            member->numeric.scalar.width != 32 ||
            (expected[i].unsigned_integer &&
             member->numeric.scalar.signedness != 0) ||
            member->numeric.matrix.column_count != 0 ||
            member->numeric.matrix.row_count != 0) {
            return false;
        }
    }

    return true;
}

void pgraph_vk_clear_shader_module_layout(ShaderModuleInfo *info)
{
    if (!info) {
        return;
    }
    shader_uniform_layout_clear(&info->uniforms);
    shader_uniform_layout_clear(&info->push_constants);
    g_free(info->descriptor_sets);
    info->descriptor_sets = NULL;
    if (info->reflect_module_initialized) {
        spvReflectDestroyShaderModule(&info->reflect_module);
        info->reflect_module_initialized = false;
    }
    info->uses_uber_controls = false;
}

bool pgraph_vk_init_shader_module_layout_from_spv(
    ShaderModuleInfo *info, VkShaderStageFlagBits expected_stage)
{
    if (!info || !info->spirv) {
        return false;
    }
    SpvReflectResult result = spvReflectCreateShaderModule(
        info->spirv->len, info->spirv->data, &info->reflect_module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }
    info->reflect_module_initialized = true;
    if (!pgraph_vk_spirv_stage_matches(
            expected_stage, info->reflect_module.shader_stage) ||
        info->reflect_module.entry_point_count != 1 ||
        !info->reflect_module.entry_point_name ||
        strcmp(info->reflect_module.entry_point_name, "main")) {
        goto fail;
    }

    uint32_t descriptor_set_count = 0;
    result = spvReflectEnumerateDescriptorSets(&info->reflect_module,
                                               &descriptor_set_count, NULL);
    if (result != SPV_REFLECT_RESULT_SUCCESS ||
        descriptor_set_count > PGRAPH_VK_SPIRV_LAYOUT_MAX_DESCRIPTOR_SETS) {
        goto fail;
    }

    info->descriptor_sets = g_try_malloc_n(
        descriptor_set_count, sizeof(SpvReflectDescriptorSet *));
    if (descriptor_set_count && !info->descriptor_sets) {
        goto fail;
    }
    result = spvReflectEnumerateDescriptorSets(
        &info->reflect_module, &descriptor_set_count, info->descriptor_sets);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        goto fail;
    }

    info->uniforms.num_uniforms = 0;
    info->uniforms.uniforms = NULL;

    bool uniform_block_seen = false;
    bool uber_block_seen = false;
    uint32_t total_binding_count = 0;
    for (uint32_t i = 0; i < descriptor_set_count; ++i) {
        const SpvReflectDescriptorSet *descriptor_set =
            info->descriptor_sets[i];
        if (!descriptor_set ||
            descriptor_set->binding_count >
                PGRAPH_VK_SPIRV_LAYOUT_MAX_BINDINGS - total_binding_count ||
            (descriptor_set->binding_count && !descriptor_set->bindings)) {
            goto fail;
        }
        total_binding_count += descriptor_set->binding_count;
        for (uint32_t j = 0; j < descriptor_set->binding_count; ++j) {
            const SpvReflectDescriptorBinding *binding =
                descriptor_set->bindings[j];
            if (!binding) {
                goto fail;
            }
            if (binding->binding == PGRAPH_VK_PSH_UBER_UBO_BINDING) {
                if (expected_stage != VK_SHADER_STAGE_FRAGMENT_BIT ||
                    descriptor_set->set != 0 ||
                    binding->descriptor_type !=
                        SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                    uber_block_seen ||
                    !pgraph_vk_uber_controls_block_matches_abi(
                        &binding->block)) {
                    goto fail;
                }
                uber_block_seen = true;
                continue;
            }
            if (binding->descriptor_type !=
                SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                continue;
            }

            const SpvReflectBlockVariable *block = &binding->block;
            if (!block_to_uniforms(block, &info->uniforms,
                                   &uniform_block_seen)) {
                goto fail;
            }
        }
    }
    info->uses_uber_controls = uber_block_seen;

    info->push_constants.num_uniforms = 0;
    info->push_constants.uniforms = NULL;
    if (info->reflect_module.push_constant_block_count > 1 ||
        (info->reflect_module.push_constant_block_count &&
         !info->reflect_module.push_constant_blocks)) {
        goto fail;
    }
    if (info->reflect_module.push_constant_block_count) {
        bool push_block_seen = false;
        if (!block_to_uniforms(&info->reflect_module.push_constant_blocks[0],
                               &info->push_constants, &push_block_seen)) {
            goto fail;
        }
    }
    return true;

fail:
    pgraph_vk_clear_shader_module_layout(info);
    return false;
}

static void shader_module_info_free(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    if (!info) {
        return;
    }
    if (info->module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(r->device, info->module, NULL);
    }
    pgraph_vk_clear_shader_module_layout(info);
    free(info->glsl);
    if (info->spirv) {
        g_byte_array_unref(info->spirv);
    }
    g_free(info);
}

static glslang_stage_t vk_shader_stage_to_glslang_stage(VkShaderStageFlagBits stage)
{
    switch (stage) {
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return GLSLANG_STAGE_GEOMETRY;
    case VK_SHADER_STAGE_VERTEX_BIT:
        return GLSLANG_STAGE_VERTEX;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return GLSLANG_STAGE_FRAGMENT;
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return GLSLANG_STAGE_COMPUTE;
    default:
        assert(0);
    }
}

ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl)
{
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_SHADER_COMPILE);
    GByteArray *spirv = pgraph_vk_compile_glsl_to_spv(
        r, vk_shader_stage_to_glslang_stage(stage), glsl);
    if (!spirv) {
        return NULL;
    }
    ShaderModuleInfo *info = pgraph_vk_create_shader_module_from_spirv(
        r, stage, glsl, spirv);
    g_byte_array_unref(spirv);
    if (!info) {
        error_report("nv2a/vk: failed to construct freshly compiled shader");
        abort();
    }
    return info;
}

ShaderModuleInfo *pgraph_vk_create_shader_module_from_spirv(
    PGRAPHVkState *r, VkShaderStageFlagBits expected_stage, const char *glsl,
    GByteArray *spirv)
{
    if (!r || !glsl || !spirv) {
        return NULL;
    }
    ShaderModuleInfo *info = g_try_malloc0(sizeof(*info));
    if (!info) {
        return NULL;
    }
    info->refcnt = 0;
    info->glsl = strdup(glsl);
    info->spirv = g_byte_array_ref(spirv);
    if (!info->glsl || !pgraph_vk_init_shader_module_layout_from_spv(
                           info, expected_stage)) {
        shader_module_info_free(r, info);
        return NULL;
    }

    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv->len,
        .pCode = (uint32_t *)spirv->data,
    };
    if (vkCreateShaderModule(r->device, &create_info, NULL, &info->module) !=
        VK_SUCCESS) {
        shader_module_info_free(r, info);
        return NULL;
    }
    return info;
}

void pgraph_vk_ref_shader_module(ShaderModuleInfo *info)
{
    info->refcnt++;
}

void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    assert(info->refcnt >= 1);

    info->refcnt--;
    if (info->refcnt == 0) {
        pgraph_vk_destroy_shader_module(r, info);
    }
}

void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    assert(info->refcnt == 0);
    shader_module_info_free(r, info);
}

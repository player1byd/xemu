/*
 * NV2A Vulkan hybrid fallback-family cache ownership
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"

#include "renderer.h"
#include "hybrid-family-codec.h"
#include "hybrid-ready.h"
#include "pipeline-key.h"

void pgraph_vk_init_pipeline_key_for_state(
    PGRAPHState *pg, const ShaderState *shader_state,
    PGRAPHVkFragmentRoute route, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    key->render_pass_state.color_format = r->color_binding ?
        r->color_binding->host_fmt.vk_format : VK_FORMAT_UNDEFINED;
    key->render_pass_state.zeta_format = r->zeta_binding ?
        r->zeta_binding->host_fmt.vk_format : VK_FORMAT_UNDEFINED;
    pgraph_vk_pipeline_key_set_shader(key, shader_state, route);
    key->binding_description_count =
        r->num_active_vertex_binding_descriptions;
    key->attribute_description_count =
        r->num_active_vertex_attribute_descriptions;
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_CONTROL_0,   NV_PGRAPH_CONTROL_1,
        NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,   NV_PGRAPH_SETUPRASTER,
        NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
    pgraph_vk_pipeline_key_canonicalize_uniform_regs(key);
}

static PGRAPHVkReadyDrawCandidate probe_ready_draw_candidate(
    PGRAPHState *pg, const ShaderState *state, PGRAPHVkFragmentRoute route)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderBindingKey shader_key = {
        .state = *state,
        .fragment_route = route,
    };
    uint64_t shader_hash = fast_hash((const uint8_t *)&shader_key,
                                     sizeof(shader_key));
    PGRAPHVkReadyDrawCandidate candidate = { 0 };
    candidate.shader = pgraph_vk_shader_binding_find_ready(
        &r->shader_cache, shader_hash, &shader_key,
        pgraph_glsl_need_geom(&state->geom));
    pgraph_vk_init_pipeline_key_for_state(pg, state, route, &candidate.key);
    candidate.hash = fast_hash((const uint8_t *)&candidate.key,
                               sizeof(candidate.key));
    candidate.pipeline = pgraph_vk_pipeline_cache_find_ready(
        &r->pipeline_cache, candidate.hash, &candidate.key);
    if (r->hybrid_trace) {
        uint64_t state_hash = fast_hash((const uint8_t *)state,
                                        sizeof(*state));
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_SHADER_BINDING_PROBE,
            route, candidate.hash, state_hash, 0,
            candidate.shader != NULL, 0, shader_hash, 2);
        pgraph_vk_hybrid_trace_record(
            r->hybrid_trace, VK_HYBRID_TRACE_PIPELINE_PROBE,
            route, candidate.hash, state_hash, 0,
            candidate.pipeline != NULL, 0, candidate.shader != NULL, 2);
    }
    return candidate;
}

static uint64_t shader_module_key_hash(const ShaderModuleCacheKey *key)
{
    return fast_hash((const uint8_t *)key,
                     pgraph_vk_shader_module_key_active_size(key));
}

static ShaderModuleCacheEntry *find_shader_module_for_key(
    PGRAPHVkState *r, const ShaderModuleCacheKey *key)
{
    LruNode *node = lru_find_existing(
        &r->shader_module_cache, shader_module_key_hash(key), key);
    return node ? container_of(node, ShaderModuleCacheEntry, node) : NULL;
}

static void init_fragment_module_key(ShaderModuleCacheKey *key,
                                     const PshState *state,
                                     PGRAPHVkFragmentRoute route)
{
    memset(key, 0, sizeof(*key));
    key->kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key->fragment_route = route;
    key->psh.state = *state;
    if (route == PGRAPH_VK_FRAGMENT_UBERSHADER) {
        pgraph_vk_canonicalize_uber_combiner_state(&key->psh.state);
    }
    key->psh.glsl_opts.vulkan = true;
    key->psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key->psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    key->psh.glsl_opts.ubershader =
        route == PGRAPH_VK_FRAGMENT_UBERSHADER;
    key->psh.glsl_opts.uber_binding = PGRAPH_VK_PSH_UBER_UBO_BINDING;
}

/* This path only creates binding metadata after all required modules exist;
 * it cannot enter glslang or Vulkan shader-module creation. */
ShaderBinding *pgraph_vk_prepare_binding_from_ready_modules(
    PGRAPHState *pg, const ShaderState *state, PGRAPHVkFragmentRoute route)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderBindingKey binding_key = {
        .state = *state,
        .fragment_route = route,
    };
    uint64_t hash = fast_hash((const uint8_t *)&binding_key,
                              sizeof(binding_key));
    LruNode *node = lru_find_existing(&r->shader_cache, hash, &binding_key);
    if (node) {
        return container_of(node, ShaderBinding, node);
    }

    bool need_geom = pgraph_glsl_need_geom(&state->geom);
    ShaderModuleCacheKey module_key;
    if (need_geom) {
        memset(&module_key, 0, sizeof(module_key));
        module_key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        module_key.geom.state = state->geom;
        module_key.geom.glsl_opts.vulkan = true;
        ShaderModuleCacheEntry *entry =
            find_shader_module_for_key(r, &module_key);
        if (!entry || !entry->module_info) {
            return NULL;
        }
    }

    memset(&module_key, 0, sizeof(module_key));
    module_key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    module_key.vsh.state = state->vsh;
    module_key.vsh.glsl_opts.vulkan = true;
    module_key.vsh.glsl_opts.prefix_outputs = need_geom;
    module_key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    module_key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    ShaderModuleCacheEntry *entry =
        find_shader_module_for_key(r, &module_key);
    if (!entry || !entry->module_info) {
        return NULL;
    }

    init_fragment_module_key(&module_key, &state->psh, route);
    entry = find_shader_module_for_key(r, &module_key);
    if (!entry || !entry->module_info) {
        return NULL;
    }
    node = lru_try_lookup(&r->shader_cache, hash, &binding_key);
    return node ? container_of(node, ShaderBinding, node) : NULL;
}

PGRAPHVkFallbackShaderPreparation
pgraph_vk_fallback_family_prepare_shader(
    void *opaque, PGRAPHVkFallbackBindingProbeFunc probe_binding,
    PGRAPHVkFallbackFragmentPrepareFunc prepare_fragment,
    ShaderBinding **binding)
{
    assert(probe_binding);
    assert(prepare_fragment);
    assert(binding);

    *binding = probe_binding(opaque);
    if (*binding) {
        return PGRAPH_VK_FALLBACK_SHADER_READY;
    }
    if (!prepare_fragment(opaque)) {
        return PGRAPH_VK_FALLBACK_SHADER_REJECTED;
    }

    /* A persistent SPIR-V hit publishes the missing module synchronously.
     * Re-probe once so this service pass can submit its graphics pipeline. */
    *binding = probe_binding(opaque);
    return *binding ? PGRAPH_VK_FALLBACK_SHADER_READY :
                      PGRAPH_VK_FALLBACK_SHADER_WAITING;
}

static void get_uber_control_source(PGRAPHState *pg, const PshState *state,
                                    PGRAPHUberControlSource *source)
{
    memset(source, 0, sizeof(*source));
    source->combiner_control = state->combiner_control;
    memcpy(source->rgb_inputs, state->rgb_inputs, sizeof(source->rgb_inputs));
    memcpy(source->alpha_inputs, state->alpha_inputs,
           sizeof(source->alpha_inputs));
    memcpy(source->rgb_outputs, state->rgb_outputs,
           sizeof(source->rgb_outputs));
    memcpy(source->alpha_outputs, state->alpha_outputs,
           sizeof(source->alpha_outputs));
    source->final_inputs_0 = state->final_inputs_0;
    source->final_inputs_1 = state->final_inputs_1;

    for (int i = 0; i < 9; i++) {
        uint32_t packed[2];
        if (i == 8) {
            packed[0] = pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR0);
            packed[1] = pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR1);
        } else {
            packed[0] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0 + i * 4);
            packed[1] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR1 + i * 4);
        }
        for (int j = 0; j < 2; j++) {
            pgraph_argb_pack32_to_rgba_float(
                packed[j], source->constants[i * 2 + j]);
        }
    }
}

bool pgraph_vk_pack_fallback_controls(PGRAPHState *pg,
                                      const PshState *state,
                                      PGRAPHUberControls *packet)
{
    PGRAPHUberControlSource source;
    get_uber_control_source(pg, state, &source);
    return pgraph_vk_pack_ubershader_controls(packet, &source, NULL);
}

void pgraph_vk_publish_fallback_controls(PGRAPHVkState *r,
                                         const PGRAPHUberControls *packet)
{
    r->uber_controls = *packet;
    r->uber_controls_valid = true;
    /* This path publishes a packet without its raw constant-register key.
     * Force the next stable-binding refresh to associate the two again. */
    r->uber_constant_regs_valid = false;
}

/* The admitted program is unchanged on a stable fallback binding. Compare
 * the raw constant registers so snapshot/restoration or direct state writes
 * cannot rely on a dirty hint to refresh the 448-byte control packet. */
bool pgraph_vk_refresh_fallback_controls(PGRAPHState *pg,
                                         const PshState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint32_t current[18];
    for (unsigned int i = 0; i < 8; i++) {
        current[i * 2] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0 + i * 4);
        current[i * 2 + 1] =
            pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR1 + i * 4);
    }
    current[16] = pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR0);
    current[17] = pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR1);
    if (r->uber_controls_valid && r->uber_constant_regs_valid &&
        memcmp(current, r->uber_constant_regs, sizeof(current)) == 0) {
        return true;
    }
    r->uber_controls_valid = pgraph_vk_pack_fallback_controls(
        pg, state, &r->uber_controls);
    if (r->uber_controls_valid) {
        memcpy(r->uber_constant_regs, current, sizeof(current));
        r->uber_constant_regs_valid = true;
    } else {
        r->uber_constant_regs_valid = false;
    }
    return r->uber_controls_valid;
}

void pgraph_vk_resolve_ready_execution_candidates(
    PGRAPHState *pg, const ShaderState *state, bool force_ubershader,
    PGRAPHVkReadyExecutionCandidates *candidates)
{
    memset(candidates, 0, sizeof(*candidates));
    if (force_ubershader) {
        candidates->controls_checked = true;
        candidates->controls_supported = pgraph_vk_pack_fallback_controls(
            pg, &state->psh, &candidates->controls);
        if (candidates->controls_supported) {
            candidates->fallback = probe_ready_draw_candidate(
                pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER);
            if (!candidates->fallback.shader &&
                candidates->fallback.pipeline &&
                pgraph_vk_prepare_binding_from_ready_modules(
                    pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER)) {
                candidates->fallback = probe_ready_draw_candidate(
                    pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER);
            }
            return;
        }

        /* The interpreter rejected this state. Keep the emulator usable by
         * resolving the normal specialized executable as a safety path. */
        candidates->specialized = probe_ready_draw_candidate(
            pg, state, PGRAPH_VK_FRAGMENT_SPECIALIZED);
        return;
    }

    candidates->specialized = probe_ready_draw_candidate(
        pg, state, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    if (candidates->specialized.shader &&
        candidates->specialized.pipeline) {
        return;
    }

    candidates->fallback = probe_ready_draw_candidate(
        pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER);
    candidates->controls_checked = true;
    candidates->controls_supported = pgraph_vk_pack_fallback_controls(
        pg, &state->psh, &candidates->controls);
    if (pgraph_vk_hybrid_should_prepare_fallback_binding(
            candidates->specialized.shader != NULL,
            candidates->specialized.pipeline != NULL,
            candidates->fallback.shader != NULL,
            candidates->fallback.pipeline != NULL,
            candidates->controls_supported) &&
        pgraph_vk_prepare_binding_from_ready_modules(
            pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER)) {
        candidates->specialized = probe_ready_draw_candidate(
            pg, state, PGRAPH_VK_FRAGMENT_SPECIALIZED);
        candidates->fallback = probe_ready_draw_candidate(
            pg, state, PGRAPH_VK_FRAGMENT_UBERSHADER);
    }
}

void pgraph_vk_pipeline_family_set_state(PGRAPHVkState *r,
                                         PipelineBinding *binding,
                                         PGRAPHVkFamilyLearnState state)
{
    if (binding->family_learn_state == state) {
        return;
    }
    if (binding->family_learn_state == PGRAPH_VK_FAMILY_RETRY_PENDING) {
        assert(r->fallback_family_retry_count > 0);
        r->fallback_family_retry_count--;
    }
    binding->family_learn_state = state;
    if (state == PGRAPH_VK_FAMILY_RETRY_PENDING) {
        r->fallback_family_retry_count++;
    }
}

void pgraph_vk_pipeline_family_owner_evict(PGRAPHVkState *r,
                                           PipelineBinding *binding)
{
    if (binding->family_learn_state == PGRAPH_VK_FAMILY_RETRY_PENDING) {
        assert(r->fallback_family_retry_count > 0);
        r->fallback_family_retry_count--;
    }
}

void pgraph_vk_track_specialized_fallback_family(
    PGRAPHVkState *r, PipelineBinding *owner,
    bool controls_supported, bool fallback_pipeline_ready,
    uint64_t synchronous_create_us)
{
    if (!owner || owner->pipeline == VK_NULL_HANDLE || owner->key.clear ||
        owner->key.fragment_route != PGRAPH_VK_FRAGMENT_SPECIALIZED) {
        return;
    }

    if (!controls_supported) {
        pgraph_vk_pipeline_family_set_state(
            r, owner, PGRAPH_VK_FAMILY_REJECTED);
    } else if (fallback_pipeline_ready) {
        pgraph_vk_pipeline_family_set_state(r, owner, PGRAPH_VK_FAMILY_READY);
    } else if (owner->family_learn_state == PGRAPH_VK_FAMILY_UNCHECKED) {
        if (r->fallback_family_history_initialized) {
            PipelineKey family_key;
            PGRAPHVkFamilyKeyBlob blob = { 0 };
            pgraph_vk_fallback_family_key_from_specialized(
                owner, &family_key);
            if (pgraph_vk_family_key_encode(&family_key, &blob)) {
                pgraph_vk_family_history_note_cold_miss(
                    &r->fallback_family_history, blob.data, blob.size,
                    synchronous_create_us);
                pgraph_vk_family_key_blob_destroy(&blob);
            }
        }
        pgraph_vk_pipeline_family_set_state(
            r, owner, PGRAPH_VK_FAMILY_RETRY_PENDING);
    }
}

void pgraph_vk_note_interpreter_family(PGRAPHVkState *r,
                                      const PipelineKey *key,
                                      uint64_t synchronous_create_us)
{
    if (!r->fallback_family_history_initialized || !key || key->clear ||
        key->fragment_route != PGRAPH_VK_FRAGMENT_UBERSHADER) {
        return;
    }
    PGRAPHVkFamilyKeyBlob blob = { 0 };
    if (pgraph_vk_family_key_encode(key, &blob)) {
        /* Called only after the first synchronous executable construction;
         * warm draws never serialize or update history. */
        pgraph_vk_family_history_note_cold_miss(
            &r->fallback_family_history, blob.data, blob.size,
            synchronous_create_us);
        pgraph_vk_family_key_blob_destroy(&blob);
    }
}

void pgraph_vk_fallback_family_key_from_specialized(
    const PipelineBinding *binding, PipelineKey *key)
{
    assert(binding->key.fragment_route == PGRAPH_VK_FRAGMENT_SPECIALIZED);
    *key = binding->key;
    pgraph_vk_pipeline_key_set_shader(
        key, &binding->key.shader_state, PGRAPH_VK_FRAGMENT_UBERSHADER);
}

void pgraph_vk_fallback_family_mark_pipeline_owners(
    PGRAPHVkState *r, const PipelineKey *family_key,
    PGRAPHVkFamilyLearnState state)
{
    size_t capacity = r->pipeline_cache.num_used +
                      r->pipeline_cache.num_free;
    for (size_t i = 0; i < capacity; i++) {
        PipelineBinding *binding = &r->pipeline_cache_entries[i];
        if (!lru_is_node_in_use(&r->pipeline_cache, &binding->node) ||
            binding->key.clear ||
            binding->key.fragment_route != PGRAPH_VK_FRAGMENT_SPECIALIZED) {
            continue;
        }
        if (binding->family_learn_state != PGRAPH_VK_FAMILY_RETRY_PENDING &&
            binding->family_learn_state != PGRAPH_VK_FAMILY_TRACKED) {
            continue;
        }
        PipelineKey candidate;
        pgraph_vk_fallback_family_key_from_specialized(binding, &candidate);
        if (memcmp(&candidate, family_key, sizeof(candidate)) == 0) {
            pgraph_vk_pipeline_family_set_state(r, binding, state);
        }
    }
}

static PGRAPHVkFallbackFamilyRequest *fallback_family_find_request(
    PGRAPHVkState *r, const PipelineKey *key)
{
    for (size_t i = 0; i < ARRAY_SIZE(r->fallback_family_requests); i++) {
        PGRAPHVkFallbackFamilyRequest *request =
            &r->fallback_family_requests[i];
        if (request->in_use &&
            memcmp(&request->key, key, sizeof(*key)) == 0) {
            return request;
        }
    }
    return NULL;
}

void pgraph_vk_fallback_family_finish_request(
    PGRAPHVkState *r, PGRAPHVkFallbackFamilyRequest *request,
    PGRAPHVkFamilyLearnState state)
{
    pgraph_vk_fallback_family_mark_pipeline_owners(r, &request->key, state);
    request->status = state == PGRAPH_VK_FAMILY_READY ?
        PGRAPH_VK_FAMILY_REQUEST_READY :
        PGRAPH_VK_FAMILY_REQUEST_REJECTED;
    request->in_use = false;
}

void pgraph_vk_fallback_family_note_pipeline_ready(
    PGRAPHVkState *r, const PipelineKey *key)
{
    if (key->fragment_route != PGRAPH_VK_FRAGMENT_UBERSHADER) {
        return;
    }
    PGRAPHVkFallbackFamilyRequest *request =
        fallback_family_find_request(r, key);
    if (request) {
        pgraph_vk_fallback_family_finish_request(
            r, request, PGRAPH_VK_FAMILY_READY);
    } else {
        pgraph_vk_fallback_family_mark_pipeline_owners(
            r, key, PGRAPH_VK_FAMILY_READY);
    }
}

void pgraph_vk_fallback_family_note_pipeline_failure_at(
    PGRAPHVkState *r, const PipelineKey *key, int64_t now_us)
{
    if (key->fragment_route != PGRAPH_VK_FRAGMENT_UBERSHADER) {
        return;
    }
    PGRAPHVkFallbackFamilyRequest *request =
        fallback_family_find_request(r, key);
    if (!request) {
        return;
    }
    bool retained = pgraph_vk_fallback_family_note_pipeline_failure(
        request, now_us);
    pgraph_vk_fallback_family_mark_pipeline_owners(
        r, key, retained ? PGRAPH_VK_FAMILY_TRACKED :
                           PGRAPH_VK_FAMILY_REJECTED);
}

void pgraph_vk_enqueue_retained_fallback_families(PGRAPHVkState *r)
{
    if (!r->fallback_family_retry_count) {
        return;
    }
    size_t capacity = r->pipeline_cache.num_used +
                      r->pipeline_cache.num_free;
    unsigned int visited = 0;
    unsigned int processed = 0;
    while (visited < capacity && processed < 2 &&
           r->fallback_family_retry_count) {
        unsigned int index = r->fallback_family_pipeline_cursor++ % capacity;
        PipelineBinding *binding = &r->pipeline_cache_entries[index];
        visited++;
        if (!lru_is_node_in_use(&r->pipeline_cache, &binding->node) ||
            binding->family_learn_state !=
                PGRAPH_VK_FAMILY_RETRY_PENDING ||
            binding->pipeline == VK_NULL_HANDLE || binding->key.clear ||
            binding->key.fragment_route !=
                PGRAPH_VK_FRAGMENT_SPECIALIZED) {
            continue;
        }
        processed++;
        PipelineKey family_key;
        pgraph_vk_fallback_family_key_from_specialized(binding, &family_key);
        bool queued = pgraph_vk_fallback_family_enqueue(
            r->fallback_family_requests,
            ARRAY_SIZE(r->fallback_family_requests), &family_key,
            &binding->key.shader_state, false);
        if (queued) {
            pgraph_vk_pipeline_family_set_state(
                r, binding, PGRAPH_VK_FAMILY_TRACKED);
        }
    }
}

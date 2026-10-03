/*
 * NV2A Vulkan ubershader runtime-key regression tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"

#include "hw/xbox/nv2a/pgraph/vk/renderer.h"
#include "hw/xbox/nv2a/pgraph/vk/hybrid-ready.h"
#include "hw/xbox/nv2a/pgraph/vk/pipeline-key.h"
#include "hw/xbox/nv2a/pgraph/polygon-offset.h"
#include "hw/xbox/nv2a/pgraph/psh_regs.h"

static unsigned int probe_inits;
static unsigned int probe_evictions;
static bool probe_shader_populate_modules;
static ShaderModuleInfo probe_vertex_module;
static ShaderModuleInfo probe_fragment_module;

static ShaderState base_state(void);
static PipelineKey pipeline_key(PGRAPHVkFragmentRoute route,
                                const ShaderState *state);

static void probe_pipeline_init(Lru *cache, LruNode *node, const void *key)
{
    PipelineBinding *binding = container_of(node, PipelineBinding, node);

    (void)cache;
    probe_inits++;
    binding->key = *(const PipelineKey *)key;
    binding->pipeline = VK_NULL_HANDLE;
}

static bool probe_pipeline_different(Lru *cache, LruNode *node,
                                     const void *key)
{
    PipelineBinding *binding = container_of(node, PipelineBinding, node);

    (void)cache;
    return memcmp(&binding->key, key, sizeof(binding->key)) != 0;
}

static void probe_post_evict(Lru *cache, LruNode *node)
{
    (void)cache;
    (void)node;
    probe_evictions++;
}

static void test_pipeline_ready_probe_is_side_effect_free(void)
{
    static Lru cache;
    PipelineBinding entries[2] = { 0 };
    PipelineKey first = { .fragment_route = PGRAPH_VK_FRAGMENT_SPECIALIZED };
    PipelineKey second = { .fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER };
    PipelineKey missing = first;
    const uint64_t first_hash = 17;
    const uint64_t second_hash = 18;

    missing.regs[0] = 1;
    probe_inits = probe_evictions = 0;
    lru_init(&cache);
    cache.init_node = probe_pipeline_init;
    cache.compare_nodes = probe_pipeline_different;
    cache.post_node_evict = probe_post_evict;
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        lru_add_free(&cache, &entries[i].node);
    }
    LruNode *first_node = lru_lookup(&cache, first_hash, &first);
    LruNode *second_node = lru_lookup(&cache, second_hash, &second);
    PipelineBinding *first_binding =
        container_of(first_node, PipelineBinding, node);

    g_assert_null(pgraph_vk_pipeline_cache_find_ready(
        &cache, first_hash, &first));
    first_binding->pipeline = (VkPipeline)(uintptr_t)1;
    g_assert_true(pgraph_vk_pipeline_cache_find_ready(
        &cache, first_hash, &first) == first_binding);
    g_assert_null(pgraph_vk_pipeline_cache_find_ready(
        &cache, first_hash, &missing));
    g_assert_true(QTAILQ_FIRST(&cache.global) == second_node);
    g_assert_cmpint(cache.num_used, ==, 2);
    g_assert_cmpint(cache.num_free, ==, 0);
    g_assert_cmpuint(probe_inits, ==, 2);
    g_assert_cmpuint(probe_evictions, ==, 0);
}

static void test_pipeline_key_distinguishes_vertex_input_counts(void)
{
    static Lru cache;
    PipelineBinding entry = { 0 };
    PipelineKey one = { .binding_description_count = 1,
                        .attribute_description_count = 1 };
    PipelineKey two = one;
    const uint64_t forced_collision = 31;

    /* The unused array entries can remain byte-identical. Count is still
     * part of the Vulkan vertex-input recipe and must change identity. */
    two.binding_description_count = 2;
    two.attribute_description_count = 2;
    lru_init(&cache);
    cache.init_node = probe_pipeline_init;
    cache.compare_nodes = probe_pipeline_different;
    lru_add_free(&cache, &entry.node);
    PipelineBinding *binding = container_of(
        lru_lookup(&cache, forced_collision, &one), PipelineBinding, node);
    binding->pipeline = (VkPipeline)(uintptr_t)1;

    g_assert_true(pgraph_vk_pipeline_cache_find_ready(
        &cache, forced_collision, &one) == binding);
    g_assert_null(pgraph_vk_pipeline_cache_find_ready(
        &cache, forced_collision, &two));
}

static void test_pipeline_key_ignores_uniform_only_register_values(void)
{
    PipelineKey a = { 0 };
    PipelineKey b = { 0 };
    const uint32_t setup_raster =
        NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL |
        NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE;

    a.regs[0] = b.regs[0] = NV_PGRAPH_BLEND_EN;
    a.regs[1] = 0x12000011;
    b.regs[1] = 0x120000ee;
    a.regs[6] = 0x3f800000;
    b.regs[6] = 0x40000000;
    a.regs[7] = 0x40400000;
    b.regs[7] = 0x40800000;

    g_assert_cmpuint(GET_MASK(a.regs[1], NV_PGRAPH_CONTROL_0_ALPHAREF),
                     !=,
                     GET_MASK(b.regs[1], NV_PGRAPH_CONTROL_0_ALPHAREF));
    PGRAPHPolygonOffsetUniformKey offset_a =
        pgraph_polygon_offset_uniform_key(
            NV097_SET_BEGIN_END_OP_TRIANGLES, setup_raster,
            a.regs[6], a.regs[7]);
    PGRAPHPolygonOffsetUniformKey offset_b =
        pgraph_polygon_offset_uniform_key(
            NV097_SET_BEGIN_END_OP_TRIANGLES, setup_raster,
            b.regs[6], b.regs[7]);
    g_assert_false(pgraph_polygon_offset_uniform_key_equal(
        offset_a, offset_b));

    pgraph_vk_pipeline_key_canonicalize_uniform_regs(&a);
    pgraph_vk_pipeline_key_canonicalize_uniform_regs(&b);
    g_assert_cmpmem(&a, sizeof(a), &b, sizeof(b));

    b.regs[1] ^= NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE;
    pgraph_vk_pipeline_key_canonicalize_uniform_regs(&b);
    g_assert_cmpint(memcmp(&a, &b, sizeof(a)), !=, 0);
}

static void probe_shader_init(Lru *cache, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    const ShaderBindingKey *shader_key = key;

    (void)cache;
    probe_inits++;
    binding->state = shader_key->state;
    binding->fragment_route = shader_key->fragment_route;
    if (probe_shader_populate_modules) {
        binding->vsh.module_info = &probe_vertex_module;
        binding->psh.module_info = &probe_fragment_module;
    }
}

static bool probe_shader_different(Lru *cache, LruNode *node,
                                   const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    const ShaderBindingKey *shader_key = key;

    (void)cache;
    return binding->fragment_route != shader_key->fragment_route ||
           memcmp(&binding->state, &shader_key->state,
                  sizeof(binding->state)) != 0;
}

static void probe_module_init(Lru *cache, LruNode *node, const void *key)
{
    ShaderModuleCacheEntry *entry =
        container_of(node, ShaderModuleCacheEntry, node);

    (void)cache;
    entry->key = *(const ShaderModuleCacheKey *)key;
    entry->module_info = entry->key.kind == VK_SHADER_STAGE_VERTEX_BIT ?
        &probe_vertex_module : &probe_fragment_module;
}

static bool probe_module_different(Lru *cache, LruNode *node,
                                   const void *key)
{
    ShaderModuleCacheEntry *entry =
        container_of(node, ShaderModuleCacheEntry, node);

    (void)cache;
    return !pgraph_vk_shader_module_key_equal(&entry->key, key);
}

static void test_shader_ready_probe_requires_runtime_metadata(void)
{
    static Lru cache;
    ShaderBinding entry = { 0 };
    static ShaderModuleInfo vertex;
    static ShaderModuleInfo fragment;
    static ShaderModuleInfo geometry;
    ShaderBindingKey key = {
        .fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER,
    };
    ShaderBindingKey other = key;
    const uint64_t hash = 29;

    other.fragment_route = PGRAPH_VK_FRAGMENT_SPECIALIZED;
    probe_inits = probe_evictions = 0;
    lru_init(&cache);
    cache.init_node = probe_shader_init;
    cache.compare_nodes = probe_shader_different;
    cache.post_node_evict = probe_post_evict;
    lru_add_free(&cache, &entry.node);
    ShaderBinding *binding = container_of(
        lru_lookup(&cache, hash, &key), ShaderBinding, node);

    g_assert_null(pgraph_vk_shader_binding_find_ready(
        &cache, hash, &key, false));
    binding->vsh.module_info = &vertex;
    g_assert_null(pgraph_vk_shader_binding_find_ready(
        &cache, hash, &key, false));
    binding->psh.module_info = &fragment;
    g_assert_true(pgraph_vk_shader_binding_find_ready(
        &cache, hash, &key, false) == binding);
    g_assert_null(pgraph_vk_shader_binding_find_ready(
        &cache, hash, &key, true));
    binding->geom.module_info = &geometry;
    g_assert_true(pgraph_vk_shader_binding_find_ready(
        &cache, hash, &key, true) == binding);
    g_assert_null(pgraph_vk_shader_binding_find_ready(
        &cache, hash, &other, false));
    g_assert_cmpint(cache.num_used, ==, 1);
    g_assert_cmpint(cache.num_free, ==, 0);
    g_assert_cmpuint(probe_inits, ==, 1);
    g_assert_cmpuint(probe_evictions, ==, 0);
}

static void test_execution_route_requires_complete_candidate(void)
{
    g_assert_true(pgraph_vk_hybrid_fastpath_route_allowed(
        false, PGRAPH_VK_FRAGMENT_SPECIALIZED));
    g_assert_false(pgraph_vk_hybrid_fastpath_route_allowed(
        true, PGRAPH_VK_FRAGMENT_SPECIALIZED));
    g_assert_true(pgraph_vk_hybrid_fastpath_route_allowed(
        true, PGRAPH_VK_FRAGMENT_UBERSHADER));
    g_assert_true(pgraph_vk_hybrid_should_schedule_specialization(
        false, PGRAPH_VK_FRAGMENT_UBERSHADER));
    g_assert_false(pgraph_vk_hybrid_should_schedule_specialization(
        true, PGRAPH_VK_FRAGMENT_UBERSHADER));
    g_assert_false(pgraph_vk_hybrid_should_schedule_specialization(
        false, PGRAPH_VK_FRAGMENT_SPECIALIZED));

    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        false, true, true, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_READY),
                    ==, PGRAPH_VK_EXECUTION_SPECIALIZED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        true, true, true, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_READY),
                    ==, PGRAPH_VK_EXECUTION_UBERSHADER);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        true, true, true, false, false,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_EXECUTION_UNCOVERED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        false, true, false, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_READY),
                    ==, PGRAPH_VK_EXECUTION_UBERSHADER);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        false, false, false, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_READY),
                    ==, PGRAPH_VK_EXECUTION_UBERSHADER);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        false, true, false, true, false,
                        PGRAPH_VK_FALLBACK_RESOURCES_READY),
                    ==, PGRAPH_VK_EXECUTION_UNCOVERED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        false, false, false, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_NEED_ROLLOVER),
                    ==, PGRAPH_VK_EXECUTION_UBERSHADER_AFTER_ROLLOVER);
    g_assert_cmpint(pgraph_vk_hybrid_choose_execution_route(
                        false, false, false, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_EXECUTION_UNCOVERED);
}

static void test_ready_fallback_pipeline_requests_missing_binding(void)
{
    g_assert_true(pgraph_vk_hybrid_should_prepare_fallback_binding(
        true, false, false, true, true));
    g_assert_false(pgraph_vk_hybrid_should_prepare_fallback_binding(
        true, true, false, true, true));
    g_assert_false(pgraph_vk_hybrid_should_prepare_fallback_binding(
        true, false, false, false, true));
    g_assert_false(pgraph_vk_hybrid_should_prepare_fallback_binding(
        true, false, false, true, false));
    g_assert_false(pgraph_vk_hybrid_should_prepare_fallback_binding(
        false, false, true, true, true));

    g_assert_true(pgraph_vk_fallback_family_learning_needed(
        true, PGRAPH_VK_FAMILY_UNCHECKED));
    g_assert_false(pgraph_vk_fallback_family_learning_needed(
        true, PGRAPH_VK_FAMILY_TRACKED));
    g_assert_false(pgraph_vk_fallback_family_learning_needed(
        true, PGRAPH_VK_FAMILY_READY));
    g_assert_false(pgraph_vk_fallback_family_learning_needed(
        true, PGRAPH_VK_FAMILY_REJECTED));
}

static void test_uncovered_build_uses_closer_binding(void)
{
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        false, true, false, false, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        false, false, true, false, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_UBERSHADER);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        false, false, false, false, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        false, true, true, false, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        false, false, true, false, false,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        true, true, false, false, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_UBERSHADER);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        true, true, true, false, false,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        true, true, true, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
    /* The forced resolver normally has no specialized binding. A complete
     * fallback whose draw resources are unavailable must still escape to the
     * synchronous specialized path instead of reselecting that fallback. */
    g_assert_cmpint(pgraph_vk_hybrid_choose_uncovered_route(
                        true, false, true, true, true,
                        PGRAPH_VK_FALLBACK_RESOURCES_UNAVAILABLE),
                    ==, PGRAPH_VK_FRAGMENT_SPECIALIZED);
}

static void test_fallback_promotion_probe_has_time_gate(void)
{
    g_assert_true(pgraph_vk_hybrid_promotion_due(100, 0));
    g_assert_false(pgraph_vk_hybrid_promotion_due(115, 116));
    g_assert_true(pgraph_vk_hybrid_promotion_due(116, 116));
    g_assert_true(pgraph_vk_hybrid_promotion_due(117, 116));
}

static bool publication_allow_evict;

static bool publication_pre_evict(Lru *cache, LruNode *node)
{
    (void)cache;
    (void)node;
    return publication_allow_evict;
}

static void test_pipeline_publication_eviction_is_late(void)
{
    static Lru cache;
    PipelineBinding entry = { 0 };
    PipelineKey old_key = { .regs[0] = 1 };
    PipelineKey new_key = { .regs[0] = 2 };

    lru_init(&cache);
    cache.init_node = probe_pipeline_init;
    cache.compare_nodes = probe_pipeline_different;
    cache.pre_node_evict = publication_pre_evict;
    lru_add_free(&cache, &entry.node);
    PipelineBinding *old = container_of(
        lru_lookup(&cache, 1, &old_key), PipelineBinding, node);
    old->pipeline = (VkPipeline)(uintptr_t)1;
    g_assert_cmpint(cache.num_free, ==, 0);

    /* Work may compile while the old executable remains in the LRU. */
    publication_allow_evict = false;
    g_assert_true(pgraph_vk_pipeline_cache_find_ready(
        &cache, 1, &old_key) == old);
    g_assert_null(pgraph_vk_pipeline_cache_publish_slot(
        &cache, 2, &new_key));
    g_assert_true(pgraph_vk_pipeline_cache_find_ready(
        &cache, 1, &old_key) == old);

    publication_allow_evict = true;
    PipelineBinding *published = pgraph_vk_pipeline_cache_publish_slot(
        &cache, 2, &new_key);
    g_assert_true(published == old);
    g_assert_null(pgraph_vk_pipeline_cache_find_ready(
        &cache, 1, &old_key));
    g_assert_cmpuint(published->key.regs[0], ==, 2);
    g_assert_cmpint(cache.num_used, ==, 1);
    g_assert_cmpint(cache.num_free, ==, 0);
}

static void test_fallback_family_queue_deduplicates_and_bounds(void)
{
    PGRAPHVkFallbackFamilyRequest requests[2] = { 0 };
    ShaderState state = { 0 };
    PipelineKey a = { .regs[0] = 1 };
    PipelineKey b = { .regs[0] = 2 };
    PipelineKey c = { .regs[0] = 3 };

    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &a, &state, false));
    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &a, &state, false));
    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &b, &state, false));
    g_assert_false(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &c, &state, false));
    g_assert_cmpuint(requests[0].key.regs[0], ==, 1);
    g_assert_cmpuint(requests[1].key.regs[0], ==, 2);
    g_assert_cmpint(requests[0].status, ==,
                    PGRAPH_VK_FAMILY_WAITING_FOR_SHADER);
    g_assert_cmpuint(requests[0].attempts, ==, 0);
}

static void test_fallback_family_prewarm_provenance_is_monotonic(void)
{
    PGRAPHVkFallbackFamilyRequest requests[1] = { 0 };
    PGRAPHVkHybridPipelineWork work = { 0 };
    ShaderState state = { 0 };
    PipelineKey key = { .regs[0] = 1 };

    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &key, &state, false));
    g_assert_false(requests[0].from_prewarm);
    g_assert_cmpint(requests[0].priority, ==,
                    PGRAPH_VK_HYBRID_PRIORITY_VISIBLE);

    /* An opportunistic prewarm owner joining the same exact family upgrades
     * its provenance; a later demand owner must not erase it. */
    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &key, &state, true));
    g_assert_true(requests[0].from_prewarm);
    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &key, &state, false));
    g_assert_true(requests[0].from_prewarm);

    pgraph_vk_hybrid_pipeline_note_prewarm(
        &work, requests[0].from_prewarm);
    g_assert_true(work.prewarm);
    pgraph_vk_hybrid_pipeline_note_prewarm(&work, false);
    g_assert_true(work.prewarm);

    requests[0] = (PGRAPHVkFallbackFamilyRequest) { 0 };
    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &key, &state, true));
    g_assert_cmpint(requests[0].priority, ==,
                    PGRAPH_VK_HYBRID_PRIORITY_PREWARM);
    g_assert_true(pgraph_vk_fallback_family_enqueue(
        requests, G_N_ELEMENTS(requests), &key, &state, false));
    g_assert_true(requests[0].from_prewarm);
    g_assert_cmpint(requests[0].priority, ==,
                    PGRAPH_VK_HYBRID_PRIORITY_VISIBLE);
}

static void test_shader_alias_priority_and_owned_retry_source(void)
{
    PGRAPHVkHybridShaderWork work[3] = { 0 };
    char source[] = "owned-source";
    for (unsigned int i = 0; i < 2; i++) {
        work[i].in_use = true;
        work[i].prewarm = true;
        work[i].priority = PGRAPH_VK_HYBRID_PRIORITY_PREWARM;
        work[i].metadata.generation = 7;
        work[i].metadata.ticket = 11;
    }
    work[0].glsl = source;
    work[0].glsl_size = strlen(source);
    work[2].in_use = true;
    work[2].metadata.generation = 7;
    work[2].metadata.ticket = 12;

    g_assert_cmpuint(pgraph_vk_hybrid_shader_promote_aliases(
                         work, G_N_ELEMENTS(work), 7, 11,
                         PGRAPH_VK_HYBRID_PRIORITY_VISIBLE),
                     ==, 2);
    g_assert_cmpint(work[0].priority, ==,
                    PGRAPH_VK_HYBRID_PRIORITY_VISIBLE);
    g_assert_cmpint(work[1].priority, ==,
                    PGRAPH_VK_HYBRID_PRIORITY_VISIBLE);
    g_assert_cmpint(work[2].priority, ==,
                    PGRAPH_VK_HYBRID_PRIORITY_PREWARM);

    const char *owned = NULL;
    size_t owned_size = 0;
    g_assert_true(pgraph_vk_hybrid_shader_owned_source(
        &work[0], &owned, &owned_size));
    g_assert_true(owned == source);
    g_assert_cmpuint(owned_size, ==, strlen(source));
}

static void test_fallback_family_tracks_pipeline_until_ready(void)
{
    PGRAPHVkFallbackFamilyRequest request = {
        .in_use = true,
        .status = PGRAPH_VK_FAMILY_WAITING_FOR_SHADER,
    };

    g_assert_true(pgraph_vk_fallback_family_note_pipeline_submit(
        &request, PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED, 1000));
    g_assert_true(request.in_use);
    g_assert_cmpint(request.status, ==,
                    PGRAPH_VK_FAMILY_PIPELINE_PENDING);
    g_assert_cmpuint(request.attempts, ==, 0);

    g_assert_true(pgraph_vk_fallback_family_note_pipeline_failure(
        &request, 2000));
    g_assert_true(request.in_use);
    g_assert_cmpint(request.status, ==,
                    PGRAPH_VK_FAMILY_PIPELINE_RETRY_BACKOFF);
    g_assert_cmpuint(request.attempts, ==, 1);
    g_assert_cmpint(request.retry_after_us, >, 2000);
    g_assert_false(pgraph_vk_fallback_family_retry_due(
        &request, request.retry_after_us - 1));
    g_assert_true(pgraph_vk_fallback_family_retry_due(
        &request, request.retry_after_us));
}

static void test_fallback_family_deferral_and_failure_are_bounded(void)
{
    PGRAPHVkFallbackFamilyRequest request = {
        .in_use = true,
        .status = PGRAPH_VK_FAMILY_WAITING_FOR_SHADER,
    };

    g_assert_true(pgraph_vk_fallback_family_note_pipeline_submit(
        &request, PGRAPH_VK_HYBRID_PIPELINE_QUEUE_FULL, 1000));
    g_assert_true(request.in_use);
    g_assert_cmpint(request.status, ==,
                    PGRAPH_VK_FAMILY_QUEUE_DEFERRED);
    g_assert_cmpuint(request.attempts, ==, 0);
    g_assert_cmpint(request.retry_after_us, >, 1000);

    for (unsigned int attempt = 1;
         attempt < PGRAPH_VK_FAMILY_MAX_PIPELINE_ATTEMPTS; attempt++) {
        request.status = PGRAPH_VK_FAMILY_PIPELINE_PENDING;
        g_assert_true(pgraph_vk_fallback_family_note_pipeline_failure(
            &request, 2000 + attempt));
        g_assert_cmpuint(request.attempts, ==, attempt);
    }
    request.status = PGRAPH_VK_FAMILY_PIPELINE_PENDING;
    g_assert_false(pgraph_vk_fallback_family_note_pipeline_failure(
        &request, 3000));
    g_assert_false(request.in_use);
    g_assert_cmpint(request.status, ==,
                    PGRAPH_VK_FAMILY_REQUEST_REJECTED);

    request = (PGRAPHVkFallbackFamilyRequest) {
        .in_use = true,
        .status = PGRAPH_VK_FAMILY_WAITING_FOR_SHADER,
    };
    g_assert_false(pgraph_vk_fallback_family_note_pipeline_submit(
        &request, PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE, 4000));
    g_assert_false(request.in_use);
    g_assert_cmpint(request.status, ==,
                    PGRAPH_VK_FAMILY_REQUEST_REJECTED);
}

static void test_fallback_family_wake_is_shader_specific(void)
{
    ShaderModuleCacheKey requested = {
        .kind = VK_SHADER_STAGE_FRAGMENT_BIT,
        .fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER,
    };
    ShaderModuleCacheKey unrelated = requested;
    PGRAPHVkFallbackFamilyRequest request = {
        .in_use = true,
        .status = PGRAPH_VK_FAMILY_WAITING_FOR_SHADER,
        .retry_after_us = 5000,
    };

    unrelated.psh.state.shader_stage_program = 1;
    g_assert_false(pgraph_vk_fallback_family_wake_for_module(
        &request, &requested, &unrelated));
    g_assert_cmpint(request.retry_after_us, ==, 5000);
    g_assert_true(pgraph_vk_fallback_family_wake_for_module(
        &request, &requested, &requested));
    g_assert_cmpint(request.retry_after_us, ==, 0);

    request.status = PGRAPH_VK_FAMILY_PIPELINE_RETRY_BACKOFF;
    request.retry_after_us = 9000;
    g_assert_false(pgraph_vk_fallback_family_wake_for_module(
        &request, &requested, &requested));
    g_assert_cmpint(request.retry_after_us, ==, 9000);
}

typedef struct FallbackBindingPrepareFixture {
    ShaderBinding binding;
    unsigned int probes;
    unsigned int preparations;
    bool initial_binding_ready;
    bool preparation_succeeds;
    bool binding_ready_after_preparation;
} FallbackBindingPrepareFixture;

static ShaderBinding *fallback_binding_probe(void *opaque)
{
    FallbackBindingPrepareFixture *fixture = opaque;

    fixture->probes++;
    if (fixture->initial_binding_ready ||
        (fixture->preparations &&
         fixture->binding_ready_after_preparation)) {
        return &fixture->binding;
    }

    return NULL;
}

static bool fallback_fragment_prepare(void *opaque)
{
    FallbackBindingPrepareFixture *fixture = opaque;

    fixture->preparations++;
    return fixture->preparation_succeeds;
}

static void test_fallback_family_shader_preparation_outcomes(void)
{
    FallbackBindingPrepareFixture fixture = {
        .initial_binding_ready = true,
    };
    ShaderBinding *binding = NULL;

    PGRAPHVkFallbackShaderPreparation result =
        pgraph_vk_fallback_family_prepare_shader(
            &fixture, fallback_binding_probe, fallback_fragment_prepare,
            &binding);

    g_assert_cmpint(result, ==, PGRAPH_VK_FALLBACK_SHADER_READY);
    g_assert_true(binding == &fixture.binding);
    g_assert_cmpuint(fixture.probes, ==, 1);
    g_assert_cmpuint(fixture.preparations, ==, 0);

    fixture = (FallbackBindingPrepareFixture) {
        .preparation_succeeds = false,
    };
    binding = NULL;
    result = pgraph_vk_fallback_family_prepare_shader(
        &fixture, fallback_binding_probe, fallback_fragment_prepare, &binding);

    g_assert_cmpint(result, ==, PGRAPH_VK_FALLBACK_SHADER_REJECTED);
    g_assert_null(binding);
    g_assert_cmpuint(fixture.probes, ==, 1);
    g_assert_cmpuint(fixture.preparations, ==, 1);

    fixture = (FallbackBindingPrepareFixture) {
        .preparation_succeeds = true,
        .binding_ready_after_preparation = false,
    };
    binding = NULL;
    result = pgraph_vk_fallback_family_prepare_shader(
        &fixture, fallback_binding_probe, fallback_fragment_prepare, &binding);

    g_assert_cmpint(result, ==, PGRAPH_VK_FALLBACK_SHADER_WAITING);
    g_assert_null(binding);
    g_assert_cmpuint(fixture.probes, ==, 2);
    g_assert_cmpuint(fixture.preparations, ==, 1);

    fixture = (FallbackBindingPrepareFixture) {
        .preparation_succeeds = true,
        .binding_ready_after_preparation = true,
    };
    binding = NULL;
    result = pgraph_vk_fallback_family_prepare_shader(
        &fixture, fallback_binding_probe, fallback_fragment_prepare, &binding);

    g_assert_cmpint(result, ==, PGRAPH_VK_FALLBACK_SHADER_READY);
    g_assert_true(binding == &fixture.binding);
    g_assert_cmpuint(fixture.probes, ==, 2);
    g_assert_cmpuint(fixture.preparations, ==, 1);
}

static void test_fallback_family_production_lifecycle(void)
{
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);
    PipelineBinding entries[4] = { 0 };
    ShaderState first_state = base_state();
    ShaderState second_state = first_state;
    PipelineKey first_key;
    PipelineKey second_key;
    PipelineKey family_key;

    first_state.psh.combiner_control = 1;
    second_state.psh.combiner_control = 2;
    first_key = pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &first_state);
    second_key = pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &second_state);
    family_key = pipeline_key(PGRAPH_VK_FRAGMENT_UBERSHADER, &first_state);

    lru_init(&r->pipeline_cache);
    r->pipeline_cache.init_node = probe_pipeline_init;
    r->pipeline_cache.compare_nodes = probe_pipeline_different;
    r->pipeline_cache_entries = entries;
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        lru_add_free(&r->pipeline_cache, &entries[i].node);
    }
    PipelineBinding *first = container_of(
        lru_lookup(&r->pipeline_cache, 101, &first_key),
        PipelineBinding, node);
    PipelineBinding *second = container_of(
        lru_lookup(&r->pipeline_cache, 102, &second_key),
        PipelineBinding, node);
    PipelineBinding *unchecked = container_of(
        lru_lookup(&r->pipeline_cache, 103, &first_key),
        PipelineBinding, node);
    PipelineBinding *rejected = container_of(
        lru_lookup(&r->pipeline_cache, 104, &second_key),
        PipelineBinding, node);
    first->pipeline = second->pipeline = unchecked->pipeline =
        rejected->pipeline = (VkPipeline)(uintptr_t)1;
    pgraph_vk_pipeline_family_set_state(
        r, rejected, PGRAPH_VK_FAMILY_REJECTED);
    pgraph_vk_pipeline_family_set_state(
        r, first, PGRAPH_VK_FAMILY_RETRY_PENDING);
    pgraph_vk_pipeline_family_set_state(
        r, second, PGRAPH_VK_FAMILY_RETRY_PENDING);
    g_assert_cmpuint(r->fallback_family_retry_count, ==, 2);

    /* A full request table retains both owners for a later bounded pass. */
    for (size_t i = 0; i < ARRAY_SIZE(r->fallback_family_requests); i++) {
        r->fallback_family_requests[i].in_use = true;
        r->fallback_family_requests[i].key.regs[0] = i + 1;
    }
    pgraph_vk_enqueue_retained_fallback_families(r);
    g_assert_cmpint(first->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_RETRY_PENDING);
    g_assert_cmpint(second->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_RETRY_PENDING);
    g_assert_cmpuint(r->fallback_family_retry_count, ==, 2);

    /* Once capacity exists, the exact family is admitted once and both
     * matching specialized owners transfer to TRACKED. */
    r->fallback_family_requests[0].in_use = false;
    pgraph_vk_enqueue_retained_fallback_families(r);
    PGRAPHVkFallbackFamilyRequest *request = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(r->fallback_family_requests); i++) {
        if (r->fallback_family_requests[i].in_use &&
            memcmp(&r->fallback_family_requests[i].key, &family_key,
                   sizeof(family_key)) == 0) {
            request = &r->fallback_family_requests[i];
            break;
        }
    }
    g_assert_nonnull(request);
    g_assert_cmpint(first->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_TRACKED);
    g_assert_cmpint(second->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_TRACKED);
    g_assert_cmpuint(r->fallback_family_retry_count, ==, 0);

    g_assert_true(pgraph_vk_fallback_family_note_pipeline_submit(
        request, PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED, 1000));
    pgraph_vk_fallback_family_note_pipeline_failure_at(
        r, &family_key, 2000);
    g_assert_true(request->in_use);
    g_assert_cmpint(request->status, ==,
                    PGRAPH_VK_FAMILY_PIPELINE_RETRY_BACKOFF);
    int64_t retry_after_us = request->retry_after_us;
    ShaderModuleCacheKey requested_module = {
        .kind = VK_SHADER_STAGE_FRAGMENT_BIT,
        .fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER,
    };
    g_assert_false(pgraph_vk_fallback_family_wake_for_module(
        request, &requested_module, &requested_module));
    g_assert_cmpint(request->retry_after_us, ==, retry_after_us);

    request->status = PGRAPH_VK_FAMILY_PIPELINE_PENDING;
    pgraph_vk_fallback_family_note_pipeline_ready(r, &family_key);
    g_assert_false(request->in_use);
    g_assert_cmpint(first->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_READY);
    g_assert_cmpint(second->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_READY);
    g_assert_cmpint(unchecked->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_UNCHECKED);
    g_assert_cmpint(rejected->family_learn_state, ==,
                    PGRAPH_VK_FAMILY_REJECTED);

    pgraph_vk_pipeline_family_set_state(
        r, first, PGRAPH_VK_FAMILY_RETRY_PENDING);
    g_assert_cmpuint(r->fallback_family_retry_count, ==, 1);
    pgraph_vk_pipeline_family_owner_evict(r, first);
    g_assert_cmpuint(r->fallback_family_retry_count, ==, 0);
    g_free(r);
}

static void test_fallback_family_owner_observation_requires_admission(void)
{
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);
    ShaderState state = base_state();
    PipelineBinding unsupported = {
        .pipeline = (VkPipeline)(uintptr_t)1,
        .key = pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &state),
    };
    PipelineBinding supported = {
        .pipeline = (VkPipeline)(uintptr_t)1,
        .key = pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &state),
    };

    pgraph_vk_track_specialized_fallback_family(
        r, &unsupported, false, true, 0);
    g_assert_cmpint(unsupported.family_learn_state, ==,
                    PGRAPH_VK_FAMILY_REJECTED);

    pgraph_vk_track_specialized_fallback_family(
        r, &supported, true, true, 0);
    g_assert_cmpint(supported.family_learn_state, ==,
                    PGRAPH_VK_FAMILY_READY);
    g_free(r);
}

static void test_changed_register_marks_shortcut_dirty(void)
{
    PGRAPHState *pg = g_new0(PGRAPHState, 1);

    g_assert_false(pg->regs_written_since_draw);
    pgraph_reg_w(pg, NV_PGRAPH_CONTROL_0, 0);
    g_assert_false(pg->regs_written_since_draw);
    pgraph_reg_w(pg, NV_PGRAPH_CONTROL_0, 1);
    g_assert_true(pg->regs_written_since_draw);
    g_free(pg);
}

static void test_snapshot_restore_invalidates_execution_hints(void)
{
    PGRAPHState *pg = g_new0(PGRAPHState, 1);

    pgraph_invalidate_all_register_hints(pg);
    g_assert_true(pg->regs_written_since_draw);
    g_assert_true(pg->program_data_dirty);
    g_assert_true(pgraph_is_reg_dirty(pg, NV_PGRAPH_CONTROL_0));
    g_assert_true(pgraph_is_reg_dirty(pg, NV_PGRAPH_ZOFFSETFACTOR));
    g_free(pg);
}

static void test_owner_budget_is_shared_across_draw_service(void)
{
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);

    pgraph_vk_hybrid_owner_budget_begin(r);
    int64_t first_deadline = r->hybrid_owner_service_deadline_us;
    pgraph_vk_hybrid_owner_budget_begin(r);
    g_assert_cmpint(r->hybrid_owner_service_deadline_us, ==,
                    first_deadline);
    g_assert_true(pgraph_vk_hybrid_owner_budget_available(r));

    g_usleep(10);
    r->hybrid_owner_service_deadline_us = g_get_monotonic_time() - 1;
    pgraph_vk_hybrid_owner_budget_begin(r);
    g_assert_cmpint(r->hybrid_owner_service_deadline_us, >,
                    first_deadline);
    g_free(r);
}

static ShaderState base_state(void)
{
    ShaderState state = { 0 };

    state.psh.shader_stage_program = 0x12345678;
    state.psh.other_stage_input = 0x9abcdef0;
    state.psh.alpha_test = true;
    state.psh.alpha_func = ALPHA_FUNC_LESS;
    return state;
}

static PipelineKey pipeline_key(PGRAPHVkFragmentRoute route,
                                const ShaderState *state)
{
    PipelineKey key = { 0 };

    pgraph_vk_pipeline_key_set_shader(&key, state, route);
    return key;
}

static uint32_t control_input_word(uint8_t a, uint8_t b,
                                   uint8_t c, uint8_t d)
{
    return (uint32_t)a << 24 | (uint32_t)b << 16 |
           (uint32_t)c << 8 | d;
}

static void set_uber_constant_registers(PGRAPHState *pg, uint32_t value)
{
    for (unsigned int i = 0; i < 8; i++) {
        pgraph_reg_w(pg, NV_PGRAPH_COMBINEFACTOR0 + i * 4, value);
        pgraph_reg_w(pg, NV_PGRAPH_COMBINEFACTOR1 + i * 4, value);
    }
    pgraph_reg_w(pg, NV_PGRAPH_SPECFOGFACTOR0, value);
    pgraph_reg_w(pg, NV_PGRAPH_SPECFOGFACTOR1, value);
}

static void test_fallback_control_snapshot_tracks_published_packet(void)
{
    PGRAPHState *pg = g_new0(PGRAPHState, 1);
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);
    PshState state_a = { 0 };
    PshState state_b;
    PGRAPHUberControls packet_b_l;
    PGRAPHUberControls expected_b_k;
    const uint32_t constants_k = 0xff112233;
    const uint32_t constants_l = 0xffaabbcc;

    pg->vk_renderer_state = r;
    state_a.combiner_control = PS_COMBINERCOUNT_MUX_MSB << 8;
    state_a.final_inputs_0 = control_input_word(
        PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1R0_SUM | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_EF_PROD | PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    state_a.final_inputs_1 = control_input_word(
        PS_REGISTER_C0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_C1 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_R0 | PS_CHANNEL_ALPHA, 0);
    state_b = state_a;
    state_b.final_inputs_0 = control_input_word(
        PS_REGISTER_V1 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1R0_SUM | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_EF_PROD | PS_INPUTMAPPING_UNSIGNED_IDENTITY);

    set_uber_constant_registers(pg, constants_k);
    g_assert_true(pgraph_vk_refresh_fallback_controls(pg, &state_a));
    g_assert_true(r->uber_constant_regs_valid);

    set_uber_constant_registers(pg, constants_l);
    g_assert_true(pgraph_vk_pack_fallback_controls(
        pg, &state_b, &packet_b_l));
    pgraph_vk_publish_fallback_controls(r, &packet_b_l);
    g_assert_false(r->uber_constant_regs_valid);

    set_uber_constant_registers(pg, constants_k);
    g_assert_true(pgraph_vk_refresh_fallback_controls(pg, &state_b));
    g_assert_true(pgraph_vk_pack_fallback_controls(
        pg, &state_b, &expected_b_k));
    g_assert_cmpmem(&r->uber_controls, sizeof(r->uber_controls),
                    &expected_b_k, sizeof(expected_b_k));
    g_assert_true(r->uber_constant_regs_valid);

    PGRAPHUberControls stable = r->uber_controls;
    g_assert_true(pgraph_vk_refresh_fallback_controls(pg, &state_b));
    g_assert_cmpmem(&r->uber_controls, sizeof(r->uber_controls),
                    &stable, sizeof(stable));

    g_free(r);
    g_free(pg);
}

static void test_production_resolver_materializes_hidden_fallback(void)
{
    PGRAPHState *pg = g_new0(PGRAPHState, 1);
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);
    ShaderBinding shader_entries[2] = { 0 };
    ShaderModuleCacheEntry module_entries[2] = { 0 };
    PipelineBinding pipeline_entries[2] = { 0 };
    ShaderState state = { 0 };

    pg->vk_renderer_state = r;
    state.psh.combiner_control = PS_COMBINERCOUNT_MUX_MSB << 8;
    state.psh.final_inputs_0 = control_input_word(
        PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_V1R0_SUM | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_EF_PROD | PS_INPUTMAPPING_UNSIGNED_IDENTITY);
    state.psh.final_inputs_1 = control_input_word(
        PS_REGISTER_C0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_C1 | PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_REGISTER_R0 | PS_CHANNEL_ALPHA,
        0);

    lru_init(&r->shader_cache);
    r->shader_cache.init_node = probe_shader_init;
    r->shader_cache.compare_nodes = probe_shader_different;
    for (size_t i = 0; i < ARRAY_SIZE(shader_entries); i++) {
        lru_add_free(&r->shader_cache, &shader_entries[i].node);
    }
    lru_init(&r->shader_module_cache);
    r->shader_module_cache.init_node = probe_module_init;
    r->shader_module_cache.compare_nodes = probe_module_different;
    for (size_t i = 0; i < ARRAY_SIZE(module_entries); i++) {
        lru_add_free(&r->shader_module_cache, &module_entries[i].node);
    }
    lru_init(&r->pipeline_cache);
    r->pipeline_cache.init_node = probe_pipeline_init;
    r->pipeline_cache.compare_nodes = probe_pipeline_different;
    for (size_t i = 0; i < ARRAY_SIZE(pipeline_entries); i++) {
        lru_add_free(&r->pipeline_cache, &pipeline_entries[i].node);
    }

    probe_shader_populate_modules = true;
    ShaderBindingKey specialized_key = {
        .state = state,
        .fragment_route = PGRAPH_VK_FRAGMENT_SPECIALIZED,
    };
    uint64_t specialized_hash = fast_hash(
        (const uint8_t *)&specialized_key, sizeof(specialized_key));
    ShaderBinding *specialized = container_of(
        lru_lookup(&r->shader_cache, specialized_hash, &specialized_key),
        ShaderBinding, node);

    ShaderModuleCacheKey vertex_key = { 0 };
    vertex_key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    vertex_key.vsh.state = state.vsh;
    vertex_key.vsh.glsl_opts.vulkan = true;
    vertex_key.vsh.glsl_opts.ubo_binding = 0;
    uint64_t vertex_hash = fast_hash(
        (const uint8_t *)&vertex_key,
        pgraph_vk_shader_module_key_active_size(&vertex_key));
    lru_lookup(&r->shader_module_cache, vertex_hash, &vertex_key);

    ShaderModuleCacheKey fragment_key = { 0 };
    fragment_key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragment_key.fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER;
    fragment_key.psh.state = state.psh;
    pgraph_vk_canonicalize_uber_combiner_state(&fragment_key.psh.state);
    fragment_key.psh.glsl_opts.vulkan = true;
    fragment_key.psh.glsl_opts.ubo_binding = 1;
    fragment_key.psh.glsl_opts.tex_binding = 2;
    fragment_key.psh.glsl_opts.ubershader = true;
    fragment_key.psh.glsl_opts.uber_binding =
        PGRAPH_VK_PSH_UBER_UBO_BINDING;
    uint64_t fragment_hash = fast_hash(
        (const uint8_t *)&fragment_key,
        pgraph_vk_shader_module_key_active_size(&fragment_key));
    lru_lookup(&r->shader_module_cache, fragment_hash, &fragment_key);

    PipelineKey fallback_key =
        pipeline_key(PGRAPH_VK_FRAGMENT_UBERSHADER, &state);
    uint64_t fallback_hash = fast_hash(
        (const uint8_t *)&fallback_key, sizeof(fallback_key));
    PipelineBinding *fallback_pipeline = container_of(
        lru_lookup(&r->pipeline_cache, fallback_hash, &fallback_key),
        PipelineBinding, node);
    fallback_pipeline->pipeline = (VkPipeline)(uintptr_t)1;

    PGRAPHVkReadyExecutionCandidates candidates;
    pgraph_vk_resolve_ready_execution_candidates(
        pg, &state, false, &candidates);
    g_assert_true(candidates.specialized.shader == specialized);
    g_assert_null(candidates.specialized.pipeline);
    g_assert_true(candidates.controls_checked);
    g_assert_true(candidates.controls_supported);
    g_assert_nonnull(candidates.fallback.shader);
    g_assert_true(candidates.fallback.pipeline == fallback_pipeline);

    /* Once specialization is complete, the production resolver returns before
     * fallback probing, packet packing, or metadata construction. */
    PipelineKey specialized_pipeline_key =
        pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &state);
    uint64_t specialized_pipeline_hash = fast_hash(
        (const uint8_t *)&specialized_pipeline_key,
        sizeof(specialized_pipeline_key));
    PipelineBinding *specialized_pipeline = container_of(
        lru_lookup(&r->pipeline_cache, specialized_pipeline_hash,
                   &specialized_pipeline_key), PipelineBinding, node);
    specialized_pipeline->pipeline = (VkPipeline)(uintptr_t)2;
    pgraph_vk_resolve_ready_execution_candidates(
        pg, &state, false, &candidates);
    g_assert_true(candidates.specialized.shader == specialized);
    g_assert_true(candidates.specialized.pipeline == specialized_pipeline);
    g_assert_false(candidates.controls_checked);
    g_assert_null(candidates.fallback.shader);
    g_assert_null(candidates.fallback.pipeline);

    /* Always mode ignores an exact specialized executable and resolves only
     * the supported interpreter candidate. */
    pgraph_vk_resolve_ready_execution_candidates(
        pg, &state, true, &candidates);
    g_assert_null(candidates.specialized.shader);
    g_assert_null(candidates.specialized.pipeline);
    g_assert_true(candidates.controls_checked);
    g_assert_true(candidates.controls_supported);
    g_assert_nonnull(candidates.fallback.shader);
    g_assert_true(candidates.fallback.pipeline == fallback_pipeline);

    probe_shader_populate_modules = false;
    g_free(r);
    g_free(pg);
}

static void test_dynamic_control_binding_matches_packet_abi(void)
{
    g_assert_cmpuint(PGRAPH_VK_PSH_UBER_UBO_BINDING, ==, 6);
    g_assert_cmpuint(sizeof(PGRAPHUberControls), ==, 448);
    g_assert_cmpuint(_Alignof(PGRAPHUberControls), ==, 16);
}

static void test_control_only_upload_does_not_require_descriptor_update(void)
{
    bool descriptor_update =
        pgraph_vk_descriptor_update_needed(false, false, false);

    g_assert_false(descriptor_update);
    g_assert_true(pgraph_vk_reuses_descriptor_set_for_control_update(
        true, descriptor_update));
    g_assert_true(pgraph_vk_descriptor_update_needed(true, false, false));
    g_assert_true(pgraph_vk_descriptor_update_needed(false, true, false));
    g_assert_true(pgraph_vk_descriptor_update_needed(false, false, true));
}

static void test_disabled_runtime_uses_baseline_descriptor_layout(void)
{
    g_assert_cmpuint(pgraph_vk_descriptor_layout_binding_count(false), ==, 6);
    g_assert_cmpuint(pgraph_vk_descriptor_layout_binding_count(true), ==, 7);
    g_assert_cmpuint(pgraph_vk_descriptor_dynamic_offset_count(false), ==, 0);
    g_assert_cmpuint(pgraph_vk_descriptor_dynamic_offset_count(true), ==, 1);
}

static void test_shader_binding_key_equality_requires_route_and_full_state(void)
{
    ShaderBindingKey a = {
        .state = base_state(),
        .fragment_route = PGRAPH_VK_FRAGMENT_SPECIALIZED,
    };
    ShaderBindingKey b = a;

    g_assert_true(pgraph_vk_shader_binding_key_equal(&a, &b));
    g_assert_false(pgraph_vk_shader_binding_key_different(&a, &b));
    b.fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER;
    g_assert_false(pgraph_vk_shader_binding_key_equal(&a, &b));
    g_assert_true(pgraph_vk_shader_binding_key_different(&a, &b));
    b = a;
    b.state.psh.combiner_control = 1;
    g_assert_false(pgraph_vk_shader_binding_key_equal(&a, &b));
    g_assert_true(pgraph_vk_shader_binding_key_different(&a, &b));
}

static void test_shader_module_key_uses_only_active_stage(void)
{
    ShaderModuleCacheKey a = { 0 };
    ShaderModuleCacheKey b;
    size_t active_size;

    a.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
    a.fragment_route = PGRAPH_VK_FRAGMENT_SPECIALIZED;
    a.geom.glsl_opts.vulkan = true;
    b = a;
    active_size = pgraph_vk_shader_module_key_active_size(&a);
    g_assert_cmpuint(active_size, <, sizeof(a));
    ((uint8_t *)&b)[sizeof(b) - 1] = 0x5a;
    g_assert_true(pgraph_vk_shader_module_key_equal(&a, &b));
    b.geom.glsl_opts.vulkan = false;
    g_assert_false(pgraph_vk_shader_module_key_equal(&a, &b));

    a.kind = VK_SHADER_STAGE_VERTEX_BIT;
    a.vsh.glsl_opts.ubo_binding = 4;
    b = a;
    g_assert_true(pgraph_vk_shader_module_key_equal(&a, &b));
    b.vsh.glsl_opts.ubo_binding = 5;
    g_assert_false(pgraph_vk_shader_module_key_equal(&a, &b));

    a.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    a.psh.glsl_opts.uber_binding = 6;
    b = a;
    g_assert_true(pgraph_vk_shader_module_key_equal(&a, &b));
    b.psh.glsl_opts.uber_binding = 7;
    g_assert_false(pgraph_vk_shader_module_key_equal(&a, &b));
    b = a;
    b.fragment_route = PGRAPH_VK_FRAGMENT_UBERSHADER;
    g_assert_false(pgraph_vk_shader_module_key_equal(&a, &b));
    b = a;
    b.kind = VK_SHADER_STAGE_VERTEX_BIT;
    g_assert_false(pgraph_vk_shader_module_key_equal(&a, &b));
}

static void test_uber_pipeline_key_ignores_only_combiner_words(void)
{
    ShaderState a = base_state();
    ShaderState b = a;

    a.psh.combiner_control = 1;
    a.psh.rgb_inputs[0] = 0x11111111;
    a.psh.alpha_inputs[0] = 0x22222222;
    a.psh.rgb_outputs[0] = 0x33333333;
    a.psh.alpha_outputs[0] = 0x44444444;
    a.psh.final_inputs_0 = 0x55555555;
    a.psh.final_inputs_1 = 0x66666666;
    b.psh.combiner_control = 8;
    b.psh.rgb_inputs[0] = 0xaaaaaaaa;
    b.psh.alpha_inputs[0] = 0xbbbbbbbb;
    b.psh.rgb_outputs[0] = 0xcccccccc;
    b.psh.alpha_outputs[0] = 0xdddddddd;
    b.psh.final_inputs_0 = 0xeeeeeeee;
    b.psh.final_inputs_1 = 0xffffffff;

    PipelineKey uber_a = pipeline_key(PGRAPH_VK_FRAGMENT_UBERSHADER, &a);
    PipelineKey uber_b = pipeline_key(PGRAPH_VK_FRAGMENT_UBERSHADER, &b);
    PipelineKey specialized_a =
        pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &a);
    PipelineKey specialized_b =
        pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &b);

    g_assert_cmpmem(&uber_a, sizeof(uber_a), &uber_b, sizeof(uber_b));
    g_assert_cmpint(memcmp(&specialized_a, &specialized_b,
                           sizeof(specialized_a)), !=, 0);
}

static void test_fragment_route_keeps_pipeline_keys_isolated(void)
{
    ShaderState state = base_state();
    PipelineKey specialized =
        pipeline_key(PGRAPH_VK_FRAGMENT_SPECIALIZED, &state);
    PipelineKey uber = pipeline_key(PGRAPH_VK_FRAGMENT_UBERSHADER, &state);

    g_assert_cmpint(memcmp(&specialized, &uber, sizeof(specialized)), !=, 0);
}

static void test_canonicalization_preserves_fragment_shell_state(void)
{
    PshState state = { 0 };

    state.combiner_control = 0xffffffff;
    memset(state.rgb_inputs, 0x11, sizeof(state.rgb_inputs));
    memset(state.alpha_inputs, 0x22, sizeof(state.alpha_inputs));
    memset(state.rgb_outputs, 0x33, sizeof(state.rgb_outputs));
    memset(state.alpha_outputs, 0x44, sizeof(state.alpha_outputs));
    state.final_inputs_0 = 0x55555555;
    state.final_inputs_1 = 0x66666666;
    state.shader_stage_program = 0x77777777;
    state.other_stage_input = 0x88888888;
    state.alpha_test = true;
    state.alpha_func = ALPHA_FUNC_GREATER;

    pgraph_vk_canonicalize_uber_combiner_state(&state);

    g_assert_cmpuint(state.combiner_control, ==, 0);
    g_assert_cmpmem(state.rgb_inputs, sizeof(state.rgb_inputs),
                    (uint32_t[8]) { 0 }, sizeof(state.rgb_inputs));
    g_assert_cmpmem(state.alpha_inputs, sizeof(state.alpha_inputs),
                    (uint32_t[8]) { 0 }, sizeof(state.alpha_inputs));
    g_assert_cmpmem(state.rgb_outputs, sizeof(state.rgb_outputs),
                    (uint32_t[8]) { 0 }, sizeof(state.rgb_outputs));
    g_assert_cmpmem(state.alpha_outputs, sizeof(state.alpha_outputs),
                    (uint32_t[8]) { 0 }, sizeof(state.alpha_outputs));
    g_assert_cmpuint(state.final_inputs_0, ==, 0);
    g_assert_cmpuint(state.final_inputs_1, ==, 0);
    g_assert_cmpuint(state.shader_stage_program, ==, 0x77777777);
    g_assert_cmpuint(state.other_stage_input, ==, 0x88888888);
    g_assert_true(state.alpha_test);
    g_assert_cmpint(state.alpha_func, ==, ALPHA_FUNC_GREATER);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/ubershader/runtime/control-abi",
                    test_dynamic_control_binding_matches_packet_abi);
    g_test_add_func("/xbox/vk/ubershader/runtime/control-only-upload",
                    test_control_only_upload_does_not_require_descriptor_update);
    g_test_add_func("/xbox/vk/ubershader/runtime/control-snapshot-publication",
                    test_fallback_control_snapshot_tracks_published_packet);
    g_test_add_func("/xbox/vk/ubershader/runtime/baseline-layout",
                    test_disabled_runtime_uses_baseline_descriptor_layout);
    g_test_add_func("/xbox/vk/ubershader/runtime/shader-binding-key",
                    test_shader_binding_key_equality_requires_route_and_full_state);
    g_test_add_func("/xbox/vk/ubershader/runtime/shader-module-key",
                    test_shader_module_key_uses_only_active_stage);
    g_test_add_func("/xbox/vk/ubershader/runtime/canonical-key",
                    test_uber_pipeline_key_ignores_only_combiner_words);
    g_test_add_func("/xbox/vk/ubershader/runtime/route-isolation",
                    test_fragment_route_keeps_pipeline_keys_isolated);
    g_test_add_func("/xbox/vk/ubershader/runtime/shell-state",
                    test_canonicalization_preserves_fragment_shell_state);
    g_test_add_func("/xbox/vk/ubershader/runtime/production-resolver",
                    test_production_resolver_materializes_hidden_fallback);
    g_test_add_func("/xbox/vk/ubershader/runtime/pipeline-ready-probe",
                    test_pipeline_ready_probe_is_side_effect_free);
    g_test_add_func("/xbox/vk/ubershader/runtime/vertex-input-count-key",
                    test_pipeline_key_distinguishes_vertex_input_counts);
    g_test_add_func("/xbox/vk/ubershader/runtime/uniform-register-key",
                    test_pipeline_key_ignores_uniform_only_register_values);
    g_test_add_func("/xbox/vk/ubershader/runtime/shader-ready-probe",
                    test_shader_ready_probe_requires_runtime_metadata);
    g_test_add_func("/xbox/vk/ubershader/runtime/complete-route",
                    test_execution_route_requires_complete_candidate);
    g_test_add_func("/xbox/vk/ubershader/runtime/hidden-fallback-binding",
                    test_ready_fallback_pipeline_requests_missing_binding);
    g_test_add_func("/xbox/vk/ubershader/runtime/uncovered-choice",
                    test_uncovered_build_uses_closer_binding);
    g_test_add_func("/xbox/vk/ubershader/runtime/promotion-time-gate",
                    test_fallback_promotion_probe_has_time_gate);
    g_test_add_func("/xbox/vk/ubershader/runtime/pipeline-late-publication",
                    test_pipeline_publication_eviction_is_late);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-queue",
                    test_fallback_family_queue_deduplicates_and_bounds);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-prewarm",
                    test_fallback_family_prewarm_provenance_is_monotonic);
    g_test_add_func("/xbox/vk/ubershader/runtime/alias-priority-owned-source",
                    test_shader_alias_priority_and_owned_retry_source);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-tracked",
                    test_fallback_family_tracks_pipeline_until_ready);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-retry",
                    test_fallback_family_deferral_and_failure_are_bounded);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-wake",
                    test_fallback_family_wake_is_shader_specific);
    g_test_add_func(
        "/xbox/vk/ubershader/runtime/fallback-family-shader-preparation",
        test_fallback_family_shader_preparation_outcomes);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-production",
                    test_fallback_family_production_lifecycle);
    g_test_add_func("/xbox/vk/ubershader/runtime/fallback-family-admission",
                    test_fallback_family_owner_observation_requires_admission);
    g_test_add_func("/xbox/vk/ubershader/runtime/register-shortcut-dirty",
                    test_changed_register_marks_shortcut_dirty);
    g_test_add_func("/xbox/vk/ubershader/runtime/snapshot-invalidation",
                    test_snapshot_restore_invalidates_execution_hints);
    g_test_add_func("/xbox/vk/ubershader/runtime/shared-owner-budget",
                    test_owner_budget_is_shared_across_draw_service);
    return g_test_run();
}

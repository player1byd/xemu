// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the generated configuration used by xemu, including saved Off values.
#include "qemu/osdep.h"
#include <cassert>
#include <cnode.h>
#include "ui/xemu-settings.h"
#include "ui/xemu-settings-migration.hh"
#include "ui/xemu-tweaks.h"
extern "C" {
#include "qemu/timer.h"
}
#define DEFINE_CONFIG_TREE
#include "xemu-config.h"

struct config g_config;

static void load_tweaks_table(const char *text)
{
    toml::table table = toml::parse(text);

    config_tree.reset_to_defaults();
    xemu_settings_apply_ubershader_migration(config_tree, table);
    config_tree.free_allocations(&g_config);
    config_tree.store_to_struct(&g_config);
}

static void load_config_table(const char *text)
{
    toml::table table = toml::parse(text);

    config_tree.reset_to_defaults();
    config_tree.update_from_table(table);
    config_tree.free_allocations(&g_config);
    config_tree.store_to_struct(&g_config);
}

static void test_dsp_jit_default_migration_policy()
{
    auto dsp_jit = config_tree.child("audio")->child("use_dsp_jit");
    assert(dsp_jit && dsp_jit->type == CNodeType::Boolean);

    load_config_table("");
    assert(g_config.audio.use_dsp_jit);

    load_config_table("[audio]\nuse_dsp = true\n");
    assert(g_config.audio.use_dsp_jit);

    load_config_table("[audio]\nuse_dsp_jit = false\n");
    assert(!g_config.audio.use_dsp_jit);

    load_config_table("[audio]\nuse_dsp_jit = true\n");
    assert(g_config.audio.use_dsp_jit);
}

static void test_ubershader_migration()
{
    load_tweaks_table("[tweaks]\npgraph_bulk_packets = true\n");
    assert(g_config.tweaks.vk_ubershader_mode ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_PREWARM);

    load_tweaks_table("[tweaks]\nvk_hybrid_ubershaders = true\n");
    assert(g_config.tweaks.vk_ubershader_mode ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK);
    assert(!g_config.tweaks.vk_hybrid_ubershaders);

    load_tweaks_table("[tweaks]\nvk_hybrid_ubershaders = false\n");
    assert(g_config.tweaks.vk_ubershader_mode ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_OFF);

    load_tweaks_table(
        "[tweaks]\nvk_ubershader_mode = 'off'\n"
        "vk_hybrid_ubershaders = true\n");
    assert(g_config.tweaks.vk_ubershader_mode ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_OFF);
    assert(!g_config.tweaks.vk_hybrid_ubershaders);

    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK;
    config_tree.update_from_struct(&g_config);
    std::string saved = config_tree.generate_delta_toml();
    assert(saved.find("vk_hybrid_ubershaders") == std::string::npos);
    assert(saved.find("vk_ubershader_mode = 'fallback'") !=
           std::string::npos);
    load_tweaks_table(saved.c_str());
    assert(g_config.tweaks.vk_ubershader_mode ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK);
}

static void test_ubershader_runtime_lifecycle()
{
    XemuVulkanUbershaderRuntimeState state;

    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK;
    xemu_tweaks_apply(true);
    xemu_vulkan_ubershader_publish_runtime(false, false);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.policy == XEMU_VK_UBERSHADER_FALLBACK);
    assert(state.active == XEMU_VK_UBERSHADER_OFF);
    assert(!state.available);
    assert(!state.restart_pending);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));

    /* Installing Vulkan consumes the process policy without relatching it. */
    xemu_vulkan_ubershader_publish_runtime(true, true);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.policy == XEMU_VK_UBERSHADER_FALLBACK);
    assert(state.active == XEMU_VK_UBERSHADER_FALLBACK);
    assert(state.available);

    /* A failed Vulkan request that falls back to OpenGL is not active. */
    xemu_vulkan_ubershader_publish_runtime(false, false);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.active == XEMU_VK_UBERSHADER_OFF);
    assert(!state.available);

    /* Saved edits remain pending through OpenGL and another Vulkan install. */
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_OFF;
    xemu_tweaks_apply(false);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.policy == XEMU_VK_UBERSHADER_FALLBACK);
    assert(state.active == XEMU_VK_UBERSHADER_OFF);
    assert(state.restart_pending);
    xemu_vulkan_ubershader_publish_runtime(true, true);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.active == XEMU_VK_UBERSHADER_FALLBACK);
    assert(state.restart_pending);

    /* A failed hybrid setup reports degraded specialized execution. */
    xemu_vulkan_ubershader_publish_runtime(true, false);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.active == XEMU_VK_UBERSHADER_OFF);
    assert(!state.available);
    assert(state.restart_pending);

    /* A live edit to Always remains pending until the next process start. */
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_ALWAYS;
    xemu_tweaks_apply(false);
    state = xemu_vulkan_ubershader_runtime_state();
    assert(state.policy == XEMU_VK_UBERSHADER_FALLBACK);
    assert(state.active == XEMU_VK_UBERSHADER_OFF);
    assert(!state.available);
    assert(state.restart_pending);
}

static void test_boolean_tweak_runtime_state()
{
    config_tree.reset_to_defaults();
    config_tree.free_allocations(&g_config);
    config_tree.store_to_struct(&g_config);
    xemu_tweaks_apply(true);

    xemu_tweaks_publish_renderer(XEMU_TWEAK_RENDERER_NONE);
    XemuTweakRuntimeState state =
        xemu_tweak_runtime_state(XEMU_TWEAK_VK_COLOR_DOWNLOAD_FOLDING);
    assert(state.requested && state.selected && !state.effective);
    assert(!state.available && state.reason && state.reason[0]);

    xemu_tweaks_publish_renderer(XEMU_TWEAK_RENDERER_VULKAN);
    assert(!g_config.tweaks.nv20_vertex_arithmetic);
    g_config.tweaks.nv20_vertex_arithmetic = true;
    xemu_tweaks_apply(false);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_NV20_VERTEX_ARITHMETIC);
    assert(state.requested && state.selected && state.effective);
    assert(state.available && !state.restart_pending);

    xemu_tweaks_publish_renderer(XEMU_TWEAK_RENDERER_OPENGL);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_NV20_VERTEX_ARITHMETIC);
    assert(state.requested && state.selected && state.effective);
    assert(state.available && !state.restart_pending);

    xemu_tweaks_publish_renderer(XEMU_TWEAK_RENDERER_VULKAN);
    state = xemu_tweak_runtime_state(
        XEMU_TWEAK_VK_COLOR_DOWNLOAD_FOLDING);
    assert(state.requested && state.selected && state.effective);
    assert(state.available && !state.restart_pending);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_GL_NATIVE_S3TC);
    assert(state.requested && state.selected && !state.effective);
    assert(!state.available);

    g_config.tweaks.vk_transient_buffer_growth = false;
    xemu_tweaks_apply(false);
    state = xemu_tweak_runtime_state(
        XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH);
    assert(!state.requested && state.selected && state.effective);
    assert(state.restart_pending);
    xemu_tweaks_apply(true);
    state = xemu_tweak_runtime_state(
        XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH);
    assert(!state.requested && !state.selected && !state.effective);
    assert(!state.restart_pending);

    g_config.tweaks.vk_shader_fastpath = true;
    xemu_tweaks_apply(false);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_VK_SHADER_FASTPATH);
    assert(state.requested && state.selected && !state.effective);
    assert(!state.available);

    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK;
    xemu_tweaks_apply(true);
    xemu_vulkan_ubershader_publish_runtime(true, true);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_VK_SHADER_FASTPATH);
    assert(state.effective && state.available);

    xemu_tweaks_publish_renderer(XEMU_TWEAK_RENDERER_OPENGL);
    xemu_vulkan_ubershader_publish_runtime(false, false);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_VK_SHADER_FASTPATH);
    assert(state.selected && !state.effective && !state.available);
    state = xemu_tweak_runtime_state(XEMU_TWEAK_GL_NATIVE_S3TC);
    assert(state.effective && state.available);
}

int main()
{
    test_dsp_jit_default_migration_policy();
    test_boolean_tweak_runtime_state();
    test_ubershader_migration();
    test_ubershader_runtime_lifecycle();
    const char *default_on_keys[] = {
        "cpu_saving_wait", "pgraph_bulk_packets", "pgraph_fence_fastpath",
        "vk_color_download_folding", "vk_bounded_vertex_uploads",
        "vk_vertex_copy_shortcuts",
        "vk_transient_buffer_growth", "gl_native_s3tc",
        "vk_shader_fastpath",
    };
    config_tree.reset_to_defaults();
    config_tree.free_allocations(&g_config);
    config_tree.store_to_struct(&g_config);
    xemu_tweaks_apply(true);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    assert(qemu_poll_get_cpu_saving());
    for (unsigned i = 0; i < XEMU_TWEAK_VK_HYBRID_UBERSHADERS; i++) {
        assert(xemu_tweak_enabled(static_cast<XemuTweak>(i)));
    }
    assert(!g_config.tweaks.vk_hybrid_ubershaders);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    assert(g_config.tweaks.vk_shader_fastpath);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_SHADER_FASTPATH));
    assert(!g_config.tweaks.nv20_vertex_arithmetic);
    assert(!xemu_tweak_enabled(XEMU_TWEAK_NV20_VERTEX_ARITHMETIC));
    assert(g_config.perf.cache_shaders);
    auto tweaks = config_tree.child("tweaks");
    auto ubershader_mode = tweaks->child("vk_ubershader_mode");
    assert(ubershader_mode && ubershader_mode->type == CNodeType::Enum);
    assert(ubershader_mode->data_enum.values.size() == 4);
    assert(ubershader_mode->data_enum.values[0] == "off");
    assert(ubershader_mode->data_enum.values[1] == "fallback");
    assert(ubershader_mode->data_enum.values[2] == "prewarm");
    assert(ubershader_mode->data_enum.values[3] == "always");
    assert(ubershader_mode->data_enum.val ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_PREWARM);
    assert(xemu_vulkan_ubershader_migrate_mode(
               false, XEMU_VK_UBERSHADER_OFF, false) ==
           XEMU_VK_UBERSHADER_OFF);
    assert(xemu_vulkan_ubershader_migrate_mode(
               false, XEMU_VK_UBERSHADER_OFF, true) ==
           XEMU_VK_UBERSHADER_FALLBACK);
    assert(xemu_vulkan_ubershader_migrate_mode(
               true, XEMU_VK_UBERSHADER_OFF, true) ==
           XEMU_VK_UBERSHADER_OFF);
    assert(xemu_vulkan_ubershader_migrate_mode(
               true, XEMU_VK_UBERSHADER_PREWARM, false) ==
           XEMU_VK_UBERSHADER_PREWARM);
    assert(xemu_vulkan_ubershader_mode_selectable(
        XEMU_VK_UBERSHADER_OFF));
    assert(xemu_vulkan_ubershader_mode_selectable(
        XEMU_VK_UBERSHADER_FALLBACK));
    assert(xemu_vulkan_ubershader_mode_selectable(
        XEMU_VK_UBERSHADER_PREWARM));
    assert(xemu_vulkan_ubershader_mode_selectable(
        XEMU_VK_UBERSHADER_ALWAYS));
    for (const char *key : default_on_keys) {
        auto node = tweaks->child(key);
        assert(node && node->data.boolean.val);
        node->data.boolean.val = false;
    }
    auto hybrid = tweaks->child("vk_hybrid_ubershaders");
    assert(hybrid && !hybrid->data.boolean.val);
    auto nv20_arithmetic = tweaks->child("nv20_vertex_arithmetic");
    assert(nv20_arithmetic && !nv20_arithmetic->data.boolean.val);
    nv20_arithmetic->data.boolean.val = true;
    ubershader_mode->set_enum_by_index(
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK);
    auto cache_shaders = config_tree.child("perf")->child("cache_shaders");
    assert(cache_shaders && cache_shaders->data.boolean.val);
    cache_shaders->data.boolean.val = false;
    config_tree.free_allocations(&g_config);
    config_tree.store_to_struct(&g_config);
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    xemu_tweaks_apply(false);
    assert(!qemu_poll_get_cpu_saving());
    for (unsigned i = 0; i < XEMU_TWEAK_COUNT; i++) {
        auto tweak = static_cast<XemuTweak>(i);
        bool restart = tweak == XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH ||
                       tweak == XEMU_TWEAK_GL_NATIVE_S3TC ||
                       tweak == XEMU_TWEAK_VK_HYBRID_UBERSHADERS;
        bool expected = restart ||
                        tweak == XEMU_TWEAK_NV20_VERTEX_ARITHMETIC;
        assert(xemu_tweak_enabled(tweak) == expected);
    }
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    xemu_tweaks_apply(true);
    for (unsigned i = 0; i < XEMU_TWEAK_VK_HYBRID_UBERSHADERS; i++) {
        assert(!xemu_tweak_enabled(static_cast<XemuTweak>(i)));
    }
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    xemu_vulkan_ubershader_publish_runtime(true, true);
    XemuVulkanUbershaderRuntimeState ubershader_state =
        xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.requested ==
           XEMU_VK_UBERSHADER_FALLBACK);
    assert(ubershader_state.active ==
           XEMU_VK_UBERSHADER_FALLBACK);
    assert(ubershader_state.available);
    assert(!ubershader_state.restart_pending);
    config_tree.update_from_struct(&g_config);
    auto saved = config_tree.generate_delta_toml();
    assert(saved.find("vk_ubershader_mode = 'fallback'") !=
           std::string::npos);
    assert(saved.find("vk_hybrid_ubershaders") == std::string::npos);
    assert(saved.find("nv20_vertex_arithmetic = true") !=
           std::string::npos);
    config_tree.reset_to_defaults();
    config_tree.update_from_table(toml::parse(saved));
    for (const char *key : default_on_keys) {
        assert(!tweaks->child(key)->data.boolean.val);
    }
    assert(!tweaks->child("vk_hybrid_ubershaders")->data.boolean.val);
    assert(tweaks->child("nv20_vertex_arithmetic")->data.boolean.val);
    assert(tweaks->child("vk_ubershader_mode")->data_enum.val ==
           CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK);
    assert(!config_tree.child("perf")->child("cache_shaders")
                ->data.boolean.val);
    // An older config keeps its saved choices and adopts new defaults.
    config_tree.reset_to_defaults();
    config_tree.update_from_table(toml::parse(
        "[perf]\ncache_shaders = false\n"
        "[tweaks]\ncpu_saving_wait = false\n"));
    assert(!tweaks->child(default_on_keys[0])->data.boolean.val);
    for (unsigned i = 1;
         i < sizeof(default_on_keys) / sizeof(default_on_keys[0]); i++) {
        assert(tweaks->child(default_on_keys[i])->data.boolean.val);
    }
    assert(!tweaks->child("vk_hybrid_ubershaders")->data.boolean.val);
    assert(!config_tree.child("perf")->child("cache_shaders")
                ->data.boolean.val);
    config_tree.free_allocations(&g_config);
    config_tree.store_to_struct(&g_config);
    xemu_tweaks_apply(true);
    assert(!qemu_poll_get_cpu_saving());
    assert(!xemu_tweak_enabled(XEMU_TWEAK_CPU_SAVING_WAIT));
    for (unsigned i = 1; i < XEMU_TWEAK_VK_HYBRID_UBERSHADERS; i++) {
        assert(xemu_tweak_enabled(static_cast<XemuTweak>(i)));
    }
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    xemu_tweaks_apply(true);
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK;
    xemu_tweaks_apply(false);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.active ==
           XEMU_VK_UBERSHADER_OFF);
    assert(ubershader_state.restart_pending);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    xemu_tweaks_apply(true);
    xemu_vulkan_ubershader_publish_runtime(true, true);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.active ==
           XEMU_VK_UBERSHADER_FALLBACK);
    assert(!ubershader_state.restart_pending);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_OFF;
    xemu_tweaks_apply(false);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.active ==
           XEMU_VK_UBERSHADER_FALLBACK);
    assert(ubershader_state.restart_pending);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_PREWARM;
    xemu_tweaks_apply(true);
    xemu_vulkan_ubershader_publish_runtime(true, true);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.requested == XEMU_VK_UBERSHADER_PREWARM);
    assert(ubershader_state.active == XEMU_VK_UBERSHADER_PREWARM);
    assert(ubershader_state.available);
    assert(!ubershader_state.restart_pending);
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_ALWAYS;
    xemu_tweaks_apply(true);
    assert(xemu_vulkan_ubershader_policy() ==
           XEMU_VK_UBERSHADER_ALWAYS);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    xemu_vulkan_ubershader_publish_runtime(true, true);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.requested ==
           XEMU_VK_UBERSHADER_ALWAYS);
    assert(ubershader_state.active ==
           XEMU_VK_UBERSHADER_ALWAYS);
    assert(ubershader_state.available);
    assert(!ubershader_state.restart_pending);
    assert(ubershader_state.reason && ubershader_state.reason[0]);
    xemu_vulkan_ubershader_publish_runtime(true, false);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.active == XEMU_VK_UBERSHADER_OFF);
    assert(!ubershader_state.available);
    g_config.tweaks.vk_ubershader_mode =
        CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK;
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
    xemu_tweaks_apply(true);
    ubershader_state = xemu_vulkan_ubershader_runtime_state();
    assert(ubershader_state.requested ==
           XEMU_VK_UBERSHADER_FALLBACK);
    assert(ubershader_state.active ==
           XEMU_VK_UBERSHADER_OFF);
    assert(!ubershader_state.available);
    assert(!ubershader_state.restart_pending);
    assert(ubershader_state.reason && ubershader_state.reason[0]);
    // Toggling one live option does not change any other option.
    g_config.tweaks.pgraph_bulk_packets = false;
    xemu_tweaks_apply(false);
    assert(!xemu_tweak_enabled(XEMU_TWEAK_PGRAPH_BULK_PACKETS));
    assert(xemu_tweak_enabled(XEMU_TWEAK_PGRAPH_FENCE_FASTPATH));
    g_config.tweaks.pgraph_bulk_packets = true;
    xemu_tweaks_apply(false);
    assert(xemu_tweak_enabled(XEMU_TWEAK_PGRAPH_BULK_PACKETS));
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_HYBRID_UBERSHADERS));
    g_config.tweaks.vk_shader_fastpath = true;
    xemu_tweaks_apply(false);
    assert(xemu_tweak_enabled(XEMU_TWEAK_VK_SHADER_FASTPATH));
    g_config.tweaks.vk_shader_fastpath = false;
    xemu_tweaks_apply(false);
    assert(!xemu_tweak_enabled(XEMU_TWEAK_VK_SHADER_FASTPATH));
    config_tree.free_allocations(&g_config);
    puts("PASS: defaults, persistence, migration, live changes and restart policy");
}

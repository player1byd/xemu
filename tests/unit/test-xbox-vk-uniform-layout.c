/*
 * NV2A Vulkan uniform ownership regression tests
 *
 * Run with AddressSanitizer/LeakSanitizer to check that clearing each layout
 * releases its names, metadata and backing allocation exactly once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>

#include "hw/xbox/nv2a/pgraph/vk/uniform-layout.h"

static void assert_cleared(const ShaderUniformLayout *layout)
{
    g_assert_null(layout->uniforms);
    g_assert_null(layout->allocation);
    g_assert_cmpuint(layout->num_uniforms, ==, 0);
    g_assert_cmpuint(layout->total_size, ==, 0);
}

static void clear_twice(ShaderUniformLayout *layout)
{
    shader_uniform_layout_clear(layout);
    assert_cleared(layout);
    shader_uniform_layout_clear(layout);
    assert_cleared(layout);
}

static void test_empty_layout(void)
{
    ShaderUniformLayout layout = { 0 };
    clear_twice(&layout);
}

static void test_missing_metadata_allocation(void)
{
    /* The count is assigned before metadata allocation is attempted. */
    ShaderUniformLayout layout = {
        .num_uniforms = 3,
        .total_size = 64,
        .allocation = g_malloc0(64),
    };
    clear_twice(&layout);
}

static void test_missing_backing_allocation(void)
{
    ShaderUniformLayout layout = {
        .uniforms = g_new0(ShaderUniform, 3),
        .num_uniforms = 3,
        .total_size = 64,
    };
    clear_twice(&layout);
}

static void test_partially_copied_names(void)
{
    ShaderUniformLayout layout = {
        .uniforms = g_new0(ShaderUniform, 3),
        .num_uniforms = 3,
        .total_size = 64,
        .allocation = g_malloc0(64),
    };
    layout.uniforms[0].name = strdup("first");
    g_assert_nonnull(layout.uniforms[0].name);
    clear_twice(&layout);
}

static void test_zero_members_with_backing(void)
{
    ShaderUniformLayout layout = {
        .total_size = 16,
        .allocation = g_malloc0(16),
    };
    clear_twice(&layout);
}

static void test_complete_uniform_and_push_layouts(void)
{
    /* Repeated successful teardown must release both backing buffers. */
    for (unsigned int cycle = 0; cycle < 256; cycle++) {
        ShaderUniformLayout layouts[2] = { 0 };
        for (size_t i = 0; i < G_N_ELEMENTS(layouts); i++) {
            layouts[i] = (ShaderUniformLayout) {
                .uniforms = g_new0(ShaderUniform, 2),
                .num_uniforms = 2,
                .total_size = 64,
                .allocation = g_malloc0(64),
            };
            layouts[i].uniforms[0].name = strdup("first");
            layouts[i].uniforms[1].name = strdup("second");
            g_assert_nonnull(layouts[i].uniforms[0].name);
            g_assert_nonnull(layouts[i].uniforms[1].name);
        }
        clear_twice(&layouts[0]);
        clear_twice(&layouts[1]);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/uniform-layout/empty", test_empty_layout);
    g_test_add_func("/xbox/vk/uniform-layout/missing-metadata",
                    test_missing_metadata_allocation);
    g_test_add_func("/xbox/vk/uniform-layout/missing-backing",
                    test_missing_backing_allocation);
    g_test_add_func("/xbox/vk/uniform-layout/partial-names",
                    test_partially_copied_names);
    g_test_add_func("/xbox/vk/uniform-layout/zero-members",
                    test_zero_members_with_backing);
    g_test_add_func("/xbox/vk/uniform-layout/complete-pair",
                    test_complete_uniform_and_push_layouts);
    return g_test_run();
}

/*
 * NV2A Vulkan diagnostic failpoint tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/pgraph/vk/failpoint.h"

static void test_select_known_failpoint(void)
{
    PGRAPHVkFailpointState state = { 0 };

    g_assert_true(pgraph_vk_failpoint_state_configure(
        &state, "surface-download-map", 0));
    g_assert_cmpint(state.selected, ==,
                    PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_MAP);
}

static void test_reject_unknown_failpoint(void)
{
    PGRAPHVkFailpointState state = { 0 };

    g_assert_false(pgraph_vk_failpoint_state_configure(
        &state, "not-a-failpoint", 0));
    g_assert_cmpint(state.selected, ==, PGRAPH_VK_FAILPOINT_NONE);
}

static void test_immediate_failure_is_one_shot(void)
{
    PGRAPHVkFailpointState state = { 0 };

    g_assert_true(pgraph_vk_failpoint_state_configure(
        &state, "texture-staging-flush", 0));
    g_assert_true(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH));
    g_assert_false(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH));
    g_assert_cmpuint(state.visits, ==, 2);
    g_assert_cmpuint(state.failures, ==, 1);
}

static void test_countdown_ignores_other_failpoints(void)
{
    PGRAPHVkFailpointState state = { 0 };

    g_assert_true(pgraph_vk_failpoint_state_configure(
        &state, "surface-download-invalidate", 2));
    g_assert_false(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_MAP));
    g_assert_false(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_INVALIDATE));
    g_assert_false(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_INVALIDATE));
    g_assert_true(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_INVALIDATE));
    g_assert_false(pgraph_vk_failpoint_state_should_fail(
        &state, PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_INVALIDATE));
    g_assert_cmpuint(state.visits, ==, 4);
    g_assert_cmpuint(state.failures, ==, 1);
}

static void test_runtime_selection_reports_injection(void)
{
    g_setenv("XEMU_VK_DIAGNOSTIC_FAILPOINT", "texture-staging-flush", true);
    g_setenv("XEMU_VK_DIAGNOSTIC_FAIL_AFTER", "1", true);
    pgraph_vk_failpoint_init();
    g_assert_false(pgraph_vk_failpoint_should_fail(
        PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_MAP));
    g_assert_false(pgraph_vk_failpoint_should_fail(
        PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH));
    g_assert_true(pgraph_vk_failpoint_should_fail(
        PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH));
    g_assert_false(pgraph_vk_failpoint_should_fail(
        PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH));
    pgraph_vk_failpoint_report();
    g_unsetenv("XEMU_VK_DIAGNOSTIC_FAILPOINT");
    g_unsetenv("XEMU_VK_DIAGNOSTIC_FAIL_AFTER");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/failpoint/select-known",
                    test_select_known_failpoint);
    g_test_add_func("/xbox/vk/failpoint/reject-unknown",
                    test_reject_unknown_failpoint);
    g_test_add_func("/xbox/vk/failpoint/one-shot",
                    test_immediate_failure_is_one_shot);
    g_test_add_func("/xbox/vk/failpoint/countdown",
                    test_countdown_ignores_other_failpoints);
    g_test_add_func("/xbox/vk/failpoint/runtime-report",
                    test_runtime_selection_reports_injection);
    return g_test_run();
}

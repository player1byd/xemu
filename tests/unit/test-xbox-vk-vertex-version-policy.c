#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/vk/vertex-version-policy.h"

static void test_source_bounds(void)
{
    /* DMA limit 63 is inclusive, while VRAM size 64 is exclusive. */
    g_assert_true(pgraph_vk_vertex_version_source_fits(
        1, 16, 1, 63, 63, 63, 64));
    g_assert_true(pgraph_vk_vertex_version_source_fits(
        4, 32, 16, 0, 111, 0, 112));
    g_assert_false(pgraph_vk_vertex_version_source_fits(
        4, 32, 16, 0, 110, 0, 112));
    g_assert_false(pgraph_vk_vertex_version_source_fits(
        4, 32, 16, 0, 111, 0, 111));
    g_assert_false(pgraph_vk_vertex_version_source_fits(
        1, 16, 2, 63, 63, 63, 64));
    g_assert_false(pgraph_vk_vertex_version_source_fits(
        0, 16, 16, 0, 63, 0, 64));
    g_assert_false(pgraph_vk_vertex_version_source_fits(
        4, 8, 16, 0, 63, 0, 64));
    g_assert_false(pgraph_vk_vertex_version_source_fits(
        2, UINT64_MAX, 16, 0, UINT64_MAX, 0, UINT64_MAX));
}

static void test_budget(void)
{
    g_assert_cmpuint(PGRAPH_VK_VERTEX_VERSION_COPY_BUDGET, ==,
                     PGRAPH_VK_VERTEX_VERSION_SCRATCH_SIZE - 256);
    g_assert_true(pgraph_vk_vertex_version_copy_fits(256, 16, 4096));
    g_assert_false(pgraph_vk_vertex_version_copy_fits(257, 16, 4096));
    g_assert_false(pgraph_vk_vertex_version_copy_fits(0, 16, 4096));
    g_assert_false(pgraph_vk_vertex_version_copy_fits(UINT64_MAX, 16,
                                                       UINT64_MAX));
}

static void test_full_refresh_clears_stale(void)
{
    uint8_t pages[] = { 0, 1, 0, 1, 1 };
    size_t stale_count = 3;

    pgraph_vk_vertex_version_set_stale(pages, G_N_ELEMENTS(pages),
                                        &stale_count, 1, 3, true);
    g_assert_cmpuint(stale_count, ==, 4);
    pgraph_vk_vertex_version_set_stale(pages, G_N_ELEMENTS(pages),
                                        &stale_count, 1, 3, true);
    g_assert_cmpuint(stale_count, ==, 4);
    pgraph_vk_vertex_version_set_stale(pages, G_N_ELEMENTS(pages),
                                        &stale_count, 1, 2, false);
    g_assert_cmpuint(stale_count, ==, 2);

    pgraph_vk_vertex_version_clear_stale(pages, sizeof(pages), &stale_count);
    g_assert_cmpuint(stale_count, ==, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
        g_assert_cmpuint(pages[i], ==, 0);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/vertex-version/source-bounds",
                    test_source_bounds);
    g_test_add_func("/xbox/vk/vertex-version/budget", test_budget);
    g_test_add_func("/xbox/vk/vertex-version/full-refresh-clears-stale",
                    test_full_refresh_clears_stale);
    return g_test_run();
}

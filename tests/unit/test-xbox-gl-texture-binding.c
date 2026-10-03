/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/pgraph/gl/texture-source-identity.h"

static void test_source_identity(void)
{
    const uint64_t texture = 0x01000000;
    const uint64_t palette = 0x02000000;

    g_assert_true(pgraph_gl_texture_source_identity_matches(
        texture, texture, false, 0, 0));
    g_assert_false(pgraph_gl_texture_source_identity_matches(
        texture + 0x1000, texture, false, 0, 0));

    /* Non-indexed bindings do not consume a palette source. */
    g_assert_true(pgraph_gl_texture_source_identity_matches(
        texture, texture, false, palette + 0x1000, palette));

    g_assert_true(pgraph_gl_texture_source_identity_matches(
        texture, texture, true, palette, palette));
    g_assert_false(pgraph_gl_texture_source_identity_matches(
        texture, texture, true, palette + 0x1000, palette));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/gl/texture/source-identity",
                    test_source_identity);
    return g_test_run();
}

/*
 * NV2A Vulkan block-compressed texture layout tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/vk/bc-layout.h"

static void test_small_mips_use_one_physical_block(void)
{
    g_assert_cmpuint(pgraph_vk_bc_mip_size(1, 1, 8), ==, 8);
    g_assert_cmpuint(pgraph_vk_bc_mip_size(2, 3, 16), ==, 16);
    g_assert_cmpuint(pgraph_vk_bc_mip_size(5, 7, 8), ==, 32);
}

static void test_mip_chain_keeps_tail_blocks(void)
{
    /* 8x8, 4x4, 2x2 and 1x1 consume 4, 1, 1 and 1 DXT1 blocks. */
    g_assert_cmpuint(pgraph_vk_bc_layer_size(8, 8, 4, 8), ==, 56);
}

static void test_cube_face_alignment(void)
{
    size_t face_size = pgraph_vk_bc_layer_size(8, 8, 4, 8);

    g_assert_cmpuint(ROUND_UP(face_size, NV2A_CUBEMAP_FACE_ALIGNMENT), ==,
                     NV2A_CUBEMAP_FACE_ALIGNMENT);
}

static void test_native_bc_copy_offsets_use_block_alignment(void)
{
    /* The validation run found a BC3 upload starting at 21848 (8 mod 16). */
    size_t alignment = pgraph_vk_bc_staging_alignment(4, 16);
    size_t offset = ROUND_UP((size_t)21848, alignment);

    g_assert_cmpuint(alignment, ==, 16);
    g_assert_cmpuint(offset, ==, 21856);
    for (unsigned int level = 0; level < 9; level++) {
        g_assert_cmpuint(offset % 16, ==, 0);
        offset += pgraph_vk_bc_mip_size(512 >> level,
                                        512 >> level, 16);
    }
    g_assert_cmpuint(pgraph_vk_bc_staging_alignment(4, 8), ==, 8);
    g_assert_cmpuint(pgraph_vk_bc_staging_alignment(32, 16), ==, 32);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vulkan/bc-layout/small-mips",
                    test_small_mips_use_one_physical_block);
    g_test_add_func("/xbox/vulkan/bc-layout/mip-chain",
                    test_mip_chain_keeps_tail_blocks);
    g_test_add_func("/xbox/vulkan/bc-layout/cube-alignment",
                    test_cube_face_alignment);
    g_test_add_func("/xbox/vulkan/bc-layout/native-copy-alignment",
                    test_native_bc_copy_offsets_use_block_alignment);
    return g_test_run();
}

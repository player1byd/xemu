/*
 * NV2A texture layout tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/texture-layout.h"

static TextureShape cubemap_shape(unsigned int format, unsigned int levels,
                                  unsigned int width, unsigned int height)
{
    return (TextureShape){
        .cubemap = true,
        .dimensionality = 2,
        .color_format = format,
        .levels = levels,
        .storage_levels = levels,
        .width = width,
        .height = height,
    };
}

static void test_compressed_subblock_levels(void)
{
    const unsigned int dimensions[] = { 1, 2, 4 };
    const unsigned int wide_block_formats[] = {
        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8,
        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8,
    };

    for (size_t i = 0; i < ARRAY_SIZE(dimensions); i++) {
        TextureShape dxt1 =
            cubemap_shape(NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5, 1,
                          dimensions[i], dimensions[i]);
        size_t level_size = SIZE_MAX;

        g_assert_true(pgraph_calculate_texture_level_size(
            &dxt1, dimensions[i], dimensions[i], true, 4, &level_size));
        g_assert_cmpuint(level_size, ==, 8);
        for (size_t format = 0; format < ARRAY_SIZE(wide_block_formats);
             format++) {
            TextureShape shape = cubemap_shape(wide_block_formats[format], 1,
                                               dimensions[i], dimensions[i]);
            g_assert_true(pgraph_calculate_texture_level_size(
                &shape, dimensions[i], dimensions[i], true, 4, &level_size));
            g_assert_cmpuint(level_size, ==, 16);
        }
    }
}

static void test_compressed_mip_chain(void)
{
    TextureShape shape =
        cubemap_shape(NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5, 4, 8, 8);
    const size_t expected_sizes[] = { 32, 8, 8, 8 };
    size_t width = shape.width;
    size_t height = shape.height;
    size_t total = 0;

    for (size_t i = 0; i < ARRAY_SIZE(expected_sizes); i++) {
        size_t level_size = SIZE_MAX;
        g_assert_true(pgraph_calculate_texture_level_size(
            &shape, width, height, true, 4, &level_size));
        g_assert_cmpuint(level_size, ==, expected_sizes[i]);
        total += level_size;
        width /= 2;
        height /= 2;
    }
    g_assert_cmpuint(total, ==, 56);

    size_t stride = SIZE_MAX;
    g_assert_true(
        pgraph_calculate_texture_cubemap_face_stride(&shape, true, 4, &stride));
    g_assert_cmpuint(stride, ==, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

static void test_uncompressed_mips_and_alignment(void)
{
    const unsigned int bytes_per_pixel[] = { 1, 2, 4 };

    for (size_t i = 0; i < ARRAY_SIZE(bytes_per_pixel); i++) {
        TextureShape shape =
            cubemap_shape(NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, 4, 8, 8);
        size_t level_size = SIZE_MAX;
        size_t stride = SIZE_MAX;

        g_assert_true(pgraph_calculate_texture_level_size(
            &shape, 1, 1, false, bytes_per_pixel[i], &level_size));
        g_assert_cmpuint(level_size, ==, bytes_per_pixel[i]);
        g_assert_true(pgraph_calculate_texture_cubemap_face_stride(
            &shape, false, bytes_per_pixel[i], &stride));
        g_assert_cmpuint(stride % NV2A_CUBEMAP_FACE_ALIGNMENT, ==, 0);
    }

    TextureShape exact = cubemap_shape(
        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5, 1, 16, 16);
    TextureShape crossed = exact;
    size_t stride = SIZE_MAX;

    g_assert_true(
        pgraph_calculate_texture_cubemap_face_stride(&exact, true, 4, &stride));
    g_assert_cmpuint(stride, ==, NV2A_CUBEMAP_FACE_ALIGNMENT);
    crossed.storage_levels = crossed.levels = 2;
    g_assert_true(pgraph_calculate_texture_cubemap_face_stride(&crossed, true,
                                                               4, &stride));
    g_assert_cmpuint(stride, ==, 2 * NV2A_CUBEMAP_FACE_ALIGNMENT);
}

static void test_face_offsets_and_storage_levels(void)
{
    TextureShape shape = cubemap_shape(
        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5, 1, 16, 16);
    size_t stride = SIZE_MAX;

    shape.storage_levels = 2;
    g_assert_true(
        pgraph_calculate_texture_cubemap_face_stride(&shape, true, 4, &stride));
    g_assert_cmpuint(stride, ==, 2 * NV2A_CUBEMAP_FACE_ALIGNMENT);
    for (size_t face = 0; face < 6; face++) {
        g_assert_cmpuint(face * stride, ==,
                         face * 2 * NV2A_CUBEMAP_FACE_ALIGNMENT);
    }
}

static void test_bordered_storage_extent(void)
{
    TextureShape shape =
        cubemap_shape(NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5, 4, 8, 8);
    size_t stride = SIZE_MAX;

    shape.border = true;
    g_assert_true(
        pgraph_calculate_texture_cubemap_face_stride(&shape, true, 4, &stride));
    g_assert_cmpuint(stride, ==, 2 * NV2A_CUBEMAP_FACE_ALIGNMENT);
}

static void test_bordered_mip_crop(void)
{
    typedef struct BorderedCropCase {
        unsigned int base_width, base_height;
        unsigned int stored_width, stored_height, level;
        unsigned int width, height, skip_pixels, skip_rows;
    } BorderedCropCase;
    const BorderedCropCase cases[] = {
        { 8, 8, 16, 16, 0, 8, 8, 4, 4 },
        { 8, 8, 8, 8, 1, 4, 4, 2, 2 },
        { 8, 8, 4, 4, 2, 2, 2, 1, 1 },
        { 8, 8, 2, 2, 3, 1, 1, 0, 0 },
        { 4, 4, 16, 16, 0, 4, 4, 4, 4 },
        { 2, 2, 16, 16, 0, 2, 2, 4, 4 },
        { 1, 1, 16, 16, 0, 1, 1, 4, 4 },
        { 8, 4, 8, 8, 1, 4, 2, 2, 2 },
        { 8, 8, 1, 1, 0, 1, 1, 0, 0 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        const BorderedCropCase *c = &cases[i];
        PGRAPHTextureMipCrop crop = pgraph_bordered_texture_mip_crop(
            c->base_width, c->base_height, c->stored_width,
            c->stored_height, c->level);
        g_assert_cmpuint(crop.width, ==, c->width);
        g_assert_cmpuint(crop.height, ==, c->height);
        g_assert_cmpuint(crop.skip_pixels, ==, c->skip_pixels);
        g_assert_cmpuint(crop.skip_rows, ==, c->skip_rows);
        g_assert_cmpuint(crop.width + crop.skip_pixels, <=,
                         c->stored_width);
        g_assert_cmpuint(crop.height + crop.skip_rows, <=,
                         c->stored_height);
    }
}

static void test_invalid_and_overflow(void)
{
    TextureShape shape =
        cubemap_shape(NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, 1, 4, 4);
    size_t result;

    g_assert_false(
        pgraph_calculate_texture_cubemap_face_stride(NULL, false, 4, &result));
    g_assert_false(
        pgraph_calculate_texture_cubemap_face_stride(&shape, false, 4, NULL));
    shape.cubemap = false;
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                4, &result));
    shape.cubemap = true;
    shape.dimensionality = 3;
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                4, &result));
    shape.dimensionality = 2;
    shape.width = 0;
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                4, &result));
    shape.width = 4;
    shape.storage_levels = shape.levels = 0;
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                4, &result));

    shape = cubemap_shape(NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, 1,
                          UINT_MAX, UINT_MAX);
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                2, &result));
    shape.storage_levels = shape.levels = 2;
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                1, &result));
    shape.storage_levels = shape.levels = 1;
    shape.height = 1000000000U;
    g_assert_false(pgraph_calculate_texture_cubemap_face_stride(&shape, false,
                                                                1, &result));

    g_assert_false(pgraph_texture_size_align(
        SIZE_MAX, NV2A_CUBEMAP_FACE_ALIGNMENT, &result));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/pgraph/texture-layout/compressed-subblock",
                    test_compressed_subblock_levels);
    g_test_add_func("/xbox/pgraph/texture-layout/compressed-chain",
                    test_compressed_mip_chain);
    g_test_add_func("/xbox/pgraph/texture-layout/uncompressed",
                    test_uncompressed_mips_and_alignment);
    g_test_add_func("/xbox/pgraph/texture-layout/face-offsets",
                    test_face_offsets_and_storage_levels);
    g_test_add_func("/xbox/pgraph/texture-layout/bordered-storage",
                    test_bordered_storage_extent);
    g_test_add_func("/xbox/pgraph/texture-layout/bordered-crop",
                    test_bordered_mip_crop);
    g_test_add_func("/xbox/pgraph/texture-layout/invalid-overflow",
                    test_invalid_and_overflow);
    return g_test_run();
}

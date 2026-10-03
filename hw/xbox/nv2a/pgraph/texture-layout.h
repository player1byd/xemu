/*
 * QEMU Geforce NV2A encoded texture layout helpers
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_TEXTURE_LAYOUT_H
#define HW_XBOX_NV2A_PGRAPH_TEXTURE_LAYOUT_H

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_regs.h"

typedef struct TextureShape {
    bool cubemap;
    unsigned int dimensionality;
    unsigned int color_format;
    unsigned int levels;
    unsigned int storage_levels;
    unsigned int width, height, depth;
    bool border;

    unsigned int min_mipmap_level, max_mipmap_level;
    unsigned int pitch;
} TextureShape;

typedef struct PGRAPHTextureMipCrop {
    unsigned int width;
    unsigned int height;
    unsigned int skip_pixels;
    unsigned int skip_rows;
} PGRAPHTextureMipCrop;

static inline PGRAPHTextureMipCrop pgraph_bordered_texture_mip_crop(
    unsigned int base_width, unsigned int base_height,
    unsigned int stored_width, unsigned int stored_height, unsigned int level)
{
    unsigned int logical_width = base_width;
    unsigned int logical_height = base_height;

    for (unsigned int i = 0; i < level; i++) {
        logical_width = MAX(logical_width / 2, 1U);
        logical_height = MAX(logical_height / 2, 1U);
    }
    logical_width = MIN(logical_width, stored_width);
    logical_height = MIN(logical_height, stored_height);
    unsigned int border = level < 3 ? 4U >> level : 0;

    return (PGRAPHTextureMipCrop) {
        .width = logical_width,
        .height = logical_height,
        .skip_pixels = MIN(border, stored_width - logical_width),
        .skip_rows = MIN(border, stored_height - logical_height),
    };
}

static inline bool pgraph_texture_size_mul(size_t a, size_t b, size_t *result)
{
    if (b && a > SIZE_MAX / b) {
        return false;
    }
    *result = a * b;
    return true;
}

static inline bool pgraph_texture_size_add(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b) {
        return false;
    }
    *result = a + b;
    return true;
}

static inline bool pgraph_texture_size_align(size_t value, size_t alignment,
                                             size_t *result)
{
    if (!alignment) {
        return false;
    }

    size_t remainder = value % alignment;
    size_t padding = remainder ? alignment - remainder : 0;
    return pgraph_texture_size_add(value, padding, result);
}

static inline bool pgraph_calculate_texture_level_size(
    const TextureShape *shape, size_t width, size_t height, bool compressed,
    unsigned int bytes_per_pixel, size_t *level_size)
{
    if (!shape || !level_size || !width || !height || !bytes_per_pixel) {
        return false;
    }

    if (compressed) {
        size_t block_size =
            shape->color_format ==
                    NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;

        if (width > SIZE_MAX - 3 || height > SIZE_MAX - 3) {
            return false;
        }
        width = (width + 3) & ~(size_t)3;
        height = (height + 3) & ~(size_t)3;
        return pgraph_texture_size_mul(width / 4, height / 4, level_size) &&
               pgraph_texture_size_mul(*level_size, block_size, level_size);
    }

    return pgraph_texture_size_mul(width, height, level_size) &&
           pgraph_texture_size_mul(*level_size, bytes_per_pixel, level_size);
}

/* Return the aligned source stride between non-linear cubemap faces. */
static inline bool pgraph_calculate_texture_cubemap_face_stride(
    const TextureShape *shape, bool compressed, unsigned int bytes_per_pixel,
    size_t *stride)
{
    if (!shape || !stride || !shape->cubemap || shape->dimensionality != 2 ||
        !shape->width || !shape->height || !shape->storage_levels ||
        !bytes_per_pixel) {
        return false;
    }

    size_t width = shape->width;
    size_t height = shape->height;
    if (shape->border) {
        if (!pgraph_texture_size_mul(width, 2, &width) ||
            !pgraph_texture_size_mul(height, 2, &height)) {
            return false;
        }
        width = MAX((size_t)16, width);
        height = MAX((size_t)16, height);
    }

    size_t total = 0;
    for (unsigned int level = 0; level < shape->storage_levels; level++) {
        size_t level_size;
        if (!pgraph_calculate_texture_level_size(
                shape, MAX(width, (size_t)1), MAX(height, (size_t)1),
                compressed, bytes_per_pixel, &level_size) ||
            !pgraph_texture_size_add(total, level_size, &total)) {
            return false;
        }
        width /= 2;
        height /= 2;
    }

    if (!pgraph_texture_size_align(total, NV2A_CUBEMAP_FACE_ALIGNMENT,
                                   &total) ||
        total > SIZE_MAX / 6) {
        return false;
    }
    *stride = total;
    return true;
}

#endif

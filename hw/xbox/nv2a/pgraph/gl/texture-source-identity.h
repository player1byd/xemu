/*
 * NV2A OpenGL texture source identity
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_GL_TEXTURE_SOURCE_IDENTITY_H
#define HW_XBOX_NV2A_PGRAPH_GL_TEXTURE_SOURCE_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool pgraph_gl_texture_source_identity_matches(
    uint64_t current_texture, uint64_t bound_texture, bool indexed,
    uint64_t current_palette, uint64_t bound_palette)
{
    return current_texture == bound_texture &&
           (!indexed || current_palette == bound_palette);
}

#endif

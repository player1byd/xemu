/*
 * NV2A Vulkan reflected uniform-layout ownership
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_UNIFORM_LAYOUT_H
#define HW_XBOX_NV2A_PGRAPH_VK_UNIFORM_LAYOUT_H

#include <glib.h>
#include <stdlib.h>

typedef struct ShaderUniform {
    const char *name;
    size_t dim_v;
    size_t dim_a;
    size_t align;
    size_t stride;
    size_t offset;
} ShaderUniform;

typedef struct ShaderUniformLayout {
    ShaderUniform *uniforms;
    size_t num_uniforms;
    size_t total_size;
    void *allocation;
} ShaderUniformLayout;

/* Also accepts partially populated layouts from failed reflection. */
static inline void shader_uniform_layout_clear(ShaderUniformLayout *layout)
{
    if (layout->uniforms) {
        for (size_t i = 0; i < layout->num_uniforms; i++) {
            free((void *)layout->uniforms[i].name);
        }
    }
    g_free(layout->uniforms);
    g_free(layout->allocation);
    *layout = (ShaderUniformLayout) { 0 };
}

#endif

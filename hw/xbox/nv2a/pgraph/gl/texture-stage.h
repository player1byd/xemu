/*
 * NV2A OpenGL texture-stage ownership
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_GL_TEXTURE_STAGE_H
#define HW_XBOX_NV2A_PGRAPH_GL_TEXTURE_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <epoxy/gl.h>

typedef struct TextureBinding {
    unsigned int refcnt;
    int draw_time;
    uint64_t data_hash;
    uint64_t texture_vram_offset;
    uint64_t palette_vram_offset;
    unsigned int scale;
    unsigned int min_filter;
    unsigned int mag_filter;
    uint32_t lod_bias;
    unsigned int addru;
    unsigned int addrv;
    unsigned int addrp;
    uint32_t border_color;
    bool border_color_set;
    GLenum gl_target;
    GLuint gl_texture;
} TextureBinding;

void pgraph_gl_texture_binding_destroy(TextureBinding *binding);

/* The caller must first select the affected GL texture unit. */
void pgraph_gl_reset_texture_stage(TextureBinding **active_binding);

#endif

/*
 * NV2A OpenGL texture-stage ownership
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "texture-stage.h"

void pgraph_gl_texture_binding_destroy(TextureBinding *binding)
{
    assert(binding->refcnt > 0);
    binding->refcnt--;
    if (binding->refcnt == 0) {
        glDeleteTextures(1, &binding->gl_texture);
        g_free(binding);
    }
}

void pgraph_gl_reset_texture_stage(TextureBinding **active_binding)
{
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glBindTexture(GL_TEXTURE_1D, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_3D, 0);

    if (*active_binding) {
        pgraph_gl_texture_binding_destroy(*active_binding);
        *active_binding = NULL;
    }
}

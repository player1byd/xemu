/*
 * NV2A OpenGL draw completion lifecycle
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "draw-lifecycle.h"
#include "renderer.h"

void pgraph_gl_draw_lifecycle_reset(PGRAPHGLDrawLifecycle *lifecycle)
{
    *lifecycle = (PGRAPHGLDrawLifecycle) {
        .result = PGRAPH_GL_DRAW_EMPTY,
    };
}

void pgraph_gl_draw_lifecycle_prepare(
    PGRAPHGLDrawLifecycle *lifecycle, bool query_active,
    bool color_write, bool zeta_write, bool color_dirty, bool zeta_dirty)
{
    lifecycle->prepared = true;
    lifecycle->query_active = query_active;
    lifecycle->color_write = color_write;
    lifecycle->zeta_write = zeta_write;
    lifecycle->color_dirty = color_dirty;
    lifecycle->zeta_dirty = zeta_dirty;
}

void pgraph_gl_draw_lifecycle_record(
    PGRAPHGLDrawLifecycle *lifecycle, PGRAPHGLDrawResult result)
{
    if (lifecycle->result == PGRAPH_GL_DRAW_SUBMITTED ||
        result == PGRAPH_GL_DRAW_SUBMITTED) {
        lifecycle->result = PGRAPH_GL_DRAW_SUBMITTED;
    } else if (lifecycle->result == PGRAPH_GL_DRAW_REJECTED ||
               result == PGRAPH_GL_DRAW_REJECTED) {
        lifecycle->result = PGRAPH_GL_DRAW_REJECTED;
    }
}

bool pgraph_gl_draw_lifecycle_take_query(PGRAPHGLDrawLifecycle *lifecycle)
{
    bool active = lifecycle->query_active;
    lifecycle->query_active = false;
    return active;
}

void pgraph_gl_complete_draw_lifecycle(
    PGRAPHState *pg, PGRAPHGLState *r, PGRAPHGLDrawResult result,
    bool color_write, bool zeta_write, bool color_dirty, bool zeta_dirty)
{
    if (result != PGRAPH_GL_DRAW_SUBMITTED) {
        return;
    }

    pg->draw_time++;
    if (r->color_binding && color_write) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && zeta_write) {
        r->zeta_binding->draw_time = pg->draw_time;
    }
    pgraph_gl_set_surface_dirty(pg, color_dirty, zeta_dirty);
}

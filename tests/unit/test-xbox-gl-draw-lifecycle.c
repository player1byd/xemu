/*
 * NV2A OpenGL draw lifecycle tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/gl/draw-lifecycle.h"
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"

static unsigned int dirty_calls;
static bool last_color_dirty;
static bool last_zeta_dirty;

void pgraph_gl_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    dirty_calls++;
    last_color_dirty = color;
    last_zeta_dirty = zeta;
}

static void test_rejected_draw_does_not_publish_surface_generation(void)
{
    PGRAPHState pg = { 0 };
    PGRAPHGLState renderer = { 0 };
    SurfaceBinding color = { .draw_time = 7, .draw_dirty = false };
    SurfaceBinding zeta = { .draw_time = 8, .draw_dirty = false };

    pg.gl_renderer_state = &renderer;
    pg.draw_time = 10;
    renderer.color_binding = &color;
    renderer.zeta_binding = &zeta;
    dirty_calls = 0;

    pgraph_gl_complete_draw_lifecycle(
        &pg, &renderer, PGRAPH_GL_DRAW_REJECTED,
        true, true, true, true);

    g_assert_cmpuint(pg.draw_time, ==, 10);
    g_assert_cmpuint(color.draw_time, ==, 7);
    g_assert_cmpuint(zeta.draw_time, ==, 8);
    g_assert_false(color.draw_dirty);
    g_assert_false(zeta.draw_dirty);
    g_assert_cmpuint(dirty_calls, ==, 0);

    pgraph_gl_complete_draw_lifecycle(
        &pg, &renderer, PGRAPH_GL_DRAW_SUBMITTED,
        true, true, true, true);

    g_assert_cmpuint(pg.draw_time, ==, 11);
    g_assert_cmpuint(color.draw_time, ==, 11);
    g_assert_cmpuint(zeta.draw_time, ==, 11);
    g_assert_cmpuint(dirty_calls, ==, 1);
    g_assert_true(last_color_dirty);
    g_assert_true(last_zeta_dirty);
}

static void test_empty_draw_does_not_publish_surface_generation(void)
{
    PGRAPHState pg = { .draw_time = 20 };
    PGRAPHGLState renderer = { 0 };

    dirty_calls = 0;
    pgraph_gl_complete_draw_lifecycle(
        &pg, &renderer, PGRAPH_GL_DRAW_EMPTY,
        true, true, true, true);

    g_assert_cmpuint(pg.draw_time, ==, 20);
    g_assert_cmpuint(dirty_calls, ==, 0);
}

static void test_submitted_segment_survives_rejected_final_segment(void)
{
    PGRAPHGLDrawLifecycle lifecycle;
    PGRAPHState pg = { 0 };
    PGRAPHGLState renderer = { 0 };
    SurfaceBinding color = { 0 };
    SurfaceBinding zeta = { 0 };

    pg.gl_renderer_state = &renderer;
    renderer.color_binding = &color;
    renderer.zeta_binding = &zeta;
    dirty_calls = 0;

    pgraph_gl_draw_lifecycle_reset(&lifecycle);
    pgraph_gl_draw_lifecycle_prepare(
        &lifecycle, true, true, true, true, true);
    pgraph_gl_draw_lifecycle_record(
        &lifecycle, PGRAPH_GL_DRAW_SUBMITTED);
    pgraph_gl_draw_lifecycle_record(
        &lifecycle, PGRAPH_GL_DRAW_REJECTED);

    g_assert_cmpint(lifecycle.result, ==, PGRAPH_GL_DRAW_SUBMITTED);
    g_assert_true(pgraph_gl_draw_lifecycle_take_query(&lifecycle));
    g_assert_false(pgraph_gl_draw_lifecycle_take_query(&lifecycle));
    pgraph_gl_complete_draw_lifecycle(
        &pg, &renderer, lifecycle.result,
        lifecycle.color_write, lifecycle.zeta_write,
        lifecycle.color_dirty, lifecycle.zeta_dirty);
    g_assert_cmpuint(pg.draw_time, ==, 1);
    g_assert_cmpuint(dirty_calls, ==, 1);
}

static void test_submitted_segment_survives_empty_final_segment(void)
{
    PGRAPHGLDrawLifecycle lifecycle;

    pgraph_gl_draw_lifecycle_reset(&lifecycle);
    pgraph_gl_draw_lifecycle_prepare(
        &lifecycle, true, true, false, true, false);
    pgraph_gl_draw_lifecycle_record(
        &lifecycle, PGRAPH_GL_DRAW_SUBMITTED);
    pgraph_gl_draw_lifecycle_record(&lifecycle, PGRAPH_GL_DRAW_EMPTY);

    g_assert_cmpint(lifecycle.result, ==, PGRAPH_GL_DRAW_SUBMITTED);
    g_assert_true(lifecycle.color_write);
    g_assert_false(lifecycle.zeta_write);
}

static void test_rejected_scope_without_submission_stays_rejected(void)
{
    PGRAPHGLDrawLifecycle lifecycle;
    PGRAPHState pg = { 0 };
    PGRAPHGLState renderer = { 0 };

    pg.gl_renderer_state = &renderer;
    dirty_calls = 0;

    pgraph_gl_draw_lifecycle_reset(&lifecycle);
    pgraph_gl_draw_lifecycle_prepare(
        &lifecycle, true, true, true, true, true);
    pgraph_gl_draw_lifecycle_record(
        &lifecycle, PGRAPH_GL_DRAW_REJECTED);
    pgraph_gl_draw_lifecycle_record(&lifecycle, PGRAPH_GL_DRAW_EMPTY);

    g_assert_cmpint(lifecycle.result, ==, PGRAPH_GL_DRAW_REJECTED);
    g_assert_true(pgraph_gl_draw_lifecycle_take_query(&lifecycle));
    pgraph_gl_complete_draw_lifecycle(
        &pg, &renderer, lifecycle.result,
        lifecycle.color_write, lifecycle.zeta_write,
        lifecycle.color_dirty, lifecycle.zeta_dirty);
    g_assert_cmpuint(pg.draw_time, ==, 0);
    g_assert_cmpuint(dirty_calls, ==, 0);

    pgraph_gl_draw_lifecycle_reset(&lifecycle);
    g_assert_cmpint(lifecycle.result, ==, PGRAPH_GL_DRAW_EMPTY);
    g_assert_false(pgraph_gl_draw_lifecycle_take_query(&lifecycle));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/gl/draw-lifecycle/rejected",
                    test_rejected_draw_does_not_publish_surface_generation);
    g_test_add_func("/xbox/gl/draw-lifecycle/empty",
                    test_empty_draw_does_not_publish_surface_generation);
    g_test_add_func("/xbox/gl/draw-lifecycle/submitted-then-rejected",
                    test_submitted_segment_survives_rejected_final_segment);
    g_test_add_func("/xbox/gl/draw-lifecycle/submitted-then-empty",
                    test_submitted_segment_survives_empty_final_segment);
    g_test_add_func("/xbox/gl/draw-lifecycle/rejected-without-submit",
                    test_rejected_scope_without_submission_stays_rejected);
    return g_test_run();
}

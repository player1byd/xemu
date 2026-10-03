/*
 * Real SDL/OpenGL lifecycle regression. Requires an interactive graphics host:
 * XEMU_TEST_GLOFFSCREEN=1 ./test-xbox-gloffscreen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <SDL3/SDL.h>
#include "hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/gloffscreen.h"
#include "hw/xbox/nv2a/pgraph/gl/texture-stage.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s failed (%s)\n", \
                __LINE__, #condition, SDL_GetError()); \
        puts("not ok 1 - shared contexts and failed-stage reset lifecycle"); \
        puts("1..1"); \
        return EXIT_FAILURE; \
    } \
} while (0)

static int window_count(void)
{
    int count = -1;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_free(windows);
    return count;
}

int main(int argc, char **argv)
{
    if (!getenv("XEMU_TEST_GLOFFSCREEN")) {
        puts("1..0 # SKIP set XEMU_TEST_GLOFFSCREEN=1 on a graphics host");
        return EXIT_SUCCESS;
    }

    CHECK(SDL_Init(SDL_INIT_VIDEO));
    int baseline = window_count();
    CHECK(baseline >= 0);
    GloContext *anchor = glo_context_create();
    CHECK(window_count() == baseline + 1);
    SDL_GLContext anchor_gl = SDL_GL_GetCurrentContext();
    CHECK(anchor_gl != NULL);
    glo_context_destroy(NULL);
    CHECK(SDL_GL_GetCurrentContext() == anchor_gl);

    GLuint texture;
    const uint32_t pixel = 0xff123456;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
    CHECK(glGetError() == GL_NO_ERROR);

    for (int i = 0; i < 64; i++) {
        GloContext *temporary = glo_context_create();
        CHECK(window_count() == baseline + 2);
        CHECK(glIsTexture(texture));
        glo_context_destroy(temporary);
        CHECK(SDL_GL_GetCurrentContext() == NULL);
        SDL_PumpEvents();
        CHECK(window_count() == baseline + 1);
        glo_set_current(anchor);
        CHECK(SDL_GL_GetCurrentContext() == anchor_gl);
        CHECK(glIsTexture(texture));
        uint32_t actual = 0;
        glBindTexture(GL_TEXTURE_2D, texture);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, &actual);
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(actual == pixel);
    }

    GLuint old_2d;
    GLuint unrelated_cube;
    glGenTextures(1, &old_2d);
    glGenTextures(1, &unrelated_cube);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, old_2d);
    glBindTexture(GL_TEXTURE_CUBE_MAP, unrelated_cube);
    TextureBinding *active = g_new0(TextureBinding, 1);
    active->refcnt = 1;
    active->gl_target = GL_TEXTURE_2D;
    active->gl_texture = old_2d;

    pgraph_gl_reset_texture_stage(&active);

    GLint bound = -1;
    CHECK(active == NULL);
    glGetIntegerv(GL_TEXTURE_BINDING_1D, &bound);
    CHECK(bound == 0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    CHECK(bound == 0);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &bound);
    CHECK(bound == 0);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &bound);
    CHECK(bound == 0);
    CHECK(!glIsTexture(old_2d));

    GLuint retry_cube;
    glGenTextures(1, &retry_cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, retry_cube);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &bound);
    CHECK(bound == (GLint)retry_cube);
    glDeleteTextures(1, &retry_cube);
    glDeleteTextures(1, &unrelated_cube);

    glDeleteTextures(1, &texture);
    glo_context_destroy(anchor);
    SDL_PumpEvents();
    CHECK(window_count() == baseline);
    CHECK(SDL_GL_GetCurrentContext() == NULL);
    SDL_Quit();
    puts("ok 1 - shared contexts and failed-stage reset lifecycle");
    puts("1..1");
    return EXIT_SUCCESS;
}

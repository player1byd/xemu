/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include <SDL3/SDL.h>
#include <epoxy/gl.h>

/* This draw fixture does not classify guest texture formats. */
const BasicColorFormatInfo kelvin_color_format_info_map[66] = { 0 };

static SDL_Window *window;
static SDL_GLContext context;
static GLuint framebuffer, color, depth, vao;

static GLuint compile(GLenum kind, const char *source)
{
    GLuint shader = glCreateShader(kind);
    GLint success;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        g_error("Shader compilation failed: %s", log);
    }
    return shader;
}

static GLuint program(const PshState *state)
{
    MString *vertex = mstring_from_str("#version 400\n");
    pgraph_glsl_get_vtx_header(vertex, false, true, false, false, false);
    mstring_append(
        vertex,
        "uniform float geometricDepth;\n"
        "uniform vec3 numerator, denominator;\n"
        "uniform vec4 inputColor;\n"
        "uniform int depthStage;\n"
        "void main() {\n"
        "vec2 p = vec2(gl_VertexID == 1 ? 3 : -1, gl_VertexID == 2 ? 3 : -1);\n"
        "gl_Position = vec4(p, 0, 1);\n"
        "vtxD0 = vtxD1 = vtxB0 = vtxB1 = vec4(1);\n"
        "vtxFog = 1; vtxT0 = inputColor;\n"
        "vtxT1 = vec4(numerator, 1);\n"
        "vtxT2 = vec4(depthStage == 3 ? numerator : denominator, 1);\n"
        "vtxT3 = vec4(denominator, 1);\n"
        "vtxPos0 = vec4(0, 0, geometricDepth, 1);\n"
        "vtxPos1 = vec4(16, 0, geometricDepth, 1);\n"
        "vtxPos2 = vec4(0, 16, geometricDepth, 1); triMZ = 1;\n"
        "}\n");
    MString *fragment = pgraph_glsl_gen_psh(state, (GenPshGlslOptions){ 0 });
    GLuint vs = compile(GL_VERTEX_SHADER, mstring_get_str(vertex));
    GLuint fs = compile(GL_FRAGMENT_SHADER, mstring_get_str(fragment));
    GLuint result = glCreateProgram();
    glAttachShader(result, vs);
    glAttachShader(result, fs);
    glLinkProgram(result);
    GLint success;
    glGetProgramiv(result, GL_LINK_STATUS, &success);
    g_assert_true(success);
    glDeleteShader(vs);
    glDeleteShader(fs);
    mstring_unref(vertex);
    mstring_unref(fragment);
    return result;
}

typedef struct DepthCase {
    const char *name;
    int stage;
    enum PshDepthFormat format;
    bool perspective;
    float geometric, numerator, denominator, offset;
    float expected;
    unsigned int hilo_alpha;
    bool invalid_predecessor;
    unsigned int previous_input;
    bool disable_clipping;
} DepthCase;

static void test_depth(const void *opaque)
{
    if (!context) {
        g_test_skip("OpenGL 4 context unavailable");
        return;
    }
    const DepthCase *c = opaque;
    float maximum = c->format == DEPTH_FORMAT_D16 ? 65535 : 16777215;
    PshState state = {
        .smooth_shading = true,
        .depth_clipping = !c->disable_clipping,
        .z_perspective = c->perspective,
        .depth_format = c->format,
        .final_inputs_1 = 0x20202000,
        .shader_stage_program = PS_TEXTUREMODES_PASSTHRU,
    };
    if (c->stage) {
        if (!c->invalid_predecessor) {
            state.shader_stage_program |= PS_TEXTUREMODES_DOTPRODUCT
                                          << ((c->stage - 1) * 5);
        }
        state.shader_stage_program |= PS_TEXTUREMODES_DOT_ZW << (c->stage * 5);
    }
    if (c->hilo_alpha) {
        state.other_stage_input = (4 << 4) | (4 << 8);
    }
    if (c->stage == 3) {
        state.other_stage_input =
            (state.other_stage_input & ~(0xFu << 20)) |
            ((c->previous_input & 0xFu) << 20);
    }
    GLuint p = program(&state);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "depthStage"), c->stage);
    glUniform1f(glGetUniformLocation(p, "geometricDepth"),
                c->geometric * maximum);
    glUniform3f(glGetUniformLocation(p, "numerator"), c->numerator * maximum, 0,
                0);
    glUniform3f(glGetUniformLocation(p, "denominator"), c->denominator, 0, 0);
    glUniform4f(glGetUniformLocation(p, "inputColor"), 1, 0, 0, 1);
    if (c->hilo_alpha) {
        glUniform3f(glGetUniformLocation(p, "numerator"), 1, 1.f / 65535, 0);
        glUniform3f(glGetUniformLocation(p, "denominator"), 0, 0,
                    1.f / maximum);
        float low = c->hilo_alpha == 255 ? 1 : 0;
        glUniform4f(glGetUniformLocation(p, "inputColor"), low, low, low,
                    c->hilo_alpha / 255.f);
    }
    glUniform4f(glGetUniformLocation(p, "clipRange"), 0, maximum, 0, maximum);
    glUniform2i(glGetUniformLocation(p, "surfaceScale"), 1, 1);
    glUniform1f(glGetUniformLocation(p, "depthOffset"), c->offset);
    glUniform1f(glGetUniformLocation(p, "depthFactor"), c->offset);
    const GLint regions[32] = { 0, 0, 8, 8 };
    glUniform4iv(glGetUniformLocation(p, "clipRegion"), 8, regions);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER,
                          c->format == DEPTH_FORMAT_D16 ?
                          GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24, 8, 8);
    GLint depth_bits;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_DEPTH_SIZE,
                                 &depth_bits);
    g_assert_cmpint(depth_bits, ==,
                    c->format == DEPTH_FORMAT_D16 ? 16 : 24);
    glViewport(0, 0, 8, 8);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glClearDepth(1);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    float actual;
    glReadPixels(4, 4, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &actual);
    g_test_message("%s: expected %.8f, observed %.8f", c->name, c->expected,
                   actual);
    g_assert_cmpfloat_with_epsilon(actual, c->expected, 0.00003f);
    if (c->expected == 1 && !c->disable_clipping) {
        uint8_t pixel[4];
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        g_assert_cmpuint(pixel[1], ==, 255);
    }
    g_assert_cmpuint(glGetError(), ==, GL_NO_ERROR);
    glDeleteProgram(p);
}

static void test_restored_depth_allows_following_geometry(void)
{
    if (!context) {
        g_test_skip("OpenGL 4 context unavailable");
        return;
    }
    const DepthCase c = { "restore-before-next-draw",
                          3,
                          DEPTH_FORMAT_D24,
                          false,
                          0,
                          .75f,
                          1,
                          0,
                          .75f };
    test_depth(&c);
    GLuint vs = compile(GL_VERTEX_SHADER,
                        "#version 400\nvoid main() {\n"
                        "gl_Position = vec4(gl_VertexID == 1 ? 3 : -1,"
                        " gl_VertexID == 2 ? 3 : -1, 0, 1); }\n");
    GLuint fs = compile(GL_FRAGMENT_SHADER,
                        "#version 400\nout vec4 color;\nvoid main() {\n"
                        "color = vec4(0, 0, 1, 1); gl_FragDepth = 0.5; }\n");
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint success;
    glGetProgramiv(p, GL_LINK_STATUS, &success);
    g_assert_true(success);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t pixel[4];
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    g_assert_cmpuint(pixel[2], ==, 255);
    g_assert_cmpuint(glGetError(), ==, GL_NO_ERROR);
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

static void set_mode(PshState *state, int stage, int mode)
{
    state->shader_stage_program =
        (state->shader_stage_program & ~(0x1Fu << (stage * 5))) |
        ((uint32_t)mode << (stage * 5));
}

static void set_input(PshState *state, int stage, int input)
{
    g_assert_cmpint(stage, >=, 2);
    state->other_stage_input =
        (state->other_stage_input & ~(0xFu << (stage * 4 + 8))) |
        ((uint32_t)input << (stage * 4 + 8));
}

static void test_texture_program_consistency(void)
{
    PshState base = { 0 };
    set_mode(&base, 0, PS_TEXTUREMODES_PASSTHRU);
    set_mode(&base, 2, PS_TEXTUREMODES_DOTPRODUCT);
    set_mode(&base, 3, PS_TEXTUREMODES_DOT_ZW);

    for (int selector = 0; selector < 16; selector++) {
        PshState state = base;
        set_input(&state, 3, selector);
        pgraph_glsl_normalize_psh_state(&state);
        int effective = (state.shader_stage_program >> 15) & 0x1F;
        g_assert_cmpint(effective, ==,
                        selector == 0 ? PS_TEXTUREMODES_DOT_ZW :
                                        PS_TEXTUREMODES_NONE);
    }

    PshState no_source = base;
    set_mode(&no_source, 0, PS_TEXTUREMODES_NONE);
    pgraph_glsl_normalize_psh_state(&no_source);
    g_assert_cmpint((no_source.shader_stage_program >> 15) & 0x1F, ==,
                    PS_TEXTUREMODES_NONE);

    PshState unusable_source = base;
    set_mode(&unusable_source, 0, PS_TEXTUREMODES_CLIPPLANE);
    set_mode(&unusable_source, 1, PS_TEXTUREMODES_PASSTHRU);
    set_input(&unusable_source, 2, 1);
    pgraph_glsl_normalize_psh_state(&unusable_source);
    g_assert_cmpint((unusable_source.shader_stage_program >> 15) & 0x1F, ==,
                    PS_TEXTUREMODES_NONE);

    PshState invalid_stage_mode = base;
    set_mode(&invalid_stage_mode, 0, PS_TEXTUREMODES_DOT_ST);
    pgraph_glsl_normalize_psh_state(&invalid_stage_mode);
    g_assert_cmpint((invalid_stage_mode.shader_stage_program >> 15) & 0x1F,
                    ==, PS_TEXTUREMODES_NONE);

    PshState inconsistent_source = base;
    set_mode(&inconsistent_source, 1, PS_TEXTUREMODES_DOT_ZW);
    set_input(&inconsistent_source, 3, 1);
    pgraph_glsl_normalize_psh_state(&inconsistent_source);
    g_assert_cmpint((inconsistent_source.shader_stage_program >> 5) & 0x1F,
                    ==, PS_TEXTUREMODES_NONE);
    g_assert_cmpint((inconsistent_source.shader_stage_program >> 15) & 0x1F,
                    ==, PS_TEXTUREMODES_NONE);

    PshState duplicate = base;
    set_mode(&duplicate, 1, PS_TEXTUREMODES_DOTPRODUCT);
    set_mode(&duplicate, 2, PS_TEXTUREMODES_DOT_ZW);
    pgraph_glsl_normalize_psh_state(&duplicate);
    g_assert_cmpint((duplicate.shader_stage_program >> 10) & 0x1F, ==,
                    PS_TEXTUREMODES_DOT_ZW);
    g_assert_cmpint((duplicate.shader_stage_program >> 15) & 0x1F, ==,
                    PS_TEXTUREMODES_NONE);

    PshState first = base, second = base;
    set_input(&first, 3, 4);
    set_input(&second, 3, 15);
    g_assert_cmpint(memcmp(&first, &second, sizeof(first)), !=, 0);
    pgraph_glsl_normalize_psh_state(&first);
    pgraph_glsl_normalize_psh_state(&second);
    g_assert_cmpint(memcmp(&first, &second, sizeof(first)), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (SDL_Init(SDL_INIT_VIDEO)) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
        window = SDL_CreateWindow("Depth replacement regression", 8, 8,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (window) {
            context = SDL_GL_CreateContext(window);
        }
    }
    if (context) {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, color);
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 8, 8);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, depth);
        g_assert_cmpuint(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
                         GL_FRAMEBUFFER_COMPLETE);
    }
    static const DepthCase cases[] = {
        { "stage3-d24", 3, DEPTH_FORMAT_D24, false, 0, .75f, 1, 0, .75f },
        { "stage2-d16", 2, DEPTH_FORMAT_D16, false, 0, .5f, 2, 0, .25f },
        { "ignore-geometric-clip", 3, DEPTH_FORMAT_D24, false, 2, .75f, 1, 0,
          .75f },
        { "ignore-polygon-offset", 3, DEPTH_FORMAT_D24, false, 0, .75f, 1, 1000,
          .75f },
        { "ignore-perspective-depth", 3, DEPTH_FORMAT_D24, true, 0, .75f, 1, 0,
          .75f },
        { "reject-far", 3, DEPTH_FORMAT_D24, false, 0, 2, 1, 0, 1 },
        { "reject-near", 3, DEPTH_FORMAT_D24, false, 0, -.25f, 1, 0, 1 },
        { "ordinary-depth", 0, DEPTH_FORMAT_D24, false, .25f, 0, 0, 0, .25f },
        { "hilo-depth-units", 3, DEPTH_FORMAT_D24, false, 0, 0, 0, 0,
          .250003815f, 64 },
        { "hilo-clear-outside-range", 3, DEPTH_FORMAT_D24, false, 0, 0, 0, 0, 1,
          255 },
        { "invalid-predecessor", 3, DEPTH_FORMAT_D24, false, .25f, .75f, 1, 0,
          .25f, 0, true },
        { "selected-dot-product-source", 3, DEPTH_FORMAT_D24, false, .25f,
          .75f, 1, 0, .25f, 0, false, 2 },
        { "clamp-in-range", 3, DEPTH_FORMAT_D24, false, 0, .75f, 1, 0,
          .75f, 0, false, 0, true },
        { "clamp-below-near", 3, DEPTH_FORMAT_D24, false, 0, -.25f, 1, 0,
          0, 0, false, 0, true },
        { "clamp-above-far", 3, DEPTH_FORMAT_D24, false, 0, 2, 1, 0,
          1, 0, false, 0, true },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); ++i) {
        char *name =
            g_strdup_printf("/xbox/psh/depth-replace/%s", cases[i].name);
        g_test_add_data_func(name, &cases[i], test_depth);
        g_free(name);
    }
    g_test_add_func("/xbox/psh/depth-replace/following-geometry",
                    test_restored_depth_allows_following_geometry);
    g_test_add_func("/xbox/psh/depth-replace/texture-program-consistency",
                    test_texture_program_consistency);
    if (!context && g_getenv("XEMU_REQUIRE_GL_TEST_CONTEXT")) {
        g_error("OpenGL 4 context required for depth-replacement regression");
    }
    int result = g_test_run();
    if (context) {
        glDeleteRenderbuffers(1, &depth);
        glDeleteRenderbuffers(1, &color);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteVertexArrays(1, &vao);
        SDL_GL_DestroyContext(context);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}

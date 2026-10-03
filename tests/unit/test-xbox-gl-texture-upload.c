/*
 * Production cubemap upload regression using a surfaceless desktop GL context.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <epoxy/egl.h>

/* Keep the real private upload/face-stride implementation in this test. */
#ifndef XEMU_GL_TEXTURE_UPLOAD_SOURCE
#define XEMU_GL_TEXTURE_UPLOAD_SOURCE "hw/xbox/nv2a/pgraph/gl/texture.c"
#endif
#include XEMU_GL_TEXTURE_UPLOAD_SOURCE

NV2AStats g_nv2a_stats;
__typeof__(xemu_tweaks_active) xemu_tweaks_active;

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static PFNGLTEXIMAGE2DPROC real_tex_image;
static PFNGLCOMPRESSEDTEXIMAGE2DPROC real_compressed_tex_image;
static unsigned int uploads;
static unsigned int total_uploads, checked_texels;
static volatile uint8_t upload_checksum;

static void GLAPIENTRY checked_compressed_tex_image(
    GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
    GLint border, GLsizei image_size, const void *data)
{
    const volatile uint8_t *bytes = data;
    for (int i = 0; i < image_size; i++) {
        upload_checksum ^= bytes[i];
    }
    total_uploads++;
    real_compressed_tex_image(target, level, internal, width, height, border,
                              image_size, data);
}

/* Read exactly what GL will consume, with instrumented CPU accesses. Mesa's
 * internal memcpy is not necessarily ASan-instrumented. Then use the real GL
 * entry point: assertions below inspect driver-owned texture contents. */
static void GLAPIENTRY checked_tex_image(GLenum target, GLint level,
                                         GLint internal, GLsizei width,
                                         GLsizei height, GLint border,
                                         GLenum format, GLenum type,
                                         const void *data)
{
    unsigned int bytes;
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV) {
        bytes = 4;
    } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        bytes = 2;
    } else {
        g_assert_true(type == GL_UNSIGNED_BYTE || type == GL_BYTE);
        bytes = format == GL_RED ? 1 : format == GL_RG ? 2 : 3;
    }
    GLint row_length = 0, alignment = 0, skip_rows = 0, skip_pixels = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    g_assert_cmpint(alignment, >, 0);
    size_t row_bytes = (row_length ? row_length : width) * bytes;
    size_t stride = (row_bytes + alignment - 1) / alignment * alignment;
    const volatile uint8_t *pixels = data;
    pixels += skip_rows * stride + skip_pixels * bytes;
    for (int y = 0; y < height; y++) {
        for (size_t x = 0; x < (size_t)width * bytes; x++) {
            upload_checksum ^= pixels[y * stride + x];
        }
    }
    uploads++;
    total_uploads++;
    real_tex_image(target, level, internal, width, height, border, format, type,
                   data);
}

typedef enum InputFormat {
    INPUT_Y8,
    INPUT_G8B8,
    INPUT_RGB565,
    INPUT_RGBA8,
    INPUT_A8Y8,
    INPUT_PALETTE,
    INPUT_R6G5B5,
} InputFormat;

typedef struct FormatCase {
    InputFormat input;
    unsigned int guest_format, source_bytes, output_bytes;
    GLenum read_format, read_type;
} FormatCase;

static const FormatCase formats[] = {
    { INPUT_Y8, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8, 1, 1, GL_RED,
      GL_UNSIGNED_BYTE },
    { INPUT_G8B8, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8, 2, 2, GL_RG,
      GL_UNSIGNED_BYTE },
    { INPUT_RGB565, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5, 2, 2, GL_RGB,
      GL_UNSIGNED_SHORT_5_6_5 },
    { INPUT_RGBA8, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, 4, 4, GL_BGRA,
      GL_UNSIGNED_INT_8_8_8_8_REV },
    { INPUT_A8Y8, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8, 2, 2, GL_RG,
      GL_UNSIGNED_BYTE },
    { INPUT_PALETTE, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8, 1, 4,
      GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV },
    { INPUT_R6G5B5, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5, 2, 3, GL_RGB,
      GL_BYTE },
};

/* Literal source/output pairs: independent conversion KATs. Positive signed
 * components avoid driver-dependent -128/-127 SNORM endpoint normalization. */
static const struct {
    uint16_t source;
    int8_t rgb[3];
} rgb655_kats[] = {
    { 0x0000, { 0, 3, 3 } },    { 0xfc00, { 127, 3, 3 } },
    { 0x01e0, { 0, 127, 3 } },  { 0x000f, { 0, 3, 127 } },
    { 0x7ceb, { 62, 61, 94 } },
};

static unsigned int pattern(unsigned int face, unsigned int level,
                            unsigned int x, unsigned int y)
{
    return (face * 37 + level * 19 + x * 7 + y * 11) % 251 + 1;
}

static void make_pixel(const FormatCase *f, unsigned int face,
                       unsigned int level, unsigned int x, unsigned int y,
                       const uint32_t *palette, uint8_t *source,
                       uint8_t *output)
{
    unsigned int seed = pattern(face, level, x, y);
    uint16_t half;
    uint32_t word;
    switch (f->input) {
    case INPUT_Y8:
        source[0] = output[0] = seed;
        break;
    case INPUT_G8B8:
    case INPUT_A8Y8:
        source[0] = output[0] = seed;
        source[1] = output[1] = seed ^ 0xa5;
        break;
    case INPUT_RGB565:
        half = ((face * 5 + level) << 11) | ((y + level * 7) << 5) | x;
        memcpy(source, &half, 2);
        memcpy(output, &half, 2);
        break;
    case INPUT_RGBA8:
        word = 0xff000000 | ((face + 1) << 20) | (level << 16) | (y << 8) | x;
        memcpy(source, &word, 4);
        memcpy(output, &word, 4);
        break;
    case INPUT_PALETTE:
        source[0] = seed;
        memcpy(output, &palette[seed], 4);
        break;
    case INPUT_R6G5B5:
        seed %= ARRAY_SIZE(rgb655_kats);
        memcpy(source, &rgb655_kats[seed].source, 2);
        memcpy(output, rgb655_kats[seed].rgb, 3);
        break;
    }
}

/* Independent square Morton-address fixture writer, not the production
 * swizzler or crop helper. Expected crop insets below are literal. */
static unsigned int morton(unsigned int x, unsigned int y)
{
    unsigned int address = 0;
    for (unsigned int bit = 0; bit < 4; bit++) {
        address |= ((x >> bit) & 1) << (2 * bit);
        address |= ((y >> bit) & 1) << (2 * bit + 1);
    }
    return address;
}

static void run_format(const FormatCase *f, bool bordered,
                       unsigned int first_size, bool clamped)
{
    if (context == EGL_NO_CONTEXT) {
        g_test_skip("surfaceless desktop GL unavailable");
        return;
    }
    const unsigned int sizes[] = { 1, 2, 4, 8 };
    const unsigned int bordered_pixels[] = { 256, 320, 336, 340 };
    const unsigned int ordinary_pixels[] = { 1, 5, 21, 85 };
    const unsigned int insets[] = { 4, 2, 1, 0 };
    uint32_t palette[256];
    for (unsigned int i = 0; i < ARRAY_SIZE(palette); i++) {
        palette[i] =
            0xff000000 | (i << 16) | (((i * 3) & 255) << 8) | ((i * 5) & 255);
    }
    for (unsigned int test = first_size; test < ARRAY_SIZE(sizes); test++) {
        TextureShape shape = {
            .cubemap = true,
            .dimensionality = 2,
            .depth = 1,
            .color_format = f->guest_format,
            .border = bordered,
            .width = sizes[test],
            .height = sizes[test],
            .levels = test + 1,
            .storage_levels = test + 1,
        };
        if (clamped) {
            g_assert_cmpuint(sizes[test], ==, 8);
            shape.levels = 2;
            shape.min_mipmap_level = 1;
            shape.max_mipmap_level = 1;
        }
        size_t span =
            (bordered ? bordered_pixels[test] : ordinary_pixels[test]) *
            f->source_bytes;
        /* Documented 128-byte NV2A face alignment. */
        size_t face_stride = (span + 127) & ~(size_t)127;
        g_autofree uint8_t *input = g_malloc0(face_stride * 6);
        for (unsigned int face = 0; face < 6; face++) {
            unsigned int side = bordered ? 16 : sizes[test];
            size_t offset = face * face_stride;
            for (unsigned int level = 0; level < shape.storage_levels;
                 level++) {
                for (unsigned int y = 0; y < side; y++) {
                    for (unsigned int x = 0; x < side; x++) {
                        uint8_t unused[4];
                        make_pixel(f, face, level, x, y, palette,
                                   input + offset +
                                       morton(x, y) * f->source_bytes,
                                   unused);
                    }
                }
                offset += side * side * f->source_bytes;
                side = MAX(side / 2, 1);
            }
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        uploads = 0;
        TextureBinding *binding =
            generate_texture(shape, input, (const uint8_t *)palette);
        g_assert_nonnull(binding);
        g_assert_cmpuint(uploads, ==, 6 * shape.levels);
        g_assert_cmpint(glGetError(), ==, GL_NO_ERROR);
        GLint actual_state = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &actual_state);
        g_assert_cmpint(actual_state, ==, 8);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &actual_state);
        g_assert_cmpint(actual_state, ==, 0);
        glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL,
                            &actual_state);
        g_assert_cmpint(actual_state, ==, shape.min_mipmap_level);
        glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL,
                            &actual_state);
        g_assert_cmpint(actual_state, ==, shape.levels - 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        for (unsigned int face = 0; face < 6; face++) {
            for (unsigned int level = 0; level < shape.levels; level++) {
                unsigned int logical = sizes[test] >> level;
                GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
                GLint extent = 0;
                glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH,
                                         &extent);
                g_assert_cmpint(extent, ==, logical);
                glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT,
                                         &extent);
                g_assert_cmpint(extent, ==, logical);
                g_autofree uint8_t *actual =
                    g_malloc(logical * logical * f->output_bytes);
                glGetTexImage(target, level, f->read_format, f->read_type,
                              actual);
                g_assert_cmpint(glGetError(), ==, GL_NO_ERROR);
                unsigned int inset = bordered ? insets[level] : 0;
                for (unsigned int y = 0; y < logical; y++) {
                    for (unsigned int x = 0; x < logical; x++) {
                        uint8_t unused[4], expected[4];
                        make_pixel(f, face, level, x + inset, y + inset,
                                   palette, unused, expected);
                        g_assert_cmpmem(
                            actual + (y * logical + x) * f->output_bytes,
                            f->output_bytes, expected, f->output_bytes);
                        checked_texels++;
                    }
                }
            }
        }
        if (clamped) {
            for (unsigned int face = 0; face < 6; face++) {
                GLint extent = -1;
                glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                                         shape.levels, GL_TEXTURE_WIDTH,
                                         &extent);
                g_assert_cmpint(extent, ==, 0);
            }
        }
        glDeleteTextures(1, &binding->gl_texture);
        g_free(binding);
    }
}

static void test_direct_formats(void)
{
    for (unsigned int i = 0; i < 5; i++) {
        run_format(&formats[i], true, 0, false);
    }
}

static void test_palette(void)
{
    run_format(&formats[5], true, 0, false);
}
static void test_rgb655(void)
{
    run_format(&formats[6], true, 0, false);
}
static void test_ordinary(void)
{
    run_format(&formats[3], false, 0, false);
}
static void test_terminal_bounds(void)
{
    run_format(&formats[6], true, 3, false);
}

static void test_lod_clamped(void)
{
    run_format(&formats[3], true, 3, true);
}

static void run_compressed(bool native)
{
    if (context == EGL_NO_CONTEXT) {
        g_test_skip("surfaceless desktop GL unavailable");
        return;
    }
    if (native && !real_compressed_tex_image) {
        g_test_skip("native S3TC unavailable");
        return;
    }
    const uint16_t colors[] = {
        0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0xffe0
    };
    const uint8_t rgba[][4] = {
        { 255, 0, 0, 255 },     { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
        { 255, 255, 255, 255 }, { 0, 0, 0, 255 },   { 255, 255, 0, 255 },
    };
    TextureShape shape = {
        .cubemap = true,
        .dimensionality = 2,
        .depth = 1,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5,
        .width = 8,
        .height = 8,
        .levels = 4,
        .storage_levels = 4,
    };
    uint8_t input[6 * 128] = { 0 };
    for (unsigned int face = 0; face < 6; face++) {
        size_t offset = face * 128;
        for (unsigned int level = 0; level < 4; level++) {
            unsigned int blocks = level == 0 ? 4 : 1;
            uint16_t color = colors[(face + level) % 6];
            for (unsigned int block = 0; block < blocks; block++) {
                memcpy(input + offset, &color, 2);
                memcpy(input + offset + 2, &color, 2);
                offset += 8;
            }
        }
    }
    xemu_tweaks_active = native ? 1u << XEMU_TWEAK_GL_NATIVE_S3TC : 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    TextureBinding *binding = generate_texture(shape, input, NULL);
    g_assert_nonnull(binding);
    g_assert_cmpint(glGetError(), ==, GL_NO_ERROR);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (unsigned int face = 0; face < 6; face++) {
        for (unsigned int level = 0; level < 4; level++) {
            unsigned int side = 8 >> level;
            uint8_t actual[8 * 8 * 4];
            GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
            GLint extent = 0;
            glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &extent);
            g_assert_cmpint(extent, ==, side);
            glGetTexImage(target, level, GL_RGBA, GL_UNSIGNED_BYTE, actual);
            g_assert_cmpint(glGetError(), ==, GL_NO_ERROR);
            for (unsigned int pixel = 0; pixel < side * side; pixel++) {
                g_assert_cmpmem(actual + pixel * 4, 4, rgba[(face + level) % 6],
                                4);
                checked_texels++;
            }
        }
    }
    glDeleteTextures(1, &binding->gl_texture);
    g_free(binding);
    xemu_tweaks_active = 0;
}

static void test_dxt1_native(void)
{
    run_compressed(true);
}
static void test_dxt1_fallback(void)
{
    run_compressed(false);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL) &&
        eglBindAPI(EGL_OPENGL_API)) {
        const EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
            EGL_OPENGL_BIT,   EGL_NONE,
        };
        EGLConfig config;
        EGLint count = 0;
        if (eglChooseConfig(display, config_attributes, &config, 1, &count) &&
            count) {
            context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
            if (context != EGL_NO_CONTEXT &&
                !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                context)) {
                eglDestroyContext(display, context);
                context = EGL_NO_CONTEXT;
            }
        }
    }
    if (g_getenv("XEMU_GL_UPLOAD_REQUIRED")) {
        g_assert_true(context != EGL_NO_CONTEXT);
    }
    if (context != EGL_NO_CONTEXT) {
        g_test_message("GL renderer: %s", glGetString(GL_RENDERER));
        /* Resolve epoxy's entry point once before replacing its dispatch slot.
         */
        GLuint warmup;
        glGenTextures(1, &warmup);
        glBindTexture(GL_TEXTURE_2D, warmup);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glDeleteTextures(1, &warmup);
        real_tex_image = epoxy_glTexImage2D;
        epoxy_glTexImage2D = checked_tex_image;
        if (epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc")) {
            glGenTextures(1, &warmup);
            glBindTexture(GL_TEXTURE_2D, warmup);
            const uint8_t block[8] = { 0 };
            glCompressedTexImage2D(GL_TEXTURE_2D, 0,
                                   GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 1, 1, 0,
                                   sizeof(block), block);
            glDeleteTextures(1, &warmup);
            real_compressed_tex_image = epoxy_glCompressedTexImage2D;
            epoxy_glCompressedTexImage2D = checked_compressed_tex_image;
        }
    }
    g_test_add_func("/xbox/gl/texture-upload/bordered/direct",
                    test_direct_formats);
    g_test_add_func("/xbox/gl/texture-upload/bordered/palette", test_palette);
    g_test_add_func("/xbox/gl/texture-upload/bordered/r6g5b5", test_rgb655);
    g_test_add_func("/xbox/gl/texture-upload/unbordered/rgba8", test_ordinary);
    g_test_add_func("/xbox/gl/texture-upload/bordered/terminal-bounds",
                    test_terminal_bounds);
    g_test_add_func("/xbox/gl/texture-upload/bordered/lod-clamped",
                    test_lod_clamped);
    g_test_add_func("/xbox/gl/texture-upload/unbordered/dxt1-native",
                    test_dxt1_native);
    g_test_add_func("/xbox/gl/texture-upload/unbordered/dxt1-fallback",
                    test_dxt1_fallback);
    int result = g_test_run();
    if (context != EGL_NO_CONTEXT) {
        g_test_message("Validated %u face/mip uploads and %u texels",
                       total_uploads, checked_texels);
        epoxy_glTexImage2D = real_tex_image;
        if (real_compressed_tex_image) {
            epoxy_glCompressedTexImage2D = real_compressed_tex_image;
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
    }
    if (display != EGL_NO_DISPLAY) {
        eglTerminate(display);
    }
    return result;
}

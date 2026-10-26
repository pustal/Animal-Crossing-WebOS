/**
 * @file webos_bc_decode.c
 * @brief Implementation of the BC texture fallback (see webos_bc_decode.h).
 */
#include "webos_bc_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* bcdec (MIT / Unlicense, third_party/bcdec): only BC1/2/3/7 are used here. */
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "bcdec.h"
#pragma GCC diagnostic pop

/* Compressed formats (GL_EXT_texture_compression_s3tc / GL_ARB_texture_compression_bptc). */
#define AC_COMPRESSED_RGB_S3TC_DXT1  0x83F0
#define AC_COMPRESSED_RGBA_S3TC_DXT1 0x83F1
#define AC_COMPRESSED_RGBA_S3TC_DXT3 0x83F2
#define AC_COMPRESSED_RGBA_S3TC_DXT5 0x83F3
#define AC_COMPRESSED_RGBA_BPTC      0x8E8C

static const char *const kS3tcExtension = "GL_EXT_texture_compression_s3tc";
static const char *const kBptcExtension = "GL_ARB_texture_compression_bptc";

/** Extension list state, filled on first use (needs a current GL context). */
static int s_initialised = 0;
static GLint s_real_count = 0;
static int s_emulate_s3tc = 0;
static int s_emulate_bptc = 0;
static const char *s_fake_names[2];
static int s_fake_count = 0;

static void init_extensions(void)
{
    if (s_initialised)
        return;
    s_initialised = 1;

    glGetIntegerv(GL_NUM_EXTENSIONS, &s_real_count);
    int has_s3tc = 0, has_bptc = 0;
    for (GLint i = 0; i < s_real_count; ++i) {
        const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (!name)
            continue;
        if (strcmp(name, kS3tcExtension) == 0)
            has_s3tc = 1;
        if (strcmp(name, kBptcExtension) == 0)
            has_bptc = 1;
    }
    s_emulate_s3tc = !has_s3tc;
    s_emulate_bptc = !has_bptc;
    if (s_emulate_s3tc)
        s_fake_names[s_fake_count++] = kS3tcExtension;
    if (s_emulate_bptc)
        s_fake_names[s_fake_count++] = kBptcExtension;
    if (s_fake_count > 0)
        printf("[AC/webOS] BC textures will be decoded in software (driver lacks%s%s)\n",
               s_emulate_s3tc ? " S3TC" : "", s_emulate_bptc ? " BPTC" : "");
}

void ac_glGetIntegerv(GLenum pname, GLint *data)
{
    glGetIntegerv(pname, data);
    if (pname == GL_NUM_EXTENSIONS && data) {
        init_extensions();
        *data = s_real_count + s_fake_count;
    }
}

const GLubyte *ac_glGetStringi(GLenum name, GLuint index)
{
    if (name == GL_EXTENSIONS) {
        init_extensions();
        if ((GLint)index >= s_real_count && (GLint)index < s_real_count + s_fake_count)
            return (const GLubyte *)s_fake_names[(GLint)index - s_real_count];
    }
    return glGetStringi(name, index);
}

/** Block size in bytes of @p format, or 0 if it is not a BC format handled here. */
static int block_bytes(GLenum format)
{
    switch (format) {
    case AC_COMPRESSED_RGB_S3TC_DXT1:
    case AC_COMPRESSED_RGBA_S3TC_DXT1: return BCDEC_BC1_BLOCK_SIZE;
    case AC_COMPRESSED_RGBA_S3TC_DXT3: return BCDEC_BC2_BLOCK_SIZE;
    case AC_COMPRESSED_RGBA_S3TC_DXT5: return BCDEC_BC3_BLOCK_SIZE;
    case AC_COMPRESSED_RGBA_BPTC: return BCDEC_BC7_BLOCK_SIZE;
    default: return 0;
    }
}

unsigned char *webos_bc_decode_rgba(GLenum format, int width, int height, const void *data, int size)
{
    const int block = block_bytes(format);
    if (block == 0 || width <= 0 || height <= 0 || !data)
        return NULL;

    const int blocks_x = (width + 3) / 4;
    const int blocks_y = (height + 3) / 4;
    if ((long long)blocks_x * blocks_y * block > (long long)size)
        return NULL;

    /* Decode into a buffer padded to whole blocks, then crop to the real size. */
    const int padded_w = blocks_x * 4;
    const int padded_h = blocks_y * 4;
    const int pitch = padded_w * 4;
    unsigned char *padded = (unsigned char *)malloc((size_t)pitch * (size_t)padded_h);
    if (!padded)
        return NULL;

    const unsigned char *src = (const unsigned char *)data;
    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            unsigned char *dst = padded + (size_t)by * 4 * pitch + (size_t)bx * 16;
            switch (format) {
            case AC_COMPRESSED_RGB_S3TC_DXT1:
            case AC_COMPRESSED_RGBA_S3TC_DXT1: bcdec_bc1(src, dst, pitch); break;
            case AC_COMPRESSED_RGBA_S3TC_DXT3: bcdec_bc2(src, dst, pitch); break;
            case AC_COMPRESSED_RGBA_S3TC_DXT5: bcdec_bc3(src, dst, pitch); break;
            default: bcdec_bc7(src, dst, pitch); break;
            }
            src += block;
        }
    }

    if (padded_w == width && padded_h == height)
        return padded;

    unsigned char *cropped = (unsigned char *)malloc((size_t)width * (size_t)height * 4);
    if (cropped) {
        for (int y = 0; y < height; ++y)
            memcpy(cropped + (size_t)y * width * 4, padded + (size_t)y * pitch, (size_t)width * 4);
    }
    free(padded);
    return cropped;
}

void ac_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                               GLint border, GLsizei imageSize, const void *data)
{
    init_extensions();
    const int is_s3tc = internalformat >= AC_COMPRESSED_RGB_S3TC_DXT1 && internalformat <= AC_COMPRESSED_RGBA_S3TC_DXT5;
    const int is_bptc = internalformat == AC_COMPRESSED_RGBA_BPTC;
    if (!((is_s3tc && s_emulate_s3tc) || (is_bptc && s_emulate_bptc))) {
        glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
        return;
    }

    unsigned char *rgba = webos_bc_decode_rgba(internalformat, width, height, data, imageSize);
    if (!rgba) {
        fprintf(stderr, "[AC/webOS] could not decode a %dx%d BC texture (format 0x%04x)\n", width, height,
                (unsigned)internalformat);
        return;
    }

    /* Rows are width*4 bytes, always a multiple of the default 4-byte unpack alignment. */
    GLint alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(target, level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    free(rgba);
}

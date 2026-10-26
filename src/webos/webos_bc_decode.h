/**
 * @file webos_bc_decode.h
 * @brief Software decoding of BC1/BC2/BC3/BC7 (DXT/BPTC) textures for GPUs without them.
 *
 * Most Dolphin texture packs ship BC-compressed DDS files. The TV's Mali GPU
 * cannot sample those formats, and the port's texture pack loader only
 * accepts them when the driver lists the matching extensions. glad/gl.h
 * routes three calls here:
 *  - glGetIntegerv(GL_NUM_EXTENSIONS) and glGetStringi(GL_EXTENSIONS, i)
 *    additionally report GL_EXT_texture_compression_s3tc and
 *    GL_ARB_texture_compression_bptc when the driver lacks them;
 *  - glCompressedTexImage2D with a BC format the driver lacks decodes the
 *    data on the CPU (bcdec) and uploads plain RGBA8 instead.
 * Everything else passes straight to the driver. Decoded textures use four
 * times the memory of BC7/BC3 (eight times BC1), and each is decoded once,
 * when the game first uses it.
 */
#ifndef ACWEBOS_BC_DECODE_H
#define ACWEBOS_BC_DECODE_H

#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

void ac_glGetIntegerv(GLenum pname, GLint *data);
const GLubyte *ac_glGetStringi(GLenum name, GLuint index);
void ac_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                               GLint border, GLsizei imageSize, const void *data);

/**
 * Decodes a BC image to tightly packed RGBA8 rows (exposed for testing).
 * @param format GL_COMPRESSED_* enum of a BC1/BC2/BC3/BC7 format.
 * @return malloc'd width*height*4 bytes, or NULL if the format is not one of
 *         these or @p size is too small for the image.
 */
unsigned char *webos_bc_decode_rgba(GLenum format, int width, int height, const void *data, int size);

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_BC_DECODE_H */

/**
 * @file glad/gl.h
 * @brief webOS stand-in for the PC port's glad loader header.
 *
 * The PC port includes <glad/gl.h> (a desktop OpenGL 3.3 loader). On the TV we
 * compile against OpenGL ES 3 instead, like the Android port does: the subset
 * of GL the renderer uses is ES 3 compatible. This header maps the few
 * desktop-only names onto ES ones and routes shader sources through the
 * GLSL 3.30 -> GLSL ES 3.00 rewrite in webos_glue.c.
 *
 * It must be found before the port's own pc/include/glad, so it comes first
 * on the include path (see CMakeLists.txt).
 */
#ifndef ACWEBOS_GLAD_GL_H
#define ACWEBOS_GLAD_GL_H

#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

/** glad's loader entry point: the GL functions are linked directly, so there is nothing to load. */
typedef void *(*GLADloadfunc)(const char *name);
static inline int gladLoadGL(GLADloadfunc load)
{
    (void)load;
    return 1;
}

/* Desktop-only entry points mapped to their ES equivalents. */
#define glClearDepth(depth) glClearDepthf((GLfloat)(depth))
#define glDepthRange(nearVal, farVal) glDepthRangef((GLfloat)(nearVal), (GLfloat)(farVal))
/* glPointSize does not exist in OpenGL ES 3 (points are always 1 px). */
#define glPointSize(size) ((void)(size))

/* The port uploads some uniforms through glad's prefixed names. */
#define glad_glUniform1f glUniform1f
#define glad_glUniform1i glUniform1i
#define glad_glUniform1iv glUniform1iv
#define glad_glUniform2f glUniform2f
#define glad_glUniform2i glUniform2i
#define glad_glUniform2iv glUniform2iv
#define glad_glUniform3f glUniform3f
#define glad_glUniform3fv glUniform3fv
#define glad_glUniform3i glUniform3i
#define glad_glUniform3iv glUniform3iv
#define glad_glUniform4f glUniform4f
#define glad_glUniform4fv glUniform4fv
#define glad_glUniform4i glUniform4i
#define glad_glUniform4iv glUniform4iv
#define glad_glUniformMatrix3fv glUniformMatrix3fv
#define glad_glUniformMatrix4fv glUniformMatrix4fv

/**
 * glShaderSource with the GLSL rewrite applied (see webos_glue.c): the port's
 * shaders are written for desktop GLSL 3.30.
 */
int ac_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
#define glShaderSource(shader, count, string, length) ac_glShaderSource((shader), (count), (string), (length))

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_GLAD_GL_H */

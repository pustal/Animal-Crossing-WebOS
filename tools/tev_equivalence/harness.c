/*
 * Equivalence test for webos_tev_specialize.c: renders each variant with its
 * original main() and with the generated one (Mesa llvmpipe, GLES 3, float
 * target) using identical inputs, and compares every pixel.
 * Usage: harness <variant.frag>...
 */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webos_tev_specialize.h"

#define SIZE 48

static const char *kVertex =
    "#version 300 es\n"
    "in vec2 a_position;\n"
    "out vec4 v_color; out vec2 v_texcoord0; out vec2 v_texcoord1; out vec3 v_normal; out float v_fog_z;\n"
    "void main() {\n"
    "  vec2 p = a_position * 0.5 + 0.5;\n"
    "  v_color = vec4(p.x, p.y, 1.0 - p.x, 0.3 + 0.6 * p.y);\n"
    "  v_texcoord0 = p * 1.7 - 0.2; v_texcoord1 = vec2(p.y, p.x) * 2.3;\n"
    "  v_normal = normalize(vec3(p.x - 0.5, p.y - 0.5, 0.7));\n"
    "  v_fog_z = p.x * 100.0;\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    s[n] = 0;
    fclose(f);
    return s;
}

static GLuint compile(GLenum type, const char *src, const char *label)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "compile error (%s): %s\n", label, log);
        return 0;
    }
    return s;
}

/* Deterministic pseudo-random value from a uniform name and component index. */
static float value_for(const char *name, int index)
{
    unsigned h = 2166136261u;
    for (const char *p = name; *p; ++p) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ (unsigned)index) * 16777619u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h % 10000u) / 10000.0f;
}

static void set_uniforms(GLuint prog)
{
    GLint count = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
        char name[128];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(prog, (GLuint)i, sizeof(name), NULL, &size, &type, name);
        char *bracket = strchr(name, '[');
        if (bracket) *bracket = 0;
        for (GLint e = 0; e < size; ++e) {
            char element[160];
            if (size > 1 || bracket) snprintf(element, sizeof(element), "%s[%d]", name, e);
            else snprintf(element, sizeof(element), "%s", name);
            GLint loc = glGetUniformLocation(prog, element);
            if (loc < 0) continue;
            float v[4];
            for (int c = 0; c < 4; ++c) v[c] = value_for(name, e * 4 + c);
            if (!strcmp(name, "u_fog_params")) { v[1] = 20.0f; v[2] = 80.0f; }
            if (!strncmp(name, "u_tev_", 6)) for (int c = 0; c < 4; ++c) v[c] = v[c] * 2.0f - 0.5f;
            switch (type) {
            case GL_FLOAT: glUniform1f(loc, v[0]); break;
            case GL_FLOAT_VEC2: glUniform2f(loc, v[0], v[1]); break;
            case GL_FLOAT_VEC3: glUniform3f(loc, v[0] - 0.5f, v[1] - 0.5f, v[2]); break;
            case GL_FLOAT_VEC4: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
            case GL_INT_VEC2: glUniform2i(loc, (int)(v[0] * 255), (int)(v[1] * 255)); break;
            case GL_SAMPLER_2D: {
                int unit = 0;
                if (!strcmp(name, "u_texture1")) unit = 1;
                if (!strcmp(name, "u_texture2")) unit = 2;
                if (!strcmp(name, "u_ind_tex0")) unit = 3;
                if (!strcmp(name, "u_ind_tex1")) unit = 4;
                glUniform1i(loc, unit);
                break;
            }
            default: break;
            }
        }
    }
}

static int render(GLuint vs, const char *frag_src, const char *label, float *pixels)
{
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag_src, label);
    if (!fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "link error (%s): %s\n", label, log);
        return 0;
    }
    glUseProgram(prog);
    set_uniforms(prog);
    glClearColor(-7.0f, -7.0f, -7.0f, -7.0f); /* sentinel: shows discarded pixels */
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_FLOAT, pixels);
    glDeleteProgram(prog);
    glDeleteShader(fs);
    return 1;
}

int main(int argc, char **argv)
{
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 2; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
    const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "context failed\n"); return 2; }

    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, SIZE, SIZE);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); return 2; }
    glViewport(0, 0, SIZE, SIZE);

    /* Five random 16x16 textures with nearest filtering and repeat wrap. */
    for (int unit = 0; unit < 5; ++unit) {
        unsigned char texels[16 * 16 * 4];
        for (int i = 0; i < (int)sizeof(texels); ++i) texels[i] = (unsigned char)((i * 73 + unit * 151 + (i >> 3) * 29) & 0xFF);
        GLuint tex;
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    GLuint vs = compile(GL_VERTEX_SHADER, kVertex, "vertex");
    if (!vs) return 2;

    static float a[SIZE * SIZE * 4], b[SIZE * SIZE * 4];
    int failures = 0, tested = 0, unspecialised = 0;
    double worst = 0.0;
    for (int i = 1; i < argc; ++i) {
        char *src = read_file(argv[i]);
        char *spec = src ? webos_tev_specialize(src) : NULL;
        if (!spec) { unspecialised++; fprintf(stderr, "%s: not specialised\n", argv[i]); free(src); continue; }
        if (!render(vs, src, "original", a) || !render(vs, spec, "specialised", b)) {
            failures++;
            fprintf(stderr, "%s: render failed\n", argv[i]);
        } else {
            double diff = 0.0;
            for (int p = 0; p < SIZE * SIZE * 4; ++p) {
                double d = fabs((double)a[p] - (double)b[p]);
                if (isnan(a[p]) != isnan(b[p])) d = 1e9;
                if (d > diff) diff = d;
            }
            if (diff > worst) worst = diff;
            if (diff > 1e-4) { failures++; fprintf(stderr, "%s: max difference %g\n", argv[i], diff); }
        }
        tested++;
        free(src);
        free(spec);
    }
    printf("tested %d variants, %d failures, %d not specialised, worst difference %g\n", tested, failures, unspecialised, worst);
    return failures || unspecialised ? 1 : 0;
}

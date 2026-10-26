/**
 * @file webos_overlay.c
 * @brief In-game info panel: the upload page address, PIN and texture pack state.
 *
 * Shown for 20 seconds when the game starts and toggled with the remote's 1
 * key or a controller's left shoulder button (webos_remote.c). Drawn by webos_present.c just before each buffer
 * swap, with its own shader, vertex array and font texture. Every piece of
 * GL state it touches is saved first and restored afterwards, because the
 * port's renderer tracks GL state itself and would otherwise skip calls.
 */
#include "webos_overlay.h"

#include "webos_transfer.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/** Shown automatically for this long after the first frame. */
static const Uint32 kAutoShowMs = 20000;
/** The text is rebuilt this often (it looks up the network address). */
static const Uint32 kTextRefreshMs = 2000;

#define GLYPH_SIZE 8
#define FIRST_CHAR ' '
#define GLYPH_COUNT 95
#define MAX_TEXT 512
#define MAX_QUADS (MAX_TEXT + 1)

static int s_visible = 0;
static Uint32 s_auto_hide_at = 0;
static int s_ready = 0;
static int s_failed = 0;

static GLuint s_program = 0;
static GLuint s_vao = 0;
static GLuint s_vbo = 0;
static GLuint s_font = 0;
static GLint s_loc_screen = -1;
static GLint s_loc_color = -1;

static char s_text[MAX_TEXT];
static int s_lines = 0;
static Uint32 s_text_refreshed_at = 0;

/** One vertex: pixel position and font-atlas coordinate. */
typedef struct {
    float x, y, u, v;
} Vertex;

static Vertex s_vertices[MAX_QUADS * 6];

static const char *kVertexSource =
    "#version 300 es\n"
    "layout(location = 0) in vec4 a_vertex;\n" /* xy = pixels from the top left, zw = atlas uv */
    "uniform vec2 u_screen;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_vertex.zw;\n"
    "    vec2 ndc = a_vertex.xy / u_screen * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);\n"
    "}\n";

/* uv.x < 0 marks the background panel (solid colour); otherwise the glyph's coverage. */
static const char *kFragmentSource =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_font;\n"
    "uniform vec4 u_color;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    if (v_uv.x < 0.0) { o_color = vec4(0.0, 0.0, 0.0, 0.72); return; }\n"
    "    float coverage = texture(u_font, v_uv).r;\n"
    "    o_color = vec4(u_color.rgb, u_color.a * coverage);\n"
    "}\n";

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[AC/webOS] overlay shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/** Creates the program, font texture and vertex array. Called with the caller's state already saved. */
static int create_resources(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexSource);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentSource);
    if (!vs || !fs)
        return 0;
    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (!linked)
        return 0;
    s_loc_screen = glGetUniformLocation(s_program, "u_screen");
    s_loc_color = glGetUniformLocation(s_program, "u_color");
    glUseProgram(s_program);
    glUniform1i(glGetUniformLocation(s_program, "u_font"), 0);

    /* Font atlas: one row of glyphs, coverage in the red channel. */
    static unsigned char atlas[GLYPH_SIZE][GLYPH_COUNT * GLYPH_SIZE];
    for (int g = 0; g < GLYPH_COUNT; ++g) {
        const unsigned char *rows = webos_font_glyph((char)(FIRST_CHAR + g));
        for (int y = 0; y < GLYPH_SIZE; ++y)
            for (int x = 0; x < GLYPH_SIZE; ++x)
                atlas[y][g * GLYPH_SIZE + x] = (rows[y] & (1u << x)) ? 255 : 0;
    }
    glGenTextures(1, &s_font);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, GLYPH_COUNT * GLYPH_SIZE, GLYPH_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_vertices), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void *)0);
    return 1;
}

/** Appends a quad (two triangles) to the vertex list. */
static int add_quad(int count, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    if (count + 6 > (int)(sizeof(s_vertices) / sizeof(s_vertices[0])))
        return count;
    const Vertex quad[6] = { { x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x0, y1, u0, v1 },
                             { x1, y0, u1, v0 }, { x1, y1, u1, v1 }, { x0, y1, u0, v1 } };
    memcpy(&s_vertices[count], quad, sizeof(quad));
    return count + 6;
}

/** GL state the overlay changes, saved before drawing and restored after. */
typedef struct {
    GLint program, vao, array_buffer, active_texture, texture0, sampler0, framebuffer;
    GLint viewport[4], scissor_box[4];
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha, blend_eq_rgb, blend_eq_alpha;
    GLboolean blend, depth_test, cull_face, scissor_test, stencil_test, depth_mask;
    GLboolean color_mask[4];
    GLint unpack_alignment;
} SavedState;

static void save_state(SavedState *s)
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture0);
    glGetIntegerv(GL_SAMPLER_BINDING, &s->sampler0);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->framebuffer);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor_box);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
    s->blend = glIsEnabled(GL_BLEND);
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->cull_face = glIsEnabled(GL_CULL_FACE);
    s->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    s->stencil_test = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
}

static void set_enabled(GLenum cap, GLboolean enabled)
{
    if (enabled)
        glEnable(cap);
    else
        glDisable(cap);
}

static void restore_state(const SavedState *s)
{
    glUseProgram((GLuint)s->program);
    glBindVertexArray((GLuint)s->vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->texture0);
    glBindSampler(0, (GLuint)s->sampler0);
    glActiveTexture((GLenum)s->active_texture);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->framebuffer);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor_box[0], s->scissor_box[1], s->scissor_box[2], s->scissor_box[3]);
    glBlendFuncSeparate((GLenum)s->blend_src_rgb, (GLenum)s->blend_dst_rgb, (GLenum)s->blend_src_alpha, (GLenum)s->blend_dst_alpha);
    glBlendEquationSeparate((GLenum)s->blend_eq_rgb, (GLenum)s->blend_eq_alpha);
    set_enabled(GL_BLEND, s->blend);
    set_enabled(GL_DEPTH_TEST, s->depth_test);
    set_enabled(GL_CULL_FACE, s->cull_face);
    set_enabled(GL_SCISSOR_TEST, s->scissor_test);
    set_enabled(GL_STENCIL_TEST, s->stencil_test);
    glDepthMask(s->depth_mask);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_alignment);
}

void webos_overlay_toggle(void)
{
    s_visible = !s_visible;
    s_auto_hide_at = 0;
    s_text_refreshed_at = 0;
}

void webos_overlay_show_briefly(void)
{
    s_visible = 1;
    s_auto_hide_at = SDL_GetTicks() + kAutoShowMs;
    if (s_auto_hide_at == 0)
        s_auto_hide_at = 1;
}

void webos_overlay_draw(int width, int height)
{
    const Uint32 now = SDL_GetTicks();
    if (s_visible && s_auto_hide_at != 0 && SDL_TICKS_PASSED(now, s_auto_hide_at))
        s_visible = 0;
    if (!s_visible || s_failed || width <= 0 || height <= 0)
        return;

    if (s_text_refreshed_at == 0 || SDL_TICKS_PASSED(now, s_text_refreshed_at + kTextRefreshMs)) {
        s_lines = webos_transfer_overlay_text(s_text, sizeof(s_text));
        s_text_refreshed_at = now ? now : 1;
    }
    if (s_lines <= 0)
        return;

    SavedState saved;
    save_state(&saved);

    if (!s_ready) {
        s_ready = create_resources();
        if (!s_ready) {
            s_failed = 1;
            restore_state(&saved);
            return;
        }
    }

    /* Glyphs drawn at 3x on a 1080p drawable, scaled with the screen height. */
    const int scale = height >= 1000 ? 3 : 2;
    const float glyph = (float)(GLYPH_SIZE * scale);
    const float line_height = glyph * 1.5f;
    const float margin = glyph;

    int longest = 0, current = 0;
    for (const char *p = s_text; *p; ++p) {
        if (*p == '\n') {
            current = 0;
            continue;
        }
        if (++current > longest)
            longest = current;
    }

    int count = 0;
    const float panel_w = (float)longest * glyph + margin * 2.0f;
    const float panel_h = (float)s_lines * line_height + margin * 2.0f - (line_height - glyph);
    count = add_quad(count, margin, margin, margin + panel_w, margin + panel_h, -1.0f, -1.0f, -1.0f, -1.0f);

    float x = margin * 2.0f, y = margin * 2.0f;
    const float atlas_width = (float)(GLYPH_COUNT * GLYPH_SIZE);
    for (const char *p = s_text; *p; ++p) {
        if (*p == '\n') {
            x = margin * 2.0f;
            y += line_height;
            continue;
        }
        int index = (unsigned char)*p - FIRST_CHAR;
        if (index < 0 || index >= GLYPH_COUNT)
            index = '?' - FIRST_CHAR;
        if (*p != ' ') {
            const float u0 = (float)(index * GLYPH_SIZE) / atlas_width;
            const float u1 = (float)((index + 1) * GLYPH_SIZE) / atlas_width;
            count = add_quad(count, x, y, x + glyph, y + glyph, u0, 0.0f, u1, 1.0f);
        }
        x += glyph;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

    glUseProgram(s_program);
    glUniform2f(s_loc_screen, (float)width, (float)height);
    glUniform4f(s_loc_color, 1.0f, 0.93f, 0.7f, 1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glBindSampler(0, 0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * (int)sizeof(Vertex)), s_vertices);
    glDrawArrays(GL_TRIANGLES, 0, count);

    restore_state(&saved);
}

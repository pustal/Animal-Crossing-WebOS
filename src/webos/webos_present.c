/**
 * @file webos_present.c
 * @brief Buffer-swap hook: makes every frame opaque before the TV composites it.
 *
 * The GameCube renderer writes destination alpha (pc_gx.c follows the game's
 * alpha_update_enable), so the back buffer's alpha ends up at arbitrary values,
 * often 0. The webOS compositor blends the app's surface with its alpha, so
 * those pixels came out black, and the game only showed through while a system
 * overlay changed the composition. Before each swap the alpha channel is
 * cleared to 1 (colour untouched).
 *
 * glad/gl.h routes the port's SDL_GL_SwapWindow call here, which also gives a
 * per-frame hook for installing the remote mapping and for a heartbeat log.
 */
#include "webos_platform.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <stdio.h>

/** Declared in glad/gl.h, which renames the port's SDL_GL_SwapWindow calls to this. */
void ac_SDL_GL_SwapWindow(SDL_Window *window);

/** Log a heartbeat every this many frames (about a minute at 60 fps). */
static const unsigned long kHeartbeatFrames = 3600;

static unsigned long s_frames_presented = 0;

unsigned long webos_frames_presented(void)
{
    return s_frames_presented;
}

/** GL state touched by the helpers below, saved so the port's state tracking stays valid. */
typedef struct {
    GLboolean color_mask[4];
    GLfloat clear_color[4];
    GLboolean scissor_enabled;
    GLint scissor_box[4];
    GLint draw_framebuffer;
} SavedState;

static void save_state(SavedState *state)
{
    glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, state->clear_color);
    state->scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, state->scissor_box);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_framebuffer);
}

static void restore_state(const SavedState *state)
{
    glColorMask(state->color_mask[0], state->color_mask[1], state->color_mask[2], state->color_mask[3]);
    glClearColor(state->clear_color[0], state->clear_color[1], state->clear_color[2], state->clear_color[3]);
    if (state->scissor_enabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    glScissor(state->scissor_box[0], state->scissor_box[1], state->scissor_box[2], state->scissor_box[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)state->draw_framebuffer);
}

/** Sets the window's alpha channel to 1 everywhere, leaving colour as drawn. */
static void make_back_buffer_opaque(void)
{
    SavedState state;
    save_state(&state);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    restore_state(&state);
}

/** One-time setup and log on the first presented frame. */
static void on_first_frame(SDL_Window *window)
{
    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    printf("[AC/webOS] first frame: drawable %dx%d, %s / %s\n", width, height,
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    webos_remote_install();
}

void ac_SDL_GL_SwapWindow(SDL_Window *window)
{
    if (s_frames_presented == 0)
        on_first_frame(window);

    make_back_buffer_opaque();
    SDL_GL_SwapWindow(window);
    ++s_frames_presented;

    if (s_frames_presented % kHeartbeatFrames == 0) {
        const GLenum error = glGetError();
        printf("[AC/webOS] %lu frames presented (GL error 0x%04x)\n", s_frames_presented, (unsigned)error);
    }
}

void webos_show_boot_activity(void)
{
    if (s_frames_presented != 0)
        return;
    SDL_Window *window = SDL_GL_GetCurrentWindow();
    if (!window)
        return;

    /* A short bar sliding along the bottom: enough to show the app is working.
     * Throttled to ~30 updates a second so it never slows the work it reports on. */
    static Uint32 last_update = 0;
    static int step = 0;
    const Uint32 now = SDL_GetTicks();
    if (last_update != 0 && now - last_update < 33)
        return;
    last_update = now;

    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return;

    const int steps     = 60;
    const int bar_w     = width / 6;
    const int bar_h     = height / 90 > 2 ? height / 90 : 2;
    const int track_y   = height / 12;
    const int travel    = width - bar_w;
    const int bar_x     = travel * (step % steps) / (steps - 1);
    step++;

    SavedState state;
    save_state(&state);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.05f, 0.10f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(bar_x, track_y, bar_w, bar_h);
    glClearColor(0.45f, 0.78f, 0.40f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    restore_state(&state);

    SDL_GL_SwapWindow(window); /* the real swap: not counted as a game frame */
}

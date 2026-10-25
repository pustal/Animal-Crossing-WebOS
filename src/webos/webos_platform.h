/**
 * @file webos_platform.h
 * @brief Internal interface between the webOS modules in src/webos/.
 *
 * The port's own code reaches these through android_glue.h (bootstrap) and
 * glad/gl.h (GL and buffer-swap hooks); the functions here are how those
 * hooks talk to each other.
 */
#ifndef ACWEBOS_PLATFORM_H
#define ACWEBOS_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- webos_remote.c ---- */

/** Sets the SDL hints for the remote (must run before SDL_Init). */
void webos_remote_set_hints(void);

/** Installs the remote's SDL event filter (after SDL_Init). */
void webos_remote_install(void);

/** Writes the webOS keyboard layout to keybindings.ini once (before the port loads it). */
void webos_write_keybindings_once(void);

/* ---- webos_present.c ---- */

/** Number of frames presented so far (0 while the game is still booting). */
unsigned long webos_frames_presented(void);

/**
 * Shows a simple activity bar while the game is still booting (before the
 * first frame), e.g. while shaders compile. Does nothing once frames are
 * being presented. Leaves the GL state as it found it.
 */
void webos_show_boot_activity(void);

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_PLATFORM_H */

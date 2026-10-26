/**
 * @file webos_overlay.h
 * @brief In-game info panel (upload page address, PIN, texture pack state).
 */
#ifndef ACWEBOS_OVERLAY_H
#define ACWEBOS_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/** Shows or hides the panel (remote key 1). */
void webos_overlay_toggle(void);

/** Shows the panel for a few seconds (called when the game starts). */
void webos_overlay_show_briefly(void);

/**
 * Draws the panel over the back buffer if it is visible. Called just before
 * the buffer swap; leaves all GL state as it found it.
 */
void webos_overlay_draw(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_OVERLAY_H */

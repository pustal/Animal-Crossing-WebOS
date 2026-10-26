/**
 * @file webos_transfer.h
 * @brief C interface to the upload page and its helpers (src/webos/transfer/, C++).
 */
#ifndef ACWEBOS_TRANSFER_H
#define ACWEBOS_TRANSFER_H

#include <stddef.h>

/*
 * These functions live in libacwebos_transfer.so, which hides everything
 * else (see CMakeLists.txt for why the C++ code is kept out of the game
 * executable).
 */
#if defined(__GNUC__)
#define ACWEBOS_TRANSFER_API __attribute__((visibility("default")))
#else
#define ACWEBOS_TRANSFER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called by android_bootstrap() once it is in the data folder, before the
 * game starts: applies files staged during the last session, checks root
 * access, points texture_pack/ at a USB drive's pack, and starts the upload
 * server. If no disc image is installed it shows the setup screen until one
 * arrives; if the player leaves that screen, the process exits.
 */
ACWEBOS_TRANSFER_API void webos_transfer_boot(void);

/**
 * Writes the lines for the in-game info overlay (upload address, PIN, USB
 * and texture pack state) into @p buffer, separated by '\n'.
 * @return Number of lines written.
 */
ACWEBOS_TRANSFER_API int webos_transfer_overlay_text(char *buffer, size_t size);

/** 8 rows of the font8x8 glyph for printable ASCII @p c ('?' for anything else); bit 0 is the left pixel. */
ACWEBOS_TRANSFER_API const unsigned char *webos_font_glyph(char c);

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_TRANSFER_H */

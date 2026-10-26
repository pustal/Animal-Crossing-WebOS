/**
 * @file SetupScreen.h
 * @brief First-run screen shown while no disc image is installed.
 *
 * It shows the address to open on a phone or computer, the PIN, the free
 * space, USB/texture pack state and the upload progress, and returns as soon
 * as a disc image has arrived.
 */
#ifndef ACWEBOS_SETUP_SCREEN_H
#define ACWEBOS_SETUP_SCREEN_H

namespace acwebos
{

class Storage;
class TransferServer;
class UsbAccess;

/** How the setup screen ended. */
enum class SetupResult {
    DiscReady, ///< A disc image is installed; start the game.
    Exit,      ///< The player closed the app.
    Failed,    ///< The screen could not be created.
};

/**
 * Runs the setup screen until a disc image arrives or the player exits.
 * Creates and destroys its own window; SDL video is shut down on return so
 * the game can initialise it from scratch.
 *
 * @param server May be null if the upload server failed to start.
 */
SetupResult RunSetupScreen(const Storage &storage, UsbAccess &usb, const TransferServer *server);

} // namespace acwebos

#endif // ACWEBOS_SETUP_SCREEN_H

/**
 * @file TransferServer.h
 * @brief A small HTTP server for the browser upload page: the disc image,
 *        memory-card saves and texture pack files.
 *
 * Design notes for a TV:
 *  - Uploads are streamed straight to disk, never held in memory, and free
 *    space is checked against the announced size before anything is written.
 *  - Only clients on private networks are answered, and every API call must
 *    carry the PIN shown on the TV.
 *  - Two worker threads at most; the game's main thread is never blocked.
 */
#ifndef ACWEBOS_TRANSFER_SERVER_H
#define ACWEBOS_TRANSFER_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib
{
class Server;
}

namespace acwebos
{

class Storage;
class UsbAccess;

/** Snapshot of what the server is doing, for the TV screens to display. */
struct TransferStatus {
    bool uploading              = false; ///< A disc image upload is in progress.
    std::uint64_t receivedBytes = 0;     ///< Bytes of the current/last disc upload written so far.
    std::uint64_t expectedBytes = 0;     ///< Announced size of the current/last disc upload.
    bool discReady              = false; ///< A valid disc image was installed during setup.
    bool restartPending         = false; ///< Files were received that the game only reads at launch.
    std::uint32_t eventSerial   = 0;     ///< Increments whenever lastEvent changes.
    std::string lastEvent;               ///< Short description of the latest event.
};

class TransferServer
{
public:
    /**
     * Port tried first, clear of other homebrew web servers (SonicWebMania
     * uses 47801), common dev ports and webOS' own (3000/3001/9922/9998).
     * If it is busy, the next nine are tried.
     */
    static constexpr std::uint16_t kPreferredPort = 47811;

    TransferServer(Storage &storage, UsbAccess &usb, std::string pin, std::string version);
    ~TransferServer();

    TransferServer(const TransferServer &)            = delete;
    TransferServer &operator=(const TransferServer &) = delete;

    /** Binds the first free port in [kPreferredPort, kPreferredPort + 9] and serves on a background thread. */
    bool Start();

    /** Stops the server and joins its thread. Safe to call more than once. */
    void Stop();

    /**
     * Setup mode: the game has not started, so a disc image is installed
     * straight away and saves are written in place. In game mode, saves are
     * staged for the next launch.
     */
    void SetSetupMode(bool setupMode) { setupMode_.store(setupMode); }

    /** The bound port, or 0 when not running. */
    std::uint16_t Port() const { return port_.load(); }

    /** The PIN clients must send. */
    const std::string &Pin() const { return pin_; }

    /** Thread-safe copy of the current status. */
    TransferStatus Snapshot() const;

private:
    void RegisterRoutes();
    void PublishEvent(const std::string &message);
    void MarkRestartPending();

    Storage &storage_;
    UsbAccess &usb_;
    const std::string pin_;
    const std::string version_;

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<std::uint16_t> port_{ 0 };
    std::atomic<bool> setupMode_{ true };
    std::atomic<bool> discUploadInProgress_{ false };

    mutable std::mutex statusMutex_;
    TransferStatus status_;
};

/** Returns the PIN stored in the data folder, creating a random 4-digit one the first time. */
std::string LoadOrCreatePin(const Storage &storage);

} // namespace acwebos

#endif // ACWEBOS_TRANSFER_SERVER_H

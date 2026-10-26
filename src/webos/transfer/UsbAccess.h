/**
 * @file UsbAccess.h
 * @brief Root access, USB drives and the texture pack folder on them.
 *
 * webOS mounts USB drives under /tmp/usb/<disk>/<partition>. An app normally
 * runs inside a jail as an unprivileged user, so whether it can see, read or
 * write a drive depends on the firmware. Everything here therefore tries the
 * direct route first and falls back to root commands, which are available
 * when the app itself runs as root or when the Homebrew Channel's
 * luna://org.webosbrew.hbchannel.service/exec accepts the app's calls (rooted
 * TVs; newer firmwares may refuse calls from apps).
 *
 * The texture pack lives in "<drive>/Animal Crossing/texture_pack". At
 * launch, PlaceTexturePack() makes the game's texture_pack/ folder point
 * there (a symlink, or a root bind mount when the app cannot read the drive
 * itself).
 */
#ifndef ACWEBOS_USB_ACCESS_H
#define ACWEBOS_USB_ACCESS_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace acwebos
{

/** What kind of root access the app has. */
struct RootStatus {
    bool runningAsRoot = false;   ///< The app process itself is root.
    bool homebrewChannel = false; ///< Homebrew Channel exec works from this app.
    std::string detail;           ///< Short explanation for the page (e.g. why exec failed).

    bool Available() const { return runningAsRoot || homebrewChannel; }
};

/** A mounted USB drive. */
struct UsbDrive {
    std::string mountPoint;      ///< e.g. /tmp/usb/sda/sda1
    std::string label;           ///< Short name for people, e.g. "sda1".
    std::uint64_t freeBytes = 0;
    std::uint64_t totalBytes = 0;
    bool spaceKnown = false;     ///< statvfs (or root df) succeeded.
    bool readable = false;       ///< The app can list it directly.
    bool writable = false;       ///< The app can create files on it directly.
    bool packExists = false;     ///< "<drive>/Animal Crossing/texture_pack" exists.
    std::uint32_t packFiles = 0; ///< .dds files in the pack (counted when readable).
};

/** Where the game's texture_pack/ comes from for this session. */
enum class PackSource {
    None,      ///< No texture pack.
    Internal,  ///< Files in the data folder itself.
    UsbLink,   ///< Symlink to a USB drive the app can read.
    UsbMount,  ///< Root bind mount of a USB folder the app cannot read on its own.
};

class UsbAccess
{
public:
    /** The pack folder on a drive, relative to its mount point. */
    static constexpr const char *kPackFolder = "Animal Crossing/texture_pack";

    /** Probes root access (once; it takes up to a few seconds). */
    void ProbeRoot();

    /** Last probe result. */
    RootStatus Root() const;

    /**
     * Lists mounted USB drives with their space and access. Pack file counts
     * are cached for a while, as counting a large pack is slow.
     */
    std::vector<UsbDrive> ListDrives();

    /** Whether @p mountPoint is currently a mounted USB drive (guards upload targets). */
    bool IsUsbMount(const std::string &mountPoint);

    /** Forgets cached pack file counts (after uploads). */
    void InvalidatePackCounts();

    /**
     * Creates the pack folder on the drive mounted at @p mountPoint.
     * @return Empty string on success, otherwise the reason.
     */
    std::string CreatePackFolder(const std::string &mountPoint);

    /**
     * Moves @p sourcePath (a finished upload in the data folder) to
     * @p destination on a USB drive, creating its folders, using root.
     * @return Empty string on success, otherwise the reason.
     */
    std::string MoveWithRoot(const std::string &sourcePath, const std::string &destination);

    /**
     * Points the data folder's texture_pack/ at the first USB drive holding a
     * pack (see the file comment). Called at launch, before the game scans it.
     * @param dataRoot Absolute data folder path.
     */
    void PlaceTexturePack(const std::string &dataRoot);

    /** Where texture_pack/ came from at launch, and the drive folder if any. */
    PackSource ActivePackSource() const;
    std::string ActivePackPath() const;

    /**
     * Runs @p command as root (sh -c). Requires Root().Available().
     * @return True if it exited successfully; @p output receives stdout.
     */
    bool RunAsRoot(const std::string &command, std::string *output = nullptr);

    /** Quotes @p text for a POSIX shell (single quotes). */
    static std::string ShellQuote(const std::string &text);

private:
    /** Mount points that look like USB drives, from /proc/mounts (or root's view). */
    std::vector<std::string> UsbMountPoints();

    mutable std::mutex mutex_;
    RootStatus root_;
    bool rootProbed_ = false;
    PackSource packSource_ = PackSource::None;
    std::string packPath_;

    struct CountCache {
        std::string mountPoint;
        std::uint32_t files;
        std::chrono::steady_clock::time_point expiresAt;
    };
    std::vector<CountCache> countCache_;
};

} // namespace acwebos

#endif // ACWEBOS_USB_ACCESS_H

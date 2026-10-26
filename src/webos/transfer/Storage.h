/**
 * @file Storage.h
 * @brief The app's data folder as the transfer page sees it: the disc image,
 *        the memory-card saves, free space, and files staged for the next launch.
 *
 * The game uses paths relative to its working directory, which
 * android_bootstrap() (webos_glue.c) sets to the data folder:
 *   rom/<image>            the disc image (the port prefers rom/GAFE01.iso)
 *   save/card_a/           memory card A (.gci files; the player's town)
 *   save/card_b/           memory card B
 *   texture_pack/          the texture pack (or a link to one on a USB drive)
 *
 * While the game runs it keeps its saves open, so uploaded saves go to
 * incoming/ first and are moved into place at the next launch, before the
 * game starts (ApplyPendingFiles).
 */
#ifndef ACWEBOS_STORAGE_H
#define ACWEBOS_STORAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace acwebos
{

/** A memory-card save file. */
struct SaveFile {
    char card;          ///< 'a' or 'b'.
    std::string name;   ///< File name (no directory).
    std::uint64_t size; ///< Bytes.
    std::int64_t mtime; ///< Last change, seconds since the Unix epoch.
};

/** What a disc image's header says. */
struct DiscInfo {
    bool valid = false;  ///< A GameCube disc image (plain or CISO).
    std::string gameId;  ///< Six-character ID such as "GAFE01"; empty for CISO.
    std::string format;  ///< "iso" or "ciso".
};

class Storage
{
public:
    /** Space kept free on top of any upload, so the TV is never filled to the brim. */
    static constexpr std::uint64_t kSafetyMarginBytes = 32ull * 1024ull * 1024ull;
    /** A GameCube disc holds 1.4 GB; anything larger is not a disc image. */
    static constexpr std::uint64_t kMaxDiscBytes = 1460ull * 1024ull * 1024ull;
    /** The smallest upload accepted as a disc image (header plus file table). */
    static constexpr std::uint64_t kMinDiscBytes = 0x440;
    /** Memory-card saves are a few hundred KB at most. */
    static constexpr std::uint64_t kMaxSaveBytes = 4ull * 1024ull * 1024ull;

    /** Uses the current working directory (the data folder) as the root. */
    Storage();

    /** Absolute path of the data folder, without a trailing slash. */
    const std::string &Root() const { return root_; }

    /** Absolute path of @p relative inside the data folder. */
    std::string PathFor(const std::string &relative) const;

    /** Bytes available to the app on the filesystem holding @p path (default: the data folder). */
    std::uint64_t FreeBytes(const std::string &path = std::string()) const;

    /** Size of @p path in bytes, or 0 if it does not exist. */
    static std::uint64_t FileSize(const std::string &path);

    /** Reads the header of the image at @p path. */
    static DiscInfo InspectDisc(const std::string &path);

    /**
     * The disc image the game will use: rom/GAFE01.iso first, then the first
     * .iso/.gcm/.ciso in rom/ (the same order as the port's pc_disc.c).
     * @return Relative path, or an empty string when there is none.
     */
    std::string FindDiscImage() const;

    /** File name a disc image with @p info is stored under in rom/. */
    static std::string DiscFileName(const DiscInfo &info);

    /** The saves on both memory cards. */
    std::vector<SaveFile> ListSaves() const;

    /** Whether @p name may be read or written through the page: a plain name ending in .gci. */
    static bool IsAllowedSaveName(const std::string &name);

    /** Relative directory of memory card @p card ('a' or 'b'), or empty for any other value. */
    static std::string CardDir(char card);

    /** Directory where files received during play wait for the next launch. */
    std::string IncomingDir() const;

    /**
     * Moves files staged in incoming/ during the previous session into place.
     * Called at launch, before the game opens anything.
     * @return Number of files applied.
     */
    int ApplyPendingFiles() const;

private:
    std::string root_;
};

/** Formats a byte count for people ("1.5 GB"). */
std::string HumanBytes(std::uint64_t bytes);

} // namespace acwebos

#endif // ACWEBOS_STORAGE_H

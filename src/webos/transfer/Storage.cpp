/**
 * @file Storage.cpp
 * @brief Implementation of the data folder helpers (see Storage.h).
 */
#include "Storage.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace acwebos
{
namespace fs = std::filesystem;

namespace
{

/** GameCube disc magic word at offset 0x1C (big endian). */
constexpr std::uint32_t kGameCubeMagic = 0xC2339F3Du;
/** Folder (under the data folder) holding files received during play. */
constexpr const char *kIncomingDir = "incoming";

/** True if @p name ends with @p suffix, ignoring case. */
bool EndsWithNoCase(const std::string &name, const std::string &suffix)
{
    if (name.size() < suffix.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

} // namespace

std::string HumanBytes(std::uint64_t bytes)
{
    constexpr const char *kUnits[] = { "B", "KB", "MB", "GB", "TB" };
    double value     = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

Storage::Storage()
{
    std::error_code ec;
    root_ = fs::current_path(ec).string();
    if (ec || root_.empty())
        root_ = ".";
}

std::string Storage::PathFor(const std::string &relative) const { return root_ + "/" + relative; }

std::uint64_t Storage::FreeBytes(const std::string &path) const
{
    struct statvfs info {};
    if (statvfs(path.empty() ? root_.c_str() : path.c_str(), &info) != 0)
        return 0;
    return static_cast<std::uint64_t>(info.f_bavail) * static_cast<std::uint64_t>(info.f_frsize);
}

std::uint64_t Storage::FileSize(const std::string &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return 0;
    return static_cast<std::uint64_t>(info.st_size);
}

DiscInfo Storage::InspectDisc(const std::string &path)
{
    DiscInfo info;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return info;

    std::array<unsigned char, 0x20> header{};
    const bool read = std::fread(header.data(), 1, header.size(), file) == header.size();
    std::fclose(file);
    if (!read)
        return info;

    if (header[0] == 'C' && header[1] == 'I' && header[2] == 'S' && header[3] == 'O') {
        info.valid  = true;
        info.format = "ciso";
        return info;
    }

    const std::uint32_t magic = (static_cast<std::uint32_t>(header[0x1C]) << 24) | (static_cast<std::uint32_t>(header[0x1D]) << 16)
                                | (static_cast<std::uint32_t>(header[0x1E]) << 8) | static_cast<std::uint32_t>(header[0x1F]);
    if (magic != kGameCubeMagic)
        return info;

    std::string id(reinterpret_cast<const char *>(header.data()), 6);
    const bool printable = std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
    info.valid  = true;
    info.gameId = printable ? id : std::string();
    info.format = "iso";
    return info;
}

std::string Storage::FindDiscImage() const
{
    if (FileSize(PathFor("rom/GAFE01.iso")) > 0)
        return "rom/GAFE01.iso";

    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(PathFor("rom"), ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(ec)
            && (EndsWithNoCase(name, ".iso") || EndsWithNoCase(name, ".gcm") || EndsWithNoCase(name, ".ciso")))
            return "rom/" + name;
    }
    return {};
}

std::string Storage::DiscFileName(const DiscInfo &info)
{
    if (info.format == "ciso")
        return "game.ciso";
    return (info.gameId.empty() ? std::string("game") : info.gameId) + ".iso";
}

std::string Storage::CardDir(char card)
{
    if (card == 'a')
        return "save/card_a";
    if (card == 'b')
        return "save/card_b";
    return {};
}

bool Storage::IsAllowedSaveName(const std::string &name)
{
    if (name.empty() || name.size() > 64 || name.front() == '.' || !EndsWithNoCase(name, ".gci"))
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
    });
}

std::vector<SaveFile> Storage::ListSaves() const
{
    std::vector<SaveFile> saves;
    for (const char card : { 'a', 'b' }) {
        std::error_code ec;
        for (const fs::directory_entry &entry : fs::directory_iterator(PathFor(CardDir(card)), ec)) {
            const std::string name = entry.path().filename().string();
            if (!IsAllowedSaveName(name) || !entry.is_regular_file(ec))
                continue;
            struct stat info {};
            if (stat(entry.path().c_str(), &info) != 0)
                continue;
            saves.push_back({ card, name, static_cast<std::uint64_t>(info.st_size), static_cast<std::int64_t>(info.st_mtime) });
        }
    }
    std::sort(saves.begin(), saves.end(), [](const SaveFile &a, const SaveFile &b) {
        return a.card != b.card ? a.card < b.card : a.name < b.name;
    });
    return saves;
}

std::string Storage::IncomingDir() const { return PathFor(kIncomingDir); }

int Storage::ApplyPendingFiles() const
{
    int applied = 0;
    std::error_code ec;
    const fs::path incoming = IncomingDir();
    if (!fs::exists(incoming, ec))
        return 0;

    // Collect first, move afterwards: the directory is not changed while it is being walked.
    std::vector<fs::path> staged;
    for (fs::recursive_directory_iterator it(incoming, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && !EndsWithNoCase(it->path().filename().string(), ".part"))
            staged.push_back(it->path()); // ".part" files are uploads that never finished
    }

    // incoming/ mirrors the data folder layout (incoming/save/card_a/X.gci -> save/card_a/X.gci).
    for (const fs::path &source : staged) {
        const fs::path relative    = fs::relative(source, incoming, ec);
        const fs::path destination = fs::path(root_) / relative;
        fs::create_directories(destination.parent_path(), ec);
        fs::rename(source, destination, ec);
        if (!ec) {
            ++applied;
            std::printf("[AC/webOS] applied staged file %s\n", relative.c_str());
        }
    }
    fs::remove_all(incoming, ec);
    return applied;
}

} // namespace acwebos

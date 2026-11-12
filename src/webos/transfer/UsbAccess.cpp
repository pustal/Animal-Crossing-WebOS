/**
 * @file UsbAccess.cpp
 * @brief Implementation of root access, USB drive discovery and texture pack
 *        placement (see UsbAccess.h).
 */
#include "UsbAccess.h"

#include "Storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace acwebos
{
namespace fs = std::filesystem;

namespace
{

/** Where webOS (and some other Linux setups) mount removable drives. */
constexpr std::array<const char *, 4> kUsbMountPrefixes = { "/tmp/usb/", "/media/usb", "/mnt/usb", "/run/media/" };
/** The Homebrew Channel's root command endpoint. */
constexpr const char *kHbChannelExec = "luna://org.webosbrew.hbchannel.service/exec";
/** luna-send-pub is the bus client apps may use; luna-send needs root. */
constexpr std::array<const char *, 2> kLunaClients = { "/usr/bin/luna-send-pub", "/usr/bin/luna-send" };
/** How long a counted pack size is reused before counting again. */
constexpr std::chrono::seconds kCountCacheTime{ 20 };
/** Stop counting at this many entries; the page then shows "at least". */
constexpr std::uint32_t kCountLimit = 200000;

/** Runs @p command with /bin/sh and captures stdout. @return The exit status, or -1. */
int RunShell(const std::string &command, std::string *output)
{
    std::FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
        return -1;
    std::string captured;
    std::array<char, 4096> buffer{};
    std::size_t length = 0;
    while ((length = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0)
        captured.append(buffer.data(), length);
    const int status = pclose(pipe);
    if (output)
        *output = std::move(captured);
    return status;
}

/** Escapes @p text as a JSON string body. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

/**
 * Extracts the string value of "@p key" from a flat JSON reply (luna-send
 * output). Handles the escapes JSON strings use for shell output.
 * @return False if the key is missing.
 */
bool JsonStringField(const std::string &json, const std::string &key, std::string *value)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at           = json.find(needle);
    if (at == std::string::npos)
        return false;
    at = json.find('"', json.find(':', at + needle.size()));
    if (at == std::string::npos)
        return false;

    std::string out;
    for (std::size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"')
            break;
        if (c != '\\' || i + 1 >= json.size()) {
            out += c;
            continue;
        }
        const char escaped = json[++i];
        switch (escaped) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u':
                if (i + 4 < json.size()) {
                    out += static_cast<char>(std::strtol(json.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                }
                break;
            default: out += escaped; break;
        }
    }
    *value = std::move(out);
    return true;
}

/** Decodes the octal escapes /proc/mounts uses in paths ("\040" = space). */
std::string DecodeMountPath(const std::string &raw)
{
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size()) {
            const std::string digits = raw.substr(i + 1, 3);
            if (std::all_of(digits.begin(), digits.end(), [](char d) { return d >= '0' && d <= '7'; })) {
                out += static_cast<char>(std::strtol(digits.c_str(), nullptr, 8));
                i += 3;
                continue;
            }
        }
        out += raw[i];
    }
    return out;
}

bool IsUsbMountPoint(const std::string &path)
{
    return std::any_of(kUsbMountPrefixes.begin(), kUsbMountPrefixes.end(),
                       [&](const char *prefix) { return path.rfind(prefix, 0) == 0; });
}

/** Mount points (field 2) of the /proc/mounts text @p mounts that look like USB drives. */
std::vector<std::string> ParseUsbMounts(const std::string &mounts)
{
    std::vector<std::string> found;
    std::istringstream lines(mounts);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string device, mountPoint;
        if (!(fields >> device >> mountPoint))
            continue;
        const std::string decoded = DecodeMountPath(mountPoint);
        if (IsUsbMountPoint(decoded) && std::find(found.begin(), found.end(), decoded) == found.end())
            found.push_back(decoded);
    }
    return found;
}

/** Whether the app can create (and remove) a file in @p dir. */
bool CanWrite(const std::string &dir)
{
    const std::string probe = dir + "/.acwebos_write_test";
    const int fd            = open(probe.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    close(fd);
    unlink(probe.c_str());
    return true;
}

/** Whether the app can list @p dir. */
bool CanRead(const std::string &dir)
{
    DIR *handle = opendir(dir.c_str());
    if (!handle)
        return false;
    closedir(handle);
    return true;
}

/** Counts .dds files below @p dir (up to kCountLimit entries visited). */
std::uint32_t CountDdsFiles(const std::string &dir)
{
    std::uint32_t files   = 0;
    std::uint32_t visited = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && visited < kCountLimit; it.increment(ec), ++visited) {
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dds") == 0)
            ++files;
    }
    return files;
}

/** True if @p path is a mount point in /proc/mounts (used to spot our own bind mount). */
bool IsMountedAt(const std::string &path)
{
    std::ifstream mounts("/proc/mounts");
    std::string device, mountPoint, rest;
    while (mounts >> device >> mountPoint && std::getline(mounts, rest)) {
        if (DecodeMountPath(mountPoint) == path)
            return true;
    }
    return false;
}

} // namespace

std::string UsbAccess::ShellQuote(const std::string &text)
{
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

void UsbAccess::ProbeRoot()
{
    RootStatus status;
    status.runningAsRoot = geteuid() == 0;
    if (status.runningAsRoot) {
        status.detail = "The app runs as root.";
    }
    else {
        // Ask the Homebrew Channel to run "id -u" as root.
        std::string luna;
        for (const char *client : kLunaClients) {
            if (access(client, X_OK) == 0) {
                luna = client;
                break;
            }
        }
        if (luna.empty()) {
            status.detail = "No luna-send client available to this app.";
        }
        else {
            const std::string payload = "{\"command\":\"id -u\"}";
            std::string reply;
            RunShell(luna + " -n 1 -w 5000 " + kHbChannelExec + " " + ShellQuote(payload) + " 2>&1", &reply);
            std::string stdoutText;
            if (JsonStringField(reply, "stdoutString", &stdoutText) && stdoutText.rfind("0", 0) == 0) {
                status.homebrewChannel = true;
                status.detail          = "Root commands run through the Homebrew Channel.";
            }
            else {
                std::string error;
                if (!JsonStringField(reply, "errorText", &error))
                    error = reply.substr(0, 160);
                while (!error.empty() && (error.back() == '\n' || error.back() == ' '))
                    error.pop_back();
                status.detail = "Homebrew Channel root commands unavailable" + (error.empty() ? std::string(".") : ": " + error);
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    root_       = status;
    rootProbed_ = true;
    std::printf("[AC/webOS] root: %s\n", status.detail.c_str());
}

RootStatus UsbAccess::Root() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_;
}

bool UsbAccess::RunAsRoot(const std::string &command, std::string *output)
{
    const RootStatus status = Root();
    if (status.runningAsRoot)
        return RunShell("sh -c " + ShellQuote(command), output) == 0;
    if (!status.homebrewChannel)
        return false;

    std::string luna;
    for (const char *client : kLunaClients) {
        if (access(client, X_OK) == 0) {
            luna = client;
            break;
        }
    }
    // The exec method reports success through returnValue; append a marker to
    // learn the command's own exit status as well.
    const std::string wrapped = "(" + command + ") && echo __ACWEBOS_OK__";
    const std::string payload = "{\"command\":\"" + JsonEscape(wrapped) + "\"}";
    std::string reply;
    RunShell(luna + " -n 1 -w 60000 " + kHbChannelExec + " " + ShellQuote(payload) + " 2>&1", &reply);

    std::string stdoutText;
    if (!JsonStringField(reply, "stdoutString", &stdoutText))
        return false;
    const std::size_t marker = stdoutText.rfind("__ACWEBOS_OK__");
    if (output)
        *output = stdoutText.substr(0, marker == std::string::npos ? stdoutText.size() : marker);
    return marker != std::string::npos;
}

std::vector<std::string> UsbAccess::UsbMountPoints()
{
    std::string mounts;
    {
        std::ifstream file("/proc/mounts");
        std::ostringstream text;
        text << file.rdbuf();
        mounts = text.str();
    }
    std::vector<std::string> found = ParseUsbMounts(mounts);

    // Inside the jail /proc/mounts may not list the drives; root sees the real table.
    if (found.empty() && Root().Available()) {
        std::string rootMounts;
        if (RunAsRoot("cat /proc/mounts", &rootMounts))
            found = ParseUsbMounts(rootMounts);
    }
    return found;
}

std::vector<UsbDrive> UsbAccess::ListDrives()
{
    std::vector<UsbDrive> drives;
    const bool root = Root().Available();
    for (const std::string &mountPoint : UsbMountPoints()) {
        UsbDrive drive;
        drive.mountPoint = mountPoint;
        drive.label      = fs::path(mountPoint).filename().string();
        drive.readable   = CanRead(mountPoint);
        drive.writable   = drive.readable && CanWrite(mountPoint);

        struct statvfs info {};
        if (statvfs(mountPoint.c_str(), &info) == 0 && info.f_blocks > 0) {
            drive.freeBytes  = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
            drive.totalBytes = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
            drive.spaceKnown = true;
        }
        else if (root) {
            // df -k prints: Filesystem 1K-blocks Used Available Use% Mounted-on
            std::string df;
            if (RunAsRoot("df -k " + ShellQuote(mountPoint) + " | tail -n 1", &df)) {
                std::istringstream fields(df);
                std::string filesystem;
                unsigned long long blocks = 0, used = 0, available = 0;
                if (fields >> filesystem >> blocks >> used >> available) {
                    drive.totalBytes = blocks * 1024ull;
                    drive.freeBytes  = available * 1024ull;
                    drive.spaceKnown = true;
                }
            }
        }

        const std::string packDir = mountPoint + "/" + kPackFolder;
        if (drive.readable) {
            struct stat st {};
            drive.packExists = stat(packDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        else if (root) {
            drive.packExists = RunAsRoot("test -d " + ShellQuote(packDir));
        }

        if (drive.packExists && drive.readable) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now    = std::chrono::steady_clock::now();
            const auto cached = std::find_if(countCache_.begin(), countCache_.end(),
                                             [&](const CountCache &entry) { return entry.mountPoint == mountPoint; });
            if (cached != countCache_.end() && cached->expiresAt > now) {
                drive.packFiles = cached->files;
            }
            else {
                drive.packFiles = CountDdsFiles(packDir);
                if (cached != countCache_.end())
                    *cached = { mountPoint, drive.packFiles, now + kCountCacheTime };
                else
                    countCache_.push_back({ mountPoint, drive.packFiles, now + kCountCacheTime });
            }
        }
        drive.discFile = FindDriveDisc(mountPoint, &drive.discBytes);
        drives.push_back(drive);
    }
    return drives;
}

bool UsbAccess::IsUsbMount(const std::string &mountPoint)
{
    const std::vector<std::string> mounts = UsbMountPoints();
    return std::find(mounts.begin(), mounts.end(), mountPoint) != mounts.end();
}

void UsbAccess::InvalidatePackCounts()
{
    std::lock_guard<std::mutex> lock(mutex_);
    countCache_.clear();
}

std::string UsbAccess::CreatePackFolder(const std::string &mountPoint)
{
    const std::string packDir = mountPoint + "/" + kPackFolder;
    std::error_code ec;
    if (CanWrite(mountPoint)) {
        fs::create_directories(packDir, ec);
        return ec ? "Could not create the folder: " + ec.message() : std::string();
    }
    if (Root().Available()) {
        return RunAsRoot("mkdir -p " + ShellQuote(packDir)) ? std::string()
                                                            : "Root could not create the folder on the drive.";
    }
    return "The app cannot write to this drive and has no root access.";
}

std::string UsbAccess::MoveWithRoot(const std::string &sourcePath, const std::string &destination)
{
    const std::string dir = fs::path(destination).parent_path().string();
    // mv across filesystems copies, then removes the source.
    if (RunAsRoot("mkdir -p " + ShellQuote(dir) + " && mv -f " + ShellQuote(sourcePath) + " " + ShellQuote(destination)))
        return {};
    return "Root could not move the file onto the drive.";
}

void UsbAccess::PlaceTexturePack(const std::string &dataRoot)
{
    const std::string local = dataRoot + "/texture_pack";
    std::error_code ec;

    // A bind mount left from an earlier session: keep using it if it still works.
    if (IsMountedAt(local) && CanRead(local)) {
        std::lock_guard<std::mutex> lock(mutex_);
        packSource_ = PackSource::UsbMount;
        packPath_   = local;
        std::printf("[AC/webOS] texture pack: USB bind mount still active\n");
        return;
    }

    const bool localIsLink = fs::is_symlink(local, ec);
    const bool localHasFiles =
        !localIsLink && fs::is_directory(local, ec) && fs::directory_iterator(local, ec) != fs::directory_iterator();

    // A pack copied into the data folder by hand wins: it is what the user put there.
    if (localHasFiles) {
        std::lock_guard<std::mutex> lock(mutex_);
        packSource_ = PackSource::Internal;
        packPath_   = local;
        std::printf("[AC/webOS] texture pack: using the files in the data folder\n");
        return;
    }

    for (const UsbDrive &drive : ListDrives()) {
        if (!drive.packExists)
            continue;
        const std::string packDir = drive.mountPoint + "/" + kPackFolder;

        if (drive.readable) {
            fs::remove(local, ec); // an old link, or an empty folder
            fs::create_directory_symlink(packDir, local, ec);
            if (!ec) {
                std::lock_guard<std::mutex> lock(mutex_);
                packSource_ = PackSource::UsbLink;
                packPath_   = packDir;
                std::printf("[AC/webOS] texture pack: linked to %s\n", packDir.c_str());
                return;
            }
        }
        else if (Root().Available()) {
            // The app cannot read the drive: let root bind the folder into the data folder.
            if (localIsLink)
                fs::remove(local, ec);
            fs::create_directories(local, ec);
            if (RunAsRoot("mount --bind " + ShellQuote(packDir) + " " + ShellQuote(local)) && CanRead(local)) {
                std::lock_guard<std::mutex> lock(mutex_);
                packSource_ = PackSource::UsbMount;
                packPath_   = packDir;
                std::printf("[AC/webOS] texture pack: bind-mounted %s\n", packDir.c_str());
                return;
            }
            RunAsRoot("umount " + ShellQuote(local));
            std::printf("[AC/webOS] texture pack: could not make %s readable to the app\n", packDir.c_str());
        }
    }

    // No pack on any drive: leave an empty folder (replacing a link to a drive that is gone).
    if (localIsLink)
        fs::remove(local, ec);
    fs::create_directories(local, ec);
    std::lock_guard<std::mutex> lock(mutex_);
    packSource_ = PackSource::None;
    packPath_.clear();
}

std::string UsbAccess::FindDriveDisc(const std::string &mountPoint, std::uint64_t *size)
{
    const std::string folder = mountPoint + "/" + kDiscFolder;
    std::vector<std::string> names;
    if (CanRead(mountPoint)) {
        std::error_code ec;
        for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec)) {
            if (entry.is_regular_file(ec))
                names.push_back(entry.path().filename().string());
        }
    }
    else if (Root().Available()) {
        std::string listing;
        if (RunAsRoot("test -d " + ShellQuote(folder) + " && ls -1 " + ShellQuote(folder), &listing)) {
            std::istringstream lines(listing);
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty())
                    names.push_back(line);
            }
        }
    }

    // GAFE01.iso first, as the game itself prefers it (pc_disc.c), then the first image by name.
    std::sort(names.begin(), names.end());
    std::string chosen;
    for (const std::string &name : names) {
        if (!Storage::IsDiscImageName(name) || name.size() < 5 || name.compare(name.size() - 5, 5, ".part") == 0)
            continue;
        if (name == "GAFE01.iso") {
            chosen = name;
            break;
        }
        if (chosen.empty())
            chosen = name;
    }
    if (chosen.empty())
        return {};

    if (size) {
        const std::string path = folder + "/" + chosen;
        *size                  = Storage::FileSize(path);
        std::string bytes;
        if (*size == 0 && Root().Available() && RunAsRoot("stat -c %s " + ShellQuote(path), &bytes))
            *size = std::strtoull(bytes.c_str(), nullptr, 10);
    }
    return chosen;
}

void UsbAccess::ReleaseDiscLinks(const std::string &dataRoot)
{
    std::lock_guard<std::mutex> placing(discMutex_);
    const std::string rom = dataRoot + "/rom";
    std::error_code ec;
    std::vector<fs::path> entries;
    for (const fs::directory_entry &entry : fs::directory_iterator(rom, ec))
        entries.push_back(entry.path());
    for (const fs::path &entry : entries) {
        const std::string path = entry.string();
        if (!Storage::IsDiscImageName(entry.filename().string()))
            continue;
        if (IsMountedAt(path)) {
            // Lazy: detaches it even while the game reads the disc through it.
            RunAsRoot("umount -l " + ShellQuote(path));
            fs::remove(path, ec); // the empty file it covered
        }
        else if (fs::is_symlink(entry, ec)) {
            fs::remove(entry, ec);
        }
    }
}

void UsbAccess::PlaceDiscImage(const std::string &dataRoot)
{
    const std::string rom = dataRoot + "/rom";
    std::error_code ec;
    fs::create_directories(rom, ec);

    // Links and bind mounts from earlier sessions are made again from what the drives hold now.
    ReleaseDiscLinks(dataRoot);

    std::lock_guard<std::mutex> placing(discMutex_);
    // Empty files left where a bind mount failed would pass for images.
    for (const fs::directory_entry &entry : fs::directory_iterator(rom, ec)) {
        if (Storage::IsDiscImageName(entry.path().filename().string()) && entry.is_regular_file(ec) && entry.file_size(ec) == 0)
            fs::remove(entry.path(), ec);
    }

    const std::vector<std::string> internal = Storage::DiscImagesIn(rom);
    if (!internal.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        discSource_ = DiscSource::Internal;
        discPath_   = internal.front();
        std::printf("[AC/webOS] disc image: %s in the data folder\n", discPath_.c_str());
        return;
    }

    for (const UsbDrive &drive : ListDrives()) {
        if (drive.discFile.empty())
            continue;
        const std::string source = drive.mountPoint + "/" + kDiscFolder + "/" + drive.discFile;
        const std::string local  = rom + "/" + drive.discFile;

        if (drive.readable) {
            fs::create_symlink(source, local, ec);
            if (!ec) {
                std::lock_guard<std::mutex> lock(mutex_);
                discSource_ = DiscSource::UsbLink;
                discPath_   = source;
                std::printf("[AC/webOS] disc image: linked to %s\n", source.c_str());
                return;
            }
        }
        else if (Root().Available()) {
            // Root binds the file over an empty one, so the app can open it by that name.
            std::ofstream(local).close();
            if (RunAsRoot("mount --bind " + ShellQuote(source) + " " + ShellQuote(local)) && Storage::FileSize(local) > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                discSource_ = DiscSource::UsbMount;
                discPath_   = source;
                std::printf("[AC/webOS] disc image: bind-mounted %s\n", source.c_str());
                return;
            }
            RunAsRoot("umount " + ShellQuote(local));
            fs::remove(local, ec);
            std::printf("[AC/webOS] disc image: could not make %s readable to the app\n", source.c_str());
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    discSource_ = DiscSource::None;
    discPath_.clear();
}

DiscSource UsbAccess::ActiveDiscSource() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return discSource_;
}

std::string UsbAccess::ActiveDiscPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return discPath_;
}

std::string UsbAccess::CreateDriveFolder(const std::string &mountPoint, const std::string &relative)
{
    const std::string folder = mountPoint + "/" + relative;
    std::error_code ec;
    if (CanWrite(mountPoint)) {
        fs::create_directories(folder, ec);
        return ec ? "Could not create the folder: " + ec.message() : std::string();
    }
    if (Root().Available()) {
        return RunAsRoot("mkdir -p " + ShellQuote(folder)) ? std::string() : "Root could not create the folder on the drive.";
    }
    return "The app cannot write to this drive and has no root access.";
}

bool UsbAccess::RemoveFile(const std::string &path)
{
    std::error_code ec;
    if (fs::remove(path, ec) || (!ec && !fs::exists(fs::symlink_status(path, ec))))
        return true;
    return Root().Available() && RunAsRoot("rm -f " + ShellQuote(path));
}

bool UsbAccess::RenameFile(const std::string &from, const std::string &to)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    return Root().Available() && RunAsRoot("mv -f " + ShellQuote(from) + " " + ShellQuote(to));
}

std::string UsbAccess::CopyFile(const std::string &source, const std::string &destination,
                                const std::function<void(std::uint64_t)> &onProgress)
{
    // Directly, with progress, when the app may read the source and write the destination's folder.
    const std::string folder = fs::path(destination).parent_path().string();
    std::FILE *in            = access(folder.c_str(), W_OK) == 0 ? std::fopen(source.c_str(), "rb") : nullptr;
    if (in) {
        std::FILE *out = std::fopen(destination.c_str(), "wb");
        if (!out) {
            std::fclose(in);
            return "Could not create " + destination + ".";
        }
        std::vector<char> buffer(4u << 20);
        std::uint64_t copied = 0;
        bool ok              = true;
        std::size_t length   = 0;
        while ((length = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
            if (std::fwrite(buffer.data(), 1, length, out) != length) {
                ok = false; // disk full or I/O error
                break;
            }
            copied += length;
            onProgress(copied);
        }
        ok = ok && !std::ferror(in) && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        std::fclose(in);
        std::fclose(out);
        return ok ? std::string() : "The copy could not be written (is the destination full?).";
    }

    if (!Root().Available())
        return "The app cannot read the disc image or write there, and has no root access.";
    if (RunAsRoot("cp -f " + ShellQuote(source) + " " + ShellQuote(destination) + " && sync"))
        return {};
    return "Root could not copy the disc image there (is the destination full?).";
}

PackSource UsbAccess::ActivePackSource() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return packSource_;
}

std::string UsbAccess::ActivePackPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return packPath_;
}

} // namespace acwebos

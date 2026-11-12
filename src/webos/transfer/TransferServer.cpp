/**
 * @file TransferServer.cpp
 * @brief Implementation of the upload server (see TransferServer.h).
 *
 * HTTP API (all /api routes need the PIN, as an "X-Pin" header or ?pin=):
 *   GET  /                          the upload page (web/index.html, embedded at build time)
 *   GET  /api/status                JSON: disc, space, root, USB drives, texture pack
 *   PUT  /api/disc?drive=<mount|internal>   raw body = disc image (.iso/.gcm/.ciso), streamed to rom/
 *                                   or to "Animal Crossing/rom" on a USB drive; other copies are removed
 *   POST /api/disc/move?drive=<mount|internal>   moves the disc image there
 *   GET  /api/saves                 JSON list of memory-card saves
 *   GET  /api/saves/:card/:name     download one save (card = a or b)
 *   PUT  /api/saves/:card/:name     raw body = save (staged for the next launch while playing)
 *   POST /api/texture-pack/folder?drive=<mount>   create "Animal Crossing/texture_pack" on a drive
 *   PUT  /api/texture-pack/file?drive=<mount|internal>&path=<relative .dds path>
 */
#include "TransferServer.h"

#include "NetInfo.h"
#include "Storage.h"
#include "UsbAccess.h"
#include "WebPage.generated.h"

// Four workers: the page uploads texture files three at a time and polls the status alongside.
#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include <httplib.h>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>

namespace acwebos
{
namespace fs = std::filesystem;

namespace
{

constexpr const char *kJsonType  = "application/json";
constexpr const char *kTextType  = "text/plain; charset=utf-8";
constexpr const char *kPinHeader = "X-Pin";
constexpr const char *kPinFile   = "transfer_pin.txt";
constexpr const char *kInternal  = "internal";
constexpr int kPortAttempts      = 10;
/** Largest single texture file accepted (a 4K RGBA DDS with mipmaps is ~85 MB; HD packs stay far below). */
constexpr std::uint64_t kMaxTextureBytes = 96ull * 1024ull * 1024ull;
/** Deepest folder nesting accepted inside a texture pack upload. */
constexpr int kMaxPathDepth = 12;

/** Escapes @p text for use inside a JSON string literal. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

const char *JsonBool(bool value) { return value ? "true" : "false"; }

/** Sends a plain-text error. */
void Fail(httplib::Response &res, int status, const std::string &message)
{
    res.status = status;
    res.set_content(message, kTextType);
}

/** Reads the announced body size. @return False if the header is missing or malformed. */
bool ContentLength(const httplib::Request &req, std::uint64_t *out)
{
    const std::string value = req.get_header_value("Content-Length");
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return false;
    try {
        *out = static_cast<std::uint64_t>(std::stoull(value));
        return true;
    }
    catch (const std::exception &) {
        return false;
    }
}

/**
 * Streams the request body into @p path, refusing more than @p expected
 * bytes, and flushes it to storage.
 * @return Bytes written, or -1 on error.
 */
template <typename ProgressFn>
std::int64_t StreamBodyToFile(const httplib::ContentReader &reader, const std::string &path, std::uint64_t expected,
                              ProgressFn onProgress)
{
    std::FILE *out = std::fopen(path.c_str(), "wb");
    if (!out)
        return -1;

    std::uint64_t written = 0;
    const bool completed  = reader([&](const char *data, std::size_t length) {
        if (written + length > expected)
            return false; // more than announced
        if (std::fwrite(data, 1, length, out) != length)
            return false; // disk full or I/O error
        written += length;
        onProgress(written);
        return true;
    });

    const bool flushed = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    std::fclose(out);
    if (!completed || !flushed)
        return -1;
    return static_cast<std::int64_t>(written);
}

/**
 * Validates a texture path from the page: relative, at most kMaxPathDepth
 * folders, no "." / ".." parts or hidden names (which also skips macOS "._"
 * metadata files), no control characters or characters FAT/exFAT drives
 * reject, and a .dds file name. The length limit keeps
 * "texture_pack/<path>" inside the port's 300-byte path buffer
 * (pc_texture_pack.c, scan_directory).
 */
bool IsAllowedTexturePath(const std::string &path)
{
    constexpr std::size_t kMaxPathBytes = 260;
    if (path.empty() || path.size() > kMaxPathBytes || path.front() == '/')
        return false;
    int depth = 0;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end     = path.find('/', start);
        const std::string segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment.front() == '.' || segment.back() == ' ')
            return false;
        const bool allowed = std::none_of(segment.begin(), segment.end(), [](char c) {
            const unsigned char u = static_cast<unsigned char>(c);
            return u < 0x20 || u == 0x7F || std::strchr("\\:*?\"<>|", c) != nullptr;
        });
        if (!allowed || ++depth > kMaxPathDepth)
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".dds") == 0;
}

/** Where the disc image is stored: in rom/ or on a drive. */
struct DiscLocation {
    std::string path;  ///< Absolute path of the image, empty when there is none.
    std::string drive; ///< Mount point of its drive, empty for the TV storage.
    std::uint64_t size = 0;
};

/**
 * The disc image as stored: one in rom/ first (the game prefers it, see
 * UsbAccess::PlaceDiscImage), else the first drive's. Not the link or bind
 * mount in rom/ that points at a drive's.
 */
DiscLocation FindStoredDisc(const Storage &storage, UsbAccess &usb)
{
    DiscLocation location;
    const std::vector<std::string> internal = storage.InternalDiscImages();
    if (!internal.empty()) {
        location.path = internal.front();
        location.size = Storage::FileSize(location.path);
        return location;
    }
    for (const UsbDrive &drive : usb.ListDrives()) {
        if (drive.discFile.empty())
            continue;
        location.path  = drive.mountPoint + "/" + UsbAccess::kDiscFolder + "/" + drive.discFile;
        location.drive = drive.mountPoint;
        location.size  = drive.discBytes;
        return location;
    }
    return location;
}

/** Where a disc image upload or move goes. */
struct DiscTarget {
    bool usb = false;
    std::string drive;  ///< Mount point, for a USB drive.
    std::string folder; ///< Absolute folder the image goes in.
    std::uint64_t free = 0;
    bool direct = true; ///< The app writes there itself (else root moves it there).
};

/**
 * Reads the "drive" parameter (a USB mount point, or "internal", the
 * default) into @p target. Answers @p res and returns false when it is not
 * usable.
 */
bool ParseDiscTarget(const httplib::Request &req, Storage &storage, UsbAccess &usb, DiscTarget *target, httplib::Response &res)
{
    const std::string drive = req.has_param("drive") ? req.get_param_value("drive") : std::string(kInternal);
    if (drive == kInternal) {
        target->folder = storage.PathFor("rom");
        target->free   = storage.FreeBytes();
        return true;
    }
    for (const UsbDrive &found : usb.ListDrives()) {
        if (found.mountPoint != drive)
            continue;
        target->usb    = true;
        target->drive  = drive;
        target->folder = drive + "/" + UsbAccess::kDiscFolder;
        target->direct = found.writable;
        target->free   = found.spaceKnown ? found.freeBytes : 0;
        if (!found.writable && !usb.Root().Available()) {
            Fail(res, 403, "The app cannot write to this USB drive and has no root access.");
            return false;
        }
        if (!found.spaceKnown) {
            Fail(res, 507, "The free space on this USB drive could not be read.");
            return false;
        }
        return true;
    }
    Fail(res, 400, "That USB drive is not connected.");
    return false;
}

/**
 * Removes every stored disc image except @p keep: in rom/ (with the links and
 * bind mounts there) and in each drive's "Animal Crossing/rom", so exactly
 * one copy is left and the game cannot pick an older one first.
 */
void RemoveOtherDiscCopies(Storage &storage, UsbAccess &usb, const std::string &keep)
{
    usb.ReleaseDiscLinks(storage.Root());
    for (const std::string &image : storage.InternalDiscImages()) {
        if (image != keep)
            usb.RemoveFile(image);
    }
    for (const UsbDrive &drive : usb.ListDrives()) {
        if (drive.discFile.empty())
            continue;
        const std::string image = drive.mountPoint + "/" + UsbAccess::kDiscFolder + "/" + drive.discFile;
        if (image != keep)
            usb.RemoveFile(image);
    }
}

const char *DiscSourceName(DiscSource source)
{
    switch (source) {
        case DiscSource::Internal: return "internal";
        case DiscSource::UsbLink: return "usb";
        case DiscSource::UsbMount: return "usb-mount";
        default: return "none";
    }
}

const char *PackSourceName(PackSource source)
{
    switch (source) {
        case PackSource::Internal: return "internal";
        case PackSource::UsbLink: return "usb";
        case PackSource::UsbMount: return "usb-mount";
        default: return "none";
    }
}

} // namespace

std::string LoadOrCreatePin(const Storage &storage)
{
    const std::string path = storage.PathFor(kPinFile);
    std::ifstream in(path);
    std::string pin;
    if (in >> pin && pin.size() == 4 && std::all_of(pin.begin(), pin.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return pin;

    std::random_device device;
    std::uniform_int_distribution<int> digits(0, 9999);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04d", digits(device));
    std::ofstream(path) << buffer << "\n";
    return buffer;
}

TransferServer::TransferServer(Storage &storage, UsbAccess &usb, std::string pin, std::string version)
    : storage_(storage), usb_(usb), pin_(std::move(pin)), version_(std::move(version))
{
}

TransferServer::~TransferServer() { Stop(); }

bool TransferServer::Start()
{
    if (server_)
        return port_.load() != 0;

    server_ = std::make_unique<httplib::Server>();
    server_->new_task_queue = [] { return new httplib::ThreadPool(CPPHTTPLIB_THREAD_POOL_COUNT); };
    server_->set_payload_max_length(std::numeric_limits<std::size_t>::max());
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(30, 0);
    RegisterRoutes();

    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        const int port = kPreferredPort + attempt;
        if (server_->bind_to_port("0.0.0.0", port)) {
            port_.store(static_cast<std::uint16_t>(port));
            thread_ = std::thread([this] { server_->listen_after_bind(); });
            return true;
        }
    }
    server_.reset();
    return false;
}

void TransferServer::Stop()
{
    if (!server_)
        return;
    server_->stop();
    if (thread_.joinable())
        thread_.join();
    server_.reset();
    port_.store(0);
}

TransferStatus TransferServer::Snapshot() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void TransferServer::PublishEvent(const std::string &message)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.lastEvent = message;
    ++status_.eventSerial;
    std::printf("[AC/webOS] transfer: %s\n", message.c_str());
}

void TransferServer::MarkRestartPending()
{
    if (setupMode_.load())
        return;
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.restartPending = true;
}

void TransferServer::RegisterRoutes()
{
    httplib::Server &server = *server_;

    // Every request: private networks only, and /api needs the PIN shown on the TV.
    server.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        if (!IsPrivateNetworkAddress(req.remote_addr)) {
            Fail(res, 403, "Only devices on the local network may connect.");
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string pin = req.has_header(kPinHeader) ? req.get_header_value(kPinHeader) : req.get_param_value("pin");
        if (req.path.rfind("/api/", 0) == 0 && pin != pin_) {
            Fail(res, 401, "Wrong or missing PIN. Use the PIN shown on the TV.");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(kWebPageHtml, "text/html; charset=utf-8");
    });

    server.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
        const TransferStatus status = Snapshot();
        const DiscLocation disc     = FindStoredDisc(storage_, usb_);
        const DiscInfo info         = disc.path.empty() ? DiscInfo{} : Storage::InspectDisc(disc.path);
        const RootStatus root       = usb_.Root();

        std::ostringstream json;
        json << "{\"version\":\"" << JsonEscape(version_) << "\""
             << ",\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"disc\":{\"present\":" << JsonBool(!disc.path.empty()) << ",\"file\":\""
             << JsonEscape(fs::path(disc.path).filename().string()) << "\""
             << ",\"size\":" << disc.size << ",\"gameId\":\"" << JsonEscape(info.gameId) << "\""
             << ",\"location\":\"" << (disc.path.empty() ? "none" : disc.drive.empty() ? "internal" : "usb") << "\""
             << ",\"drive\":\"" << JsonEscape(disc.drive) << "\""
             << ",\"source\":\"" << DiscSourceName(usb_.ActiveDiscSource()) << "\""
             << ",\"folder\":\"" << UsbAccess::kDiscFolder << "\"}"
             << ",\"restartPending\":" << JsonBool(status.restartPending)
             << ",\"upload\":{\"active\":" << JsonBool(status.uploading) << ",\"received\":" << status.receivedBytes
             << ",\"expected\":" << status.expectedBytes << "}"
             << ",\"root\":{\"process\":" << JsonBool(root.runningAsRoot) << ",\"homebrewChannel\":"
             << JsonBool(root.homebrewChannel) << ",\"detail\":\"" << JsonEscape(root.detail) << "\"}"
             << ",\"texturePack\":{\"source\":\"" << PackSourceName(usb_.ActivePackSource()) << "\",\"path\":\""
             << JsonEscape(usb_.ActivePackPath()) << "\",\"folder\":\"" << UsbAccess::kPackFolder << "\"}"
             << ",\"drives\":[";
        bool first = true;
        for (const UsbDrive &drive : usb_.ListDrives()) {
            json << (first ? "" : ",") << "{\"mount\":\"" << JsonEscape(drive.mountPoint) << "\",\"label\":\""
                 << JsonEscape(drive.label) << "\",\"spaceKnown\":" << JsonBool(drive.spaceKnown)
                 << ",\"freeBytes\":" << drive.freeBytes << ",\"totalBytes\":" << drive.totalBytes
                 << ",\"readable\":" << JsonBool(drive.readable) << ",\"writable\":" << JsonBool(drive.writable)
                 << ",\"packExists\":" << JsonBool(drive.packExists) << ",\"packFiles\":" << drive.packFiles
                 << ",\"discFile\":\"" << JsonEscape(drive.discFile) << "\",\"discBytes\":" << drive.discBytes << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    // ---- Disc image ----
    server.Put("/api/disc", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected < Storage::kMinDiscBytes)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxDiscBytes)
            return Fail(res, 413, "That is larger than a GameCube disc; refusing " + HumanBytes(expected) + ".");

        // Where it goes: rom/ on the TV, or "Animal Crossing/rom" on a USB drive.
        DiscTarget target;
        if (!ParseDiscTarget(req, storage_, usb_, &target, res))
            return;
        const std::uint64_t needed = expected + Storage::kSafetyMarginBytes;
        if (needed > target.free) {
            return Fail(res, 507, std::string("Not enough space on the ") + (target.usb ? "USB drive" : "TV") + ": this file needs "
                                      + HumanBytes(needed) + " (including a safety margin), " + HumanBytes(target.free) + " is free.");
        }
        // A drive the app cannot write to: the upload waits in the data folder until root moves it.
        const bool staged = target.usb && !target.direct;
        if (staged && needed > storage_.FreeBytes()) {
            return Fail(res, 507, "The app writes to this USB drive through root, so the upload first goes to the TV, which has "
                                      + HumanBytes(storage_.FreeBytes()) + " free; it needs " + HumanBytes(needed) + ".");
        }

        if (discBusy_.exchange(true))
            return Fail(res, 409, "Another disc upload or move is already running.");
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> busyGuard(
            &discBusy_, [](std::atomic<bool> *flag) { flag->store(false); });

        std::error_code ec;
        const std::string folderError = target.usb ? usb_.CreateDriveFolder(target.drive, UsbAccess::kDiscFolder) : std::string();
        if (!folderError.empty())
            return Fail(res, 500, folderError);
        fs::create_directories(storage_.PathFor("rom"), ec);

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = expected;
        }
        PublishEvent(std::string("Receiving the disc image") + (target.usb ? " for the USB drive" : "") + "...");

        const std::string partPath = (staged || !target.usb ? storage_.PathFor("rom") : target.folder) + "/upload.part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this](std::uint64_t total) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.receivedBytes = total;
        });
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading = false;
        }

        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            fs::remove(partPath, ec);
            PublishEvent("Upload interrupted. Please try again.");
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }
        const DiscInfo info = Storage::InspectDisc(partPath);
        if (!info.valid) {
            fs::remove(partPath, ec);
            PublishEvent("That file is not a GameCube disc image.");
            return Fail(res, 415, "That file is not a GameCube disc image (.iso, .gcm or .ciso).");
        }

        // Into place, then every other copy goes, so the game cannot pick an older one first.
        const std::string destination = target.folder + "/" + Storage::DiscFileName(info);
        if (!target.usb)
            usb_.ReleaseDiscLinks(storage_.Root()); // a link or bind mount may hold the name
        const bool placed = staged ? usb_.MoveWithRoot(partPath, destination).empty() : usb_.RenameFile(partPath, destination);
        if (!placed) {
            fs::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }
        RemoveOtherDiscCopies(storage_, usb_, destination);

        const bool setup = setupMode_.load();
        if (setup) {
            // A drive's image is linked into rom/ now, so the game can start with it.
            usb_.PlaceDiscImage(storage_.Root());
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.discReady = !storage_.FindDiscImage().empty();
        }
        MarkRestartPending();
        std::string message = std::string("Disc image installed") + (target.usb ? " on the USB drive" : "");
        if (!info.gameId.empty())
            message += " (" + info.gameId + ")";
        if (!info.gameId.empty() && info.gameId != "GAFE01")
            message += ". Note: this port is made for the US version, GAFE01";
        PublishEvent(message + ".");
        res.set_content(setup ? "installed" : "staged", kTextType);
    });

    server.Post("/api/disc/move", [this](const httplib::Request &req, httplib::Response &res) {
        const DiscLocation source = FindStoredDisc(storage_, usb_);
        if (source.path.empty())
            return Fail(res, 404, "There is no disc image to move.");

        DiscTarget target;
        if (!ParseDiscTarget(req, storage_, usb_, &target, res))
            return;
        if (target.usb == !source.drive.empty() && target.drive == source.drive)
            return Fail(res, 400, std::string("The disc image is already on ") + (target.usb ? "that USB drive." : "the TV."));
        const std::uint64_t needed = source.size + Storage::kSafetyMarginBytes;
        if (needed > target.free) {
            return Fail(res, 507, "Not enough space there: the disc image needs " + HumanBytes(needed)
                                      + " (including a safety margin), " + HumanBytes(target.free) + " is free.");
        }

        if (discBusy_.exchange(true))
            return Fail(res, 409, "Another disc upload or move is already running.");
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> busyGuard(
            &discBusy_, [](std::atomic<bool> *flag) { flag->store(false); });

        std::error_code ec;
        const std::string folderError = target.usb ? usb_.CreateDriveFolder(target.drive, UsbAccess::kDiscFolder) : std::string();
        if (!folderError.empty())
            return Fail(res, 500, folderError);
        fs::create_directories(target.folder, ec);

        const std::string where       = target.usb ? "the USB drive" : "the TV";
        const std::string name        = fs::path(source.path).filename().string();
        const std::string destination = target.folder + "/" + name;
        const std::string partPath    = destination + ".part";
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = source.size;
        }
        PublishEvent("Moving the disc image to " + where + "...");

        // Copy, then remove the original: the TV and a drive are different file systems.
        const std::string copyError = usb_.CopyFile(source.path, partPath, [this](std::uint64_t total) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.receivedBytes = total;
        });
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading = false;
        }
        // A copy root made may not be readable to the app (an unreadable drive): only a readable one is size-checked.
        const std::uint64_t copiedSize = Storage::FileSize(partPath);
        if (!copyError.empty() || (copiedSize != 0 && copiedSize != source.size)) {
            usb_.RemoveFile(partPath);
            PublishEvent("Could not move the disc image.");
            return Fail(res, 500, (copyError.empty() ? std::string("The copy came out the wrong size.") : copyError)
                                      + " The original is untouched.");
        }
        if (!target.usb)
            usb_.ReleaseDiscLinks(storage_.Root()); // a link or bind mount may hold the name
        if (!usb_.RenameFile(partPath, destination)) {
            usb_.RemoveFile(partPath);
            return Fail(res, 500, "Could not move the copy into place. The original is untouched.");
        }
        if (!usb_.RemoveFile(source.path)) {
            // Keep exactly one copy: the original stays, the new one goes.
            usb_.RemoveFile(destination);
            return Fail(res, 500, "Could not remove the original disc image, so it was left where it was.");
        }
        usb_.ReleaseDiscLinks(storage_.Root()); // links to where it was

        const bool setup = setupMode_.load();
        if (setup)
            usb_.PlaceDiscImage(storage_.Root());
        MarkRestartPending();
        PublishEvent("Disc image moved to " + where + ".");
        res.set_content(setup ? "moved" : "staged", kTextType);
    });

    // ---- Saves ----
    server.Get("/api/saves", [this](const httplib::Request &, httplib::Response &res) {
        std::ostringstream json;
        json << "{\"saves\":[";
        bool first = true;
        for (const SaveFile &save : storage_.ListSaves()) {
            json << (first ? "" : ",") << "{\"card\":\"" << save.card << "\",\"name\":\"" << JsonEscape(save.name)
                 << "\",\"size\":" << save.size << ",\"mtime\":" << save.mtime << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/saves/:card/:name", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string card = req.path_params.at("card");
        const std::string name = req.path_params.at("name");
        const std::string dir  = card.size() == 1 ? Storage::CardDir(card[0]) : std::string();
        if (dir.empty() || !Storage::IsAllowedSaveName(name))
            return Fail(res, 400, "That save name is not allowed.");

        const std::string path   = storage_.PathFor(dir + "/" + name);
        const std::uint64_t size = Storage::FileSize(path);
        if (size == 0 || size > Storage::kMaxSaveBytes)
            return Fail(res, 404, "Save not found.");
        std::ifstream in(path, std::ios::binary);
        std::string body(static_cast<std::size_t>(size), '\0');
        if (!in.read(&body[0], static_cast<std::streamsize>(size)))
            return Fail(res, 500, "Could not read the save.");
        res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
        res.set_content(std::move(body), "application/octet-stream");
    });

    server.Put("/api/saves/:card/:name", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const std::string card = req.path_params.at("card");
        const std::string name = req.path_params.at("name");
        const std::string dir  = card.size() == 1 ? Storage::CardDir(card[0]) : std::string();
        if (dir.empty() || !Storage::IsAllowedSaveName(name))
            return Fail(res, 400, "Only GameCube .gci saves can be uploaded.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxSaveBytes)
            return Fail(res, 413, "Saves are never this big; refusing " + HumanBytes(expected) + ".");
        if (expected + Storage::kSafetyMarginBytes > storage_.FreeBytes())
            return Fail(res, 507, "Not enough space on the TV.");

        // While playing, the game owns its saves: stage the upload for the next launch.
        const bool setup              = setupMode_.load();
        const std::string destination = setup ? storage_.PathFor(dir + "/" + name) : storage_.IncomingDir() + "/" + dir + "/" + name;
        std::error_code ec;
        fs::create_directories(fs::path(destination).parent_path(), ec);
        const std::string partPath = destination + ".part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [](std::uint64_t) {});
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            fs::remove(partPath, ec);
            return Fail(res, 400, "The upload was interrupted.");
        }
        fs::rename(partPath, destination, ec);
        if (ec) {
            fs::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }
        MarkRestartPending();
        PublishEvent(setup ? name + " uploaded to card " + card + "." : name + " received. It is used from the next launch.");
        res.set_content(setup ? "installed" : "staged", kTextType);
    });

    // ---- Texture pack ----
    server.Post("/api/texture-pack/folder", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string drive = req.get_param_value("drive");
        if (!usb_.IsUsbMount(drive))
            return Fail(res, 400, "That USB drive is not connected.");
        const std::string error = usb_.CreatePackFolder(drive);
        if (!error.empty())
            return Fail(res, 500, error);
        usb_.InvalidatePackCounts();
        MarkRestartPending();
        PublishEvent("Created " + std::string(UsbAccess::kPackFolder) + " on the USB drive.");
        res.set_content("created", kTextType);
    });

    server.Put("/api/texture-pack/file", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const std::string drive = req.get_param_value("drive");
        const std::string path  = req.get_param_value("path");
        if (!IsAllowedTexturePath(path))
            return Fail(res, 400, "Only .dds texture files with plain names can be uploaded.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > kMaxTextureBytes)
            return Fail(res, 413, "Texture files are never this big; refusing " + HumanBytes(expected) + ".");

        const bool internal = drive == kInternal;
        if (!internal && !usb_.IsUsbMount(drive))
            return Fail(res, 400, "That USB drive is not connected.");

        const std::string packDir     = internal ? storage_.PathFor("texture_pack") : drive + "/" + UsbAccess::kPackFolder;
        const std::string destination = packDir + "/" + path;
        std::error_code ec;

        // Direct write when the app may write there; otherwise stage in the data folder and let root move it.
        const bool direct = internal || access(drive.c_str(), W_OK) == 0;
        if (!direct && !usb_.Root().Available())
            return Fail(res, 403, "The app cannot write to this USB drive and has no root access.");

        const std::string spaceOn = direct ? (internal ? storage_.Root() : drive) : storage_.Root();
        const std::uint64_t free  = storage_.FreeBytes(spaceOn);
        if (free != 0 && expected + Storage::kSafetyMarginBytes > free)
            return Fail(res, 507, "Not enough space: " + HumanBytes(free) + " free.");

        std::string writePath;
        if (direct) {
            fs::create_directories(fs::path(destination).parent_path(), ec);
            writePath = destination + ".part";
        }
        else {
            fs::create_directories(storage_.PathFor("incoming_textures"), ec);
            writePath = storage_.PathFor("incoming_textures/upload-" + std::to_string(std::hash<std::string>{}(path)) + ".part");
        }

        const std::int64_t written = StreamBodyToFile(reader, writePath, expected, [](std::uint64_t) {});
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            fs::remove(writePath, ec);
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }

        if (direct) {
            fs::rename(writePath, destination, ec);
            if (ec) {
                fs::remove(writePath, ec);
                return Fail(res, 500, "Could not move the file into place.");
            }
        }
        else {
            const std::string error = usb_.MoveWithRoot(writePath, destination);
            if (!error.empty()) {
                fs::remove(writePath, ec);
                return Fail(res, 500, error);
            }
        }
        MarkRestartPending();
        res.set_content("stored", kTextType);
    });
}

} // namespace acwebos

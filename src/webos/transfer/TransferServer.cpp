/**
 * @file TransferServer.cpp
 * @brief Implementation of the upload server (see TransferServer.h).
 *
 * HTTP API (all /api routes need the PIN, as an "X-Pin" header or ?pin=):
 *   GET  /                          the upload page (web/index.html, embedded at build time)
 *   GET  /api/status                JSON: disc, space, root, USB drives, texture pack
 *   PUT  /api/disc                  raw body = disc image (.iso/.gcm/.ciso), streamed to rom/
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
        const std::string disc      = storage_.FindDiscImage();
        const DiscInfo info         = disc.empty() ? DiscInfo{} : Storage::InspectDisc(storage_.PathFor(disc));
        const RootStatus root       = usb_.Root();

        std::ostringstream json;
        json << "{\"version\":\"" << JsonEscape(version_) << "\""
             << ",\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"disc\":{\"present\":" << JsonBool(!disc.empty()) << ",\"file\":\"" << JsonEscape(disc) << "\""
             << ",\"size\":" << (disc.empty() ? 0 : Storage::FileSize(storage_.PathFor(disc)))
             << ",\"gameId\":\"" << JsonEscape(info.gameId) << "\"}"
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
                 << ",\"packExists\":" << JsonBool(drive.packExists) << ",\"packFiles\":" << drive.packFiles << "}";
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
        if (discUploadInProgress_.exchange(true))
            return Fail(res, 409, "Another disc upload is already running.");
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> uploadGuard(
            &discUploadInProgress_, [](std::atomic<bool> *flag) { flag->store(false); });

        const std::uint64_t needed = expected + Storage::kSafetyMarginBytes;
        const std::uint64_t free   = storage_.FreeBytes();
        if (needed > free) {
            return Fail(res, 507, "Not enough space on the TV: this file needs " + HumanBytes(needed)
                                      + " (including a safety margin), " + HumanBytes(free) + " is free.");
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = expected;
        }
        PublishEvent("Receiving the disc image...");

        std::error_code ec;
        fs::create_directories(storage_.PathFor("rom"), ec);
        const std::string partPath = storage_.PathFor("rom/upload.part");
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

        // Replace any previous image so the game cannot pick an older one first.
        for (const fs::directory_entry &entry : fs::directory_iterator(storage_.PathFor("rom"), ec)) {
            const std::string name = entry.path().filename().string();
            if (name != "upload.part" && entry.is_regular_file(ec))
                fs::remove(entry.path(), ec);
        }
        const std::string destination = storage_.PathFor("rom/" + Storage::DiscFileName(info));
        fs::rename(partPath, destination, ec);
        if (ec) {
            fs::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }

        const bool setup = setupMode_.load();
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            if (setup)
                status_.discReady = true;
        }
        MarkRestartPending();
        std::string message = "Disc image installed";
        if (!info.gameId.empty())
            message += " (" + info.gameId + ")";
        if (!info.gameId.empty() && info.gameId != "GAFE01")
            message += ". Note: this port is made for the US version, GAFE01";
        PublishEvent(message + ".");
        res.set_content(setup ? "installed" : "staged", kTextType);
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

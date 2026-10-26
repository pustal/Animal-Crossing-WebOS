/**
 * @file webos_transfer.cpp
 * @brief C entry points of the upload page (see webos_transfer.h).
 *
 * The storage, USB and server objects live for the whole process. They are
 * created with new and deliberately never destroyed: the game can end the
 * process from anywhere, and static destructors stopping a server thread at
 * exit would only add ways for the exit to hang.
 */
#include "webos_transfer.h"

#include "BitmapFont.h"
#include "NetInfo.h"
#include "SetupScreen.h"
#include "Storage.h"
#include "TransferServer.h"
#include "UsbAccess.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef ACWEBOS_VERSION
#define ACWEBOS_VERSION "dev"
#endif

namespace
{

acwebos::Storage *g_storage        = nullptr;
acwebos::UsbAccess *g_usb          = nullptr;
acwebos::TransferServer *g_server  = nullptr;

} // namespace

extern "C" void webos_transfer_boot(void)
{
    g_storage = new acwebos::Storage();
    g_usb     = new acwebos::UsbAccess();

    const int applied = g_storage->ApplyPendingFiles();
    if (applied > 0)
        std::printf("[AC/webOS] applied %d file(s) received during the last session\n", applied);

    g_usb->ProbeRoot();
    g_usb->PlaceTexturePack(g_storage->Root());

    g_server = new acwebos::TransferServer(*g_storage, *g_usb, acwebos::LoadOrCreatePin(*g_storage), ACWEBOS_VERSION);
    if (g_server->Start())
        std::printf("[AC/webOS] upload page on port %u\n", static_cast<unsigned>(g_server->Port()));
    else
        std::fprintf(stderr, "[AC/webOS] the upload page could not start (no free port)\n");

    if (g_storage->FindDiscImage().empty()) {
        g_server->SetSetupMode(true);
        const acwebos::SetupResult result =
            acwebos::RunSetupScreen(*g_storage, *g_usb, g_server->Port() != 0 ? g_server : nullptr);
        if (result != acwebos::SetupResult::DiscReady) {
            std::printf("[AC/webOS] setup screen closed without a disc image; exiting\n");
            std::exit(0);
        }
    }
    g_server->SetSetupMode(false);
}

extern "C" int webos_transfer_overlay_text(char *buffer, size_t size)
{
    if (!buffer || size == 0)
        return 0;
    buffer[0] = '\0';
    if (!g_server || !g_usb)
        return 0;

    std::string text;
    int lines = 0;
    const std::string address = acwebos::FindLanAddress();
    if (g_server->Port() == 0)
        text += "Upload page: not running";
    else if (address.empty())
        text += "Upload page: connect the TV to your network";
    else
        text += "Upload page: http://" + address + ":" + std::to_string(g_server->Port()) + "   PIN " + g_server->Pin();
    ++lines;

    switch (g_usb->ActivePackSource()) {
        case acwebos::PackSource::UsbLink:
        case acwebos::PackSource::UsbMount: text += "\nTexture pack: USB drive"; break;
        case acwebos::PackSource::Internal: text += "\nTexture pack: TV storage"; break;
        default: text += "\nTexture pack: none"; break;
    }
    ++lines;

    if (g_server->Snapshot().restartPending) {
        text += "\nFiles received: restart the game to use them";
        ++lines;
    }
    text += "\nPress 1 to hide";
    ++lines;

    std::snprintf(buffer, size, "%s", text.c_str());
    return lines;
}

extern "C" const unsigned char *webos_font_glyph(char c)
{
    int index = static_cast<unsigned char>(c) - static_cast<unsigned char>(acwebos::BitmapFont::kFirstChar);
    if (index < 0 || index >= acwebos::BitmapFont::kGlyphCount)
        index = '?' - acwebos::BitmapFont::kFirstChar;
    return acwebos::BitmapFont::kGlyphs[static_cast<std::size_t>(index)].data();
}

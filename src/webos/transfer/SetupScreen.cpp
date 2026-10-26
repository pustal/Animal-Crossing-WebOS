/**
 * @file SetupScreen.cpp
 * @brief Implementation of the first-run setup screen (see SetupScreen.h).
 */
#include "SetupScreen.h"

#include "BitmapFont.h"
#include "NetInfo.h"
#include "Storage.h"
#include "TransferServer.h"
#include "UsbAccess.h"

#include <SDL.h>

#include <string>
#include <vector>

namespace acwebos
{
namespace
{

/** Logical canvas: divides 1080p and 2160p panels by whole numbers so text stays crisp. */
constexpr int kCanvasWidth  = 480;
constexpr int kCanvasHeight = 270;
constexpr int kLineHeight   = 12;
constexpr std::uint32_t kInfoRefreshMs = 3000;
constexpr std::uint32_t kStartDelayMs  = 1200;
constexpr std::uint32_t kFrameDelayMs  = 33;

/** webOS remote keys (SDL-webOS backport scancodes). */
constexpr int kScancodeWebOSBack = 482;
constexpr int kScancodeWebOSExit = 505;

constexpr Color kBackground = { 18, 40, 24, 255 };
constexpr Color kTitle      = { 255, 222, 120, 255 };
constexpr Color kText       = { 240, 240, 230, 255 };
constexpr Color kDim        = { 170, 190, 170, 255 };
constexpr Color kAccent     = { 130, 210, 255, 255 };
constexpr Color kWarning    = { 255, 140, 100, 255 };
constexpr Color kBarBack    = { 40, 70, 45, 255 };
constexpr Color kBarFill    = { 120, 210, 110, 255 };

/** Everything the screen draws, refreshed periodically. */
struct ScreenState {
    std::string address;
    std::uint64_t freeBytes = 0;
    std::vector<std::string> usbLines;
    TransferStatus transfer;
    bool starting = false;
};

/** One line per USB drive, e.g. "USB sda1: 12.3 GB free, texture pack found (1520 files)". */
std::vector<std::string> DescribeDrives(UsbAccess &usb)
{
    std::vector<std::string> lines;
    for (const UsbDrive &drive : usb.ListDrives()) {
        std::string line = "USB " + drive.label + ": ";
        line += drive.spaceKnown ? HumanBytes(drive.freeBytes) + " free" : std::string("space unknown");
        if (drive.packExists)
            line += drive.readable ? ", texture pack (" + std::to_string(drive.packFiles) + " files)" : ", texture pack found";
        lines.push_back(line);
    }
    if (lines.empty())
        lines.push_back("No USB drive found.");
    return lines;
}

/** True for the keys and buttons that leave the screen (and the app). */
bool IsExitRequest(const SDL_Event &event)
{
    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        const int scancode = static_cast<int>(event.key.keysym.scancode);
        return scancode == kScancodeWebOSBack || scancode == kScancodeWebOSExit || scancode == SDL_SCANCODE_AC_BACK
               || scancode == SDL_SCANCODE_ESCAPE;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        const int button = event.cbutton.button;
        return button == SDL_CONTROLLER_BUTTON_B || button == SDL_CONTROLLER_BUTTON_BACK || button == SDL_CONTROLLER_BUTTON_GUIDE;
    }
    return event.type == SDL_QUIT || event.type == SDL_APP_TERMINATING;
}

void DrawScreen(SDL_Renderer *renderer, const TextRenderer &text, const ScreenState &state, const UsbAccess &usb,
                const TransferServer *server)
{
    SDL_SetRenderDrawColor(renderer, kBackground.r, kBackground.g, kBackground.b, kBackground.a);
    SDL_RenderClear(renderer);

    const int center = kCanvasWidth / 2;
    int y            = 16;
    text.DrawTextCentered(center, y, "Animal Crossing", 2, kTitle);
    y += 26;

    if (state.starting) {
        text.DrawTextCentered(center, kCanvasHeight / 2, "Disc image installed. Starting...", 1, kText);
        SDL_RenderPresent(renderer);
        return;
    }

    text.DrawTextCentered(center, y, "A disc image of the game is needed (.iso, .gcm or .ciso).", 1, kText);
    y += kLineHeight;
    text.DrawTextCentered(center, y, "Use your own copy of the US release (GAFE01).", 1, kDim);
    y += kLineHeight * 2;

    if (!server || server->Port() == 0) {
        text.DrawTextCentered(center, y, "The upload service could not start.", 1, kWarning);
        y += kLineHeight;
        text.DrawTextCentered(center, y, "Restart the app to try again.", 1, kDim);
        y += kLineHeight * 2;
    }
    else if (state.address.empty()) {
        text.DrawTextCentered(center, y, "Connect the TV to your network to continue.", 1, kWarning);
        y += kLineHeight * 3;
    }
    else {
        text.DrawTextCentered(center, y, "On a phone or computer on this network, open:", 1, kText);
        y += kLineHeight + 4;
        text.DrawTextCentered(center, y, "http://" + state.address + ":" + std::to_string(server->Port()), 2, kAccent);
        y += 22;
        text.DrawTextCentered(center, y, "PIN  " + server->Pin(), 2, kTitle);
        y += 26;
    }

    text.DrawTextCentered(center, y, "Free space on the TV: " + HumanBytes(state.freeBytes), 1, kDim);
    y += kLineHeight;
    for (const std::string &line : state.usbLines) {
        text.DrawTextCentered(center, y, line, 1, kDim);
        y += kLineHeight;
    }
    const RootStatus root = usb.Root();
    text.DrawTextCentered(center, y, root.Available() ? "Root access: yes" : "Root access: no", 1, kDim);
    y += kLineHeight + 4;

    if (state.transfer.uploading && state.transfer.expectedBytes > 0) {
        constexpr int kBarWidth  = 300;
        constexpr int kBarHeight = 8;
        const int barX           = center - kBarWidth / 2;
        const int filled = static_cast<int>(state.transfer.receivedBytes * kBarWidth / state.transfer.expectedBytes);
        text.FillRect(barX, y, kBarWidth, kBarHeight, kBarBack);
        text.FillRect(barX, y, filled, kBarHeight, kBarFill);
        y += kBarHeight + 4;
        text.DrawTextCentered(center, y, HumanBytes(state.transfer.receivedBytes) + " of " + HumanBytes(state.transfer.expectedBytes), 1, kText);
    }
    else if (!state.transfer.lastEvent.empty()) {
        text.DrawTextCentered(center, y, state.transfer.lastEvent, 1, kText);
    }

    text.DrawTextCentered(center, kCanvasHeight - 18, "Press BACK to exit", 1, kDim);
    SDL_RenderPresent(renderer);
}

} // namespace

SetupResult RunSetupScreen(const Storage &storage, UsbAccess &usb, const TransferServer *server)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
        return SetupResult::Failed;

    SDL_Window *window = SDL_CreateWindow("Animal Crossing", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer) {
        if (window)
            SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
        return SetupResult::Failed;
    }
    SDL_RenderSetLogicalSize(renderer, kCanvasWidth, kCanvasHeight);
    SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i))
            SDL_GameControllerOpen(i);
    }

    TextRenderer text;
    text.Attach(renderer);

    ScreenState state;
    std::uint32_t nextInfoRefresh = 0;
    std::uint32_t startAt         = 0;
    SetupResult result            = SetupResult::Exit;
    bool running                  = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED)
                SDL_GameControllerOpen(event.cdevice.which);
            else if (IsExitRequest(event))
                running = false;
        }

        const std::uint32_t now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now, nextInfoRefresh)) {
            state.address   = FindLanAddress();
            state.freeBytes = storage.FreeBytes();
            state.usbLines  = DescribeDrives(usb);
            nextInfoRefresh = now + kInfoRefreshMs;
        }
        if (server)
            state.transfer = server->Snapshot();

        if (!state.starting && (state.transfer.discReady || !storage.FindDiscImage().empty())) {
            state.starting = true;
            startAt        = now + kStartDelayMs;
        }
        if (state.starting && SDL_TICKS_PASSED(now, startAt)) {
            result  = SetupResult::DiscReady;
            running = false;
        }

        DrawScreen(renderer, text, state, usb, server);
        SDL_Delay(kFrameDelayMs);
    }

    text.Detach();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    return result;
}

} // namespace acwebos

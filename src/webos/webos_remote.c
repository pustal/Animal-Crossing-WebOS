/**
 * @file webos_remote.c
 * @brief LG remote control support: Back opens the pause menu, and the remote's
 *        keys drive the GameCube pad.
 *
 * The port reads the keyboard through SDL_GetKeyboardState and keybindings.ini,
 * which allows one key per pad button. The remote needs aliases (OK and the
 * green key are both A), so remote keys are handled here instead: an SDL event
 * filter tracks which remote keys are held and publishes the result through
 * the port's touch-overlay inputs (g_android_touch_*), which pc_pad.c ORs into
 * the pad state every frame. A game controller keeps working as before.
 *
 * Layout (remote held normally):
 *   Arrows: walk (main stick)      OK, green: A      red: B
 *   blue: X       yellow: Y        0, Play: Start    5: Z
 *   CH-: L        CH+: R           2/8/4/6: camera (C-stick, keypad layout)
 *   Back, Exit: pause menu (Escape, as the port does for Android's Back)
 */
#include "webos_platform.h"

#include <SDL.h>
#include <stdio.h>

/* ---- webOS scancodes (SDL-webOS backport, SDL_scancode.h) ---- */
/* Spelled out so this file does not depend on the backport's enumerator names. */
enum {
    kScancodeWebOSChannelUp   = 480,
    kScancodeWebOSChannelDown = 481,
    kScancodeWebOSBack        = 482,
    kScancodeWebOSRed         = 486,
    kScancodeWebOSGreen       = 487,
    kScancodeWebOSYellow      = 488,
    kScancodeWebOSBlue        = 489,
    kScancodeWebOSExit        = 505,
};

/* ---- GameCube pad bits (dolphin/pad.h) ---- */
enum {
    kPadLeft  = 0x0001,
    kPadRight = 0x0002,
    kPadDown  = 0x0004,
    kPadUp    = 0x0008,
    kPadZ     = 0x0010,
    kPadR     = 0x0020,
    kPadL     = 0x0040,
    kPadA     = 0x0100,
    kPadB     = 0x0200,
    kPadX     = 0x0400,
    kPadY     = 0x0800,
    kPadStart = 0x1000,
};

/** Stick deflection used for digital directions (pc_pad.c's STICK_MAGNITUDE). */
static const int kStickMagnitude = 80;

/** What a remote key does. */
typedef enum {
    kActionButton,       /**< Presses GameCube button(s) in `bits`. */
    kActionStickUp,
    kActionStickDown,
    kActionStickLeft,
    kActionStickRight,
    kActionCStickUp,
    kActionCStickDown,
    kActionCStickLeft,
    kActionCStickRight,
} RemoteAction;

typedef struct {
    int scancode;        /**< int: webOS scancodes are outside the SDL_Scancode enum. */
    RemoteAction action;
    int bits;            /**< Pad bits for kActionButton. */
} RemoteMapping;

static const RemoteMapping kMappings[] = {
    { SDL_SCANCODE_UP,           kActionStickUp,     0 },
    { SDL_SCANCODE_DOWN,         kActionStickDown,   0 },
    { SDL_SCANCODE_LEFT,         kActionStickLeft,   0 },
    { SDL_SCANCODE_RIGHT,        kActionStickRight,  0 },
    { SDL_SCANCODE_RETURN,       kActionButton,      kPadA },   /* OK */
    { SDL_SCANCODE_KP_ENTER,     kActionButton,      kPadA },   /* OK on some remotes */
    { SDL_SCANCODE_SELECT,       kActionButton,      kPadA },   /* OK on some remotes */
    { kScancodeWebOSGreen,       kActionButton,      kPadA },
    { kScancodeWebOSRed,         kActionButton,      kPadB },
    { kScancodeWebOSBlue,        kActionButton,      kPadX },
    { kScancodeWebOSYellow,      kActionButton,      kPadY },
    { SDL_SCANCODE_0,            kActionButton,      kPadStart },
    { SDL_SCANCODE_KP_0,         kActionButton,      kPadStart },
    { SDL_SCANCODE_AUDIOPLAY,    kActionButton,      kPadStart },
    { SDL_SCANCODE_5,            kActionButton,      kPadZ },
    { SDL_SCANCODE_KP_5,         kActionButton,      kPadZ },
    { kScancodeWebOSChannelDown, kActionButton,      kPadL },
    { kScancodeWebOSChannelUp,   kActionButton,      kPadR },
    { SDL_SCANCODE_2,            kActionCStickUp,    0 },
    { SDL_SCANCODE_KP_2,         kActionCStickUp,    0 },
    { SDL_SCANCODE_8,            kActionCStickDown,  0 },
    { SDL_SCANCODE_KP_8,         kActionCStickDown,  0 },
    { SDL_SCANCODE_4,            kActionCStickLeft,  0 },
    { SDL_SCANCODE_KP_4,         kActionCStickLeft,  0 },
    { SDL_SCANCODE_6,            kActionCStickRight, 0 },
    { SDL_SCANCODE_KP_6,         kActionCStickRight, 0 },
};
#define MAPPING_COUNT ((int)(sizeof(kMappings) / sizeof(kMappings[0])))

/** Held state per mapping (index into kMappings). Touched only on the event thread. */
static unsigned char s_held[MAPPING_COUNT];

/* The port's touch-overlay inputs (defined in webos_glue.c, read by pc_pad.c). */
extern volatile int g_android_touch_buttons;
extern volatile int g_android_touch_stick_x, g_android_touch_stick_y;
extern volatile int g_android_touch_cstick_x, g_android_touch_cstick_y;

/** Recomputes the published pad state from the held remote keys. */
static void publish_state(void)
{
    int buttons = 0;
    int stick_x = 0, stick_y = 0, cstick_x = 0, cstick_y = 0;

    for (int i = 0; i < MAPPING_COUNT; ++i) {
        if (!s_held[i])
            continue;
        switch (kMappings[i].action) {
        case kActionButton:      buttons |= kMappings[i].bits; break;
        case kActionStickUp:     stick_y += kStickMagnitude; break;
        case kActionStickDown:   stick_y -= kStickMagnitude; break;
        case kActionStickLeft:   stick_x -= kStickMagnitude; break;
        case kActionStickRight:  stick_x += kStickMagnitude; break;
        case kActionCStickUp:    cstick_y += kStickMagnitude; break;
        case kActionCStickDown:  cstick_y -= kStickMagnitude; break;
        case kActionCStickLeft:  cstick_x -= kStickMagnitude; break;
        case kActionCStickRight: cstick_x += kStickMagnitude; break;
        }
    }

    /* Opposite keys held together cancel out; clamp keeps aliases from adding up. */
    if (stick_x > kStickMagnitude) stick_x = kStickMagnitude;
    if (stick_x < -kStickMagnitude) stick_x = -kStickMagnitude;
    if (stick_y > kStickMagnitude) stick_y = kStickMagnitude;
    if (stick_y < -kStickMagnitude) stick_y = -kStickMagnitude;

    g_android_touch_buttons  = buttons;
    g_android_touch_stick_x  = stick_x;
    g_android_touch_stick_y  = stick_y;
    g_android_touch_cstick_x = cstick_x;
    g_android_touch_cstick_y = cstick_y;
}

/** True for the keys that open the pause menu. */
static int is_pause_key(int scancode)
{
    return scancode == kScancodeWebOSBack || scancode == kScancodeWebOSExit || scancode == SDL_SCANCODE_AC_BACK;
}

/**
 * Event filter: updates the remote pad state and rewrites Back/Exit to Escape
 * (the port's pause key) and the OK aliases to Return (the pause menu's confirm
 * key). Always keeps the event.
 */
static int SDLCALL remote_event_filter(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
        return 1;

    const int scancode = (int)event->key.keysym.scancode;
    const int pressed  = event->type == SDL_KEYDOWN;

    if (is_pause_key(scancode)) {
        event->key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        event->key.keysym.sym      = SDLK_ESCAPE;
        return 1;
    }

    int changed = 0;
    for (int i = 0; i < MAPPING_COUNT; ++i) {
        if (kMappings[i].scancode == scancode && s_held[i] != pressed) {
            s_held[i] = (unsigned char)pressed;
            changed   = 1;
        }
    }
    if (changed)
        publish_state();

    if (scancode == SDL_SCANCODE_KP_ENTER || scancode == SDL_SCANCODE_SELECT) {
        event->key.keysym.scancode = SDL_SCANCODE_RETURN;
        event->key.keysym.sym      = SDLK_RETURN;
    }
    return 1;
}

void webos_remote_set_hints(void)
{
    /* Deliver Back and Exit to the app instead of letting webOS close it. */
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    /* Hide the Magic Remote pointer soon after it stops moving. */
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "1000");
}

void webos_remote_install(void)
{
    SDL_EventFilter existing = NULL;
    void *existing_data      = NULL;
    if (SDL_GetEventFilter(&existing, &existing_data)) {
        /* The port sets none today; never silently replace one if it starts to. */
        fprintf(stderr, "[AC/webOS] an SDL event filter is already set: remote mapping disabled\n");
        return;
    }
    SDL_SetEventFilter(remote_event_filter, NULL);
    printf("[AC/webOS] remote control mapping active\n");
}

/** keybindings.ini written once so the keyboard layout does not use keys the remote sends. */
static const char *const kKeybindingsFile  = "keybindings.ini";
/** Marks that the webOS layout was written; the player's own edits are kept afterwards. */
static const char *const kKeybindingsStamp = "keybindings.webos-v1";

void webos_write_keybindings_once(void)
{
    FILE *stamp = fopen(kKeybindingsStamp, "r");
    if (stamp) {
        fclose(stamp);
        return;
    }

    FILE *ini = fopen(kKeybindingsFile, "w");
    if (!ini) {
        perror("[AC/webOS] keybindings.ini");
        return;
    }
    /* Same as the PC defaults except the C-stick (arrows) and Start (Return),
     * which the remote uses for walking and A. The gamepad section is left
     * out, so the port's controller defaults apply. */
    fputs("[Keyboard]\n"
          "# webOS layout: the TV remote is handled separately (see the README).\n"
          "A = Space\nB = Left Shift\nX = X\nY = Y\nStart = P\nZ = Z\nL = Q\nR = E\n"
          "Stick_Up = W\nStick_Down = S\nStick_Left = A\nStick_Right = D\n"
          "CStick_Up = T\nCStick_Down = G\nCStick_Left = F\nCStick_Right = H\n"
          "DPad_Up = I\nDPad_Down = K\nDPad_Left = J\nDPad_Right = L\n",
          ini);
    fclose(ini);

    stamp = fopen(kKeybindingsStamp, "w");
    if (stamp)
        fclose(stamp);
    printf("[AC/webOS] wrote the webOS keyboard layout to %s\n", kKeybindingsFile);
}

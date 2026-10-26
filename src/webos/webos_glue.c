/**
 * @file webos_glue.c
 * @brief webOS platform hooks for the Animal Crossing PC port (see android_glue.h).
 *
 * - android_bootstrap(): picks the data folder, creates the port's folders,
 *   links shaders/ to the copy shipped in the app, redirects logs, installs a
 *   crash logger, turns on the frame profiler if asked, prepares the remote
 *   control (hints, keyboard layout) and starts the upload page.
 * - The Android touch overlay's input variables, which pc_pad.c reads; a TV
 *   has no touch overlay, so they stay zero.
 */
#include "android_glue.h"
#include "webos_platform.h"
#include "webos_transfer.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Touch overlay inputs (Android only; always idle on a TV) ---- */

volatile int g_android_touch_buttons  = 0;
volatile int g_android_touch_stick_x  = 0;
volatile int g_android_touch_stick_y  = 0;
volatile int g_android_touch_cstick_x = 0;
volatile int g_android_touch_cstick_y = 0;

/* ---- Bootstrap ---- */

/** Folder name used under $HOME on the TV (the app's private, persistent data folder). */
static const char *const kDataFolder = "data";
/** Used when $HOME cannot be written; lost at reboot. */
static const char *const kVolatileFallback = "/tmp/animal-crossing-webos";

/** Creates @p path if missing. @return 0 on success or if it already exists. */
static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

/** Creates every component of @p path. @return 0 on success. */
static int ensure_dirs(const char *path)
{
    char buffer[PATH_MAX];
    if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer))
        return -1;
    for (char *p = buffer + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (ensure_dir(buffer) != 0)
                return -1;
            *p = '/';
        }
    }
    return ensure_dir(buffer);
}

/**
 * Chooses and enters the data folder: $AC_DATA_DIR if set, else $HOME/data,
 * else a temporary folder. @return The folder entered.
 */
static const char *enter_data_dir(void)
{
    static char chosen[PATH_MAX];

    const char *override_dir = getenv("AC_DATA_DIR");
    if (override_dir && *override_dir && ensure_dirs(override_dir) == 0 && chdir(override_dir) == 0) {
        snprintf(chosen, sizeof(chosen), "%s", override_dir);
        return chosen;
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(chosen, sizeof(chosen), "%s/%s", home, kDataFolder);
        if (ensure_dirs(chosen) == 0 && chdir(chosen) == 0)
            return chosen;
    }

    ensure_dirs(kVolatileFallback);
    if (chdir(kVolatileFallback) != 0)
        return ".";
    snprintf(chosen, sizeof(chosen), "%s", kVolatileFallback);
    return chosen;
}

/**
 * Points ./shaders at the shaders shipped next to the executable
 * (<app>/share/shaders), so updates to the app update the shaders too.
 */
static void link_shaders(void)
{
    char exe[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0)
        return;
    exe[length] = '\0';

    /* <app>/bin/AnimalCrossing -> <app>/share/shaders */
    char *slash = strrchr(exe, '/');
    if (!slash)
        return;
    *slash = '\0';
    slash  = strrchr(exe, '/');
    if (!slash)
        return;
    *slash = '\0';

    char shipped[PATH_MAX];
    if (snprintf(shipped, sizeof(shipped), "%s/share/shaders", exe) >= (int)sizeof(shipped))
        return;

    struct stat info;
    if (stat(shipped, &info) != 0 || !S_ISDIR(info.st_mode))
        return; /* desktop build run from the build tree: shaders/ is provided there */

    /* Replace whatever is there (an old link from a previous install location). */
    unlink("shaders");
    if (symlink(shipped, "shaders") != 0)
        fprintf(stderr, "[AC/webOS] could not link shaders: %s\n", strerror(errno));
}

/* ---- Crash logging ---- */

/** Writes which fatal signal arrived to stderr.log, then lets it terminate the app as usual. */
static void on_fatal_signal(int signal_number)
{
    /* Only async-signal-safe calls here: write(), signal(), raise(). */
    static const char prefix[] = "[AC/webOS] fatal signal ";
    char number[4] = { (char)('0' + signal_number / 10 % 10), (char)('0' + signal_number % 10), '\n', '\0' };
    ssize_t ignored = write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    ignored = write(STDERR_FILENO, number, 3);
    (void)ignored;
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void install_crash_logger(void)
{
    const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i)
        signal(fatal[i], on_fatal_signal);
}

/* ---- Frame profiler switch ---- */

/** The port's frame profiler (pc_profiler.c), normally enabled with --profile. */
extern int g_pc_profile_enabled;
extern int g_pc_profile_interval;

/** Marker file in the data folder that turns the profiler on (webOS cannot pass arguments). */
static const char *const kProfileMarker = "profile.enable";
/** Report every 5 s at 60 fps: readable in stdout.log without flooding it. */
static const int kProfileInterval = 300;

static void enable_profiler_if_requested(void)
{
    struct stat info;
    if (stat(kProfileMarker, &info) != 0)
        return;
    g_pc_profile_enabled  = 1;
    g_pc_profile_interval = kProfileInterval;
    printf("[AC/webOS] frame profiler on (%s found), report every %d frames\n", kProfileMarker, kProfileInterval);
}

void android_bootstrap(void)
{
    const char *base = enter_data_dir();
    ensure_dir("rom");
    ensure_dir("save");
    ensure_dir("save/card_a");
    ensure_dir("save/card_b");
    ensure_dir("texture_pack");
    link_shaders();

    /* Logs next to the data, unbuffered so a crash still leaves them complete. */
    if (!getenv("AC_LOG_TO_CONSOLE")) {
        if (!freopen("stdout.log", "w", stdout) || !freopen("stderr.log", "w", stderr))
            perror("[AC/webOS] log redirect");
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    install_crash_logger();
    enable_profiler_if_requested();
    webos_gpu_profile_init();          /* before any shader is created */
    webos_remote_set_hints();          /* before pc_main.c calls SDL_Init */
    webos_write_keybindings_once();    /* before pc_main.c loads keybindings.ini */
    webos_transfer_boot();             /* upload page; waits on the setup screen without a disc image */

    printf("[AC/webOS] bootstrap done (cwd=%s)\n", base);
}

/**
 * @file webos_glue.c
 * @brief webOS platform hooks for the Animal Crossing PC port (see android_glue.h).
 *
 * - android_bootstrap(): picks the data folder, creates the port's folders,
 *   links shaders/ to the copy shipped in the app, and redirects logs.
 * - ac_glShaderSource(): rewrites the port's desktop GLSL 3.30 shaders to
 *   GLSL ES 3.00 before compiling them.
 * - The Android touch overlay's input variables, which pc_pad.c reads; a TV
 *   has no touch overlay, so they stay zero.
 */
#include "android_glue.h"

#include <errno.h>
#include <limits.h>
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

    printf("[AC/webOS] bootstrap done (cwd=%s)\n", base);
}

/* ---- GLSL 3.30 -> GLSL ES 3.00 ---- */

/** Header that replaces the "#version 330 core" line. */
static const char kEsHeader[] = "#version 300 es\n"
                                "precision highp float;\n"
                                "precision highp int;\n"
                                "precision highp sampler2D;\n";

/**
 * Returns a malloc'd copy of @p source with its #version line replaced by
 * kEsHeader (or kEsHeader prepended if there is none).
 */
static char *rewrite_glsl(const char *source, size_t size)
{
    size_t skip = 0;
    if (size >= 8 && strncmp(source, "#version", 8) == 0) {
        const char *newline = memchr(source, '\n', size);
        skip = newline ? (size_t)(newline - source) + 1 : size;
    }

    const size_t header = sizeof(kEsHeader) - 1;
    char *out = (char *)malloc(header + (size - skip) + 1);
    if (!out)
        return NULL;
    memcpy(out, kEsHeader, header);
    memcpy(out + header, source + skip, size - skip);
    out[header + size - skip] = '\0';
    return out;
}

int ac_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
    /* Join the pieces first: the #version line must be the very first line of the whole source. */
    size_t total = 0;
    for (GLsizei i = 0; i < count; ++i)
        total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);

    char *joined = (char *)malloc(total + 1);
    if (!joined)
        return 0;
    size_t at = 0;
    for (GLsizei i = 0; i < count; ++i) {
        const size_t piece = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(joined + at, string[i], piece);
        at += piece;
    }
    joined[total] = '\0';

    /* The port always puts "#version" first, specialised variants included (pc_gx_tev.c). */
    char *rewritten = rewrite_glsl(joined, total);
    free(joined);
    if (!rewritten)
        return 0;

    const GLchar *sources[1] = { rewritten };
    glShaderSource(shader, 1, sources, NULL); /* the real function: the macro is not in effect here */
    free(rewritten);
    return 1;
}

/**
 * @file webos_shader_cache.c
 * @brief GLSL ES rewrite, deferred shader compiles and an on-disk cache of
 *        linked shader programs (OpenGL ES 3 program binaries).
 *
 * At boot the port compiles ~140 shader variants. On the TV's Mali driver
 * each takes 150-230 ms, about 30 s in total (the ES3-WebOS-probe measured
 * reloading a saved program binary at under 1 ms). So:
 *
 * - glShaderSource: the port's desktop GLSL 3.30 is rewritten to GLSL ES 3.00
 *   and the source's hash is remembered per shader object.
 * - glCompileShader is deferred: the shader is only marked pending, and
 *   glGetShaderiv(GL_COMPILE_STATUS) reports success for pending shaders.
 * - glLinkProgram first looks for shader_binaries/<key>.bin, where the key
 *   hashes the attached shaders' sources and the GL driver identity. On a hit
 *   glProgramBinary loads it and nothing is compiled. On a miss (or if the
 *   driver rejects the binary) the pending shaders are compiled, the program
 *   is linked normally and its binary saved for the next launch.
 *
 * A compile error therefore shows up as a link error (still logged by the
 * port) instead of a compile error. glad/gl.h routes the port's calls here.
 */
#include "webos_platform.h"

#include <GLES3/gl3.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** Folder (in the data folder) holding one file per cached program. */
static const char *const kCacheDir = "shader_binaries";
/** File header magic, "ACPB". */
static const uint32_t kCacheMagic = 0x41435042u;
/** Bump when the rewrite or key changes so old binaries are not reused. */
static const uint32_t kCacheVersion = 1;

/** Header that replaces the "#version 330 core" line. */
static const char kEsHeader[] = "#version 300 es\n"
                                "precision highp float;\n"
                                "precision highp int;\n"
                                "precision highp sampler2D;\n";

/* ---- Shader bookkeeping ---- */

typedef struct {
    GLuint id;            /**< 0 = free slot. */
    uint64_t source_hash; /**< FNV-1a of the rewritten source. */
    int pending;          /**< Source set but glCompileShader deferred. */
    char *source;         /**< Rewritten source, kept only in the GPU timing mode. */
} ShaderEntry;

/** The port keeps one vertex shader and deletes fragment shaders after linking, so few are live. */
#define SHADER_TABLE_SIZE 256
static ShaderEntry s_shaders[SHADER_TABLE_SIZE];

static ShaderEntry *find_shader(GLuint id)
{
    for (int i = 0; i < SHADER_TABLE_SIZE; ++i)
        if (s_shaders[i].id == id)
            return &s_shaders[i];
    return NULL;
}

static ShaderEntry *track_shader(GLuint id)
{
    ShaderEntry *entry = find_shader(id);
    if (!entry)
        entry = find_shader(0);
    if (entry)
        entry->id = id;
    return entry; /* NULL when full: that shader simply bypasses the cache */
}

static uint64_t fnv1a(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

static const uint64_t kFnvOffset = 0xcbf29ce484222325ull;

/** Hash of the GL driver identity: binaries are only valid for the driver that made them. */
static uint64_t driver_hash(void)
{
    static uint64_t hash = 0;
    if (hash == 0) {
        const char *strings[3] = { (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
                                   (const char *)glGetString(GL_VERSION) };
        hash = kFnvOffset;
        for (int i = 0; i < 3; ++i)
            if (strings[i])
                hash = fnv1a(hash, strings[i], strlen(strings[i]) + 1);
        hash = fnv1a(hash, &kCacheVersion, sizeof(kCacheVersion));
    }
    return hash;
}

/** Compiles @p entry's shader now if it was deferred. */
static void compile_if_pending(ShaderEntry *entry)
{
    if (!entry || !entry->pending)
        return;
    entry->pending = 0;
    glCompileShader(entry->id);

    GLint ok = GL_FALSE;
    glGetShaderiv(entry->id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(entry->id, sizeof(log), NULL, log);
        fprintf(stderr, "[AC/webOS] shader compile error: %s\n", log);
    }
}

/* ---- GL entry points used by the port (via glad/gl.h) ---- */

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
    glShaderSource(shader, 1, sources, NULL);

    ShaderEntry *entry = track_shader(shader);
    if (entry) {
        entry->source_hash = fnv1a(kFnvOffset, rewritten, strlen(rewritten));
        entry->pending     = 0;
        free(entry->source);
        entry->source = NULL;
        if (webos_gpu_profile_active()) {
            entry->source = rewritten; /* kept for webos_shader_cache_fragment_source() */
            return 1;
        }
    }
    free(rewritten);
    return 1;
}

void ac_glCompileShader(GLuint shader)
{
    ShaderEntry *entry = find_shader(shader);
    if (entry)
        entry->pending = 1; /* compiled at link time, and only if no cached binary fits */
    else
        glCompileShader(shader);
}

void ac_glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    ShaderEntry *entry = find_shader(shader);
    if (entry && entry->pending) {
        if (pname == GL_COMPILE_STATUS) {
            *params = GL_TRUE; /* real errors surface at link time */
            return;
        }
        compile_if_pending(entry); /* anything else (e.g. the info log) needs the real compile */
    }
    glGetShaderiv(shader, pname, params);
}

void ac_glDeleteShader(GLuint shader)
{
    ShaderEntry *entry = find_shader(shader);
    if (entry) {
        free(entry->source);
        memset(entry, 0, sizeof(*entry));
    }
    glDeleteShader(shader);
}

/* ---- Program binary cache ---- */

/**
 * Builds the cache key for @p program from its attached shaders.
 * @return 0 if any attached shader is unknown (then the program is not cached).
 */
static int program_key(GLuint program, uint64_t *key, ShaderEntry **attached, GLsizei *attached_count)
{
    GLuint ids[4];
    GLsizei count = 0;
    glGetAttachedShaders(program, 4, &count, ids);
    *attached_count = count;

    uint64_t hash = driver_hash();
    int known     = count > 0;
    for (GLsizei i = 0; i < count; ++i) {
        attached[i] = find_shader(ids[i]);
        if (!attached[i]) {
            known = 0;
            continue;
        }
        hash = fnv1a(hash, &attached[i]->source_hash, sizeof(attached[i]->source_hash));
    }
    *key = hash;
    return known;
}

static void cache_path(uint64_t key, char *out, size_t size)
{
    snprintf(out, size, "%s/%016llx.bin", kCacheDir, (unsigned long long)key);
}

/** @return 1 if a cached binary for @p key was loaded and linked into @p program. */
static int load_binary(GLuint program, uint64_t key)
{
    char path[64];
    cache_path(key, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;

    uint32_t header[4] = { 0, 0, 0, 0 }; /* magic, version, format, length */
    int ok = fread(header, sizeof(header), 1, file) == 1 && header[0] == kCacheMagic && header[1] == kCacheVersion
             && header[3] > 0 && header[3] < (16u << 20);
    void *data = ok ? malloc(header[3]) : NULL;
    ok = ok && data && fread(data, header[3], 1, file) == 1;
    fclose(file);

    if (ok) {
        glProgramBinary(program, (GLenum)header[2], data, (GLsizei)header[3]);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        ok = linked == GL_TRUE;
    }
    free(data);
    if (!ok)
        remove(path); /* stale or damaged: rebuilt below */
    return ok;
}

static void save_binary(GLuint program, uint64_t key)
{
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0)
        return;
    void *data = malloc((size_t)length);
    if (!data)
        return;

    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary(program, length, &written, &format, data);
    if (written > 0 && (mkdir(kCacheDir, 0755) == 0 || errno == EEXIST)) {
        char path[64], temp[72];
        cache_path(key, path, sizeof(path));
        snprintf(temp, sizeof(temp), "%s.tmp", path);
        FILE *file = fopen(temp, "wb");
        if (file) {
            const uint32_t header[4] = { kCacheMagic, kCacheVersion, (uint32_t)format, (uint32_t)written };
            int ok = fwrite(header, sizeof(header), 1, file) == 1 && fwrite(data, (size_t)written, 1, file) == 1;
            ok = fclose(file) == 0 && ok;
            /* Written under a temporary name first, so a crash never leaves a torn binary. */
            if (!ok || rename(temp, path) != 0)
                remove(temp);
        }
    }
    free(data);
}

/* ---- Fragment sources per program (GPU timing mode only) ---- */

typedef struct {
    GLuint program;       /**< 0 = free slot. */
    char *fragment;       /**< Copy of the fragment shader source. */
} ProgramSource;

#define PROGRAM_SOURCE_SLOTS 512
static ProgramSource s_program_sources[PROGRAM_SOURCE_SLOTS];

/** Remembers the fragment source linked into @p program. */
static void remember_fragment_source(GLuint program, ShaderEntry *const *attached, GLsizei count)
{
    for (GLsizei i = 0; i < count; ++i) {
        if (!attached[i] || !attached[i]->source)
            continue;
        GLint type = 0;
        glGetShaderiv(attached[i]->id, GL_SHADER_TYPE, &type);
        if (type != GL_FRAGMENT_SHADER)
            continue;

        ProgramSource *slot = NULL;
        for (int s = 0; s < PROGRAM_SOURCE_SLOTS && !slot; ++s)
            if (s_program_sources[s].program == program)
                slot = &s_program_sources[s];
        for (int s = 0; s < PROGRAM_SOURCE_SLOTS && !slot; ++s)
            if (s_program_sources[s].program == 0)
                slot = &s_program_sources[s];
        if (!slot)
            return;
        free(slot->fragment);
        slot->program  = program;
        slot->fragment = strdup(attached[i]->source);
        return;
    }
}

const char *webos_shader_cache_fragment_source(unsigned int program)
{
    for (int s = 0; s < PROGRAM_SOURCE_SLOTS; ++s)
        if (s_program_sources[s].program == program)
            return s_program_sources[s].fragment;
    return NULL;
}

void ac_glLinkProgram(GLuint program)
{
    uint64_t key = 0;
    ShaderEntry *attached[4] = { NULL, NULL, NULL, NULL };
    GLsizei count = 0;
    const int cacheable = program_key(program, &key, attached, &count);
    if (webos_gpu_profile_active())
        remember_fragment_source(program, attached, count);

    if (cacheable && load_binary(program, key))
        return;

    webos_show_boot_activity(); /* this is the slow path: show the app is alive while booting */
    for (GLsizei i = 0; i < count; ++i)
        compile_if_pending(attached[i]);

    if (cacheable)
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (cacheable && linked == GL_TRUE)
        save_binary(program, key);
}

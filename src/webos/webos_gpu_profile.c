/**
 * @file webos_gpu_profile.c
 * @brief Diagnostic GPU timing, on only when the data folder holds gpuprofile.enable.
 *
 * The port's frame profiler measures CPU time. On the TV's tile-based Mali GPU
 * the GPU work is deferred, so a slow GPU frame shows up as a long wait at the
 * next frame's first GL call and cannot be attributed. In this mode every
 * heavy GL call the port makes (draws, clears, texture and buffer uploads,
 * framebuffer reads) is followed by glFinish() and timed, so each call's cost
 * includes the GPU work it caused. Draw time is totalled per shader program.
 *
 * glFinish after every call is far slower than normal rendering and adds a
 * fixed cost per call (on a tiler each finish writes the frame out to memory),
 * so the numbers are for comparing calls with each other, not absolute
 * frame times. The report goes to stdout.log every kReportFrames frames, and
 * the fragment shaders of the most expensive programs are written to
 * gpu_hot_<rank>.frag in the data folder.
 *
 * When the mode is off each wrapper is a single branch and a direct call.
 */
#include "webos_platform.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/** Marker file that turns this mode on. */
static const char *const kMarker = "gpuprofile.enable";
/** Frames per report. */
static const unsigned long kReportFrames = 300;
/** Number of programs listed (and dumped) per report. */
#define HOT_COUNT 6

static int s_active = 0;

/* ---- Accumulators ---- */

typedef enum {
    kCallClear,
    kCallTexImage,
    kCallBufferData,
    kCallReadPixels,
    kCallOpaquePass,
    kCallSwap,
    kCallCount
} CallKind;

static const char *const kCallNames[kCallCount] = { "clear", "tex_image", "buffer_data", "read_pixels",
                                                    "opaque_pass", "swap" };

typedef struct {
    unsigned long calls;
    double ms;
    double max_ms;
} Timing;

typedef struct {
    GLuint program;       /**< 0 = free slot. */
    Timing timing;
    unsigned long vertices;
} ProgramTiming;

#define PROGRAM_SLOTS 512
static ProgramTiming s_programs[PROGRAM_SLOTS];
static Timing s_calls[kCallCount];
static Timing s_draws_total;
static double s_finish_baseline_ms = 0.0;
static unsigned long s_frames = 0;

static double now_ms(void)
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void add(Timing *timing, double ms)
{
    timing->calls++;
    timing->ms += ms;
    if (ms > timing->max_ms)
        timing->max_ms = ms;
}

static ProgramTiming *program_slot(GLuint program)
{
    ProgramTiming *free_slot = NULL;
    for (int i = 0; i < PROGRAM_SLOTS; ++i) {
        if (s_programs[i].program == program)
            return &s_programs[i];
        if (!free_slot && s_programs[i].program == 0)
            free_slot = &s_programs[i];
    }
    if (free_slot)
        free_slot->program = program;
    return free_slot;
}

/* ---- Public switches ---- */

void webos_gpu_profile_init(void)
{
    struct stat info;
    s_active = stat(kMarker, &info) == 0;
    if (s_active)
        printf("[AC/webOS] GPU timing mode on (%s found): rendering will be slow\n", kMarker);
}

int webos_gpu_profile_active(void)
{
    return s_active;
}

double webos_gpu_profile_begin(void)
{
    glFinish(); /* finish earlier work so it is not charged to the next call */
    return now_ms();
}

void webos_gpu_profile_end_opaque_pass(double start)
{
    glFinish();
    add(&s_calls[kCallOpaquePass], now_ms() - start);
}

void webos_gpu_profile_end_swap(double start)
{
    add(&s_calls[kCallSwap], now_ms() - start);
}

/* ---- Report ---- */

static void print_timing(const char *name, const Timing *timing, unsigned long frames)
{
    if (timing->calls == 0)
        return;
    printf("[GPU] %-12s %7.2f ms/frame  %6.1f calls/frame  avg %.3f ms  max %.2f ms\n", name, timing->ms / frames,
           (double)timing->calls / frames, timing->ms / timing->calls, timing->max_ms);
}

void webos_gpu_profile_frame_done(void)
{
    if (!s_active)
        return;
    if (++s_frames < kReportFrames)
        return;

    /* Cost of a glFinish with nothing to do: subtract mentally from each call's average. */
    glFinish();
    const double baseline_start = now_ms();
    glFinish();
    s_finish_baseline_ms = now_ms() - baseline_start;

    printf("[GPU] ---- %lu frames, empty glFinish %.3f ms ----\n", s_frames, s_finish_baseline_ms);
    print_timing("draws", &s_draws_total, s_frames);
    for (int i = 0; i < kCallCount; ++i)
        print_timing(kCallNames[i], &s_calls[i], s_frames);

    /* Most expensive programs by total draw time. */
    for (int rank = 0; rank < HOT_COUNT; ++rank) {
        ProgramTiming *best = NULL;
        for (int i = 0; i < PROGRAM_SLOTS; ++i) {
            ProgramTiming *slot = &s_programs[i];
            if (slot->program != 0 && slot->timing.calls > 0 && (!best || slot->timing.ms > best->timing.ms))
                best = slot;
        }
        if (!best)
            break;
        printf("[GPU] hot #%d program %u: %.2f ms/frame, %.1f draws/frame, avg %.3f ms, max %.2f ms, %lu verts/frame\n",
               rank + 1, best->program, best->timing.ms / s_frames, (double)best->timing.calls / s_frames,
               best->timing.ms / best->timing.calls, best->timing.max_ms, best->vertices / s_frames);

        const char *source = webos_shader_cache_fragment_source(best->program);
        if (source) {
            char path[32];
            snprintf(path, sizeof(path), "gpu_hot_%d.frag", rank + 1);
            FILE *file = fopen(path, "w");
            if (file) {
                fprintf(file, "// program %u: %.2f ms/frame over %lu frames\n", best->program,
                        best->timing.ms / s_frames, s_frames);
                fputs(source, file);
                fclose(file);
            }
        }
        best->timing.calls = 0; /* exclude from the next rank; everything is reset below anyway */
    }

    memset(s_programs, 0, sizeof(s_programs));
    memset(s_calls, 0, sizeof(s_calls));
    memset(&s_draws_total, 0, sizeof(s_draws_total));
    s_frames = 0;
}

/* ---- Wrapped GL calls (routed here by glad/gl.h) ---- */

/** Charges a finished draw to the current program. */
static void end_draw(double start, GLsizei vertices)
{
    glFinish();
    const double ms = now_ms() - start;
    add(&s_draws_total, ms);

    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    ProgramTiming *slot = program_slot((GLuint)program);
    if (slot) {
        add(&slot->timing, ms);
        slot->vertices += (unsigned long)(vertices > 0 ? vertices : 0);
    }
}

void ac_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (!s_active) {
        glDrawArrays(mode, first, count);
        return;
    }
    const double start = webos_gpu_profile_begin();
    glDrawArrays(mode, first, count);
    end_draw(start, count);
}

void ac_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (!s_active) {
        glDrawElements(mode, count, type, indices);
        return;
    }
    const double start = webos_gpu_profile_begin();
    glDrawElements(mode, count, type, indices);
    end_draw(start, count);
}

/** Times @p kind around a call that is already made between begin and here. */
static void end_call(CallKind kind, double start)
{
    glFinish();
    add(&s_calls[kind], now_ms() - start);
}

void ac_glClear(GLbitfield mask)
{
    if (!s_active) {
        glClear(mask);
        return;
    }
    const double start = webos_gpu_profile_begin();
    glClear(mask);
    end_call(kCallClear, start);
}

void ac_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const void *pixels)
{
    if (!s_active) {
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        return;
    }
    const double start = webos_gpu_profile_begin();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    end_call(kCallTexImage, start);
}

void ac_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    if (!s_active) {
        glBufferData(target, size, data, usage);
        return;
    }
    const double start = webos_gpu_profile_begin();
    glBufferData(target, size, data, usage);
    end_call(kCallBufferData, start);
}

void ac_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
    if (!s_active) {
        glReadPixels(x, y, width, height, format, type, pixels);
        return;
    }
    const double start = webos_gpu_profile_begin();
    glReadPixels(x, y, width, height, format, type, pixels);
    end_call(kCallReadPixels, start);
}

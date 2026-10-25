/**
 * @file webos_forced_decls.h
 * @brief Forced into every translation unit (-include, see CMakeLists.txt).
 *
 * The webOS counterpart of the Android port's android_ansi_decls.h. It
 * reconciles the decomp's own libc declarations with the TV's glibc:
 *
 * - The C library headers are included first, so their declarations are the
 *   first ones the compiler sees. The decomp's later re-declarations (_mem.h's
 *   memcpy/memset) are then accepted, where glibc's noexcept versions coming
 *   second are a hard error in C++.
 * - libultra.h declares bcmp/bcopy/bzero with GameCube types (u32 is
 *   "unsigned long" here, glibc uses size_t / const pointers), which conflicts
 *   with glibc's <strings.h>. Renaming them to ac_* makes the decomp's
 *   declarations, its calls and our definitions (webos_bstring.c) refer to
 *   private functions that do not clash with glibc. pc_stubs.c's bcmp becomes
 *   ac_bcmp in the same way.
 * - Fills in a few things the TV's (older) headers lack.
 */
#ifndef ACWEBOS_FORCED_DECLS_H
#define ACWEBOS_FORCED_DECLS_H

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* See the file comment: keep the decomp's bstring functions away from glibc's. */
#define bcmp ac_bcmp
#define bcopy ac_bcopy
#define bzero ac_bzero

/* CLOCK_BOOTTIME (Linux 2.6.39+) is supported by the TV's kernel, but older
 * libc headers do not name it. The port uses it under TARGET_ANDROID so the
 * game clock keeps running while the TV is in standby. */
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif

#endif /* ACWEBOS_FORCED_DECLS_H */

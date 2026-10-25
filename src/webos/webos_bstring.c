/**
 * @file webos_bstring.c
 * @brief bcopy/bzero for the decomp, under their private ac_* names.
 *
 * webos_forced_decls.h renames the decomp's bcmp/bcopy/bzero to ac_bcmp,
 * ac_bcopy and ac_bzero (glibc's own declarations have different types). The
 * port's pc_stubs.c already defines bcmp (renamed to ac_bcmp); the other two
 * are only defined there for Windows, so they are provided here.
 *
 * The parameter types follow libultra.h; on the 32-bit TV u32 and size_t are
 * both 32-bit unsigned, so the calling convention is the same.
 */
#include <string.h>

void bcopy(void *src, void *dst, size_t n)
{
    memmove(dst, src, n);
}

void bzero(void *ptr, size_t size)
{
    memset(ptr, 0, size);
}

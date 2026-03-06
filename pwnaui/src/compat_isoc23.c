/*
 * compat_isoc23.c - Compatibility shims for glibc header/runtime mismatch
 *
 * libc6-dev 2.42 headers redirect strtol/sscanf/fscanf to __isoc23_* variants,
 * but runtime glibc 2.36 doesn't provide them. We compile this file WITHOUT
 * _GNU_SOURCE so the redirects don't activate, allowing us to call the real
 * libc functions directly.
 */

/* DO NOT define _GNU_SOURCE here — we need the un-redirected function names */
#undef _GNU_SOURCE
#undef __USE_GNU
#undef __USE_ISOC23

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/*
 * These wrapper functions have __isoc23_* names (what the redirected code calls)
 * but their bodies call the REAL strtol/vfscanf/vsscanf (not redirected, because
 * we didn't define _GNU_SOURCE).
 */

long int __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

int __isoc23_fscanf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfscanf(stream, format, ap);
    va_end(ap);
    return ret;
}

int __isoc23_sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsscanf(str, format, ap);
    va_end(ap);
    return ret;
}

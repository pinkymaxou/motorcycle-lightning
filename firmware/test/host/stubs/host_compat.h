/* Force-included when the host tests compile firmware sources that expect an
 * ESP-IDF-ish libc. Two gaps only: strlcpy (glibc has it since 2.38, the
 * firmware gets it from newlib) and the logging macros. */
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

static inline size_t hostStrlcpy(char* const dst, const char* const src,
                                 const size_t cap)
{
    const size_t len = std::strlen(src);
    if (0 != cap)
    {
        const size_t n = (len < cap - 1) ? len : cap - 1;
        std::memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#define strlcpy hostStrlcpy

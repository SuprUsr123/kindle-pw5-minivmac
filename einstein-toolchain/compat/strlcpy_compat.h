/* Force-included (-include) on every translation unit for this
 * cross-compile target: the device's glibc 2.20 (2014) predates
 * strlcpy/strlcat (added upstream only in glibc 2.38, 2023), so
 * <string.h> declares neither. Definitions are in strlcpy_shim.c,
 * linked into every executable via the toolchain file.
 */
#ifndef EINSTEIN_STRLCPY_COMPAT_H
#define EINSTEIN_STRLCPY_COMPAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t strlcpy(char *dst, const char *src, size_t dstsize);
size_t strlcat(char *dst, const char *src, size_t dstsize);

#ifdef __cplusplus
}
#endif

#endif

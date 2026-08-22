/* Shim for the Kindle Oasis's glibc 2.20 (2014), which predates
 * strlcpy/strlcat (added to glibc only in 2.38, 2023). BSD-standard
 * semantics: always NUL-terminates (unlike strncpy), returns the
 * source length (unlike strncpy's return value).
 */
#include <string.h>

size_t strlcpy(char *dst, const char *src, size_t dstsize)
{
	size_t srclen = strlen(src);
	if (dstsize != 0) {
		size_t copylen = (srclen < dstsize - 1) ? srclen : dstsize - 1;
		memcpy(dst, src, copylen);
		dst[copylen] = '\0';
	}
	return srclen;
}

size_t strlcat(char *dst, const char *src, size_t dstsize)
{
	size_t dstlen = strnlen(dst, dstsize);
	if (dstlen == dstsize)
		return dstlen + strlen(src);
	return dstlen + strlcpy(dst + dstlen, src, dstsize - dstlen);
}

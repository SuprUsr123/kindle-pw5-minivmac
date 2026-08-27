/* compat-glibc.c -- bridge glibc >= 2.33 header symbols down to the
 * device's glibc 2.20 shared libc.
 *
 * The device (PW5, glibc 2.20) predates the glibc 2.33 change that made
 * stat()/fstat()/lstat()/mknod() real exported symbols. In 2.20 those
 * live in libc_nonshared.a as stubs calling the __xstat/__xmknod
 * internal interface. A binary compiled against modern (2.33+) headers
 * emits calls to `stat` which the device's libc.so.6 does not export,
 * so this file provides the missing glue. Same pattern as the
 * einstein-toolchain compat shims.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>

/* glibc 2.20-era internal stat interface, exported by the device's
 * libc.so.6 as __xstat@GLIBC_2.4 etc. but no longer declared in
 * glibc >= 2.33 headers. ARM EABI never bumped _STAT_VER/_MKNOD_VER. */
#define _STAT_VER 0
#define _MKNOD_VER 0

extern int __xstat(int, const char *, struct stat *);
extern int __fxstat(int, int, struct stat *);
extern int __lxstat(int, const char *, struct stat *);
extern int __xmknod(int, const char *, mode_t, dev_t *);
extern int __xstat64(int, const char *, struct stat64 *);
extern int __fxstat64(int, int, struct stat64 *);
extern int __lxstat64(int, const char *, struct stat64 *);
extern int __xmknod64(int, const char *, mode_t, dev_t *);

int stat(const char *path, struct stat *buf)
{
	return __xstat(_STAT_VER, path, buf);
}

int fstat(int fd, struct stat *buf)
{
	return __fxstat(_STAT_VER, fd, buf);
}

int lstat(const char *path, struct stat *buf)
{
	return __lxstat(_STAT_VER, path, buf);
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
	return __xmknod(_MKNOD_VER, path, mode, &dev);
}

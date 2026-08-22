/* Shim for the Kindle Oasis's glibc 2.20 (2014), which predates
 * getentropy() (added in glibc 2.25, 2017). The Homebrew
 * arm-unknown-linux-gnueabi cross-toolchain's libstdc++ unconditionally
 * references getentropy() from std::random_device -- present in the
 * archive's random.o, and unconditionally NEEDED once anything (even
 * <iostream>'s static init object) pulls that translation unit in.
 *
 * The device kernel (Linux 3.0.35-lab126) also predates getrandom(2)
 * (added 3.17), so there's no real syscall path either -- fall back to
 * /dev/urandom, which is present on any Linux kernel and is exactly
 * what glibc's own getentropy() falls back to when getrandom() isn't
 * available.
 */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int getentropy(void *buffer, size_t length)
{
	if (length > 256) {
		errno = EIO;
		return -1;
	}

	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;

	unsigned char *p = buffer;
	size_t got = 0;
	while (got < length) {
		ssize_t n = read(fd, p + got, length - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	return (got == length) ? 0 : -1;
}

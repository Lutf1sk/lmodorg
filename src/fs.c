#include <lt/mem.h>

#include <unistd.h>
#include <sys/file.h>

#define alloc lt_libc_heap

int copyat(int from_fd, char* from_path, int to_fd, char* to_path) {
	int infd = openat(from_fd, from_path, O_RDONLY, 0);
	if (infd < 0)
		return -1;

	int outfd = openat(to_fd, to_path, O_WRONLY|O_CREAT|O_TRUNC, 0666);
 	if (outfd < 0)
		return -1;

	usz copy_bufsz = LT_KB(64);
	char* copy_buf = lt_malloc(alloc, copy_bufsz);

	isz res;
	while ((res = read(infd, copy_buf, copy_bufsz))) {
		if (res < 0)
			return -1;
		res = write(outfd, copy_buf, res);
		if (res < 0)
			return -1;
	}

	close(infd);
	close(outfd);

	lt_mfree(alloc, copy_buf);

	return 0;
}

#include <lt/lt.h>
#include <lt/debug.h>
#include <lt/str.h>
#include <lt/mem.h>

#define alloc lt_libc_heap

#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

static
char* path_parent(char* path, b8* out_last, usz* out_len) {
	char* it = path;
	while (*it && *it != '/')
		++it;
	*out_len = it - path;

	LT_ASSERT(*out_len != 0);

	while (*it && *it == '/')
		++it;
	*out_last = *it == 0;

	return it;
}

int rebuild_path_case_at(int fd, char* path) {
	b8 last;
	lstr_t name = LSTR(path, 0);
	char* child_path = path_parent(path, &last, &name.len);

	DIR* d = fdopendir(dup(fd));
	rewinddir(d);
	LT_ASSERT(d != NULL);

	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (!lt_lseq_nocase(name, lt_lsfroms(ent->d_name)))
			continue;

		memcpy(name.str, ent->d_name, name.len);

		if (last) {
			closedir(d);
			return 0;
		}

		int dfd = openat(fd, ent->d_name, O_RDONLY|O_DIRECTORY);
		if (dfd < 0) {
			closedir(d);
			return -errno;
		}

		int res = rebuild_path_case_at(dfd, child_path);
		close(dfd);
		closedir(d);
		return res;
	}
	closedir(d);
	return -ENOENT;
}

int rebuild_path_case(char* path) {
	int fd = open(".", O_RDONLY|O_DIRECTORY);
	if (fd < 0)
		return -1;
	int res = rebuild_path_case_at(fd, path);
	close(fd);
	return res;
}

static
int openat_errno(int fd, char* path, int flags, mode_t mode) {
	int rfd = openat(fd, path, flags, mode);
	if (rfd < 0)
		return -errno;
	return rfd;
}

int openat_nocase(int fd, char* path, int flags, mode_t mode) {
	b8 last;
	lstr_t name = LSTR(path, 0);
	char* child_path = path_parent(path, &last, &name.len);

	DIR* d = fdopendir(dup(fd));
	rewinddir(d);
	LT_ASSERT(d != NULL);

	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (!lt_lseq_nocase(name, lt_lsfroms(ent->d_name)))
			continue;

		if (last) {
			closedir(d);
			return openat_errno(fd, ent->d_name, flags, mode);
		}

		int dfd = openat(fd, ent->d_name, O_RDONLY|O_DIRECTORY);
		if (dfd < 0) {
			closedir(d);
			return -errno;
		}

		int newfd = openat_nocase(dfd, child_path, flags, mode);
		close(dfd);
		closedir(d);
		return newfd;
	}
	closedir(d);

	if (last && (flags & O_CREAT))
		return openat_errno(fd, path, flags, mode);
	return -ENOENT;
}

int fstatat_nocase(int fd, char* path, struct stat* st, int flags) {
	b8 last;
	lstr_t name = LSTR(path, 0);
	char* child_path = path_parent(path, &last, &name.len);

	DIR* d = fdopendir(dup(fd));
	rewinddir(d);
	LT_ASSERT(d != NULL);

	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (!lt_lseq_nocase(name, lt_lsfroms(ent->d_name)))
			continue;

		if (last) {
			closedir(d);
			int res = fstatat(fd, ent->d_name, st, flags);
			if (res < 0)
				return -errno;
			return res;
		}

		int dfd = openat(fd, ent->d_name, O_RDONLY|O_DIRECTORY);
		if (dfd < 0) {
			closedir(d);
			return -errno;
		}

		int res = fstatat_nocase(dfd, child_path, st, flags);
		close(dfd);
		closedir(d);
		return res;
	}
	closedir(d);
	return -ENOENT;
}

int unlinkat_nocase(int fd, char* path, int flags) {
	b8 last;
	lstr_t name = LSTR(path, 0);
	char* child_path = path_parent(path, &last, &name.len);

	DIR* d = fdopendir(dup(fd));
	rewinddir(d);
	LT_ASSERT(d != NULL);

	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (!lt_lseq_nocase(name, lt_lsfroms(ent->d_name)))
			continue;

		if (last) {
			closedir(d);
			int res = unlinkat(fd, ent->d_name, flags);
			if (res < 0)
				return -errno;
			return res;
		}

		int dfd = openat(fd, ent->d_name, O_RDONLY|O_DIRECTORY);
		if (dfd < 0) {
			closedir(d);
			return -errno;
		}

		int res = unlinkat_nocase(dfd, child_path, flags);
		close(dfd);
		closedir(d);
		return res;
	}
	closedir(d);
	return -ENOENT;
}

static
int mkdirat_errno(int fd, char* path, mode_t mode) {
	int res = mkdirat(fd, path, mode);
	if (res < 0)
		return -errno;
	return res;
}

int mkdirat_nocase(int fd, char* path, mode_t mode) {
	b8 last;
	lstr_t name = LSTR(path, 0);
	char* child_path = path_parent(path, &last, &name.len);

	DIR* d = fdopendir(dup(fd));
	rewinddir(d);
	LT_ASSERT(d != NULL);

	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (!lt_lseq_nocase(name, lt_lsfroms(ent->d_name)))
			continue;

		if (last) {
			closedir(d);
			return mkdirat_errno(fd, ent->d_name, mode);
		}

		int dfd = openat(fd, ent->d_name, O_RDONLY|O_DIRECTORY);
		if (dfd < 0) {
			closedir(d);
			return -errno;
		}

		int res = mkdirat_nocase(dfd, child_path, mode);
		close(dfd);
		closedir(d);
		return res;
	}
	closedir(d);

	if (last)
		return mkdirat_errno(fd, path, mode);
	return -ENOENT;
}

int copyat_nocase(int from_fd, char* from_path, int to_fd, char* to_path) {
	int infd = openat_nocase(from_fd, from_path, O_RDONLY, 0);
	if (infd < 0)
		return infd;

	int outfd = openat_nocase(to_fd, to_path, O_WRONLY|O_CREAT|O_TRUNC, 0666);
 	if (outfd < 0)
		return outfd;

	usz copy_bufsz = LT_KB(64);
	char* copy_buf = lt_malloc(alloc, copy_bufsz);

	isz res;
	while ((res = read(infd, copy_buf, copy_bufsz))) {
		LT_ASSERT(res >= 0);
		res = write(outfd, copy_buf, res);
		LT_ASSERT(res >= 0);
	}

	close(infd);
	close(outfd);

	lt_mfree(alloc, copy_buf);

	return 0;
}

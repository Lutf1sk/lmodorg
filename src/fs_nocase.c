#include <lt/lt.h>
#include <lt/debug.h>
#include <lt/str.h>

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
		if (!lt_lstr_case_eq(name, lt_lstr_from_cstr(ent->d_name)))
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
		if (!lt_lstr_case_eq(name, lt_lstr_from_cstr(ent->d_name)))
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
		if (!lt_lstr_case_eq(name, lt_lstr_from_cstr(ent->d_name)))
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
		if (!lt_lstr_case_eq(name, lt_lstr_from_cstr(ent->d_name)))
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

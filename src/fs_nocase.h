#ifndef FS_NOCASE_H
#define FS_NOCASE_H

#include <unistd.h>
#include <sys/stat.h>

int openat_nocase(int fd, char* path, int flags, mode_t mode);
int fstatat_nocase(int fd, char* path, struct stat* st, int flags);
int unlinkat_nocase(int fd, char* path, int flags);
int mkdirat_nocase(int fd, char* path, mode_t mode);

#endif
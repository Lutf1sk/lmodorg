#ifndef VFS_H
#define VFS_H

#include <lt/fwd.h>
#include <lt/err.h>

#define VI_REG	0
#define VI_DIR	1
#define VI_LNK	2

typedef struct mod mod_t;
typedef struct avail_mod avail_mod_t;

typedef struct vfs_inode vfs_inode_t;

typedef
struct vfs_dirent {
	b8 present;
	lstr_t name;
	char* cname;
	usz id;
} vfs_dirent_t;

typedef
struct  vfs_inode {
	b8 allocated;

	union {
		struct {
			u8 type;
			u32 links;
			u32 fds;
			u32 lookups;
			lt_darr(vfs_dirent_t) entries;

			mod_t* mod;
			char* real_path;
		};

		usz next_id;
	};
} vfs_inode_t;

#define VFD_READ		0x0100000000
#define VFD_WRITE		0x0200000000
#define VFD_MODE		0x0300000000
#define VFD_APPEND		0x0400000000
#define VFD_TRUNC		0x0800000000
#define VFD_EXCLUSIVE	0x1000000000

typedef i64 vfs_fd_t;

u64 new_inode_id(void);

lt_err_t inode_insert_dirent(usz parent_id, lstr_t name, usz child_id);
usz inode_register(u8 type, mod_t* mod, char* path);

vfs_inode_t* inode_find_by_id(usz ino);
usz inode_find_dirent(usz parent_id, lstr_t name);

void vfs_thread_proc(void* mountpoint);

void vfs_mount(char* argv0_, char* mountpoint, lt_darr(mod_t*) avail_mod_t, char* output_path);
void vfs_unmount(void);


#endif

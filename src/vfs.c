#define _FILE_OFFSET_BITS 64

#include "vfs.h"
#include "mod.h"

#include <lt/thread.h>
#include <lt/str.h>
#include <lt/sort.h>
#include <lt/darr.h>
#include <lt/io.h>
#include <lt/ctype.h>

#include <string.h>
#include <ctype.h>

#include "fs_nocase.h"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/poll.h>
#include <sys/file.h>
#include <dirent.h>
#include <errno.h>

#define alloc lt_libc_heap

#define ATTR_TIMEOUT 512.0
#define ENTRY_TIMEOUT 512.0

static lt_thread_t* vfs_thread = NULL;

static char* argv0;

#define ID_INVAL 0
#define ID_ROOT 1

static lt_darr(vfs_inode_t) ino_tab = NULL;
static usz inode_id_free = ID_INVAL;

extern b8 verbose;

struct fuse_session* fuse_session;

static mod_t* output_mod;
static mod_t* loopback_mod;

void inode_unlink(usz id, usz n);
void inode_link(usz id);

lt_err_t inode_insert_dirent(usz parent_id, lstr_t name, usz child_id) {
	vfs_inode_t* parent = &ino_tab[parent_id];
	LT_ASSERT(parent->type == VI_DIR);

	lstr_t dupname = lt_lsbuild(alloc, "%S%c", name, 0);
	--dupname.len;

	vfs_dirent_t ent = { .present = 1, .name = dupname, .cname = dupname.str, .id = child_id };
	lt_darr_push(parent->entries, ent);
	inode_link(child_id);
	return LT_SUCCESS;
}

void inode_erase_dirent(usz parent_id, usz ent_idx) {
	LT_ASSERT(ino_tab[parent_id].entries[ent_idx].present);
	vfs_dirent_t* ent = &ino_tab[parent_id].entries[ent_idx];

	inode_unlink(ent->id, 1);

	if (ino_tab[parent_id].fds != 0)
		ent->present = 0;
	else {
		lt_mfree(alloc, ino_tab[parent_id].entries[ent_idx].cname);
		lt_darr_erase(ino_tab[parent_id].entries, ent_idx, 1);
	}
}

void inode_register_at(usz id, u8 type, mod_t* mod, char* path) {
	LT_ASSERT(!ino_tab[id].allocated);

	ino_tab[id] = (vfs_inode_t) {
			.allocated = 1,
			.type = type,
			.mod = mod,
			.real_path = path };

	if (type == VI_DIR)
		ino_tab[id].entries = lt_darr_create(vfs_dirent_t, 8, alloc);
}

usz inode_register(u8 type, mod_t* mod, char* path) {
	LT_ASSERT(inode_id_free != ID_INVAL);
	usz id = inode_id_free;
	inode_id_free = ino_tab[id].next_id;

	inode_register_at(id, type, mod, path);
	return id;
}

LT_INLINE
vfs_inode_t* inode_find_by_id(usz id) {
	return &ino_tab[id];
}

void inode_free(usz id) {
	LT_ASSERT(id != ID_ROOT);
	LT_ASSERT(ino_tab[id].allocated);

	if (ino_tab[id].type == VI_DIR) {
		lt_mfree(alloc, ino_tab[id].entries[0].cname);
		lt_mfree(alloc, ino_tab[id].entries[1].cname);
		for (usz i = 2; i < lt_darr_count(ino_tab[id].entries); ++i) {
			vfs_dirent_t ent = ino_tab[id].entries[i];
			if (ent.present)
				inode_unlink(ent.id, 1);
			lt_mfree(alloc, ent.cname);
		}
		lt_darr_destroy(ino_tab[id].entries);
		ino_tab[id].entries = NULL;
	}

	lt_mfree(alloc, ino_tab[id].real_path);

	ino_tab[id].allocated = 0;
	ino_tab[id].next_id = inode_id_free;
	inode_id_free = id;
}

void inode_force_free(usz id) {
	LT_ASSERT(ino_tab[id].allocated);

	if (ino_tab[id].type == VI_DIR) {
		lt_mfree(alloc, ino_tab[id].entries[0].cname);
		lt_mfree(alloc, ino_tab[id].entries[1].cname);
		for (usz i = 2; i < lt_darr_count(ino_tab[id].entries); ++i) {
			vfs_dirent_t ent = ino_tab[id].entries[i];
			if (ent.present)
				inode_force_free(ent.id);
			lt_mfree(alloc, ent.cname);
		}
		lt_darr_destroy(ino_tab[id].entries);
	}

	lt_mfree(alloc, ino_tab[id].real_path);
	ino_tab[id].allocated = 0;
}

b8 inode_freeable(usz id) {
	if (ino_tab[id].type == VI_REG && ino_tab[id].links > 0)
		return 0;
	if (ino_tab[id].type == VI_DIR && ino_tab[id].links > 1)
		return 0;

	return ino_tab[id].lookups == 0 && ino_tab[id].fds == 0;
}

void inode_unlink(usz id, usz n) {
	LT_ASSERT(ino_tab[id].allocated);
	LT_ASSERT(ino_tab[id].links >= n);
	ino_tab[id].links -= n;
	if (inode_freeable(id))
		inode_free(id);
}

void inode_link(usz id) {
	ino_tab[id].links++;
}

void inode_forget(usz id, usz n) {
	LT_ASSERT(ino_tab[id].allocated);
	LT_ASSERT(ino_tab[id].lookups >= n);
	ino_tab[id].lookups -= n;
	if (inode_freeable(id))
		inode_free(id);
}

void inode_lookup(usz id) {
	ino_tab[id].lookups++;
}

void inode_close(usz id, usz n) {
	LT_ASSERT(ino_tab[id].allocated);
	LT_ASSERT(ino_tab[id].fds >= n);
	ino_tab[id].fds -= n;
	if (ino_tab[id].fds == 0 && ino_tab[id].type == VI_DIR && ino_tab[id].entries != NULL) {
		for (usz i = 0; i < lt_darr_count(ino_tab[id].entries); ++i) {
			vfs_dirent_t ent = ino_tab[id].entries[i];
			if (!ent.present) {
				lt_mfree(alloc, ent.cname);
				lt_darr_erase(ino_tab[id].entries, i--, 1);
			}
		}
	}
	if (inode_freeable(id))
		inode_free(id);
}

void inode_open(usz id) {
	ino_tab[id].fds++;
}

isz inode_find_dirent_index(usz parent_id, lstr_t name) {
	lt_darr(vfs_dirent_t) ents = ino_tab[parent_id].entries;
	for (usz i = 0; i < lt_darr_count(ents); ++i)
		if (ents[i].present && lt_lseq_nocase(ents[i].name, name))
			return i;

	return -1;
}

usz inode_find_dirent(usz parent_id, lstr_t name) {
	isz idx = inode_find_dirent_index(parent_id, name);
	if (idx == -1)
		return ID_INVAL;
	return ino_tab[parent_id].entries[idx].id;
}

mode_t vi_type_to_st_mode(int vi) {
	switch (vi) {
	case VI_DIR: return S_IFDIR | 0755;
	case VI_REG: return S_IFREG | 0666;
	default: LT_ASSERT_NOT_REACHED(); return 0;
	}
}

void approximate_stat(fuse_ino_t ino, struct stat* out) {
	*out = (struct stat) {
			.st_ino = ino,
			.st_nlink = ino_tab[ino].links,
			.st_mode = vi_type_to_st_mode(ino_tab[ino].type) };
}

int stat_ino(fuse_ino_t ino, struct stat* stat_buf) {
	approximate_stat(ino, stat_buf);

	struct stat stat_real;
	int res = fstatat_nocase(ino_tab[ino].mod->rootfd, ino_tab[ino].real_path, &stat_real, AT_SYMLINK_NOFOLLOW);
	if (res < 0) {
		if (-res != ENOENT) {
			lt_werrf("stat failed for [%S] '%s'(%uq): %s\n", ino_tab[ino].mod->name, ino_tab[ino].real_path, ino, strerror(-res));
		}
		return res;
	}

	stat_buf->st_blocks = stat_real.st_blocks;
	stat_buf->st_atime = stat_real.st_atime;
	stat_buf->st_mtime = stat_real.st_mtime;
	stat_buf->st_ctime = stat_real.st_ctime;
	stat_buf->st_size = stat_real.st_size;
	return LT_SUCCESS;
}

void approximate_entry(fuse_ino_t ino, struct fuse_entry_param* out) {
	*out = (struct fuse_entry_param) {
			.ino = ino,
			.attr_timeout = ATTR_TIMEOUT,
			.entry_timeout = ENTRY_TIMEOUT,
			.attr.st_ino = ino,
			.attr.st_mode = vi_type_to_st_mode(ino_tab[ino].type),
			.attr.st_nlink = ino_tab[ino].links };
}

void lookup_ino(fuse_ino_t ino, struct fuse_entry_param* out) {
	approximate_entry(ino, out);
	LT_ASSERT(stat_ino(ino, &out->attr) == 0);
	inode_lookup(ino);
}

void make_output_path(char* dir_path) {
	char* it = dir_path;
	usz parent_id = ID_ROOT;

	for (;;) {
		char* name_start = it;
		while (*it != 0 && *it != '/')
			++it;
		lstr_t name = lt_lsfrom_range(name_start, it);
		if (name.len <= 0)
			goto next;

		char* cpath = lt_lstos(lt_lsfrom_range(dir_path, it), alloc);
		int res = mkdirat_nocase(output_mod->rootfd, cpath, 0755);
		if (res < 0 && errno != EEXIST)
			lt_werrf("failed to create output directory: %s\n", lt_os_err_str());

		usz child_id = inode_find_dirent(parent_id, name);
		if (child_id != ID_INVAL) {
			lt_mfree(alloc, cpath);

			if (ino_tab[child_id].type != VI_DIR)
				return;
			ino_tab[child_id].mod = output_mod;
		}
		else {
			child_id = inode_register(VI_DIR, output_mod, cpath);
			inode_insert_dirent(parent_id, name, child_id);
		}
		parent_id = child_id;

	next:
		if (*it == 0)
			return;
		++it;
	}
}

void vfs_init(void* usr, struct fuse_conn_info* conn) {
	if (conn->capable & FUSE_CAP_SPLICE_WRITE)
		conn->want |= FUSE_CAP_SPLICE_WRITE;
	if (conn->capable & FUSE_CAP_SPLICE_READ)
		conn->want |= FUSE_CAP_SPLICE_READ;
	if (conn->capable & FUSE_CAP_SPLICE_MOVE)
		conn->want |= FUSE_CAP_SPLICE_MOVE;
}

void vfs_destroy(void* usr) {

}

void vfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_getattr called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	struct stat stat_buf;
	int res = stat_ino(ino, &stat_buf);
	if (res < 0)
		fuse_reply_err(req, -res);
	else
		fuse_reply_attr(req, &stat_buf, ATTR_TIMEOUT);
}

void vfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_setattr called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);
	vfs_inode_t* inode = &ino_tab[ino];

	if (to_set & FUSE_SET_ATTR_MODE) {
		if (verbose)
			lt_ierrf("SETATTR_MODE\n");

		fuse_reply_err(req, EACCES);
		return;

// 		if (fchmodat(inode->mod->rootfd, inode->real_path, attr->st_mode, AT_SYMLINK_NOFOLLOW) < 0) {
// 			lt_werrf("fchmodat failed: %s\n", lt_os_err_str());
// 			fuse_reply_err(req, errno);
// 			return;
// 		}
	}
	if (to_set & FUSE_SET_ATTR_UID) {
		if (verbose)
			lt_ierrf("SETATTR_UID\n");

		fuse_reply_err(req, EACCES);
		return;
// 		if (fchownat(inode->mod->rootfd, inode->real_path, attr->st_uid, -1, AT_SYMLINK_NOFOLLOW) < 0) {
// 			lt_werrf("fchownat failed: %s\n", lt_os_err_str());
// 			fuse_reply_err(req, errno);
// 			return;
// 		}
	}
	if (to_set & FUSE_SET_ATTR_GID) {
		if (verbose)
			lt_ierrf("SETATTR_GID\n");

		fuse_reply_err(req, EACCES);
		return;
// 		if (fchownat(inode->mod->rootfd, inode->real_path, -1, attr->st_gid, AT_SYMLINK_NOFOLLOW) < 0) {
// 			lt_werrf("fchownat failed: %s\n", lt_os_err_str());
// 			fuse_reply_err(req, errno);
// 			return;
// 		}
	}
	if (to_set & FUSE_SET_ATTR_SIZE) {
		if (verbose)
			lt_ierrf("SETATTR_SIZE\n");
		LT_ASSERT(inode->mod == output_mod);
		LT_ASSERT(fi != NULL);

		if (ftruncate(fi->fh, attr->st_size) < 0) {
			fuse_reply_err(req, errno);
			lt_werrf("ftruncate failed\n");
			return;
		}
	}
	if (to_set & (FUSE_SET_ATTR_ATIME|FUSE_SET_ATTR_MTIME|FUSE_SET_ATTR_ATIME_NOW|FUSE_SET_ATTR_MTIME_NOW)) {
		struct timespec tv[2];
		tv[0].tv_sec = 0;
		tv[0].tv_nsec = UTIME_OMIT;
		tv[1].tv_sec = 0;
		tv[1].tv_nsec = UTIME_OMIT;

		if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
			if (verbose)
				lt_ierrf("SETATTR_ATIME_NOW\n");
			tv[0].tv_nsec = UTIME_NOW;
		}
		else if (to_set & FUSE_SET_ATTR_ATIME) {
			if (verbose)
				lt_ierrf("SETATTR_ATIME\n");
			tv[0].tv_nsec = attr->st_atime;
		}

		if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
			if (verbose)
				lt_ierrf("SETATTR_MTIME_NOW\n");
			tv[1].tv_nsec = UTIME_NOW;
		}
		else if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
			if (verbose)
				lt_ierrf("SETATTR_MTIME\n");
			tv[1].tv_nsec = attr->st_mtime;
		}

		if (utimensat(inode->mod->rootfd, inode->real_path, tv, AT_SYMLINK_NOFOLLOW) < 0) {
			int err = errno;
			fuse_reply_err(req, err);
			lt_werrf("utimensat failed: %s\n", strerror(err));
			return;
		}
	}

	// Unhandled
	int err = 0;
	if (to_set & FUSE_SET_ATTR_FORCE) {
		lt_werrf("unhandled FUSE_SET_ATTR_FORCE\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_CTIME) {
		lt_werrf("unhandled FUSE_SET_ATTR_CTIME\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_KILL_SUID) {
		lt_werrf("unhandled FUSE_SET_ATTR_KILL_SUID\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_KILL_SGID) {
		lt_werrf("unhandled FUSE_SET_ATTR_KILL_SGID\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_FILE) {
		lt_werrf("unhandled FUSE_SET_ATTR_FILE\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_KILL_PRIV) {
		lt_werrf("unhandled FUSE_SET_ATTR_KILL_PRIV\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_OPEN) {
		lt_werrf("unhandled FUSE_SET_ATTR_OPEN\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_TIMES_SET) {
		lt_werrf("unhandled FUSE_SET_ATTR_TIMES_SET\n");
		err = EOPNOTSUPP;
	}
	if (to_set & FUSE_SET_ATTR_TOUCH) {
		lt_werrf("unhandled FUSE_SET_ATTR_TOUCH\n");
		err = EOPNOTSUPP;
	}

	if (err) {
		fuse_reply_err(req, err);
		return;
	}

	struct stat stat_buf;
	int res = stat_ino(ino, &stat_buf);
	if (res < 0)
		fuse_reply_err(req, -res);
	else
		fuse_reply_attr(req, &stat_buf, ATTR_TIMEOUT);
}

void vfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char* key, size_t size) {
	if (verbose)
		lt_ierrf("vfs_getxattr called for '%s'(%uq) with key '%s'\n", ino_tab[ino].real_path, ino, key);

	fuse_reply_err(req, EOPNOTSUPP);
}

void vfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char* key, const char* val, size_t size, int flags) {
	lt_werrf("vfs_setxattr called for '%s'(%uq) with key '%s'\n", ino_tab[ino].real_path, ino, key);
	fuse_reply_err(req, EOPNOTSUPP);
}

void vfs_lookup(fuse_req_t req, fuse_ino_t ino, const char* cname) {
	if (verbose)
		lt_ierrf("vfs_lookup called for '%s'(%uq)/'%s'\n", ino_tab[ino].real_path, ino, cname);

	lstr_t name = lt_lsfroms((char*)cname);

	usz child_id = inode_find_dirent(ino, name);
	if (child_id == ID_INVAL) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	struct fuse_entry_param ent;
	lookup_ino(child_id, &ent);
	fuse_reply_entry(req, &ent);
}

void vfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_readdir called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	usz bufoff = 0;

	lt_darr(vfs_dirent_t) ents = ino_tab[ino].entries;
	usz entcount = lt_darr_count(ents);

	char* buf = lt_malloc(alloc, size);
	for (usz i = off; i < entcount; ++i) {
		struct stat st;
		if (ents[i].present)
			stat_ino(ents[i].id, &st);
		else {
			memset(&st, 0, sizeof(st));
			continue;
		}
		usz adv = fuse_add_direntry(req, &buf[bufoff], size - bufoff, ents[i].cname, &st, i + 1);
		if (bufoff + adv > size)
			break;
		bufoff += adv;
	}

reply:
	fuse_reply_buf(req, buf, bufoff);
	lt_mfree(alloc, buf);
	return;
}

void vfs_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_readdirplus called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	usz bufoff = 0;

	lt_darr(vfs_dirent_t) ents = ino_tab[ino].entries;
	usz entcount = lt_darr_count(ents);

	char* buf = lt_malloc(alloc, size);
	for (usz i = off; i < entcount; ++i) {
		struct fuse_entry_param ent;
		if (ents[i].present) {
			approximate_entry(ents[i].id, &ent);
			stat_ino(ents[i].id, &ent.attr);
		}
		else {
			memset(&ent, 0, sizeof(ent));
			continue;
		}
		usz adv = fuse_add_direntry_plus(req, &buf[bufoff], size - bufoff, ents[i].cname, &ent, i + 1);
		if (bufoff + adv > size)
			break;
		if (i >= 2)
			inode_lookup(ents[i].id);
		bufoff += adv;
	}

reply:
	fuse_reply_buf(req, buf, bufoff);
	lt_mfree(alloc, buf);
	return;
}

void vfs_mknod(fuse_req_t req, fuse_ino_t ino, const char* cname, mode_t mode, dev_t dev) {
	lt_ferrf("vfs_mknod called for '%s'(%uq)/'%s'\n", ino_tab[ino].real_path, ino, cname);
	fuse_reply_err(req, EOPNOTSUPP);
}

void vfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info* fi) {
	if (verbose)
		lt_werrf("vfs_fsync called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);
	int res;
	if (datasync)
		res = fdatasync(fi->fh);
	else
		res = fsync(fi->fh);

	if (res < 0)
		fuse_reply_err(req, errno);
	else
		fuse_reply_err(req, 0);
}

void vfs_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_flush called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);
	int res = close(dup(fi->fh));
	if (res < 0)
		fuse_reply_err(req, errno);
	else
		fuse_reply_err(req, 0);
}

void vfs_rename(fuse_req_t req, fuse_ino_t ino1, const char* cname1, fuse_ino_t ino2, const char* cname2, unsigned int flags) {
	if (verbose)
		lt_ierrf("vfs_rename called for '%s'(%uq)/'%s' to '%s'(%uq)/'%s'\n", ino_tab[ino1].real_path, ino1, cname1, ino_tab[ino2].real_path, ino2, cname2);

	lstr_t name1 = lt_lsfroms((char*)cname1);
	lstr_t name2 = lt_lsfroms((char*)cname2);

	isz ent_idx = inode_find_dirent_index(ino1, name1);
	if (ent_idx == -1) {
		fuse_reply_err(req, ENOENT);
		return;
	}
	usz from_id = ino_tab[ino1].entries[ent_idx].id;
	vfs_inode_t* from = &ino_tab[from_id];

	isz to_ent_idx = inode_find_dirent_index(ino2, name2);
	usz to_id = to_ent_idx == -1 ? ID_INVAL : ino_tab[ino2].entries[to_ent_idx].id;
	if (to_id != ID_INVAL)
		LT_ASSERT(ino_tab[to_id].type == from->type);

	if (to_id == from_id) {
		fuse_reply_err(req, 0);
		return;
	}

	char* to_path = lt_lsbuild(alloc, "%s/%S%c", ino_tab[ino2].real_path, name2, 0).str;

	make_output_path(from->real_path);
	if (from->mod == output_mod) {
		int res = renameat(from->mod->rootfd, from->real_path, output_mod->rootfd, to_path);
		if (res < 0) {
			fuse_reply_err(req, errno);
			lt_mfree(alloc, to_path);
			return;
		}
	}
	else {
		LT_ASSERT(from->type == VI_REG); // !! should also copy directories
		int res = copyat_nocase(from->mod->rootfd, from->real_path, output_mod->rootfd, to_path);
		if (res < 0) {
			fuse_reply_err(req, -res);
			lt_mfree(alloc, to_path);
			return;
		}
	}

	if (to_id != ID_INVAL) {
		inode_erase_dirent(ino2, to_ent_idx);
		if (ino1 == ino2 && to_ent_idx < ent_idx)
			--ent_idx;
	}

	to_id = inode_register(VI_REG, output_mod, to_path);
	inode_insert_dirent(ino2, name2, to_id);

	inode_erase_dirent(ino1, ent_idx);

	fuse_reply_err(req, 0);
}

void redirect_to_output(usz id) {
	vfs_inode_t* inode = &ino_tab[id];
	if (inode->mod == output_mod)
		return;

	char* cpath_dir = lt_lstos(lt_lsdirname(lt_lsfroms(inode->real_path)), alloc);
	make_output_path(cpath_dir);

	if (copyat_nocase(inode->mod->rootfd, inode->real_path, output_mod->rootfd, inode->real_path) < 0)
		lt_werrf("failed to copy '%s'(%uz) to output directory\n", inode->real_path, id);

	inode->mod = output_mod;
	lt_mfree(alloc, cpath_dir);
}

int open_child(fuse_ino_t ino, char* cname, int flags, mode_t mode) {
	LT_ASSERT(!(flags & __O_PATH));
	LT_ASSERT(!(flags & O_NOFOLLOW));
	LT_ASSERT(!(flags & O_NOCTTY));
	LT_ASSERT(!(flags & __O_NOATIME));
	LT_ASSERT(!(flags & __O_LARGEFILE));
	LT_ASSERT(!(flags & O_DSYNC));
	LT_ASSERT(!(flags & O_DIRECTORY));
	LT_ASSERT(!(flags & __O_DIRECT));
	LT_ASSERT(!(flags & O_CLOEXEC));
	LT_ASSERT(!(flags & O_ASYNC));

	vfs_fd_t vflags = 0;

	char flag_buf[32];
	char* flag_it = flag_buf;

	if ((flags & O_ACCMODE) == O_WRONLY) {
		vflags |= VFD_WRITE;
		*flag_it++ = 'W';
	}

	if ((flags & O_ACCMODE) == O_RDONLY) {
		vflags |= VFD_READ;
		*flag_it++ = 'R';
	}

	if ((flags & O_ACCMODE) == O_RDWR) {
		vflags |= VFD_READ|VFD_WRITE;
		*flag_it++ = 'R';
		*flag_it++ = 'W';
	}

	if (flags & O_APPEND) {
// 		lt_werrf("masked O_APPEND\n");
// 		flags &= ~O_APPEND;
		vflags |= VFD_APPEND;
		*flag_it++ = 'A';
	}

	if (flags & O_TRUNC) {
		vflags |= VFD_TRUNC;
		*flag_it++ = 'T';
	}

	if (flags & O_EXCL) {
		vflags |= VFD_EXCLUSIVE;
		*flag_it++ = 'E';
	}

	vfs_inode_t* inode = &ino_tab[ino];

	if (cname != NULL) {
		if (inode->type != VI_DIR)
			return -ENOTDIR;

		lstr_t name = lt_lsfroms(cname);

		usz child_id = inode_find_dirent(ino, name);
		if (child_id == ID_INVAL) {
			make_output_path(inode->real_path);
			char* real_path = lt_lsbuild(alloc, "%s/%s%c", inode->real_path, cname, 0).str;

			int fd = openat_nocase(output_mod->rootfd, real_path, flags|O_CREAT, mode);
			if (fd < 0) {
				lt_mfree(alloc, real_path);
				return fd;
			}

			child_id = inode_register(VI_REG, output_mod, real_path);
			inode_insert_dirent(ino, name, child_id);

			inode_open(child_id);
			return fd;
		}

		ino = child_id;
	}

	if (ino_tab[ino].type == VI_DIR)
		return -EISDIR;

	if (vflags & VFD_WRITE) {
		if (ino_tab[ino].mod != output_mod) {
			redirect_to_output(ino);
		}
	}
	else if (!(vflags & VFD_READ)) {
		lt_werrf("vfs_open called with unsupported flags\n");
		return -EOPNOTSUPP;
	}

	int fd = openat_nocase(inode->mod->rootfd, inode->real_path, flags, 0);
	if (fd < 0)
		return fd;
	inode_open(ino);
	return fd;
}

void vfs_create(fuse_req_t req, fuse_ino_t ino, const char* cname, mode_t mode, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_create called for '%s'(%uq)/'%s'\n", ino_tab[ino].real_path, ino, cname);

	int fd = open_child(ino, (char*)cname, fi->flags, mode);
	if (fd < 0) {
		fuse_reply_err(req, -(int)fd);
		return;
	}

	usz child_id = inode_find_dirent(ino, lt_lsfroms((char*)cname));
	LT_ASSERT(child_id != ID_INVAL);

	struct fuse_entry_param ent;
	lookup_ino(child_id, &ent);
	fi->fh = fd;
	fuse_reply_create(req, &ent, fi);
}

void vfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_open called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	int fd = open_child(ino, NULL, fi->flags, 0);
	if (fd < 0) {
		fuse_reply_err(req, -(int)fd);
		return;
	}

	fi->fh = fd;
	fuse_reply_open(req, fi);
}

void vfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_release called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	LT_ASSERT(ino_tab[ino].type == VI_REG);

	close(fi->fh);
	inode_close(ino, 1);

	fuse_reply_err(req, 0);
}

void vfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_read called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
	buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	buf.buf[0].fd = (int)fi->fh;
	buf.buf[0].pos = off;

	fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_MOVE);
}

void vfs_write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_write called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	ssize_t res = pwrite(fi->fh, buf, size, off);
	if (res < 0) {
		fuse_reply_err(req, errno);
		return;
	}

	fuse_reply_write(req, res);
}

void vfs_write_buf(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec* in_buf, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_write called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	struct fuse_bufvec out_buf = FUSE_BUFVEC_INIT(fuse_buf_size(in_buf));
	out_buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	out_buf.buf[0].fd = (int)fi->fh;
	out_buf.buf[0].pos = off;

	ssize_t res = fuse_buf_copy(&out_buf, in_buf, 0);

	if (res < 0)
		fuse_reply_err(req, -res);
	else
		fuse_reply_write(req, res);
}

void vfs_statfs(fuse_req_t req, fuse_ino_t ino) {
	struct statvfs stat_buf = { .f_namemax = 256 };
	fuse_reply_statfs(req, &stat_buf);
}

void vfs_unlink(fuse_req_t req, fuse_ino_t ino, const char* cname) {
	if (verbose)
		lt_ierrf("vfs_unlink called for '%s'(%uq)/'%s'\n", ino_tab[ino].real_path, ino, cname);

	isz ent_idx = inode_find_dirent_index(ino, lt_lsfroms((char*)cname));
	if (ent_idx == -1) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	usz child_id = ino_tab[ino].entries[ent_idx].id;
	if (ino_tab[child_id].type == VI_DIR) {
		fuse_reply_err(req, EISDIR);
		return;
	}

	if (ino_tab[child_id].mod == output_mod) {
		int res = unlinkat_nocase(output_mod->rootfd, ino_tab[child_id].real_path, 0);
		if (res < 0) {
			fuse_reply_err(req, errno);
			return;
		}
	}

	inode_erase_dirent(ino, ent_idx);
	fuse_reply_err(req, 0);
}

void vfs_mkdir(fuse_req_t req, fuse_ino_t ino, const char* cname, mode_t mode) {
	if (verbose)
		lt_ierrf("vfs_mkdir called for '%s'(%uq)/'%s'\n", ino_tab[ino].real_path, ino, cname);

	lstr_t name = lt_lsfroms((char*)cname);
	usz child_id = inode_find_dirent(ino, name);
	if (child_id != ID_INVAL) {
		fuse_reply_err(req, EEXIST);
		return;
	}

	char* real_path = lt_lsbuild(alloc, "%s/%s%c", ino_tab[ino].real_path, cname, 0).str;
	make_output_path(ino_tab[ino].real_path);
	int res = mkdirat_nocase(output_mod->rootfd, real_path, mode);
	if (res < 0) {
		fuse_reply_err(req, errno);
		lt_mfree(alloc, real_path);
		return;
	}

	child_id = inode_register(VI_DIR, output_mod, real_path);
	inode_insert_dirent(child_id, CLSTR("."), child_id);
	inode_insert_dirent(child_id, CLSTR(".."), ino);

	inode_insert_dirent(ino, name, child_id);

	struct fuse_entry_param ent;
	lookup_ino(child_id, &ent);
	fuse_reply_entry(req, &ent);
}

void vfs_rmdir(fuse_req_t req, fuse_ino_t ino, const char* cname) {
	if (verbose)
		lt_ierrf("vfs_rmdir called for '%s'(%uq)/'%s'\n", ino_tab[ino].real_path, ino, cname);

	isz ent_idx = inode_find_dirent_index(ino, lt_lsfroms((char*)cname));
	if (ent_idx == -1) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	usz child_id = ino_tab[ino].entries[ent_idx].id;
	if (ino_tab[child_id].type == VI_REG) {
		fuse_reply_err(req, ENOTDIR);
		return;
	}

	// this check is commented out because it causes problems when attempting to recreate a deleted
	// directory. as this behaviour is inconsistent, the current approach should be revised.
	//if (ino_tab[child_id].mod == output_mod) {
		int res = unlinkat_nocase(output_mod->rootfd, ino_tab[child_id].real_path, AT_REMOVEDIR);
		if (res < 0) {
			fuse_reply_err(req, errno);
			return;
		}
	//}

	inode_erase_dirent(ino, ent_idx);
	fuse_reply_err(req, 0);
}

void vfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_opendir called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	inode_open(ino);

	fuse_reply_open(req, fi);
}

void vfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_releasedir called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	inode_close(ino, 1);

	fuse_reply_err(req, 0);
}

void vfs_fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t off, off_t len, struct fuse_file_info* fi) {
	lt_ferrf("vfs_fallocate called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);
	fuse_reply_err(req, EOPNOTSUPP);
}


void vfs_forget(fuse_req_t req, fuse_ino_t ino, u64 nlookup) {
	if (verbose)
		lt_ierrf("vfs_forget called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);
	inode_forget(ino, nlookup);
// 	lt_printf("'%s' allocated:%ub fds:%uz links:%uz lookups:%uz\n", ino_tab[ino].real_path, ino_tab[ino].allocated, ino_tab[ino].fds, ino_tab[ino].links, ino_tab[ino].lookups);
	fuse_reply_none(req);
}

void vfs_forget_multi(fuse_req_t req, size_t count, struct fuse_forget_data* forgets) {
	if (verbose)
		lt_ierrf("vfs_forget_multi called with count %uz\n", count);

	for (usz i = 0; i < count; ++i)
		inode_forget(forgets[i].ino, forgets[i].nlookup);
	fuse_reply_none(req);
}

void vfs_lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_lseek called for '%s'(%uq)\n", ino_tab[ino].real_path, ino);

	off_t res = lseek(fi->fh, off, whence);
	if (res != -1)
		fuse_reply_err(req, errno);
	else
		fuse_reply_lseek(req, res);
}

void vfs_symlink(fuse_req_t req, const char* link, fuse_ino_t ino, const char* name) {
	lt_ferrf("vfs_symlink\n");
	fuse_reply_err(req, EOPNOTSUPP);
}

void vfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char* newname) {
	lt_ferrf("vfs_link\n");
	fuse_reply_err(req, EOPNOTSUPP);
}

void vfs_readlink(fuse_req_t req, fuse_ino_t ino) {
	lt_ferrf("vfs_readlink\n");
	fuse_reply_err(req, EOPNOTSUPP);
}

const struct fuse_lowlevel_ops fuse_oper = {
	.init = vfs_init,
	.destroy = vfs_destroy,

	.mknod = vfs_mknod,
	.create = vfs_create,
	.open = vfs_open,
	.release = vfs_release,
	.symlink = vfs_symlink,
	.link = vfs_link,
	.unlink = vfs_unlink,
	.readlink = vfs_readlink,

	.lookup = vfs_lookup,
	.forget = vfs_forget,
	.forget_multi = vfs_forget_multi,

	.getattr = vfs_getattr,
	.setattr = vfs_setattr,
	.getxattr = vfs_getxattr,
	.setxattr = vfs_setxattr,
// 	.listxattr = vfs_listxattr,
// 	.removexattr = vfs_removexattr,
	.statfs = vfs_statfs,

	.mkdir = vfs_mkdir,
	.rmdir = vfs_rmdir,
	.opendir = vfs_opendir,
	.releasedir = vfs_releasedir,
	.readdir = vfs_readdir,
	.readdirplus = vfs_readdirplus,
// 	.fsyncdir = vfs_fsyncdir,

	.fallocate = vfs_fallocate,
	.read = vfs_read,
	.write = vfs_write,
	.write_buf = vfs_write_buf,
	.lseek = vfs_lseek,
	.fsync = vfs_fsync,
	.flush = vfs_flush,
// 	.flock = vfs_flock,

	.rename = vfs_rename,
};

void vfs_thread_proc(void* mountpoint) {
	char* fuse_argv[] = { argv0, mountpoint, "-f", NULL, };
	int fuse_argc = sizeof(fuse_argv) / sizeof(*fuse_argv) - 1;

	struct fuse_args fuse_args = FUSE_ARGS_INIT(fuse_argc, fuse_argv);
	struct fuse_cmdline_opts fuse_opts;

	if (fuse_parse_cmdline(&fuse_args, &fuse_opts) != 0)
		lt_ferrf("failed to parse libfuse arguments\n");

	fuse_session = fuse_session_new(&fuse_args, &fuse_oper, sizeof(fuse_oper), NULL);
	if (fuse_session == NULL)
		lt_ferrf("failed to create libfuse session\n");

	if (fuse_set_signal_handlers(fuse_session) != 0)
		lt_ferrf("failed to set libfuse signal handlers\n");

	if (fuse_session_mount(fuse_session, fuse_opts.mountpoint) != 0)
		lt_ferrf("failed to mount virtual filesystem\n");

	int ret = fuse_session_loop(fuse_session);
	if (verbose)
		lt_ierrf("libfuse thread returned %id\n", ret);

	fuse_session_unmount(fuse_session);
	fuse_remove_signal_handlers(fuse_session);
	fuse_session_destroy(fuse_session);

	free(fuse_opts.mountpoint);
	fuse_opt_free_args(&fuse_args);
}

#include <libgen.h>

void register_dirent(usz parent_id, mod_t* mod, char* real_path, lstr_t name, u32 type) {
	usz child_id;
	b8 free_path_late = 0;

	switch (type) {
	case DT_DIR:
		child_id = inode_find_dirent(parent_id, name);
		if (child_id != ID_INVAL) {
			if (ino_tab[child_id].type != VI_DIR)
				lt_ferrf("incompatible mapping for '%s', cannot overwrite file with directory\n", real_path);
			free_path_late = 1;
		}
		else {
			child_id = inode_register(VI_DIR, mod, real_path);
			inode_insert_dirent(child_id, CLSTR("."), child_id);
			inode_insert_dirent(child_id, CLSTR(".."), parent_id);

			inode_insert_dirent(parent_id, name, child_id);
		}

		DIR* dir = fdopendir(openat(mod->rootfd, real_path, O_RDONLY));
		LT_ASSERT(dir != NULL);
		for (struct dirent* ent; (ent = readdir(dir));) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
				continue;

			char* child_path = lt_lsbuild(alloc, "%s/%s%c", real_path, ent->d_name, 0).str;
			register_dirent(child_id, mod, child_path, lt_lsfroms(ent->d_name), ent->d_type);
		}
		closedir(dir);

		if (free_path_late)
			lt_mfree(alloc, real_path);
		return;

	case DT_REG:
		child_id = inode_find_dirent(parent_id, name);
		if (child_id != ID_INVAL) {
			if (ino_tab[child_id].type != VI_REG)
				lt_ferrf("incompatible mapping for '%s', cannot overwrite directory with file\n", real_path);
			ino_tab[child_id].mod = mod;
			lt_mfree(alloc, real_path);
		}
		else {
			child_id = inode_register(VI_REG, mod, real_path);
			inode_insert_dirent(parent_id, name, child_id);
		}
		return;

	case DT_LNK:
	default:
		lt_werrf("unhandled file type for '%S', entry ignored\n", name);
		lt_mfree(alloc, real_path);
		return;
	}
}

void print_debug_stat(usz id) {
	
}

void print_debug_ls(usz id) {
	vfs_inode_t* inode = &ino_tab[id];

	lt_printf("listing files in [%S] '%s'(%uq):\n", inode->mod->name, inode->real_path, id);

	if (inode->type != VI_DIR) {
		lt_printf("\tfailed; not a directory\n");
		return;
	}

	for (usz i = 0; i < lt_darr_count(inode->entries); ++i) {
		usz entid = inode->entries[i].id;
		vfs_inode_t* ent = &ino_tab[entid];
		lt_printf("\t%_6uz %_14S %S\n", entid, ent->mod->name, inode->entries[i].name);
	}
}

void vfs_mount(char* argv0_, char* mountpoint, lt_darr(mod_t*) mods, char* output_path) {
	argv0 = argv0_;

#define INO_TABSZ 65535

	// initialize inode table
	ino_tab = lt_darr_create(vfs_inode_t, INO_TABSZ, alloc);
	ino_tab = lt_darr_make_space(ino_tab, INO_TABSZ);
	memset(ino_tab, 0, sizeof(vfs_inode_t) * INO_TABSZ);
	inode_id_free = ID_INVAL;

	// create loopback mod

	int loopback_fd = open(mountpoint, O_RDONLY);
	if (loopback_fd < 0)
		lt_ferrf("failed to open loopback directory: %s\n", lt_os_err_str());
	loopback_mod = lt_malloc(alloc, sizeof(mod_t));
	LT_ASSERT(loopback_mod != NULL);
	*loopback_mod = (mod_t) {
			.name = lt_strdup(alloc, CLSTR("loopback")),
			.rootfd = loopback_fd };
	mod_register(loopback_mod);

	ino_tab[ID_ROOT + 1].next_id = ID_INVAL;
	for (usz i = ID_ROOT + 2; i < lt_darr_count(ino_tab); ++i)
		ino_tab[i].next_id = i - 1;
	inode_id_free = INO_TABSZ - 1;

	inode_register_at(ID_ROOT, VI_DIR, loopback_mod, strdup("."));
	inode_insert_dirent(ID_ROOT, CLSTR("."), ID_ROOT);
	inode_insert_dirent(ID_ROOT, CLSTR(".."), ID_ROOT); // !! incorrect inode
	register_dirent(ID_ROOT, loopback_mod, strdup("."), CLSTR("."), DT_DIR);

	// register mods

	for (usz i = 0; i < lt_darr_count(mods); ++i) {
		int fd = mods[i]->rootfd;
		LT_ASSERT(fd >= 0);

		if (verbose)
			lt_ierrf("loading mod '%S'\n", mods[i]->name);
		register_dirent(ID_ROOT, mods[i], strdup("."), CLSTR("."), DT_DIR);
	}

	// create output mod

	int output_fd = open(output_path, O_RDONLY);
	if (output_fd < 0)
		lt_ferrf("failed to open output directory '%s': %s\n", output_path, lt_os_err_str());
	output_mod = lt_malloc(alloc, sizeof(mod_t));
	LT_ASSERT(output_mod != NULL);
	*output_mod = (mod_t) {
			.name = lt_strdup(alloc, lt_lsbasename(lt_lsfroms(output_path))),
			.rootfd = output_fd };
	mod_register(output_mod);

	register_dirent(ID_ROOT, output_mod, strdup("."), CLSTR("."), DT_DIR);

	print_debug_ls(ID_ROOT);

	vfs_thread = lt_thread_create(vfs_thread_proc, mountpoint, alloc);
	if (!vfs_thread)
		lt_ferrf("failed to create thread\n");
}

void vfs_unmount(void) {
	lt_thread_terminate(vfs_thread);
	while (!lt_thread_join(vfs_thread, alloc))
		;
	vfs_thread = NULL;

	if (verbose)
		lt_ierrf("freeing file tree\n");
	inode_force_free(ID_ROOT);

	lt_darr_destroy(ino_tab);
}

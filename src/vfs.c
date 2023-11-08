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
#define ID_SIGN 2

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

	if (parent->entries == NULL)
		parent->entries = lt_darr_create(vfs_dirent_t, 8, alloc);
	vfs_dirent_t ent = { .present = 1, .name = name, .id = child_id };

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
	else
		lt_darr_erase(ino_tab[parent_id].entries, ent_idx, 1);
}

usz inode_register(u8 type, mod_t* mod, char* path) {
	LT_ASSERT(inode_id_free != ID_INVAL);
	usz id = inode_id_free;
	inode_id_free = ino_tab[id].next_id;

	LT_ASSERT(!ino_tab[id].allocated);

	ino_tab[id] = (vfs_inode_t) {
			.allocated = 1,
			.type = type,
			.mod = mod,
			.real_path = path };
	return id;
}

LT_INLINE
vfs_inode_t* inode_find_by_id(usz id) {
	return &ino_tab[id];
}

lstr_t duplower(lstr_t str) {
	lstr_t dup = lt_strdup(alloc, str);
	for (usz i = 0; i < dup.len; ++i)
		dup.str[i] = lt_to_lower(dup.str[i]);
	return dup;
}

void inode_free(usz id) {
	LT_ASSERT(ino_tab[id].allocated);

	if (ino_tab[id].type == VI_DIR && ino_tab[id].entries) {
		for (usz i = 0; i < lt_darr_count(ino_tab[id].entries); ++i) {
			vfs_dirent_t ent = ino_tab[id].entries[i];
			if (ent.present)
				inode_unlink(ent.id, 1);
			lt_mfree(alloc, ent.name.str);
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

	if (ino_tab[id].type == VI_DIR && ino_tab[id].entries) {
		for (usz i = 0; i < lt_darr_count(ino_tab[id].entries); ++i) {
			vfs_dirent_t ent = ino_tab[id].entries[i];
			if (ent.present)
				inode_force_free(ent.id);
			lt_mfree(alloc, ent.name.str);
		}
		lt_darr_destroy(ino_tab[id].entries);
	}

	lt_mfree(alloc, ino_tab[id].real_path);
	ino_tab[id].allocated = 0;
}

b8 inode_freeable(usz id) {
	return ino_tab[id].lookups == 0 && ino_tab[id].fds == 0 && ino_tab[id].links == 0;
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
				lt_mfree(alloc, ent.name.str);
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
	if (ents == NULL)
		return -1;

	for (usz i = 0; i < lt_darr_count(ents); ++i)
		if (ents[i].present && lt_lstr_case_eq(ents[i].name, name))
			return i;

	return -1;
}

usz inode_find_dirent(usz parent_id, lstr_t name) {
	isz idx = inode_find_dirent_index(parent_id, name);
	if (idx == -1)
		return ID_INVAL;
	return ino_tab[parent_id].entries[idx].id;
}

int copyat(int from_fd, char* from_path, int to_fd, char* to_path) {
	int infd = openat(from_fd, from_path, O_RDONLY);
	if (infd < 0)
		return -errno;

	int outfd = openat(to_fd, to_path, O_WRONLY|O_CREAT|O_TRUNC, 0666);
 	if (outfd < 0)
		return -errno;

	usz copy_bufsz = LT_KB(64);
	char* copy_buf = lt_malloc(alloc, copy_bufsz);

	isz res;
	while ((res = read(infd, copy_buf, copy_bufsz))) {
		LT_ASSERT(res > 0);
		res = write(outfd, copy_buf, res);
		LT_ASSERT(res > 0);
	}

	close(infd);
	close(outfd);

	lt_mfree(alloc, copy_buf);

	return 0;
}

void make_output_path(char* dir_path) {
	char* it = dir_path;
	usz parent_id = ID_ROOT;

	for (;;) {
		char* name_start = it;
		while (*it != 0 && *it != '/')
			++it;
		lstr_t name = lt_lstr_from_range(name_start, it);
		if (name.len <= 0)
			goto next;

		char* cpath = lt_cstr_from_lstr(lt_lstr_from_range(dir_path, it), alloc);
		int res = mkdirat(output_mod->rootfd, cpath, 0755);
		if (res < 0 && errno != EEXIST)
			lt_werrf("failed to create output directory: %s\n", lt_os_err_str());

		usz child_id = inode_find_dirent(parent_id, name);
		if (child_id != ID_INVAL) {
			lt_mfree(alloc, cpath);
			ino_tab[child_id].mod = output_mod;
		}
		else {
			child_id = inode_register(VI_DIR, output_mod, cpath);
			lstr_t dupname = duplower(name);
			inode_insert_dirent(parent_id, dupname, child_id);
		}
		parent_id = child_id;

	next:
		if (*it == 0)
			return;
		++it;
	}
}

#define DIRBUF_INITIAL_SIZE 1024

typedef
struct dirbuf {
	char* base;
	size_t top;
	size_t asize;
} dirbuf_t;

static
void dirbuf_add(fuse_req_t req, dirbuf_t* db, lstr_t name, fuse_ino_t ino) {
	char cname[512];
	LT_ASSERT(name.len < sizeof(cname));
	memcpy(cname, name.str, name.len);
	cname[name.len] = 0;

	usz grow_by = fuse_add_direntry(req, NULL, 0, cname, NULL, 0);
	usz grow_to = db->top + grow_by;

	if (db->asize < grow_to) {
		while (db->asize < grow_to)
			db->asize <<= 1;

		db->base = lt_mrealloc(alloc, db->base, db->asize);
		LT_ASSERT(db->base != NULL);
	}

	struct stat stat_buf = { .st_ino = ino };
	fuse_add_direntry(req, &db->base[db->top], grow_by, cname, &stat_buf, grow_to);
	db->top = grow_to;
}

static
void reply_part(fuse_req_t req, const char* buf, size_t bufsz, off_t off, size_t maxsz) {
	if (off >= bufsz) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	if (off + maxsz > bufsz)
		maxsz = bufsz - off;

	fuse_reply_buf(req, buf + off, maxsz);
}

void vfs_init(void* usr, struct fuse_conn_info* conn) {

}

void vfs_destroy(void* usr) {

}

int vfs_stat(fuse_ino_t ino, struct stat* stat_buf) {
	LT_ASSERT(ino != ID_INVAL);
	vfs_inode_t* inode = &ino_tab[ino];

	struct stat stat_real;
	if (fstatat(inode->mod->rootfd, inode->real_path, &stat_real, 0) < 0)
		return -errno;

	switch (inode->type) {
	case VI_DIR:
		*stat_buf = (struct stat) {
				.st_mode = S_IFDIR | 0755,
				.st_ino = ino,
				.st_nlink = inode->links };
		return 0;

	case VI_REG:
		*stat_buf = (struct stat) {
				.st_mode = S_IFREG | 0666,
				.st_nlink = inode->links,
				.st_ino = ino };
		stat_buf->st_size = stat_real.st_size;
		return 0;

	default:
		return -ENOENT;
	}
}

void vfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_getattr called for %uq\n", ino);

	struct stat stat_buf;
	int res = vfs_stat(ino, &stat_buf);
	if (res < 0)
		fuse_reply_err(req, -res);
	else
		fuse_reply_attr(req, &stat_buf, 1.0);
}

void vfs_lookup(fuse_req_t req, fuse_ino_t ino, const char* cname) {
	if (verbose)
		lt_ierrf("vfs_lookup called for '%s' on %uq\n", cname, ino);

	lstr_t name = lt_lstr_from_cstr((char*)cname);

	usz child_id = inode_find_dirent(ino, name);
	if (child_id == ID_INVAL) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	struct fuse_entry_param ent;
	memset(&ent, 0, sizeof(ent));
	ent.ino = child_id;
	ent.attr_timeout = ATTR_TIMEOUT;
	ent.entry_timeout = ENTRY_TIMEOUT;
	int staterr = vfs_stat(child_id, &ent.attr);
	if (staterr != 0)
		fuse_reply_err(req, -staterr);
	else {
		inode_lookup(child_id);
		fuse_reply_entry(req, &ent);
	}
}

void vfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_readdir called for %uq\n", ino);

	dirbuf_t db = {
			.asize = DIRBUF_INITIAL_SIZE,
			.base = lt_malloc(alloc, DIRBUF_INITIAL_SIZE) };
	LT_ASSERT(db.base != NULL);

	dirbuf_add(req, &db, CLSTR("."), ino);
	dirbuf_add(req, &db, CLSTR(".."), ino); // !! incorrect inode
	if (ino == ID_ROOT)
		dirbuf_add(req, &db, CLSTR(".LMODORG"), ID_SIGN);

	lt_darr(vfs_dirent_t) ents = ino_tab[ino].entries;
	if (ents != NULL)
		for (usz i = 0; i < lt_darr_count(ents); ++i)
			dirbuf_add(req, &db, ents[i].name, ents[i].id);

	reply_part(req, db.base, db.top, off, size);

	lt_mfree(alloc, db.base);
}

void vfs_mknod(fuse_req_t req, fuse_ino_t ino, const char* cname, mode_t mode, dev_t dev) {
	lt_werrf("vfs_mknod called for '%s' on %uq\n", cname, ino);
	fuse_reply_err(req, EACCES);
}

void vfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info* fi) {
	if (verbose)
		lt_werrf("vfs_fsync called for %uq\n", ino);
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
		lt_ierrf("vfs_flush called for %uq\n", ino);
	int res = close(dup(fi->fh));
	if (res < 0)
		fuse_reply_err(req, errno);
	else
		fuse_reply_err(req, 0);
}

void vfs_rename(fuse_req_t req, fuse_ino_t ino1, const char* cname1, fuse_ino_t ino2, const char* cname2, unsigned int flags) {
	if (verbose)
		lt_ierrf("vfs_rename called for '%s'/'%s' on %uq/%uq\n", cname1, cname2, ino1, ino2);

	lstr_t name1 = lt_lstr_from_cstr((char*)cname1);
	lstr_t name2 = lt_lstr_from_cstr((char*)cname2);

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

	char* to_path = lt_lstr_build(alloc, "%s/%S%c", ino_tab[ino2].real_path, name2, 0).str;

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
		int res = copyat(from->mod->rootfd, from->real_path, output_mod->rootfd, to_path);
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
	lstr_t dupname = duplower(name2);
	inode_insert_dirent(ino2, dupname, to_id);

	inode_erase_dirent(ino1, ent_idx);

	fuse_reply_err(req, 0);
}

void vfs_create(fuse_req_t req, fuse_ino_t ino, const char* cname, mode_t mode, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_create called for '%s' on %uq\n", cname, ino);

	lstr_t name = lt_lstr_from_cstr((char*)cname);

	usz child_id = inode_find_dirent(ino, name);
	if (child_id != ID_INVAL) {
		fuse_reply_err(req, EEXIST);
		return;
	}

	make_output_path(ino_tab[ino].real_path);
	char* real_path = lt_lstr_build(alloc, "%s/%s%c", ino_tab[ino].real_path, cname, 0).str;
	child_id = inode_register(VI_REG, output_mod, real_path);

	int fd = openat(output_mod->rootfd, real_path, O_CREAT|O_WRONLY|O_TRUNC, mode);
	if (fd < 0) {
		inode_free(child_id);
		fuse_reply_err(req, errno);
		return;
	}
	fi->fh = fd;

	lstr_t dupname = duplower(name);
	inode_insert_dirent(ino, dupname, child_id);

	struct fuse_entry_param ent;
	memset(&ent, 0, sizeof(ent));
	ent.ino = child_id;
	ent.attr_timeout = ATTR_TIMEOUT;
	ent.entry_timeout = ENTRY_TIMEOUT;
	LT_ASSERT(vfs_stat(child_id, &ent.attr) == 0);

	inode_lookup(child_id);
	inode_open(child_id);
	fuse_reply_create(req, &ent, fi);
}

void vfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_open called for %uq\n", ino);

	if (ino_tab[ino].type == VI_DIR) {
		fuse_reply_err(req, EISDIR);
		return;
	}

	LT_ASSERT(ino_tab[ino].type == VI_REG);

	if ((fi->flags & O_APPEND) == O_APPEND) {
		lt_werrf("vfs_open called with unsupported flag O_APPEND\n");
		fuse_reply_err(req, EACCES);
		return;
	}

	if (((fi->flags & O_ACCMODE) == O_WRONLY || (fi->flags & O_ACCMODE) == O_RDWR)) {
		char* cpath_dir = lt_cstr_from_lstr(lt_lstr_path_dir(lt_lstr_from_cstr(ino_tab[ino].real_path)), alloc);
		make_output_path(cpath_dir);
		lt_mfree(alloc, cpath_dir);

		ino_tab[ino].mod = output_mod;
	}
	else if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		lt_werrf("vfs_open called with unsupported flags\n");
		fuse_reply_err(req, EACCES);
		return;
	}

	int fd = openat(ino_tab[ino].mod->rootfd, ino_tab[ino].real_path, fi->flags);
	if (fd < 0) {
		fuse_reply_err(req, errno);
		return;
	}

	fi->fh = fd;
	inode_open(ino);
	fuse_reply_open(req, fi);
}

void vfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_release called for %uq\n", ino);

	LT_ASSERT(ino_tab[ino].type == VI_REG);

	close(fi->fh);
	inode_close(ino, 1);

	fuse_reply_err(req, 0);
}

void vfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_read called for %uq\n", ino);

	char* buf = lt_malloc(alloc, size);
	ssize_t res = pread(fi->fh, buf, size, off);
	if (res < 0) {
		lt_mfree(alloc, buf);
		fuse_reply_err(req, errno);
		return;
	}

	fuse_reply_buf(req, buf, size);
	lt_mfree(alloc, buf);
}

void vfs_write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_write called for %uq\n", ino);

	ssize_t res = pwrite(fi->fh, buf, size, off);
	if (res < 0) {
		LT_ASSERT_NOT_REACHED();
		fuse_reply_err(req, errno);
		return;
	}

	fuse_reply_write(req, res);
}

void vfs_statfs(fuse_req_t req, fuse_ino_t ino) {
	struct statvfs stat_buf = { .f_namemax = 256 };
	fuse_reply_statfs(req, &stat_buf);
}

void vfs_unlink(fuse_req_t req, fuse_ino_t ino, const char* cname) {
	if (verbose)
		lt_ierrf("vfs_unlink called for '%s' on %uq\n", cname, ino);

	isz ent_idx = inode_find_dirent_index(ino, lt_lstr_from_cstr((char*)cname));
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
		int res = unlinkat(output_mod->rootfd, ino_tab[child_id].real_path, 0);
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
		lt_ierrf("vfs_mkdir called for '%s' on %uq\n", cname, ino);

	lstr_t name = lt_lstr_from_cstr((char*)cname);
	usz child_id = inode_find_dirent(ino, name);
	if (child_id != ID_INVAL) {
		fuse_reply_err(req, EEXIST);
		return;
	}

	char* real_path = lt_lstr_build(alloc, "%s/%s%c", ino_tab[ino].real_path, cname, 0).str;
	make_output_path(ino_tab[ino].real_path);
	int res = mkdirat(output_mod->rootfd, real_path, mode);
	if (res < 0) {
		lt_mfree(alloc, real_path);
		fuse_reply_err(req, errno);
		return;
	}

	lstr_t dupname = duplower(name);

	child_id = inode_register(VI_DIR, output_mod, real_path);
	inode_insert_dirent(ino, dupname, child_id);

	struct fuse_entry_param ent;
	memset(&ent, 0, sizeof(ent));
	ent.ino = child_id;
	ent.attr_timeout = ATTR_TIMEOUT;
	ent.entry_timeout = ENTRY_TIMEOUT;
	LT_ASSERT(vfs_stat(ent.ino, &ent.attr) >= 0);

	inode_lookup(child_id);
	fuse_reply_entry(req, &ent);
}

void vfs_rmdir(fuse_req_t req, fuse_ino_t ino, const char* cname) {
	if (verbose)
		lt_ierrf("vfs_rmdir called for '%s' on %uq\n", cname, ino);

	isz ent_idx = inode_find_dirent_index(ino, lt_lstr_from_cstr((char*)cname));
	if (ent_idx == -1) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	usz child_id = ino_tab[ino].entries[ent_idx].id;
	if (ino_tab[child_id].type == VI_REG) {
		fuse_reply_err(req, ENOTDIR);
		return;
	}

	if (ino_tab[child_id].mod == output_mod) {
		int res = unlinkat(output_mod->rootfd, ino_tab[child_id].real_path, AT_REMOVEDIR);
		if (res < 0) {
			fuse_reply_err(req, errno);
			return;
		}
	}

	inode_erase_dirent(ino, ent_idx);
	fuse_reply_err(req, 0);
}

void vfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_opendir called for %uq\n", ino);

	inode_open(ino);

	fuse_reply_open(req, fi);
}

void vfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	if (verbose)
		lt_ierrf("vfs_releasedir called for %uq\n", ino);

	inode_close(ino, 1);

	fuse_reply_err(req, 0);
}

void vfs_fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t off, off_t len, struct fuse_file_info* fi) {
	lt_werrf("vfs_fallocate called for %uq\n", ino);
	fuse_reply_err(req, EOPNOTSUPP);
}


void vfs_forget(fuse_req_t req, fuse_ino_t ino, u64 nlookup) {
	if (verbose)
		lt_ierrf("vfs_forget called for %uq\n", ino);
	inode_forget(ino, nlookup);
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
		lt_ierrf("vfs_lseek called for %uq\n", ino);

	off_t res = lseek(fi->fh, off, whence);
	if (res != -1)
		fuse_reply_err(req, errno);
	else
		fuse_reply_lseek(req, res);
}

const struct fuse_lowlevel_ops fuse_oper = {
	.init = vfs_init,
	.destroy = vfs_destroy,

	.mknod = vfs_mknod,
	.create = vfs_create,
	.open = vfs_open,
	.release = vfs_release,
// 	.symlink = vfs_symlink,
// 	.link = vfs_link,
	.unlink = vfs_unlink,
// 	.readlink = vfs_readlink,

	.lookup = vfs_lookup,
	.forget = vfs_forget,
	.forget_multi = vfs_forget_multi,

	.getattr = vfs_getattr,
// 	.setattr = vfs_setattr,
// 	.getxattr = vfs_getxattr,
// 	.setxattr = vfs_setxattr,
// 	.listxattr = vfs_listxattr,
// 	.removexattr = vfs_removexattr,
	.statfs = vfs_statfs,

	.mkdir = vfs_mkdir,
	.rmdir = vfs_rmdir,
	.opendir = vfs_opendir,
	.releasedir = vfs_releasedir,
	.readdir = vfs_readdir,
// 	.readdirplus = vfs_readdirplus,
// 	.fsyncdir = vfs_fsyncdir,

	.fallocate = vfs_fallocate,
	.read = vfs_read,
	.write = vfs_write,
// 	.write_buf = vfs_write_buf,
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

void register_dir(usz parent_id, mod_t* mod, int parent_fd, char* dir_path);

void register_dirent(usz parent_id, mod_t* mod, int parent_fd, char* dir_path, struct dirent* ent) {
	lstr_t dupname = duplower(lt_lstr_from_cstr(ent->d_name));

	char* real_path = lt_lstr_build(alloc, "%s/%s%c", dir_path, ent->d_name, 0).str;

	usz child_id;
	b8 free_path_late = 0;

	switch (ent->d_type) {
	case DT_DIR:
		child_id = inode_find_dirent(parent_id, dupname);
		if (child_id != ID_INVAL) {
			if (ino_tab[child_id].type != VI_DIR)
				lt_ferrf("incompatible mapping for '%s', cannot overwrite file with directory\n", real_path);
			lt_mfree(alloc, dupname.str);
			free_path_late = 1;
		}
		else {
			child_id = inode_register(VI_DIR, mod, real_path);
			inode_insert_dirent(parent_id, dupname, child_id);
		}

		int fd = openat(parent_fd, ent->d_name, O_RDONLY);
		LT_ASSERT(fd >= 0);
		register_dir(child_id, mod, fd, real_path);
		close(fd);

		if (free_path_late)
			lt_mfree(alloc, real_path);
		return;

	case DT_REG:
		child_id = inode_find_dirent(parent_id, dupname);
		if (child_id != ID_INVAL) {
			if (ino_tab[child_id].type != VI_REG)
				lt_ferrf("incompatible mapping for '%s', cannot overwrite directory with file\n", real_path);
			ino_tab[child_id].mod = mod;
			lt_mfree(alloc, dupname.str);
			lt_mfree(alloc, real_path);
		}
		else {
			child_id = inode_register(VI_REG, mod, real_path);
			inode_insert_dirent(parent_id, dupname, child_id);
		}
		return;

	case DT_LNK:
	default:
		lt_werrf("unhandled file type for '%s', entry ignored\n", ent->d_name);
		lt_mfree(alloc, real_path);
		lt_mfree(alloc, dupname.str);
		return;
	}
}

void register_dir(usz parent_id, mod_t* mod, int parent_fd, char* dir_path) {
	DIR* dir = fdopendir(parent_fd);
	LT_ASSERT(dir != NULL);

	for (struct dirent* ent; (ent = readdir(dir));) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		register_dirent(parent_id, mod, parent_fd, dir_path, ent);
	}
}

void vfs_mount(char* argv0_, char* mountpoint, lt_darr(mod_t*) mods, char* output_path) {
	argv0 = argv0_;

#define INO_TABSZ 4096

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

	ino_tab[ID_ROOT] = (vfs_inode_t) {
			.allocated = 1,
			.type = VI_DIR,
			.mod = loopback_mod,
			.links = 1,
			.real_path = strdup(".") };

	ino_tab[ID_SIGN + 1].next_id = ID_INVAL;
	for (usz i = ID_SIGN + 2; i < lt_darr_count(ino_tab); ++i)
		ino_tab[i].next_id = i - 1;
	inode_id_free = INO_TABSZ - 1;

	register_dir(ID_ROOT, loopback_mod, loopback_fd, ".");

	// create output mod

	int output_fd = open(output_path, O_RDONLY);
	if (output_fd < 0)
		lt_ferrf("failed to open output directory: %s\n", lt_os_err_str());
	output_mod = lt_malloc(alloc, sizeof(mod_t));
	LT_ASSERT(output_mod != NULL);
	*output_mod = (mod_t) {
			.name = lt_strdup(alloc, lt_lstr_from_cstr(output_path)),
			.rootfd = output_fd };
	mod_register(output_mod);

	register_dir(ID_ROOT, output_mod, output_fd, ".");

	// register mods

	for (usz i = 0; i < lt_darr_count(mods); ++i) {
		int fd = mods[i]->rootfd;
		LT_ASSERT(fd >= 0);

		if (verbose)
			lt_ierrf("loading mod '%S'\n", mods[i]->name);
		register_dir(ID_ROOT, mods[i], fd, ".");
	}

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

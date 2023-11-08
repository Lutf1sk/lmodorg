#include <lt/io.h>
#include <lt/mem.h>
#include <lt/conf.h>
#include <lt/time.h>
#include <lt/term.h>
#include <lt/str.h>
#include <lt/darr.h>
#include <lt/arg.h>
#define LT_ANSI_SHORTEN_NAMES 1
#include <lt/ansi.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>

#include "vfs.h"
#include "mod.h"

#define alloc lt_libc_heap

b8 verbose = 0;

lt_darr(lstr_t) get_modlist(lt_conf_t* cf) {
	lt_darr(lstr_t) mods = lt_darr_create(lstr_t, 32, alloc);
	LT_ASSERT(mods);

	lt_conf_t* mods_cf = lt_conf_array(cf, CLSTR("mods"));
	for (usz i = 0; i < mods_cf->child_count; ++i) {
		lt_conf_t* mod_cf = &mods_cf->children[i];

		if (mod_cf->stype != LT_CONF_STRING) {
			lt_werrf("non-mod in 'mods' list\n");
			continue;
		}
		lt_darr_push(mods, mod_cf->str_val);
	}

	return mods;
}

#include <dirent.h>

lt_darr(avail_mod_t) get_available_mods(char* path) {
	lt_darr(avail_mod_t) mods = lt_darr_create(avail_mod_t, 32, alloc);
	LT_ASSERT(mods);

	DIR* mods_dir = opendir(path);
	if (!mods_dir) {
		lt_werrf("failed to open mods directory '%s': %s\n", path, lt_os_err_str());
		return mods;
	}
	struct dirent* ent;
	while ((ent = readdir(mods_dir))) {
		if (ent->d_type != DT_DIR || ent->d_name[0] == '.')
			continue;

		avail_mod_t mod = {
				.name = lt_strdup(alloc, lt_lstr_from_cstr(ent->d_name)),
				.root_path = lt_lstr_build(alloc, "%s/%s%c", path, ent->d_name, 0).str };
		lt_darr_push(mods, mod);
	}
	closedir(mods_dir);

	return mods;
}

lt_darr(mod_t*) get_mods(lt_darr(lstr_t) modlist, lt_darr(avail_mod_t) avail_mods) {
	lt_darr(mod_t*) mods = lt_darr_create(mod_t*, 32, alloc);
	LT_ASSERT(mods);

	for (usz i = 0; i < lt_darr_count(modlist); ++i) {
		avail_mod_t* avail_mod = NULL;
		for (usz j = 0; j < lt_darr_count(avail_mods); ++j) {
			if (lt_lstr_eq(modlist[i], avail_mods[j].name)) {
				avail_mod = &avail_mods[j];
				break;
			}
		}

		if (!avail_mod) {
			lt_werrf("'%S' not found in mods directory\n", modlist[i]);
			continue;
		}

		int fd = open(avail_mod->root_path, O_RDONLY);
		if (fd < 0) {
			lt_werrf("failed to open mod directory for '%s': %s\n", modlist[i], lt_os_err_str());
			continue;
		}

		mod_t* mod = lt_malloc(alloc, sizeof(mod_t));
		LT_ASSERT(mod != NULL);
		*mod = (mod_t) {
				.name = lt_strdup(alloc, modlist[i]),
				.rootfd = fd };
		mod_register(mod);

		lt_darr_push(mods, mod);
	}

	return mods;
}

b8 mod_exists(lt_darr(avail_mod_t) avail_mods, lstr_t name) {
	for (usz i = 0; i < lt_darr_count(avail_mods); ++i)
		if (lt_lstr_eq(avail_mods[i].name, name))
			return 1;
	return 0;
}

b8 mod_enabled(lt_darr(lstr_t) modlist, lstr_t name) {
	for (usz i = 0; i < lt_darr_count(modlist); ++i)
		if (lt_lstr_eq(modlist[i], name))
			return 1;
	return 0;
}

void copy_profile_configs(lt_conf_t* cf) {
	lt_conf_t* copy = lt_conf_find_array(cf, CLSTR("copy_files"), NULL);
	if (copy == NULL)
		return;

	usz copy_bufsz = LT_KB(128);
	char* copy_buf = lt_malloc(alloc, copy_bufsz);

	for (usz i = 0; i < copy->child_count; ++i) {
		lt_conf_t* link = &copy->children[i];
		if (link->stype != LT_CONF_OBJECT)
			continue;

		lstr_t from = lt_conf_str(link, CLSTR("from"));
		lstr_t to = lt_conf_str(link, CLSTR("to"));

		lt_file_t* inf = lt_fopenp(from, LT_FILE_R, 0, alloc);
		if (inf == NULL) {
			lt_werrf("failed to copy from '%S': %s\n", from, lt_os_err_str());
			continue;
		}

		lt_file_t* outf = lt_fopenp(to, LT_FILE_W, 0, alloc);
		if (outf == NULL) {
			lt_werrf("failed to copy to '%S': %s\n", to, lt_os_err_str());
			continue;
		}

		isz res;
		while ((res = lt_fread(inf, copy_buf, copy_bufsz))) {
			LT_ASSERT(res > 0);
			res = lt_fwrite(outf, copy_buf, res);
			LT_ASSERT(res > 0);
		}

		lt_fclose(inf, alloc);
		lt_fclose(outf, alloc);
	}

	lt_mfree(alloc, copy_buf);
}

void update_config(lstr_t conf_path, lt_conf_t* cf) {
	lt_file_t* fp = lt_fopenp(conf_path, LT_FILE_W, 0, alloc);
	if (!fp)
		lt_ferrf("failed to update config file: %s\n", lt_os_err_str());
	LT_ASSERT(lt_conf_write(cf, (lt_io_callback_t)lt_fwrite, fp) >= 0);
	lt_fclose(fp, alloc);
}

b8 dir_mounted(char* path) {
	DIR* dir = opendir(path);
	if (!dir)
		return 0;
	struct dirent* ent;
	while ((ent = readdir(dir))) {
		if (strcmp(ent->d_name, ".LMODORG") == 0) {
			closedir(dir);
			return 1;
		}
	}
	closedir(dir);
	return 0;
}

int remove_recursive(int parent_fd, char* name) {
	int ret = 0;

	struct stat st;
	if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
		return -1;

	if (S_ISDIR(st.st_mode)) {
		int fd = openat(parent_fd, name, O_RDONLY|O_DIRECTORY);

		DIR* dir = fdopendir(fd);
		if (!dir)
			return -1;

		struct dirent* ent;
		while ((ent = readdir(dir))) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
				continue;
			if (remove_recursive(fd, ent->d_name) < 0)
				ret = -1;
		}

		closedir(dir);

		if (unlinkat(parent_fd, name, AT_REMOVEDIR) < 0)
			ret = -1;
	}
	else {
		if (unlinkat(parent_fd, name, 0) < 0)
			return -1;
	}

	return ret;
}

#include <libgen.h>

int rmdir_recursive(char* path) {
	int fd = open(path, O_RDONLY|O_DIRECTORY);
	if (fd < 0)
		return -1;

	int parent_fd = openat(fd, "..", O_RDONLY|O_DIRECTORY);
	close(fd);
	if (parent_fd < 0)
		return -1;

	return remove_recursive(parent_fd, basename(path));
}

int main(int argc, char** argv) {
	LT_DEBUG_INIT();

	b8 help = 0;
	b8 color = 0;
	b8 force = 0;

	char* profile_path = ".";

	lt_darr(char*) args = lt_darr_create(char*, 32, alloc);
	lt_arg_iterator_t arg_it = lt_arg_iterator_create(argc, argv);
	while (lt_arg_next(&arg_it)) {
		if (lt_arg_flag(&arg_it, 'h', CLSTR("help"))) {
			help = 1;
			continue;
		}

		if (lt_arg_flag(&arg_it, 'v', CLSTR("verbose"))) {
			verbose = 1;
			continue;
		}

		if (lt_arg_flag(&arg_it, 'c', CLSTR("color"))) {
			color = 1;
			continue;
		}

		if (lt_arg_str(&arg_it, 'C', CLSTR("profile"), &profile_path))
			continue;

		if (lt_arg_flag(&arg_it, 0, CLSTR("force"))) {
			force = 1;
			continue;
		}

		lt_darr_push(args, *arg_it.it);
	}

	if (lt_darr_count(args) == 0 || help) {
		lt_printf(
			"usage: lmodorg [OPTIONS] COMMAND\n"
			"options:\n"
			"  -h, --help            Display this information.\n"
			"  -v, --verbose         Print debugging information to stderr.\n"
			"  -c, --color           Display output in multiple colors.\n"
			"  -C, --profile=PATH    Use profile at PATH.\n"
			"commands:\n"
			"  lmodorg mount [OUTPUT]     Mount VFS with output directory OUTPUT, if no\n"
			"                             OUTPUT is provided, OUTPUT is PROFILE/output.\n"
			"  lmodorg new NAMES...       Create and enable empty mods NAMES.\n"
			"  lmodorg remove NAMES...    Permanently delete mods NAMES.\n"
			"  lmodorg enable NAMES...    Enable mods NAMES.\n"
			"  lmodorg disable NAMES...   Disable mods NAMES.\n"
			"  lmodorg install NAME PATH  Install archive at PATH to new mod NAME.\n"
			"  lmodorg mods               List installed mods.\n"
			"  lmodorg active             List active mods.\n"
			"  lmodorg sort               Sort load order with LOOT.\n"
		);
		return 0;
	}

	lt_err_t err;

	if (chdir(profile_path) != 0)
		lt_ferrf("%s: %s\n", profile_path, lt_os_err_str());

	lstr_t conf_path = CLSTR("profile.conf");
	lstr_t conf_data;
	if ((err = lt_freadallp(conf_path, &conf_data, alloc)))
		lt_ferrf("failed to read '%S': %s\n", conf_path, lt_os_err_str());

	lt_conf_t cf;
	lt_conf_err_info_t err_info;
	if ((err = lt_conf_parse(&cf, conf_data.str, conf_data.len, &err_info, alloc)))
		lt_ferrf("failed to parse config file '%S': %S\n", conf_path, err_info.err_str);

	lt_conf_t* mods_cf = lt_conf_array(&cf, CLSTR("mods"));

	char* root_path = lt_cstr_from_lstr(lt_conf_str(&cf, CLSTR("game_root")), alloc);
	char* mods_path = "mods";

	mods_init();

	lt_darr(lstr_t) modlist = get_modlist(&cf);
	lt_darr(avail_mod_t) avail_mods = get_available_mods(mods_path);
	lt_darr(mod_t*) mods = get_mods(modlist, avail_mods);

	if (strcmp(args[0], "mount") == 0) {
		if (dir_mounted(root_path))
			lt_ferrf("an lmodorg vfs is already mounted in '%s'\n", root_path);

		copy_profile_configs(&cf);
		vfs_mount(argv[0], root_path, mods, "output");

		lt_term_init(0);
		lt_printf("vfs mounted, press ctrl+d to quit.\n");

		while (lt_term_getkey() != (LT_TERM_MOD_CTRL|'D'))
			;
		lt_term_restore();

		vfs_unmount();
	}

	else if (strcmp(args[0], "new") == 0) {
		if (dir_mounted(root_path) && !force)
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");

		if (lt_darr_count(args) < 2)
			lt_ferrf("expected a name after 'new'\n");

		for (usz i = 1; i < lt_darr_count(args); ++i) {
			lstr_t arg = lt_lstr_from_cstr(args[i]);

			if (mod_exists(avail_mods, arg)) {
				lt_werrf("mod '%S' already exists, skipping...\n");
				continue;
			}

			char* path = lt_lstr_build(alloc, "%s/%S%c", mods_path, arg, 0).str;

			int res = mkdir(path, 0755);
			if (res < 0) {
				lt_werrf("failed to create directory '%s': %s\n", path, lt_os_err_str());
				lt_mfree(alloc, path);
				continue;
			}
			lt_mfree(alloc, path);

			if (mod_enabled(modlist, arg)) {
				if (verbose)
					lt_werrf("mod '%S' is already enabled, skipping...\n");
				continue;
			}

			lt_conf_t new_conf = {
					.stype = LT_CONF_STRING,
					.str_val = arg };
			lt_conf_add_child(mods_cf, &new_conf);
		}

		update_config(conf_path, &cf);
	}

	else if (strcmp(args[0], "remove") == 0) {
		if (dir_mounted(root_path) && !force)
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");

		if (lt_darr_count(args) < 2)
			lt_ferrf("expected a name after 'remove'\n");

		for (usz i = 1; i < lt_darr_count(args); ++i) {
			char* path = lt_lstr_build(alloc, "%s/%s%c", mods_path, args[i], 0).str;

			int res = rmdir_recursive(path);
			if (res < 0)
				lt_werrf("failed to remove '%s': %s\n", path, lt_os_err_str());
			lt_mfree(alloc, path);

			lt_conf_erase_str(mods_cf, lt_lstr_from_cstr(args[i]), alloc);
		}

		update_config(conf_path, &cf);
	}

	else if (strcmp(args[0], "enable") == 0) {
		if (dir_mounted(root_path) && !force)
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");

		if (lt_darr_count(args) < 2)
			lt_ferrf("expected a name after 'enable'\n");

		for (usz i = 1; i < lt_darr_count(args); ++i) {
			lstr_t arg = lt_lstr_from_cstr(args[i]);

			if (!mod_exists(avail_mods, arg)) {
				lt_werrf("mod '%S' is not present in mods directory, skipping...\n");
				continue;
			}
			if (mod_enabled(modlist, arg)) {
				if (verbose)
					lt_werrf("mod '%S' is already enabled, skipping...\n");
				continue;
			}

			lt_conf_t new_conf = {
					.stype = LT_CONF_STRING,
					.str_val = arg };
			lt_conf_add_child(mods_cf, &new_conf);
		}

		update_config(conf_path, &cf);
	}

	else if (strcmp(args[0], "disable") == 0) {
		if (dir_mounted(root_path) && !force)
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");

		if (lt_darr_count(args) < 2)
			lt_ferrf("expected a name after 'disable'\n");

		for (usz i = 1; i < lt_darr_count(args); ++i)
			lt_conf_erase_str(mods_cf, lt_lstr_from_cstr(args[i]), alloc);

		update_config(conf_path, &cf);
	}

	else if (strcmp(args[0], "mods") == 0) {
		for (usz i = 0; i < lt_darr_count(avail_mods); ++i) {
			lstr_t name = avail_mods[i].name;

			b8 is_enabled = mod_enabled(modlist, name);

			char* active = " ";
			if (is_enabled)
				active = "*";

			char* name_clr = "";
			char* reset = "";
			if (color) {
				if (is_enabled)
					name_clr = BOLD FG_BWHITE;
				else
					name_clr = FG_WHITE;
				reset = RESET;
			}

			lt_printf("%s %s%S%s\n", active, name_clr, name, reset);
		}
	}

	else if (strcmp(args[0], "active") == 0) {
		for (usz i = 0; i < lt_darr_count(modlist); ++i) {
			char* name_clr = "";
			char* reset = "";
			if (color) {
				name_clr = BOLD FG_BWHITE;
				reset = RESET;
			}

			lt_printf("* %uz. %s%S%s\n", i + 1, name_clr, modlist[i], reset);
		}
	}

	else if (strcmp(args[0], "install") == 0) {
		if (dir_mounted(root_path) && !force)
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");

		lt_ferrf("command not implemented\n");
	}

	else if (strcmp(args[0], "sort") == 0) {
		if (dir_mounted(root_path) && !force)
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");

		lt_ferrf("command not implemented\n");
	}

	else {
		lt_ferrf("unrecognized command '%s'\n", args[0]);
	}

	mods_terminate();

	lt_darr_destroy(modlist);
	for (usz i = 0; i < lt_darr_count(avail_mods); ++i) {
		lt_mfree(alloc, avail_mods[i].root_path);
		lt_mfree(alloc, avail_mods[i].name.str);
	}
	lt_darr_destroy(avail_mods);
	lt_darr_destroy(mods);

	lt_mfree(alloc, root_path);
	lt_conf_free(&cf, alloc);
	lt_mfree(alloc, conf_data.str);

	lt_darr_destroy(args);

	return 0;
}

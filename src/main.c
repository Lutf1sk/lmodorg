#include <lt/io.h>
#include <lt/mem.h>
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
#include <libgen.h>

#include "vfs.h"
#include "mod.h"
#include "fomod.h"

#define alloc lt_libc_heap

#include "darr.c"
#include "ini.c"

b8 verbose = 0;
b8 color = 0;

ini_t config;
char* profile_path;

arr_t(lstr_t) get_modlist() {
	arr_t(lstr_t) mods = arr_alloc(lstr_t);
	LT_ASSERT(mods);

	isz section_i = ini_find_section(&config, CLSTR("mods"));
	if (section_i < 0)
		return mods;

	ini_section_t* section = &config.sections[section_i];
	for (ini_line_t* it = section->lines, *end = it + section->line_count; it < end; ++it) {
		if (it->type == INI_LINE_VALUE && lt_lseq(ini_line_value(&config, it), CLSTR("1")))
			LT_ASSERT(mods = arr_push(mods, ini_line_key(&config, it)));
	}

	return mods;
}

#include <dirent.h>

arr_t(avail_mod_t) get_available_mods(char* path) {
	arr_t(avail_mod_t) mods = arr_alloc(avail_mod_t);
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
				.name = lt_strdup(alloc, lt_lsfroms(ent->d_name)),
				.root_path = lt_lsbuild(alloc, "%s/%s%c", path, ent->d_name, 0).str };
		LT_ASSERT(mods = arr_push(mods, mod));
	}
	closedir(mods_dir);

	return mods;
}

arr_t(mod_t*) get_mods(arr_t(lstr_t) modlist, arr_t(avail_mod_t) avail_mods) {
	arr_t(mod_t*) mods = arr_alloc(mod_t*);
	LT_ASSERT(mods);

	for (usz i = 0; i < arr_count(modlist); ++i) {
		avail_mod_t* avail_mod = NULL;
		for (usz j = 0; j < arr_count(avail_mods); ++j) {
			if (lt_lseq(modlist[i], avail_mods[j].name)) {
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

		LT_ASSERT(mods = arr_push(mods, mod));
	}

	return mods;
}

b8 mod_exists(arr_t(avail_mod_t) avail_mods, lstr_t name) {
	for (usz i = 0; i < arr_count(avail_mods); ++i)
		if (lt_lseq(avail_mods[i].name, name))
			return 1;
	return 0;
}

b8 mod_enabled(arr_t(lstr_t) modlist, lstr_t name) {
	for (usz i = 0; i < arr_count(modlist); ++i)
		if (lt_lseq(modlist[i], name))
			return 1;
	return 0;
}

#include <lt/strstream.h>

lstr_t expand_path(lstr_t path) {
	lt_strstream_t ss;
	lt_strstream_create(&ss, lt_libc_heap);

	char* it = path.str, *end = it + path.len;
	while (it < end) {
		if (*it != '$') {
			lt_strstream_writec(&ss, *it++);
			continue;
		}
		++it;

		if (end - it >= 7 && memcmp(it, "APPDATA", 7) == 0) {
			lstr_t replacement = ini_find_value(&config, CLSTR("paths"), CLSTR("appdata"));
			if (!replacement.len) {
				lt_werrf("path '%S' cannot be expanded, because [paths].appdata has not been set\n", path);
				return NLSTR(); // !! leaks
			}
			lt_strstream_writels(&ss, replacement);
			it += 7;
		}

		else if (end - it >= 4 && memcmp(it, "DATA", 4) == 0) {
			lstr_t replacement = ini_find_value(&config, CLSTR("paths"), CLSTR("data"));
			if (!replacement.len) {
				lt_werrf("path '%S' cannot be expanded, because [paths].data has not been set\n", path);
				return NLSTR(); // !! leaks
			}
			lt_strstream_writels(&ss, replacement);
			it += 4;
		}

		else if (end - it >= 7 && memcmp(it, "PROFILE", 7) == 0) {
			lt_strstream_writels(&ss, lt_lsfroms(profile_path));
			it += 7;
		}

		else {
			lt_strstream_writec(&ss, '$');
		}
	}

	return ss.str;
}


void copy_profile_configs(void) {
	isz section_i = ini_find_section(&config, CLSTR("copy_files"));
	if (section_i < 0)
		return;
	ini_section_t* section = &config.sections[section_i];

	usz copy_bufsz = LT_KB(64);
	char* copy_buf = lt_malloc(alloc, copy_bufsz);

	for (usz i = 0; i < section->line_count; ++i) {
		ini_line_t* line = &section->lines[i];
		if (line->type != INI_LINE_VALUE)
			continue;

		lstr_t to   = ini_line_key  (&config, line);
		lstr_t from = ini_line_value(&config, line);

		lstr_t exp_to   = expand_path(to);
		lstr_t exp_from = expand_path(from);

		lt_file_t* inf = lt_fopenp(exp_from, LT_FILE_R, 0, alloc);
		if (inf == NULL) {
			lt_werrf("failed to open '%S' for reading: %s\n", from, lt_os_err_str());
			continue;
		}

		lt_file_t* outf = lt_fopenp(exp_to, LT_FILE_W, 0, alloc);
		if (outf == NULL) {
			lt_werrf("failed to open '%S' for writing: %s\n", to, lt_os_err_str());
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

		lt_printf("copied '%S' to '%S'\n", exp_from, exp_to);
	}

	lt_mfree(alloc, copy_buf);
}

void case_adjust_data_path(lstr_t data_path) {
	lstr_t parent_path = lt_lsdirname(data_path);
	lt_dir_t* dir = lt_dopenp(parent_path, alloc);
	if (!dir) {
		lt_werrf("failed to open '%S': %s\n", parent_path, lt_os_err_str());
		return;
	}
	lt_foreach_dirent(ent, dir) {
		if (ent->type != LT_DIRENT_DIR) {
			continue;
		}
		if (lt_lseq_nocase(ent->name, CLSTR("data"))) {
			memcpy(data_path.str + data_path.len - 4, ent->name.str, 4);
			break;
		}
	}
	lt_dclose(dir, alloc);
}

#define LIST_LOADORDER 0
#define LIST_ARCHIVES 1
#define LIST_PLUGINS 2

static
lstr_t list_extensions[][4] = {
	{ CLSTR(".esm"), CLSTR(".esp"), CLSTR(".esl") },
	{ CLSTR(".bsa") },
	{ CLSTR(".esp") },
};

static
usz list_extension_counts[] = { 3, 1, 1 };

static
lstr_t default_paths[] = {
	CLSTR("$APPDATA/LoadOrder.txt"),
	CLSTR("$APPDATA/Archives.txt"),
	CLSTR("$APPDATA/Plugins.txt"),
};

static
lstr_t file_names[] = {
	CLSTR("LoadOrder.txt"),
	CLSTR("Archives.txt"),
	CLSTR("Plugins.txt"),
};

void build_list_file(lstr_t file_path, arr_t(lstr_t) data_paths, u32 type) {
	lstr_t* exts = list_extensions[type];
	usz ext_count = list_extension_counts[type];

	if (lt_lseq(file_path, CLSTR("default")))
		file_path = default_paths[type];

	file_path = expand_path(file_path);
	if (!file_path.len) {
		lt_werrf("no valid path provided, skipping generation of '%S'\n", file_names[type]);
		return;
	}

	lt_file_t* file = lt_fopenp(file_path, LT_FILE_W, LT_FILE_PERMIT_R|LT_FILE_PERMIT_W, alloc);
	if (!file) {
		lt_werrf("failed to open '%S': %S\n", file_path, lt_os_err_str());
		return;
	}

	for (usz i = 0; i < arr_count(data_paths); ++i) {
		lstr_t data_path = data_paths[i];

		lt_dir_t* dir = lt_dopenp(data_path, alloc);
		if (!dir)
			continue;

		char* prefix = "";
		if (type == LIST_PLUGINS)
			prefix = "*";

		lt_foreach_dirent(ent, dir) {
			if (ent->type != LT_DIRENT_FILE)
				continue;

			for (usz i = 0; i < ext_count; ++i) {
				if (lt_lssuffix(ent->name, exts[i])) {
					lt_fprintf(file, "%s%S\n", prefix, ent->name);
					break;
				}
			}
		}

		lt_dclose(dir, alloc);
	}

	lt_fclose(file, alloc);

	lt_printf("generated '%S'\n", file_path);
}

void autocreate_list_files(arr_t(lstr_t) data_dirs) {
	lstr_t loadorder_path = ini_find_value(&config, CLSTR("generate"), CLSTR("loadorder.txt"));
	lstr_t plugins_path   = ini_find_value(&config, CLSTR("generate"), CLSTR("plugins.txt"));
	lstr_t archives_path  = ini_find_value(&config, CLSTR("generate"), CLSTR("archives.txt"));

	if (loadorder_path.len)
		build_list_file(loadorder_path, data_dirs, LIST_LOADORDER);
	if (plugins_path.len)
		build_list_file(plugins_path, data_dirs, LIST_PLUGINS);
	if (archives_path.len)
		build_list_file(archives_path, data_dirs, LIST_ARCHIVES);
}

void update_config(lstr_t conf_path) {
	lt_file_t* fp = lt_fopenp(conf_path, LT_FILE_W, 0, alloc);
	if (!fp)
		lt_ferrf("failed to update config file: %s\n", lt_os_err_str());
	ini_write(&config, fp);
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

lt_err_t install_root(lstr_t in_path, lstr_t out_path) {
	lt_err_t err;

	if ((err = lt_dcopyp(in_path, out_path, NULL, 0, LT_DCOPY_MERGE, alloc))) {
		lt_werrf("failed to copy contents of '%S': %S\n", in_path, lt_err_str(err));
		lt_dremovep(out_path, alloc);
		return err;
	}

	lt_printf("installation complete\n");
	return LT_SUCCESS;
}

lt_err_t install_data(lstr_t in_path, lstr_t out_root_path) {
	lt_err_t err;

	lstr_t out_data_path = lt_lsbuild(alloc, "%S/Data", out_root_path);
	LT_ASSERT(out_data_path.str != NULL);

	if ((err = lt_dcopyp(in_path, out_data_path, NULL, 0, LT_DCOPY_MERGE, alloc))) {
		lt_werrf("failed to copy contents of '%S': %S\n", in_path, lt_err_str(err));
		lt_dremovep(out_root_path, alloc);
		goto err0;
	}

	lt_printf("installation complete\n");

err0:	lt_mfree(alloc, out_data_path.str);
	return err;
}

#define DIR_UNKN	0
#define DIR_ROOT	1
#define DIR_DATA	2
#define DIR_FOMOD	3

#include "fs_nocase.h"

u8 find_mod_dir(char* path, char** out_dir) {
	b8 is_root = 0;
	b8 is_data = 0;
	b8 dll_present = 0;
	b8 has_fomod_dir = 0;
	b8 is_valid_fomod = 0;

	char first_dir[256] = "";
	usz file_count = 0;

	DIR* dir = opendir(path);
	if (dir == NULL)
		lt_ferrf("failed to open '%s': %s\n", path, lt_os_err_str());
	struct dirent* ent;
	while ((ent = readdir(dir))) {
		lstr_t name = lt_lsfroms(ent->d_name);
		if (lt_lseq(name, CLSTR(".")) || lt_lseq(name, CLSTR("..")))
			continue;

		++file_count;

		if (file_count == 1 && ent->d_type == DT_DIR)
			memcpy(first_dir, name.str, name.len + 1);

		if (lt_lseq_nocase(name, CLSTR("fomod"))) {
			has_fomod_dir = 1;
			is_valid_fomod = fstatat_nocase(dirfd(dir), "fomod/ModuleConfig.xml", NULL, 0) >= 0;
		}
		else if (lt_lseq_nocase(name, CLSTR("Meshes")) ||
			lt_lseq_nocase(name, CLSTR("Scripts")) ||
			lt_lseq_nocase(name, CLSTR("Source")) ||
			lt_lseq_nocase(name, CLSTR("Textures")) ||
			lt_lseq_nocase(name, CLSTR("Interface")) ||
			lt_lseq_nocase(name, CLSTR("Strings")) ||
			lt_lseq_nocase(name, CLSTR("Video")) ||
			lt_lseq_nocase(name, CLSTR("Sound")) ||
			lt_lseq_nocase(name, CLSTR("SKSE")) ||
			lt_lseq_nocase(name, CLSTR("Shaders")) ||
			lt_lssuffix(name, CLSTR(".esp")) ||
			lt_lssuffix(name, CLSTR(".esm")) ||
			lt_lssuffix(name, CLSTR(".esl")) ||
			lt_lssuffix(name, CLSTR(".bsa")))
		{
			is_data = 1;
		}
		else if (lt_lseq_nocase(name, CLSTR("Data"))) {
			is_root = 1;
		}
		else if (lt_lssuffix(name, CLSTR(".dll"))) {
			dll_present = 1;
		}
	}
	closedir(dir);

	if (has_fomod_dir && is_valid_fomod) {
		*out_dir = strdup(path);
		return DIR_FOMOD;
	}
	if (is_data) {
		*out_dir = strdup(path);
		return DIR_DATA;
	}
	if (is_root) {
		*out_dir = strdup(path);
		return DIR_ROOT;
	}
	if (has_fomod_dir) {
		*out_dir = strdup(path);
		return DIR_FOMOD;
	}

	if (file_count == 1 && first_dir[0] != 0) {
		char* next_path = lt_lsbuild(alloc, "%s/%s%c", path, first_dir, 0).str;
		u8 type = find_mod_dir(next_path, out_dir);
		lt_mfree(alloc, next_path);
		return type;
	}

	if (dll_present) {
		*out_dir = strdup(path);
		return DIR_ROOT;
	}

	return DIR_UNKN;
}

#include <sys/wait.h>

int main(int argc, char** argv) {
	LT_DEBUG_INIT();

	lt_err_t err;

	b8 help = 0;
	b8 force = 0;

	profile_path = ".";

	arr_t(char*) args = arr_alloc(char*);

	lt_foreach_arg(arg, argc, argv) {
		if (lt_arg_flag(arg, 'h', CLSTR("help"))) {
			help = 1;
			continue;
		}

		if (lt_arg_flag(arg, 'v', CLSTR("verbose"))) {
			verbose = 1;
			continue;
		}

		if (lt_arg_flag(arg, 'c', CLSTR("color"))) {
			color = 1;
			continue;
		}

		if (lt_arg_str(arg, 'C', CLSTR("profile"), &profile_path))
			continue;

		if (lt_arg_flag(arg, 0, CLSTR("force"))) {
			force = 1;
			continue;
		}

		arr_push(args, *arg->it);
	}

	if (arr_count(args) == 0 || help) {
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
			"  lmodorg install NAME PATH  Install the archive at PATH.\n"
			"  lmodorg mods               List installed mods.\n"
			"  lmodorg active             List active mods.\n"
			"  lmodorg generate           Generate loadorder files without mounting a VFS.\n"
		);
		arr_free(args);
		return 0;
	}

	lstr_t conf_path = lt_lsbuild(alloc, "%s/profile.ini", profile_path);

	lstr_t conf_data;
	if ((err = lt_freadallp(conf_path, &conf_data, alloc)))
		lt_ferrf("failed to read '%S': %s\n", conf_path, lt_os_err_str());

	config = ini_parse(conf_data);
	if (config.error)
		lt_ferrf("failed to parse config file '%S'\n", conf_path);

	isz mods_section_i = ini_find_section(&config, CLSTR("mods"));
	if (mods_section_i < 0)
		mods_section_i = ini_add_section(&config, CLSTR("mods"));

	char* root_path = lt_lstos(ini_find_value(&config, CLSTR("paths"), CLSTR("game")), alloc);
	char* mods_path = lt_lsbuild(alloc, "%s/mods%c", profile_path, 0).str;
	char* output_path = lt_lsbuild(alloc, "%s/output%c", profile_path, 0).str;

	mods_init();

	arr_t(lstr_t) modlist = get_modlist();
	arr_t(avail_mod_t) avail_mods = get_available_mods(mods_path);
	arr_t(mod_t*) mods = get_mods(modlist, avail_mods);

	arr_t(lstr_t) data_dirs = arr_alloc(lstr_t);
	LT_ASSERT(data_dirs = arr_push(data_dirs, lt_lsbuild(alloc, "%s/data", root_path)));
	LT_ASSERT(data_dirs = arr_push(data_dirs, lt_lsbuild(alloc, "%s/data", output_path)));
	for (usz i = 0; i < arr_count(mods); ++i) {
		LT_ASSERT(data_dirs = arr_push(data_dirs, lt_lsbuild(alloc, "%s/mods/%S/data", profile_path, mods[i]->name)));
	}
	for (usz i = 0; i < arr_count(data_dirs); ++i) {
		case_adjust_data_path(data_dirs[i]);
	}

	if (strcmp(args[0], "mount") == 0) {
		if (dir_mounted(root_path)) {
			lt_ferrf("an lmodorg vfs is already mounted in '%s'\n", root_path);
		}

		autocreate_list_files(data_dirs);

		copy_profile_configs();

		vfs_mount(argv[0], root_path, mods, output_path);

		lt_term_init(0);
		lt_printf("vfs mounted, press ctrl+d to quit.\n");

		while (lt_term_getkey() != (LT_TERM_MOD_CTRL|'D'))
			;
		lt_term_restore();

		vfs_unmount();
	}

	else if (strcmp(args[0], "new") == 0) {
		if (dir_mounted(root_path) && !force) {
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");
		}

		if (arr_count(args) < 2) {
			lt_ferrf("expected a name after 'new'\n");
		}

		for (usz i = 1; i < arr_count(args); ++i) {
			lstr_t arg = lt_lsfroms(args[i]);

			if (mod_exists(avail_mods, arg)) {
				lt_werrf("mod '%S' already exists, skipping...\n", arg);
				continue;
			}

			lstr_t path = lt_lsbuild(alloc, "%s/%S", mods_path, arg);
			err = lt_mkdir(path);
			lt_mfree(alloc, path.str);

			if (err != LT_SUCCESS) {
				lt_werrf("failed to create '%S': %S\n", path, lt_err_str(err));
				continue;
			}

			if (mod_enabled(modlist, arg)) {
				if (verbose) {
					lt_werrf("mod '%S' is already enabled, skipping...\n", arg);
				}
				continue;
			}

			ini_add_value(&config, mods_section_i, arg, CLSTR("1"));
		}

		update_config(conf_path);
	}

	else if (strcmp(args[0], "remove") == 0) {
		if (dir_mounted(root_path) && !force) {
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");
		}

		if (arr_count(args) < 2) {
			lt_ferrf("expected a name after 'remove'\n");
		}

		for (usz i = 1; i < arr_count(args); ++i) {
			lstr_t path = lt_lsbuild(alloc, "%s/%s", mods_path, args[i]);

			if ((err = lt_dremovep(path, alloc))) {
				lt_werrf("failed to remove '%S': %S\n", path, lt_err_str(err));
			}
			lt_mfree(alloc, path.str);

			ini_remove_value(&config, mods_section_i, lt_lsfroms(args[i]));
		}

		update_config(conf_path);
	}

	else if (strcmp(args[0], "enable") == 0) {
		if (dir_mounted(root_path) && !force) {
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");
		}

		if (arr_count(args) < 2) {
			lt_ferrf("expected a name after 'enable'\n");
		}

		for (usz i = 1; i < arr_count(args); ++i) {
			lstr_t arg = lt_lsfroms(args[i]);

			if (!mod_exists(avail_mods, arg)) {
				lt_werrf("mod '%S' is not present in mods directory, skipping...\n", arg);
				continue;
			}
			if (mod_enabled(modlist, arg)) {
				if (verbose)
					lt_werrf("mod '%S' is already enabled, skipping...\n", arg);
				continue;
			}

			ini_set_value(&config, mods_section_i, arg, CLSTR("1"));
		}

		update_config(conf_path);
	}

	else if (strcmp(args[0], "disable") == 0) {
		if (dir_mounted(root_path) && !force) {
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");
		}

		if (arr_count(args) < 2) {
			lt_ferrf("expected a name after 'disable'\n");
		}

		for (usz i = 1; i < arr_count(args); ++i)
			ini_set_value(&config, mods_section_i, lt_lsfroms(args[i]), CLSTR("0"));

		update_config(conf_path);
	}

	else if (strcmp(args[0], "mods") == 0) {
		if (arr_count(args) != 1) {
			lt_ferrf("command 'mods' takes no arguments\n");
		}

		for (usz i = 0; i < arr_count(avail_mods); ++i) {
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
		if (arr_count(args) != 1) {
			lt_ferrf("command 'active' takes no arguments\n");
		}

		for (usz i = 0; i < arr_count(modlist); ++i) {
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
		if (dir_mounted(root_path) && !force) {
			lt_ferrf("profiles should not be edited while mounted, rerun with '--force' to try anyway\n");
		}
		if (arr_count(args) < 3) {
			lt_ferrf("expected two arguments after 'install'\n");
		}

		if (mod_exists(avail_mods, lt_lsfroms(args[1]))) {
			lt_ferrf("mod '%s' already exists\n", args[1]);
		}

		char* src_path = args[2];
		lt_stat_t st;
		if ((err = lt_statp(lt_lsfroms(src_path), &st))) {
			lt_ferrf("failed to stat '%S': %S\n", src_path, lt_err_str(err));
		}

		if (st.type == LT_DIRENT_FILE) {
			char* tmp_src_path = lt_lsbuild(alloc, "%s/tmp/%s%c", profile_path, args[1], 0).str; // !! leaked

			lt_mkpath(lt_lsdirname(lt_lsfroms(tmp_src_path)));

			char* exec_path = "/bin/7z";
			pid_t child = fork();
			if (child == 0) {
				if (lt_lssuffix(lt_lsfroms(src_path), CLSTR(".rar"))) {
					exec_path = "/bin/unrar";
					char* out_switch = lt_lsbuild(alloc, "-op%s%c", tmp_src_path, 0).str;
					execl(exec_path, exec_path, "x", "-y", out_switch, "--", src_path, (char*)NULL);
					lt_mfree(alloc, out_switch);
				}
				else {
					char* out_switch = lt_lsbuild(alloc, "-o%s%c", tmp_src_path, 0).str;
					execl(exec_path, exec_path, "x", "-y", out_switch, "--", src_path, (char*)NULL);
					lt_mfree(alloc, out_switch);
				}
			}
			if (child < 0)
				lt_ferrf("failed to launch '%s': %s\n", exec_path, lt_os_err_str());

			int status;
			wait(&status);
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				lt_printf("archive extraction successful\n");
			else
				lt_ferrf("archive extraction failed with code %ud\n", WEXITSTATUS(status));

			src_path = tmp_src_path;
		}

		char* out_path = lt_lsbuild(alloc, "%s/%s%c", mods_path, args[1], 0).str;

		if ((err = lt_mkdir(lt_lsfroms(out_path)))) {
			lt_ferrf("failed to create mod directory '%s': %S\n", out_path, lt_err_str(err));
		}

		char* mod_path = NULL;
		u8 mod_type = find_mod_dir(src_path, &mod_path);
		if (mod_type == DIR_UNKN) {
			lt_ferrf("failed to identify mod type; manual installation required\n");
		}

		switch (mod_type) {
		case DIR_FOMOD:
			lt_printf("identified '%s' as fomod root\n", mod_path);

			if (dir_mounted(root_path)) {
				lt_ferrf("an lmodorg vfs is already mounted in '%s'\n", root_path);
			}

			char* root_data_path = lt_lsbuild(alloc, "%s/Data%c", root_path, 0).str;
			char* out_data_path = lt_lsbuild(alloc, "%s/data%c", out_path, 0).str;

			if ((err = lt_mkdir(lt_lsfroms(out_data_path)))) {
				lt_ferrf("failed to create data directory '%s': %S\n", out_data_path, lt_err_str(err));
			}

			vfs_mount(argv[0], root_path, mods, out_path);
			int res = fomod_install(mod_path, out_data_path, root_data_path);
			vfs_unmount();
			lt_mfree(alloc, root_data_path);

			if (res < 0) {
				lt_dremovep(lt_lsfroms(out_path), alloc);
				lt_ferrf("failed to install mod\n");
			}
			lt_printf("installation complete\n");
			break;

		case DIR_ROOT:
			lt_printf("identified '%s' as mod root\n", mod_path);
			if ((err = install_root(lt_lsfroms(mod_path), lt_lsfroms(out_path)))) {
				lt_dremovep(lt_lsfroms(out_path), alloc);
				lt_ferrf("failed to install mod\n");
			}
			break;

		case DIR_DATA:
			lt_printf("identified '%s' as data directory\n", mod_path);
			if ((err = install_data(lt_lsfroms(mod_path), lt_lsfroms(out_path)))) {
				lt_dremovep(lt_lsfroms(out_path), alloc);
				lt_ferrf("failed to install mod\n");
			}
			break;

		default:
			lt_ferrf("unreachable state reached, something has gone terribly wrong\n");
		}

		lt_mfree(alloc, out_path);
		lt_mfree(alloc, mod_path);
	}

	else if (strcmp(args[0], "generate") == 0) {
		if (arr_count(args) != 1) {
			lt_ferrf("command 'generate' takes no arguments\n");
		}

		autocreate_list_files(data_dirs);
	}

	else {
		lt_ferrf("unrecognized command '%s'\n", args[0]);
	}

	mods_terminate();

	for (usz i = 0; i < arr_count(data_dirs); ++i) {
		lt_mfree(alloc, data_dirs[i].str);
	}
	arr_free(data_dirs);

	arr_free(modlist);
	for (usz i = 0; i < arr_count(avail_mods); ++i) {
		lt_mfree(alloc, avail_mods[i].root_path);
		lt_mfree(alloc, avail_mods[i].name.str);
	}
	arr_free(avail_mods);
	arr_free(mods);

	lt_mfree(alloc, output_path);
	lt_mfree(alloc, mods_path);
	lt_mfree(alloc, root_path);
	ini_free(&config);
	lt_mfree(alloc, conf_data.str);
	lt_mfree(alloc, conf_path.str);

	arr_free(args);
	return 0;
}

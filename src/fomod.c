#include <lt/xml.h>
#include <lt/mem.h>
#include <lt/io.h>
#include <lt/str.h>
#include <lt/darr.h>
#include <lt/term.h>
#include <lt/text.h>
#include <lt/texted.h>
#include <lt/strstream.h>
#include <lt/ctype.h>

#define LT_ANSI_SHORTEN_NAMES 1
#include <lt/ansi.h>

#include "fs_nocase.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define alloc lt_libc_heap

typedef
struct install {
	lstr_t path;
	lstr_t install_path;
	b8 always;
	b8 always_if_usable;
	i64 priority;
} install_t;

typedef
struct flag {
	lstr_t key;
	lstr_t val;
} flag_t;

typedef
struct dep_plugin_type {
	u8 type;
	lt_xml_entity_t* cond;
} dep_plugin_type_t;

typedef
struct plugin {
	lstr_t name;
	lstr_t description;

	lt_darr(install_t) files;
	lt_darr(install_t) dirs;

	lt_darr(flag_t) flags;

	u8 default_type;
	lt_darr(dep_plugin_type_t) dep_types;

	u8 eval_type;
	b8 selected;
} plugin_t;

typedef
struct group {
	u8 order;
	u8 type;
	lstr_t name;
	lt_darr(plugin_t) plugins;
} group_t;

typedef
struct install_step {
	u8 order;
	lt_xml_entity_t* visible_cond;
	lstr_t name;
	lt_darr(group_t) groups;
} install_step_t;

typedef
struct fomod {
	int data_fd;

	// config
	lstr_t name;
	lstr_t author;
	lstr_t id;
	lstr_t version;
	lstr_t description;
	lstr_t website;

	lstr_t machine_version;

	// module
	lstr_t module_name;
	lt_darr(install_t) files;
	lt_darr(install_t) dirs;
	lt_darr(install_t) install_files;
	lt_darr(install_t) install_dirs;

	lt_xml_entity_t* cond_install_files;

	lt_darr(flag_t) flags;

	u8 step_order;
	lt_darr(install_step_t) install_steps;
} fomod_t;

extern b8 color;

static
int load_info(fomod_t* fomod, lstr_t path) {
	int ret = -1;

	lstr_t info_data;
	if (lt_freadallp_utf8(path, &info_data, alloc) != LT_SUCCESS) {
		lt_werrf("failed to read '%S': \n", path);
		goto err0;
	}

	lt_xml_entity_t info_xm;
	if (lt_xml_parse(&info_xm, info_data.str, info_data.len, NULL, alloc) != LT_SUCCESS) {
		lt_werrf("failed to parse '%S'\n", path);
		goto err1;
	}

	lt_xml_entity_t* fomod_xm = NULL;

	usz child_count = lt_xml_child_count(&info_xm);
	for (usz i = 0; i < child_count; ++i) {
		if (info_xm.elem.children[i].type == LT_XML_ELEMENT && lt_lseq(info_xm.elem.children[i].elem.name, CLSTR("fomod"))) {
			fomod_xm = &info_xm.elem.children[i];
			break;
		}
	}

	if (fomod_xm == NULL) {
		lt_werrf("info.xml missing 'fomod' element\n");
		goto err1;
	}

	lstr_t* out_names[] = { &fomod->name, &fomod->author, &fomod->id, &fomod->version, &fomod->description, &fomod->website };
	lstr_t elem_names[] = { CLSTR("Name"), CLSTR("Author"), CLSTR("Id"), CLSTR("Version"), CLSTR("Description"), CLSTR("Website") };
	usz named_elem_count = sizeof(elem_names) / sizeof(*elem_names);

	child_count = lt_xml_child_count(fomod_xm);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* ent = &fomod_xm->elem.children[i];
		if (ent->type != LT_XML_ELEMENT)
			continue;

		for (usz i = 0; i < named_elem_count; ++i) {
			if (!lt_lseq(ent->elem.name, elem_names[i]))
				continue;
			if (lt_xml_generate_str(ent, out_names[i], alloc) != LT_SUCCESS)
				lt_werrf("xml generation failed for element '%S'", elem_names[i]);
			goto next_child;
		}

		lt_werrf("unknown fomod element '%S'\n", ent->elem.name);
	next_child:
	}

	ret = 0;

err1:	lt_xml_free(&info_xm, alloc);
err0:	lt_mfree(alloc, info_data.str);
		return ret;
}

b8 find_elements(lt_xml_entity_t* parent, usz count, lstr_t* names, b8* required, lt_xml_entity_t** out) {
	memset(out, 0, count * sizeof(lt_xml_entity_t*));

	usz child_count = lt_xml_child_count(parent);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &parent->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		for (usz i = 0; i < count; ++i) {
			if (!lt_lseq(names[i], child->elem.name))
				continue;

			if (out[i] != NULL) {
				lt_werrf("'%S' has multiple elements of type '%S'\n", parent->elem.name, names[i]);
				continue;
			}

			out[i] = child;
			break;
		}
	}

	b8 ret = 1;
	for (usz i = 0; i < count; ++i) {
		if (out[i] == NULL && required[i]) {
			lt_werrf("'%S' missing required element '%S'\n", parent->elem.name, names[i]);
			ret = 0;
		}
	}

	return ret;
}

#define OP_AND 0
#define OP_OR 1

b8 eval_oper(u8 oper, b8 val1, b8 val2) {
	if (oper == OP_AND)
		return val1 && val2;
	if (oper == OP_OR)
		return val1 || val2;
	LT_ASSERT_NOT_REACHED();
	return 0;
}

#define FILESTATE_MISSING 0
#define FILESTATE_INACTIVE 1
#define FILESTATE_ACTIVE 2

b8 eval_file_dep(fomod_t* fomod, lt_xml_entity_t* dep) {
	lt_xml_attrib_t* file_attr = lt_xml_find_attrib(dep, CLSTR("file"));
	lt_xml_attrib_t* state_attr = lt_xml_find_attrib(dep, CLSTR("state"));

	if (file_attr == NULL || state_attr == NULL) {
		lt_werrf("file dependency missing required attribute\n");
		return 0;
	}

	char* file_path = lt_lstos(file_attr->val, alloc);
	struct stat st;
	int res = fstatat_nocase(fomod->data_fd, file_path, &st, 0);
	lt_mfree(alloc, file_path);

	b8 exists = res >= 0 && S_ISREG(st.st_mode);

	if (lt_lseq(state_attr->val, CLSTR("Active")))
		return exists;
	else if (lt_lseq(state_attr->val, CLSTR("Inactive")))
		return !exists; // !!
	else if (lt_lseq(state_attr->val, CLSTR("Missing")))
		return !exists;
	else
		lt_werrf("unknown file dependency state '%S'\n", state_attr->val);
	return 0;
}

b8 eval_flag_dep(fomod_t* fomod, lt_xml_entity_t* dep) {
	lt_xml_attrib_t* flag_attr = lt_xml_find_attrib(dep, CLSTR("flag"));
	lt_xml_attrib_t* value_attr = lt_xml_find_attrib(dep, CLSTR("value"));

	if (flag_attr == NULL || value_attr == NULL) {
		lt_werrf("flag dependency missing required attribute\n");
		return 0;
	}

	lstr_t key = flag_attr->val;
	lstr_t val = value_attr->val;

	for (usz i = 0; i < lt_darr_count(fomod->flags); ++i)
		if (lt_lseq_nocase(fomod->flags[i].key, key) && lt_lseq_nocase(fomod->flags[i].val, val))
			return 1;
	return 0;
}

b8 eval_game_dep(fomod_t* fomod, lt_xml_entity_t* dep) {
	return 1; // !!
}

b8 eval_fomm_dep(fomod_t* fomod, lt_xml_entity_t* dep) {
	return 1; // !!
}

b8 eval_deps(fomod_t* fomod, lt_xml_entity_t* conds) {
	usz child_count = lt_xml_child_count(conds);
	if (child_count == 0) {
		lt_werrf("dependency element with no conditions\n");
		return 0;
	}

	u8 oper;
	b8 val;

	lt_xml_attrib_t* attrib = lt_xml_find_attrib(conds, CLSTR("operator"));
	if (attrib == NULL) {
		oper = OP_AND;
		val = 1;
	}
	else if (lt_lseq(attrib->val, CLSTR("And"))) {
		oper = OP_AND;
		val = 1;
	}
	else if (lt_lseq(attrib->val, CLSTR("Or"))) {
		oper = OP_OR;
		val = 0;
	}
	else {
		lt_werrf("unknown dependecy operator '%S'\n", attrib->val);
		return 0;
	}

	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &conds->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		if (lt_lseq(child->elem.name, CLSTR("fileDependency")))
			val = eval_oper(oper, val, eval_file_dep(fomod, child));
		else if (lt_lseq(child->elem.name, CLSTR("flagDependency")))
			val = eval_oper(oper, val, eval_flag_dep(fomod, child));
		else if (lt_lseq(child->elem.name, CLSTR("gameDependency")))
			val = eval_oper(oper, val, eval_game_dep(fomod, child));
		else if (lt_lseq(child->elem.name, CLSTR("fommDependency")))
			val = eval_oper(oper, val, eval_fomm_dep(fomod, child));
		else if (lt_lseq(child->elem.name, CLSTR("dependencies")))
			val = eval_oper(oper, val, eval_deps(fomod, child));
		else
			lt_werrf("unknown dependency element '%S'\n", child->elem.name);
	}

	return val;
}

b8 str_to_bool(lstr_t str) {
	if (lt_lseq_nocase(str, CLSTR("true")))
		return 1;
	else if (lt_lseq_nocase(str, CLSTR("false")))
		return 0;
	else
		lt_printf("unknown boolean value '%S'\n", str);
	return 0;
}

i64 str_to_int(lstr_t str) {
	i64 v = 0;
	if (lt_lstoi(str, &v) != LT_SUCCESS)
		lt_werrf("invalid integer '%S'\n", str);
	return v;
}

int load_install(lt_darr(install_t)* arr, lt_xml_entity_t* xml) {
	lt_xml_attrib_t* src_attr = lt_xml_find_attrib(xml, CLSTR("source"));
	lt_xml_attrib_t* dst_attr = lt_xml_find_attrib(xml, CLSTR("destination"));
	lt_xml_attrib_t* always_attr = lt_xml_find_attrib(xml, CLSTR("alwaysInstall"));
	lt_xml_attrib_t* ifusable_attr = lt_xml_find_attrib(xml, CLSTR("installIfUsable"));
	lt_xml_attrib_t* priority_attr = lt_xml_find_attrib(xml, CLSTR("priority"));

	if (src_attr == NULL) {
		lt_printf("element '%S' missing required attribute 'source'\n", xml->elem.name);
		return -1;
	}

	lstr_t dst = dst_attr ? dst_attr->val : src_attr->val;
	b8 always = always_attr ? str_to_bool(always_attr->val) : 0;
	b8 ifusable = ifusable_attr ? str_to_bool(ifusable_attr->val) : 0;
	usz priority = priority_attr ? str_to_int(priority_attr->val) : 0;

	lt_darr_push(*arr, (install_t) {
			.path = src_attr->val,
			.install_path = dst,
			.always = always,
			.always_if_usable = ifusable,
			.priority = priority });

	return 0;
}

int load_file_list(lt_darr(install_t)* install_files, lt_darr(install_t)* install_dirs, lt_xml_entity_t* list) {
	usz child_count = lt_xml_child_count(list);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &list->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		if (lt_lseq(child->elem.name, CLSTR("file")))
			load_install(install_files, child);
		else if (lt_lseq(child->elem.name, CLSTR("folder")))
			load_install(install_dirs, child);
		else
			lt_werrf("unknown file list element '%S'\n", child->elem.name);
	}

	return 0;
}

int load_flag_list(lt_darr(flag_t)* flags, lt_xml_entity_t* list) {
	usz child_count = lt_xml_child_count(list);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &list->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		if (!lt_lseq(child->elem.name, CLSTR("flag"))) {
			lt_werrf("unknown file list element '%S'\n", child->elem.name);
			continue;
		}

		lt_xml_attrib_t* name_attrib = lt_xml_find_attrib(child, CLSTR("name"));
		if (name_attrib == NULL) {
			lt_werrf("flag missing required attribute 'name'\n");
			continue;
		}

		lstr_t val;
		if (lt_xml_generate_str(child, &val, alloc) != LT_SUCCESS) {
			lt_werrf("failed to generate flag value\n");
			continue;
		}

		lt_darr_push(*flags, (flag_t) { .key = name_attrib->val, .val = lt_strdup(alloc, lt_lstrim(val)) });
		lt_mfree(alloc, val.str);
	}

	return 0;
}

#define GRP_ONEORMORE 0
#define GRP_ONEORZERO 1
#define GRP_SELECTONE 2
#define GRP_SELECTALL 3
#define GRP_SELECTANY 4

u8 str_to_group_type(lt_xml_attrib_t* order_attrib) {
	if (order_attrib == NULL) {
		lt_werrf("missing required group type attribute\n");
		return GRP_SELECTANY;
	}

	static lstr_t strs[] = { CLSTR("SelectAtLeastOne"), CLSTR("SelectAtMostOne"), CLSTR("SelectExactlyOne"), CLSTR("SelectAll"), CLSTR("SelectAny") };
	for (usz i = 0; i < sizeof(strs) / sizeof(*strs); ++i)
		if (lt_lseq(order_attrib->val, strs[i]))
			return i;

	lt_werrf("unknown group type '%S'\n", order_attrib->val);
	return GRP_SELECTANY;
}

static LT_INLINE
lstr_t group_prompt(u8 type) {
	static lstr_t prompts[] = {
		[GRP_ONEORMORE] = CLSTR("select at least one option (eg: 1 2 3)"),
		[GRP_ONEORZERO] = CLSTR("select at most one option (eg: 1 2 3)"),
		[GRP_SELECTONE] = CLSTR("select one option (eg: 1 2 3)"),
		[GRP_SELECTALL] = CLSTR("these plugins will be installed, press ctrl+d to cancel"),
		[GRP_SELECTANY] = CLSTR("select options (eg: 1 2 3)"),
	};

	return prompts[type];
}

static LT_INLINE
b8 group_selection_validate(u8 group_type, lt_darr(u64) selection) {
	switch (group_type) {
	case GRP_ONEORMORE:
		if (lt_darr_count(selection) <= 0) {
			lt_printf("you must select at least one option\n");
			return 0;
		}
		return 1;

	case GRP_ONEORZERO:
		if (lt_darr_count(selection) > 1) {
			lt_printf("cannot select more than one option\n");
			return 0;
		}
		return 1;

	case GRP_SELECTONE:
		if (lt_darr_count(selection) <= 0) {
			lt_printf("you must select an option\n");
			return 0;
		}
		if (lt_darr_count(selection) > 1) {
			lt_printf("cannot select more than one option\n");
			return 0;
		}
		return 1;

	case GRP_SELECTALL:
		if (lt_darr_count(selection) != 0) {
			lt_printf("cannot select specific options when all options are required\n");
			return 0;
		}
		return 1;

	case GRP_SELECTANY:
		return 1;
	}

	LT_ASSERT_NOT_REACHED();
	return 0;
}

#define ORD_EXPLICIT	0
#define ORD_ASCENDING	1
#define ORD_DESCENDING	2

u8 str_to_order(lt_xml_attrib_t* order_attrib) {
	if (!order_attrib)
		return ORD_ASCENDING;

	if (lt_lseq(order_attrib->val, CLSTR("Ascending")))
		return ORD_ASCENDING;
	else if (lt_lseq(order_attrib->val, CLSTR("Descending")))
		return ORD_DESCENDING;
	else if (lt_lseq(order_attrib->val, CLSTR("Explicit")))
		return ORD_EXPLICIT;

	lt_werrf("unknown sorting order '%S'\n", order_attrib->val);
	return ORD_ASCENDING;
}

#define PLG_REQUIRED		0
#define PLG_OPTIONAL		1
#define PLG_RECOMMENDED		2
#define PLG_NOTUSABLE		3
#define PLG_COULDBEUSABLE	4

char* plugin_type_strs[] = { "REQUIRED", "OPTIONAL", "RECOMMENDED", "NOTUSABLE", "COULDBEUSABLE" };

u8 str_to_plugin_type(lt_xml_attrib_t* type_attrib) {
	if (type_attrib == NULL) {
		lt_werrf("missing required plugin type attribute\n");
		return PLG_OPTIONAL;
	}

	static lstr_t strs[] = { CLSTR("Required"), CLSTR("Optional"), CLSTR("Recommended"), CLSTR("NotUsable"), CLSTR("CouldBeUsable") };
	for (usz i = 0; i < sizeof(strs) / sizeof(*strs); ++i)
		if (lt_lseq(type_attrib->val, strs[i]))
			return i;

	lt_werrf("unknown plugin type '%S'\n", type_attrib->val);
	return PLG_OPTIONAL;
}

int load_dep_pattern_list(fomod_t* fomod, lt_darr(dep_plugin_type_t)* types, lt_xml_entity_t* type_list) {
	usz child_count = lt_xml_child_count(type_list);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &type_list->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;
		if (!lt_lseq(child->elem.name, CLSTR("pattern"))) {
			lt_werrf("unknown dependency pattern list element '%S'\n", child->elem.name);
			continue;
		}

		lt_xml_entity_t* children[2];
		lstr_t names[2] = { CLSTR("dependencies"), CLSTR("type") };
		b8 required[2] = { 1, 1 };
		if (!find_elements(child, 2, names, required, children))
			continue;

		lt_darr_push(*types, (dep_plugin_type_t) {
				.type = str_to_plugin_type(lt_xml_find_attrib(children[1], CLSTR("name"))),
				.cond = children[0] });
	}

	return 0;
}

int load_plugin_list(fomod_t* fomod, lt_darr(plugin_t)* plugins, lt_xml_entity_t* plugin_list) {
	usz child_count = lt_xml_child_count(plugin_list);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &plugin_list->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;
		if (!lt_lseq(child->elem.name, CLSTR("plugin"))) {
			lt_werrf("unknown plugin element '%S'\n", child->elem.name);
			continue;
		}

		lt_xml_entity_t* children[5];
		lstr_t names[5] = { CLSTR("description"), CLSTR("image"), CLSTR("files"), CLSTR("conditionFlags"), CLSTR("typeDescriptor") };
		b8 required[5] = { 1, 0, 0, 0, 1 };
		if (!find_elements(child, 5, names, required, children))
			continue;
		lt_xml_entity_t* description = children[0], *files_xm = children[2], *flags_xm = children[3], *type_desc = children[4];

		lt_xml_attrib_t* name_attrib = lt_xml_find_attrib(child, CLSTR("name"));
		if (name_attrib == NULL) {
			lt_werrf("plugin missing required attribute 'name'\n");
			continue;
		}

		lstr_t desc_str;
		if (lt_xml_generate_str(description, &desc_str, alloc) != LT_SUCCESS) {
			lt_werrf("failed to generate plugin description\n");
			continue;
		}

		lt_xml_entity_t* tdesc_children[2];
		lstr_t tdesc_names[2] = { CLSTR("type"), CLSTR("dependencyType") };
		b8 tdesc_required[2] = { 0, 0 };
		if (!find_elements(type_desc, 2, tdesc_names, tdesc_required, tdesc_children))
			continue;
		lt_xml_entity_t* plugin_type = tdesc_children[0], *dep_type = tdesc_children[1];

		if (plugin_type == NULL && dep_type == NULL) {
			lt_werrf("plugin type descriptor missing required 'type' or 'dependencyType'\n");
			continue;
		}
		if (plugin_type != NULL && dep_type != NULL) {
			lt_werrf("plugin type descriptor has both 'type' and 'dependencyType'\n");
			continue;
		}

		lt_darr(dep_plugin_type_t) dep_types = lt_darr_create(dep_plugin_type_t, 4, alloc);

		u8 default_type = PLG_OPTIONAL;

		if (dep_type) {
			lt_xml_entity_t* children[2];
			lstr_t names[2] = { CLSTR("defaultType"), CLSTR("patterns") };
			b8 required[2] = { 1, 1 };
			if (find_elements(dep_type, 2, names, required, children)) {
				default_type = str_to_plugin_type(lt_xml_find_attrib(children[0], CLSTR("name")));
				load_dep_pattern_list(fomod, &dep_types, children[1]);
			}
		}
		else
			default_type = str_to_plugin_type(lt_xml_find_attrib(plugin_type, CLSTR("name")));

		lt_darr(install_t) files = lt_darr_create(install_t, 4, alloc);
		lt_darr(install_t) dirs = lt_darr_create(install_t, 4, alloc);
		lt_darr(flag_t) flags = lt_darr_create(flag_t, 4, alloc);

		if (files_xm != NULL)
			load_file_list(&files, &dirs, files_xm);
		if (flags_xm != NULL)
			load_flag_list(&flags, flags_xm);

		lt_darr_push(*plugins, (plugin_t) {
				.name = name_attrib->val,
				.description = desc_str,
				.default_type = default_type,
				.dep_types = dep_types,
				.files = files,
				.dirs = dirs,
				.flags = flags });
	}
	return 0;
}

int load_group_list(fomod_t* fomod, lt_darr(group_t)* groups, lt_xml_entity_t* group_list) {
	usz child_count = lt_xml_child_count(group_list);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &group_list->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;
		if (!lt_lseq(child->elem.name, CLSTR("group"))) {
			lt_werrf("unknown groupList element '%S'\n", child->elem.name);
			continue;
		}

		lt_xml_entity_t* group = child;

		u8 group_type = str_to_group_type(lt_xml_find_attrib(group, CLSTR("type")));
		lt_xml_attrib_t* name_attrib = lt_xml_find_attrib(group, CLSTR("name"));
		if (name_attrib == NULL) {
			lt_werrf("group element missing required attribute 'name'\n");
			continue;
		}

		lt_xml_entity_t* plugin_list;
		if (!find_elements(group, 1, &CLSTR("plugins"), (b8[]){ 1 }, &plugin_list))
			continue;

		u8 plugin_order = str_to_order(lt_xml_find_attrib(plugin_list, CLSTR("order")));

		lt_darr(plugin_t) plugins = lt_darr_create(plugin_t, 8, alloc);
		load_plugin_list(fomod, &plugins, plugin_list);

		lt_darr_push(*groups, (group_t) {
				.order = plugin_order,
				.name = name_attrib->val,
				.type = group_type,
				.plugins = plugins });
	}
	return 0;
}

int load_install_steps(fomod_t* fomod, lt_xml_entity_t* steps) {
	usz child_count = lt_xml_child_count(steps);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &steps->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		if (!lt_lseq(child->elem.name, CLSTR("installStep"))) {
			lt_werrf("unknown installation step element '%S'\n", child->elem.name);
			continue;
		}

		lt_xml_attrib_t* name_attrib = lt_xml_find_attrib(child, CLSTR("name"));
		if (name_attrib == NULL) {
			lt_werrf("install step missing required attribute 'name'\n");
			continue;
		}

		lt_xml_entity_t* children[2];
		lstr_t names[2] = { CLSTR("visible"), CLSTR("optionalFileGroups") };
		b8 required[2] = { 0, 1 };
		if (!find_elements(child, 2, names, required, children))
			return 0;

		lt_xml_attrib_t* order_attrib = lt_xml_find_attrib(children[1], CLSTR("order"));
		u8 group_order = str_to_order(order_attrib);

		lt_darr(group_t) groups = lt_darr_create(group_t, 16, alloc);
		load_group_list(fomod, &groups, children[1]);

		lt_darr_push(fomod->install_steps, (install_step_t) {
				.order = group_order,
				.visible_cond = children[0],
				.name = name_attrib->val,
				.groups = groups });
	}
	return 0;
}

int load_cond_file_installs(fomod_t* fomod, lt_xml_entity_t* installs) {

	lt_xml_entity_t* patterns;
	if (!find_elements(installs, 1, &CLSTR("patterns"), (b8[]){ 1 }, &patterns))
		return 0;

	usz child_count = lt_xml_child_count(patterns);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &patterns->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		if (!lt_lseq(child->elem.name, CLSTR("pattern"))) {
			lt_werrf("unknown conditional file installation pattern element '%S'\n", child->elem.name);
			continue;
		}

		lt_xml_entity_t* children[2];
		lstr_t names[2] = { CLSTR("dependencies"), CLSTR("files") };
		b8 required[2] = { 1, 1 };
		if (!find_elements(child, 2, names, required, children))
			continue;
		if (eval_deps(fomod, children[0]))
			load_file_list(&fomod->files, &fomod->dirs, children[1]);
	}

	return 0;
}

static
u8 is_param_byte(u8 c) {
	return (c >= 0x30) && (c <= 0x3F);
}

isz str_width(lstr_t str) {
	isz w = 0;
	char* it = str.str, *end = it + str.len;
	while (it < end) {
		u32 c;
		it += lt_utf8_decode(it, &c);

		if (it < end && c == 0x1B && *it == '[') {
			++it;
			while (it < end && is_param_byte(*it))
				++it;
			++it;
		}
		else {
			w += lt_glyph_width(c);
		}
	}
	return w;
}

static
isz draw_wrapped_text(lstr_t text, isz max_width, lstr_t pfx) {
	char* end = text.str + text.len;

	max_width -= pfx.len;
	if (max_width <= 0)
		max_width = 1;

	char* word_start = text.str;
	char* line_start = text.str;
	for (char* it = text.str; it < end; ++it) {
		if (*it == ' ')
			word_start = it + 1;

		if (*it == '\n') {
			lt_fwrite(lt_stdout, pfx.str, pfx.len);
			lt_fwrite(lt_stdout, line_start, it - line_start);
			lt_fwrite(lt_stdout, "\n", 1);

			line_start = it + 1;
			continue;
		}

		usz line_len = (it - line_start);

		if (str_width(lt_lsfrom_range(line_start, it)) >= max_width) {
			usz word_len = it - word_start;

			char* new_line_start = it;

			if (line_start != word_start) {
				new_line_start -= word_len;
				line_len -= word_len;
			}
			else
				word_start = it;

			lt_fwrite(lt_stdout, pfx.str, pfx.len);
			lt_fwrite(lt_stdout, line_start, line_len);
			lt_fwrite(lt_stdout, "\n", 1);

			line_start = new_line_start;
		}
	}

	usz remain = end - line_start;
	lt_fwrite(lt_stdout, pfx.str, pfx.len);
	lt_fwrite(lt_stdout, line_start, remain);
	lt_fwrite(lt_stdout, "\n", 1);

	return 0;
}

void print_texted(lt_texted_t* ed, char* sel_clr, char* normal_clr) {
	usz x1, x2;
	lt_texted_get_selection(ed, &x1, NULL, &x2, NULL);
	lstr_t str = lt_texted_line_str(ed, 0);

	char* x1_csave = x1 == ed->cursor_x ? CSAVE : "";
	char* x2_csave = x2 == ed->cursor_x ? CSAVE : "";
	lt_printf("%s%S%s%s%S%s%s%S"RESET, normal_clr, LSTR(str.str, x1), x1_csave, sel_clr, LSTR(str.str + x1, x2 - x1), x2_csave, normal_clr, LSTR(str.str + x2, str.len - x2));
}

u8 find_plugin_type(fomod_t* fomod, plugin_t* plugin) {
	u8 type = plugin->default_type;

	for (usz i = 0; i < lt_darr_count(plugin->dep_types); ++i) {
		if (eval_deps(fomod, plugin->dep_types[i].cond)) {
			type = plugin->dep_types[i].type;
			break;
		}
	}

	return type;
}

lt_darr(u64) prompt_plugin_selection(group_t* group, lt_texted_t* ed, lt_strstream_t* clipboard) {
	lt_darr(u64) selection = lt_darr_create(u64, 64, alloc);

input:
	lt_darr_clear(selection);

	lt_texted_clear(ed);
	lstr_t prompt = CLSTR(": ");
	isz prev_text_lines = prompt.len / lt_term_width;

	lt_printf("%S\n", group_prompt(group->type));

	for (;;) {
		isz line_len = lt_texted_line_len(ed, 0) + prompt.len;
		isz text_lines = line_len / lt_term_width + (line_len % lt_term_width != 0) - 1;

		for (isz i = 0; i < prev_text_lines; ++i)
			lt_printf(CLL CUP1);

		lt_printf(CLL"\r%S", prompt);
		print_texted(ed, BG_BCYAN FG_BLACK, BG_BLACK FG_BWHITE);
		lt_printf(RESET CRESTORE RESET);

		u32 key = lt_term_getkey();
		if (key == '\n')
			break;
		if (key == (LT_TERM_MOD_CTRL|'D')) {
			lt_printf("\n");
			lt_werrf("mod installation cancelled by user\n");
			lt_darr_destroy(selection);
			return NULL;
		}
		lt_texted_input_term_key(ed, clipboard, key);

		prev_text_lines = text_lines;
	}

	lstr_t line = lt_texted_line_str(ed, 0);
	char* it = line.str, *end = it + line.len;
	for (;;) {
		while (it < end && lt_is_space(*it))
			++it;
		if (it >= end)
			break;

		char* num_start = it;
		while (it < end && lt_is_digit(*it))
			++it;
		lstr_t numstr = lt_lsfrom_range(num_start, it);
		if (numstr.len == 0) {
			lt_printf("\nunexpected character '%c' in input\n", *it);
			goto input;
		}
		u64 num = 0;
		lt_err_t err = lt_lstou(numstr, &num);
		if (err != LT_SUCCESS || num > lt_darr_count(group->plugins) || num == 0) {
			lt_printf("\ninvalid number '%S'\n", numstr);
			goto input;
		}
		--num;

		lt_darr_push(selection, num);
	}

	lt_printf("\n");

	if (!group_selection_validate(group->type, selection))
		goto input;

	if (group->type == GRP_SELECTALL) {
		for (usz i = 0; i < lt_darr_count(group->plugins); ++i) {
			group->plugins[i].selected = 1;
		}
	}

	return selection;
}


static
int load_modconf(fomod_t* fomod, lstr_t path) {
	int ret = -1;

	lstr_t modconf_data;
	if (lt_freadallp_utf8(path, &modconf_data, alloc) != LT_SUCCESS) {
		lt_werrf("failed to read '%S': \n", path);
		goto err0;
	}

	lt_xml_entity_t modconf_xm;
	if (lt_xml_parse(&modconf_xm, modconf_data.str, modconf_data.len, NULL, alloc) != LT_SUCCESS) {
		lt_werrf("failed to parse '%S'\n", path);
		goto err1;
	}

	lt_xml_entity_t* config = NULL;
	usz child_count = lt_xml_child_count(&modconf_xm);
	for (usz i = 0; i < child_count; ++i) {
		if (modconf_xm.elem.children[i].type == LT_XML_ELEMENT && lt_lseq(modconf_xm.elem.children[i].elem.name, CLSTR("config"))) {
			config = &modconf_xm.elem.children[i];
			break;
		}
	}

	if (config == NULL) {
		lt_werrf("ModuleConfig.xml missing 'config' element\n");
		goto err1;
	}

	lt_xml_entity_t* module_image = NULL;
	lt_xml_entity_t* module_deps = NULL;
	lt_xml_entity_t* install_steps = NULL;
	lt_xml_entity_t* required_files = NULL;

	child_count = lt_xml_child_count(config);
	for (usz i = 0; i < child_count; ++i) {
		lt_xml_entity_t* child = &config->elem.children[i];
		if (child->type != LT_XML_ELEMENT)
			continue;

		if (lt_lseq(child->elem.name, CLSTR("moduleName"))) {
			if (lt_xml_generate_str(child, &fomod->module_name, alloc))
				lt_werrf("xml generation failed for '%S' element\n", child->elem.name);
			continue;
		}

		if (lt_lseq(child->elem.name, CLSTR("moduleImage")))
			module_image = child;
		else if (lt_lseq(child->elem.name, CLSTR("moduleDependencies")))
			module_deps = child;
		else if (lt_lseq(child->elem.name, CLSTR("requiredInstallFiles")))
			required_files = child;
		else if (lt_lseq(child->elem.name, CLSTR("installSteps")))
			install_steps = child;
		else if (lt_lseq(child->elem.name, CLSTR("conditionalFileInstalls")))
			fomod->cond_install_files = child;
		else
			lt_werrf("unknown config element '%S'\n", child->elem.name);
	}

	if (module_deps != NULL) {
		child_count = lt_xml_child_count(module_deps);
		for (usz i = 0; i < child_count; ++i) {
			lt_xml_entity_t* child = &module_deps->elem.children[i];
			if (child->type != LT_XML_ELEMENT)
				continue;

			lt_werrf("unknown module dependency element '%S'\n", child->elem.name);
		}
	}

	fomod->files = lt_darr_create(install_t, 16, alloc);
	fomod->dirs = lt_darr_create(install_t, 16, alloc);

	fomod->install_steps = lt_darr_create(install_step_t, 16, alloc);

	if (required_files != NULL)
		load_file_list(&fomod->files, &fomod->dirs, required_files);

	if (install_steps != NULL)
		load_install_steps(fomod, install_steps);

	lt_term_init(0);

	lt_texted_t ed;
	LT_ASSERT(lt_texted_create(&ed, alloc) == LT_SUCCESS);
	lt_strstream_t clipboard;
	LT_ASSERT(lt_strstream_create(&clipboard, alloc) == LT_SUCCESS);

	fomod->flags = lt_darr_create(flag_t, 64, alloc);

	usz step_num = 1;
	for (usz i = 0; i < lt_darr_count(fomod->install_steps); ++i) { // !! order is ignored
		install_step_t* step = &fomod->install_steps[i];
		if (step->visible_cond && !eval_deps(fomod, step->visible_cond))
			continue;

		if (color)
			lt_printf(FG_BGREEN "# Installation step %uz: %S\n" RESET, step_num++, step->name);
		else
			lt_printf("# Installation step %uz: %S\n", step_num++, step->name);

		for (usz i = 0; i < lt_darr_count(step->groups); ++i) { // !! order is ignored
			group_t* group = &step->groups[i];

			if (color)
				lt_printf(FG_BGREEN "## %S\n" RESET, group->name);
			else
				lt_printf("## %S\n", group->name);

			for (usz i = 0; i < lt_darr_count(group->plugins); ++i) {
				plugin_t* plugin = &group->plugins[i];
				plugin->eval_type = find_plugin_type(fomod, plugin);

				char* plugin_strs[] = { " [REQUIRED]", "", " [RECOMMENDED]", " [NOT USABLE]", "" };

				if (color)
					lt_printf(FG_BYELLOW " %uz " RESET BOLD "%S" RESET FG_BYELLOW "%s\n" RESET, i + 1, plugin->name, plugin_strs[plugin->eval_type]);
				else
					lt_printf(" %uz %S\n", i + 1, plugin->name);

				if (plugin->eval_type == PLG_REQUIRED)
					plugin->selected = 1;

				draw_wrapped_text(lt_lstrim(plugin->description), lt_term_width - 1, CLSTR("     "));
			}

			lt_darr(u64) selection = prompt_plugin_selection(group, &ed, &clipboard);
			if (selection == NULL)
				goto err3;
			lt_printf("\n");

			for (usz i = 0; i < lt_darr_count(selection); ++i)
				group->plugins[selection[i]].selected = 1;
			lt_darr_destroy(selection);

			for (usz i = 0; i < lt_darr_count(group->plugins); ++i) {
				plugin_t* plugin = &group->plugins[i];

				for (usz i = 0; i < lt_darr_count(plugin->files); ++i) {
					install_t* file = &plugin->files[i];
					if (plugin->selected || file->always || (file->always_if_usable && plugin->eval_type != PLG_NOTUSABLE))
						lt_darr_push(fomod->files, *file);
				}

				for (usz i = 0; i < lt_darr_count(plugin->dirs); ++i) {
					install_t* dir = &plugin->dirs[i];
					if (plugin->selected || dir->always || (dir->always_if_usable && plugin->eval_type != PLG_NOTUSABLE))
						lt_darr_push(fomod->dirs, *dir);
				}

				if (plugin->selected) {
					for (usz i = 0; i < lt_darr_count(plugin->flags); ++i)
						lt_darr_push(fomod->flags, plugin->flags[i]);
				}
			}
		}
	}

	lt_term_restore();

	if (fomod->cond_install_files != NULL)
		load_cond_file_installs(fomod, fomod->cond_install_files);

	fomod->install_files = lt_darr_create(install_t, lt_darr_count(fomod->files), alloc);
	fomod->install_dirs = lt_darr_create(install_t, lt_darr_count(fomod->dirs), alloc);

	for (usz i = 0; i < lt_darr_count(fomod->dirs); ++i) {
		install_t* dir = &fomod->dirs[i];
		dir->path = lt_strdup(alloc, dir->path);
		dir->install_path = lt_strdup(alloc, dir->install_path);
		lt_darr_push(fomod->install_dirs, *dir);
	}

	for (usz i = 0; i < lt_darr_count(fomod->files); ++i) {
		install_t* file = &fomod->files[i];
		file->path = lt_strdup(alloc, file->path);
		file->install_path = lt_strdup(alloc, file->install_path);
		lt_darr_push(fomod->install_files, *file);
	}

	ret = 0;

err3:	lt_texted_destroy(&ed);
err2:	lt_strstream_destroy(&clipboard);
err1:	lt_xml_free(&modconf_xm, alloc);
err0:	lt_mfree(alloc, modconf_data.str);
		return ret;
}


static
void print_info_field(lstr_t key, lstr_t val) {
	if (color)
		lt_printf(FG_BYELLOW "%S" RESET ": " FG_BCYAN "%S" RESET "\n", key, val);
	else
		lt_printf("%S: %S\n", key, val);
}

static
void plugin_free(plugin_t* plugin) {
	if (plugin->description.str)
		lt_mfree(alloc, plugin->description.str);
	if (plugin->files)
		lt_darr_destroy(plugin->files);
	if (plugin->dirs)
		lt_darr_destroy(plugin->dirs);

	if (plugin->dep_types)
		lt_darr_destroy(plugin->dep_types);

	if (plugin->flags) {
		for (usz i = 0; i < lt_darr_count(plugin->flags); ++i) {
			if (plugin->flags[i].val.str)
				lt_mfree(alloc, plugin->flags[i].val.str);
		}
		lt_darr_destroy(plugin->flags);
	}
}

static
void group_free(group_t* group) {
	if (group->plugins) {
		for (usz i = 0; i < lt_darr_count(group->plugins); ++i)
			plugin_free(&group->plugins[i]);
		lt_darr_destroy(group->plugins);
	}
}

static
void install_step_free(install_step_t* step) {
	if (step->groups) {
		for (usz i = 0; i < lt_darr_count(step->groups); ++i)
			group_free(&step->groups[i]);
		lt_darr_destroy(step->groups);
	}
}

static
void fomod_free(fomod_t* fomod) {
	if (fomod->name.str)
		lt_mfree(alloc, fomod->name.str);
	if (fomod->author.str)
		lt_mfree(alloc, fomod->author.str);
	if (fomod->id.str)
		lt_mfree(alloc, fomod->id.str);
	if (fomod->version.str)
		lt_mfree(alloc, fomod->version.str);
	if (fomod->description.str)
		lt_mfree(alloc, fomod->description.str);
	if (fomod->website.str)
		lt_mfree(alloc, fomod->website.str);

	if (fomod->machine_version.str)
		lt_mfree(alloc, fomod->machine_version.str);

	if (fomod->module_name.str)
		lt_mfree(alloc, fomod->module_name.str);

	if (fomod->files)
		lt_darr_destroy(fomod->files);
	if (fomod->dirs)
		lt_darr_destroy(fomod->dirs);

	if (fomod->install_files) {
		for (usz i = 0; i < lt_darr_count(fomod->install_files); ++i) {
			lt_mfree(alloc, fomod->install_files[i].path.str);
			lt_mfree(alloc, fomod->install_files[i].install_path.str);
		}
		lt_darr_destroy(fomod->install_files);
	}
	if (fomod->install_dirs) {
		for (usz i = 0; i < lt_darr_count(fomod->install_dirs); ++i) {
			lt_mfree(alloc, fomod->install_dirs[i].path.str);
			lt_mfree(alloc, fomod->install_dirs[i].install_path.str);
		}
		lt_darr_destroy(fomod->install_dirs);
	}

	if (fomod->install_steps) {
		for (usz i = 0; i < lt_darr_count(fomod->install_steps); ++i)
			install_step_free(&fomod->install_steps[i]);
		lt_darr_destroy(fomod->install_steps);
	}

	if (fomod->flags)
		lt_darr_destroy(fomod->flags);
}

void path_dos2unix(lstr_t str) {
	for (char* it = str.str, *end = it + str.len; it < end; ++it)
		if (*it == '\\')
			*it = '/';
}

int fomod_install(char* in_path, char* out_path, char* root_data_path) {
	fomod_t fomod = { 0 };

	fomod.data_fd = open(root_data_path, O_RDONLY|O_DIRECTORY);
	if (fomod.data_fd < 0) {
		lt_werrf("failed to open data directory '%s': %s\n", root_data_path, lt_os_err_str());
		return -1;
	}

	int ret = -1;

	char* info_path = lt_lsbuild(alloc, "%s/fomod/info.xml%c", in_path, 0).str;
	int res = rebuild_path_case(info_path);
	if (res < 0) { // missing info.xml
		ret = 0;
		goto err0;
	}

	if (load_info(&fomod, lt_lsfroms(info_path)) < 0)
		return -1;

	char* modconf_path = lt_lsbuild(alloc, "%s/fomod/ModuleConfig.xml%c", in_path, 0).str;
	res = rebuild_path_case(modconf_path);
	if (res < 0) { // missing ModuleConfig.xml
		ret = 0;
		goto err1;
	}

	if (load_modconf(&fomod, lt_lsfroms(modconf_path)) < 0)
		goto err1;

#define COPY_BUFSZ LT_KB(1024)

	void* copy_buf = lt_malloc(alloc, COPY_BUFSZ);

	for (usz i = 0; i < lt_darr_count(fomod.install_files); ++i) {
		install_t* file = &fomod.install_files[i];

		path_dos2unix(file->path);
		path_dos2unix(file->install_path);

		lt_printf("installing file '%S' to '%S'\n", file->path, file->install_path);

		lstr_t out_file_path = lt_lsbuild(alloc, "%s/%S", out_path, file->install_path);
		ls_rebuild_path_case(out_file_path);

		lt_err_t err = lt_mkpath(lt_lsdirname(out_file_path));
		if (err != LT_SUCCESS) {
			lt_werrf("failed to create output directory '%S': %S\n", out_file_path, lt_err_str(err));
			lt_mfree(alloc, out_file_path.str);
			goto err2;
		}

		lstr_t in_file_path = lt_lsbuild(alloc, "%s/%S", in_path, file->path);
		ls_rebuild_path_case(in_file_path);

		if ((err = lt_fcopyp(in_file_path, out_file_path, copy_buf, COPY_BUFSZ, alloc)))
			lt_werrf("failed to copy '%s': %S\n", in_path, lt_err_str(err));

		lt_mfree(alloc, in_file_path.str);
		lt_mfree(alloc, out_file_path.str);
	}

	for (usz i = 0; i < lt_darr_count(fomod.install_dirs); ++i) {
		install_t* dir = &fomod.install_dirs[i];

		path_dos2unix(dir->path);
		path_dos2unix(dir->install_path);

		lt_printf("installing directory '%S' to '%S'\n", dir->path, dir->install_path);

		lstr_t out_dir_path = lt_lsbuild(alloc, "%s/%S", out_path, dir->install_path);
		ls_rebuild_path_case(out_dir_path);

		lt_err_t err = lt_mkpath(lt_lsdirname(out_dir_path));
		if (err != LT_SUCCESS) {
			lt_werrf("failed to create output directory '%S': %S\n", out_dir_path, lt_err_str(err));
			lt_mfree(alloc, out_dir_path.str);
			goto err2;
		}

		lstr_t in_dir_path = lt_lsbuild(alloc, "%s/%S", in_path, dir->path);
		ls_rebuild_path_case(in_dir_path);

		if ((err = lt_dcopyp(in_dir_path, out_dir_path, copy_buf, COPY_BUFSZ, LT_DCOPY_MERGE|LT_DCOPY_OVERWRITE_FILES, alloc)))
			lt_werrf("failed to copy '%S' to '%S': %S\n", in_dir_path, out_dir_path, lt_err_str(err));

		lt_mfree(alloc, in_dir_path.str);
		lt_mfree(alloc, out_dir_path.str);
	}

	ret = 0;

err2:	lt_mfree(alloc, copy_buf);
err1:	lt_mfree(alloc, modconf_path);
err0:	lt_mfree(alloc, info_path);
		fomod_free(&fomod);
		close(fomod.data_fd);
		return ret;
}

#include "lt/err.h"
#include <lt/lt.h>
#include <lt/io.h>
#include <lt/mem.h>
#include <lt/ctype.h>
#include <lt/str.h>

#include <assert.h>

#define INI_LINE_EMPTY   0
#define INI_LINE_VALUE   1
#define INI_LINE_COMMENT 2

typedef struct ini_line {
	u8 type;
	u8 key_length;
	u16 length;
	u32 offset;
	u32 key_offset;
} ini_line_t;

typedef struct ini_section {
	u32 name_offset;
	u16 name_length;
	u16 line_count;
	ini_line_t* lines;
} ini_section_t;

typedef struct ini {
	lt_err_t error;

	ini_section_t* sections;
	usz section_count;

	char* strtab;
	usz strtab_size;
} ini_t;

#define STRTAB_BLOCKSIZE 8192

static
u32 write_string(ini_t ini[static 1], lstr_t str) {
	usz new_size = ini->strtab_size + str.len;
	if ((new_size ^ ini->strtab_size) & ~(STRTAB_BLOCKSIZE-1)) {
		usz new_capacity = lt_align_fwd(new_size, STRTAB_BLOCKSIZE);
		void* new_mem = realloc(ini->strtab, new_capacity);
		assert(new_mem);
		ini->strtab = new_mem;
	}

	u32 write_offset = ini->strtab_size;
	memcpy(ini->strtab + write_offset, str.str, str.len);
	ini->strtab_size = new_size;
	return write_offset;
}

void ini_free(ini_t ini[static 1]) {
	for (ini_section_t* it = ini->sections, *end = it + ini->section_count; it < end; ++it) {
		if (it->lines)
			free(it->lines);
	}
	if (ini->sections)
		free(ini->sections);
	if (ini->strtab)
		free(ini->strtab);
}

// ----- lookups

static LT_INLINE
lstr_t ini_section_name(const ini_t ini[static 1], isz section_i) {
	if (section_i < 0)
		return NLSTR();
	return LSTR(ini->strtab + ini->sections[section_i].name_offset, ini->sections[section_i].name_length);
}

static LT_INLINE
lstr_t ini_line_key(const ini_t ini[static 1], ini_line_t line[static 1]) {
	return LSTR(ini->strtab + line->key_offset, line->key_length);
}

static LT_INLINE
lstr_t ini_line_value(const ini_t ini[static 1], ini_line_t line[static 1]) {
	return LSTR(ini->strtab + line->offset, line->length);
}

isz ini_find_section(const ini_t ini[static 1], lstr_t name) {
	for (usz i = 0; i < ini->section_count; ++i) {
		if (lt_lseq(ini_section_name(ini, i), name))
			return i;
	}
	return -1;
}

lstr_t ini_find_value(const ini_t ini[static 1], lstr_t section_name, lstr_t key) {
	isz section_i = ini_find_section(ini, section_name);
	if (section_i < 0)
		return NLSTR();

	ini_section_t* section = &ini->sections[section_i];
	for (ini_line_t* it = section->lines, *end = it + section->line_count; it < end; ++it) {
		if (it->type == INI_LINE_VALUE && lt_lseq(ini_line_key(ini, it), key))
			return ini_line_value(ini, it);
	}
	return NLSTR();
}

isz ini_find_value_line(const ini_t ini[static 1], isz section_i, lstr_t key) {
	if (section_i < 0)
		return -1;

	ini_section_t* section = &ini->sections[section_i];
	for (ini_line_t* it = section->lines, *end = it + section->line_count; it < end; ++it) {
		if (it->type == INI_LINE_VALUE && lt_lseq(ini_line_key(ini, it), key))
			return it - section->lines;
	}
	return -1;
}

// ----- addition

#define SECTION_BLOCKSIZE 256

usz ini_add_section(ini_t ini[static 1], lstr_t name) {
	if ((ini->section_count & (SECTION_BLOCKSIZE-1)) == 0) {
		usz new_capacity = (usz)ini->section_count + SECTION_BLOCKSIZE;
		void* new_mem = realloc(ini->sections, new_capacity * sizeof(ini_section_t));
		assert(new_mem);
		ini->sections = new_mem;
	}

	ini->sections[ini->section_count] = (ini_section_t) {
		.name_offset = write_string(ini, name),
		.name_length = name.len,
	};
	return ini->section_count++;
}

#define ENTRY_BLOCKSIZE 128

isz ini_add_line(ini_t ini[static 1], isz section_i, ini_line_t line[static 1]) {
	if (section_i < 0)
		return -1;

	ini_section_t* section = &ini->sections[section_i];
	if ((section->line_count & (ENTRY_BLOCKSIZE-1)) == 0) {
		usz new_capacity = section->line_count + ENTRY_BLOCKSIZE;
		void* new_mem = realloc(section->lines, new_capacity * sizeof(ini_line_t));
		assert(new_mem);
		section->lines = new_mem;
	}

	section->lines[section->line_count] = *line;
	return section->line_count++;
}

isz ini_add_value(ini_t ini[static 1], isz section_i, lstr_t key, lstr_t value) {
	return ini_add_line(ini, section_i, &(ini_line_t) {
		.type = INI_LINE_VALUE,
		.key_offset = write_string(ini, key),
		.key_length = key.len,
		.offset = write_string(ini, value),
		.length = value.len,
	});
}

isz ini_set_value(ini_t ini[static 1], isz section_i, lstr_t key, lstr_t value) {
	isz existing_line = ini_find_value_line(ini, section_i, key);
	if (existing_line < 0)
		return ini_add_value(ini, section_i, key, value);

	ini_line_t* line = &ini->sections[section_i].lines[existing_line];
	line->offset = write_string(ini, value);
	line->length = value.len;
	return existing_line;
}

// ----- removal

void ini_remove_line(ini_t ini[static 1], isz section_i, isz line_i) {
	if (section_i < 0 || line_i < 0)
		return;
	ini_section_t* section = &ini->sections[section_i];
	memmove(section->lines + line_i, section->lines + line_i + 1, (section->line_count - line_i - 1) * sizeof(ini_line_t));
	--section->line_count;
}

void ini_remove_value(ini_t ini[static 1], isz section_i, lstr_t key) {
	ini_remove_line(ini, section_i, ini_find_value_line(ini, section_i, key));
}

// ----- parsing

static LT_INLINE
char* skip_space(char* it, char* end) {
	while (it < end && lt_is_space(*it))
		++it;
	return it;
}

static LT_INLINE
char* skip_nonlf_space(char* it, char* end) {
	while (it < end && (*it == ' ' || *it == '\t' || *it == '\v'))
		++it;
	return it;
}

static LT_INLINE
char* skip_line(char* it, char* end) {
	while (it < end) {
		if (*it == '\n')
			return ++it;
		++it;
	}
	return it;
}

ini_t ini_parse(lstr_t str) {
	ini_t ini = { 0 };
	ini.strtab = malloc(STRTAB_BLOCKSIZE);
	assert(ini.strtab);
	u32 section_i = ini_add_section(&ini, CLSTR(""));

	char* it = str.str, *end = it + str.len;
	while (it < end) {
		it = skip_nonlf_space(it, end);

		if (it >= end || *it == '\n') {
			++it;
			ini_add_line(&ini, section_i, &(ini_line_t) {
				.type = INI_LINE_EMPTY,
			});
			continue;
		}

		if (*it == ';') {
			char* start = ++it;
			it = skip_line(it, end);
			lstr_t text = lt_lstrim_right(lt_lsfrom_range(start, it));

			ini_add_line(&ini, section_i, &(ini_line_t) {
				.type   = INI_LINE_COMMENT,
				.offset = write_string(&ini, text),
				.length = text.len,
			});
			continue;
		}

		if (*it == '[') {
			char* start = ++it;
			for (;;) {
				if (it >= end || *it == '\n') {
					ini.error = LT_ERR_INVALID_SYNTAX;
					goto err;
				}
				if (*it == ']')
					break;
				++it;
			}
			lstr_t name = lt_lstrim(lt_lsfrom_range(start, it));

			++it;
			if (it < end && *it == '\n')
				++it;

			section_i = ini_add_section(&ini, name);
			continue;
		}

		char* start = it;
		for (;;) {
			if (it >= end) {
				ini.error = LT_ERR_INVALID_SYNTAX;
				goto err;
			}
			if (*it == '=')
				break;
			++it;
		}
		lstr_t key = lt_lstrim(lt_lsfrom_range(start, it));

		start = ++it;
		it = skip_line(it, end);
		lstr_t value = lt_lstrim_right(lt_lsfrom_range(start, it));

		ini_add_line(&ini, section_i, &(ini_line_t) {
			.type       = INI_LINE_VALUE,
			.offset     = write_string(&ini, value),
			.length     = value.len,
			.key_offset = write_string(&ini, key),
			.key_length = key.len,
		});
	}

	return ini;

err:
	ini_free(&ini);
	return ini;
}

ini_t ini_load(lstr_t path) {
	lstr_t file_data;
	lt_err_t err = lt_freadallp(path, &file_data, lt_libc_heap);
	if (err)
		return (ini_t) { .error = err };

	ini_t ini = ini_parse(file_data);
	lt_mfree(lt_libc_heap, file_data.str);
	return ini;
}

void ini_write(const ini_t ini[static 1], lt_file_t file[static 1]) {
	for (usz section_i = 0; section_i < ini->section_count; ++section_i) {
		ini_section_t* section = &ini->sections[section_i];
		lstr_t name = ini_section_name(ini, section_i);
		if (!name.len)
			continue;

		lt_fprintf(file, "[%S]\n", name);

		for (ini_line_t* line_it = section->lines, *line_end = line_it + section->line_count; line_it < line_end; ++line_it) {
			switch (line_it->type) {
			case INI_LINE_EMPTY:   lt_fprintf(file, "\n"); break;
			case INI_LINE_COMMENT: lt_fprintf(file, ";%S\n",   ini_line_value(ini, line_it)); break;
			case INI_LINE_VALUE:   lt_fprintf(file, "%S=%S\n", ini_line_key(ini, line_it), ini_line_value(ini, line_it)); break;
			}
		}
	}
}


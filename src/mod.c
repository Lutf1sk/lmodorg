#include "mod.h"

#include <lt/sort.h>
#include <lt/str.h>
#include <lt/mem.h>
#include <lt/darr.h>

#define alloc lt_libc_heap

#define mod_is_equal(mod, key) (lt_lseq((*mod).name, key))
#define mod_is_lesser(mod, key) (lt_lscmp((*mod).name, key) < 0)

LT_DEFINE_BINARY_SEARCH_FUNC(mod_t*, lstr_t, lookup_mod, mod_is_lesser, mod_is_equal);
LT_DEFINE_BINARY_SEARCH_NEAREST_FUNC(mod_t*, lstr_t, lookup_nearest_mod, mod_is_lesser, mod_is_equal);

static lt_darr(mod_t*) mod_tab = NULL;

void mods_init(void) {
	mod_tab = lt_darr_create(mod_t*, 64, alloc);
}

void mods_terminate(void) {
	for (usz i = 0; i < lt_darr_count(mod_tab); ++i) {
		mod_t* mod = mod_tab[i];
		lt_mfree(alloc, mod->name.str);
		lt_mfree(alloc, mod);
	}
	lt_darr_destroy(mod_tab);
}

lt_err_t mod_register(mod_t* mod) {
	usz count = lt_darr_count(mod_tab);
	mod_t** at = lookup_nearest_mod(count, mod_tab, mod->name);

	if (at < mod_tab + count && mod_is_equal(*at, mod->name))
		lt_ferrf("redefinition of mod '%S'\n", mod->name);

	usz idx = at - mod_tab;
	lt_darr_insert(mod_tab, idx, &mod, 1);
	return LT_SUCCESS;
}

mod_t* mod_find(lstr_t name) {
	mod_t** pptr = lookup_mod(lt_darr_count(mod_tab), mod_tab, name);
	if (pptr == NULL)
		return NULL;
	return *pptr;
}

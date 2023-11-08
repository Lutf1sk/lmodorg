#ifndef MOD_H
#define MOD_H 1

#include <lt/err.h>

typedef
struct mod {
	lstr_t name;
	int rootfd;
} mod_t;

void mods_init(void);
void mods_terminate(void);

lt_err_t mod_register(mod_t* mod);
mod_t* mod_find(lstr_t name);

typedef
struct avail_mod {
	lstr_t name;
	char* root_path;
} avail_mod_t;

#endif
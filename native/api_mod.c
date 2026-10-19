#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <malloc.h>

static int test(lua_State *L) {
	// What happens if it's not an int!?
	const lua_Integer i = luaL_checkint(L, 1);
	lua_pushinteger(L, i * 2 + 1);
	return 1;
}

static const luaL_Reg funcs[] = { {"test", test}, {NULL, NULL} };

__declspec(dllexport) int luaopen_api_mod(lua_State *L) {
	lua_newtable(L);
	luaL_register(L, NULL, funcs);
	return 1;
}

static lua_State *my_global;

// Example allocator from lua docs
static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
	(void)ud;  (void)osize;  /* not used */
	if (nsize == 0) {
		free(ptr);
		return NULL;
	} else {
		return realloc(ptr, nsize);
	}
}

__declspec(dllexport) void create() {
	my_global = lua_newstate(l_alloc, NULL);
	if (my_global == NULL) {
		printf("Ooops!\n");
		return;
	}
	lua_pushinteger(my_global, 0);
}

__declspec(dllexport) void increment() {
	lua_Integer i = lua_tointeger(my_global, 1);
	lua_pop(my_global, 1);
	i += 1;
	lua_pushinteger(my_global, i);
}

__declspec(dllexport) int get() {
	return lua_tointeger(my_global, 1);
}

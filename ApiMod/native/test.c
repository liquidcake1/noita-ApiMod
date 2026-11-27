#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int handle_error(lua_State *L) {
	const char *s = lua_tostring(L, 1);
	printf("Error: %s\n", s);
	return 0;
}

int main(int argc, char **argv) {
	printf("Hello, world.\n");
	for(int i=1; i < argc; i++) {
		printf("Running %s\n", argv[i]);
		lua_State *L = luaL_newstate();
		luaL_openlibs(L);
		lua_settop(L, 0);
		lua_pushcfunction(L, handle_error);
		if (luaL_loadfile(L, argv[i])) {
			printf("Error loading %s\n", argv[i]);
			return 1;
		}
		if (lua_pcall(L, 0, LUA_MULTRET, 1)) {
			printf("Error running %s\n", argv[i]);
			return 1;
		}
		printf("Ran %s\n", argv[i]);
	}
	return 0;
}

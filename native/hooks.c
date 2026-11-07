#include <lua.h>
#include <lauxlib.h>
#include <MinHook.h>
#include <windows.h>
#include "api_mod.h"
#include "util.h"

static void handle_charstar_error(lua_State *L, const char *where, const char *error) {
	if (error != NULL) {
		lua_pushfstring(L, "ApiMod: Error in %s: %s", where, error);
		lua_error(L);
	}
}

static char *ensure_not_called_again() {
	static int attempted = 0;
	if (attempted) {
		return "Cannot call initialise twice";
	}
	attempted = 1;
	return NULL;
}

static const char *pin_ourself() {
	HMODULE self; // Not actually used, but GetModuleHandleExA needs one.
	DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS // second parameter is an address of something here, not a name.
		| GET_MODULE_HANDLE_EX_FLAG_PIN; // Never unload, even if FreeLibrary is called.
	LPCSTR something_local = (LPCSTR)pin_ourself;
	if (!GetModuleHandleExA(flags, something_local, &self)) {
		return "Could not pin this module.";
	}
	return NULL;
}

static int states = 0;

static lua_State* (*orig_lua_newstate)(lua_Alloc,void*) = NULL;
static lua_State* (*orig_luaL_newstate)(void) = NULL;
static int (*orig_luaopen_math)(lua_State*) = NULL;
static int (*orig_lua_close)(lua_State*) = NULL;
static lua_State *our_lua_newstate(lua_Alloc f, void *ud) {
	lua_State *L = orig_lua_newstate(f, ud);
	fprintf(stderr, "lua_newstate called -> %lx (%i)!\n", (long)L, ++states);
	set_main_thread(L);
	luaL_register(L, "ApiMod", ApiMod_funcs);
	return L;
}
static lua_State *our_luaL_newstate() {
	fprintf(stderr, "luaL_newstate called!\n");
	lua_State *L = orig_luaL_newstate();
	return L;
}
// luaL_register, luaL_loadlibs are _not_ interestingly hookable (never called).
// It looks like Noita calls luaopen_math etc. to get the libs. So it doesn't filter the state but build it. luaopen_os is _not_ called.
static int our_luaopen_math(lua_State *L) {
	fprintf(stderr, "luaopen_math called!\n");
	return orig_luaopen_math(L);
}
static int our_lua_close(lua_State *L) {
	fprintf(stderr, "lua_close called in %lx (%i)!\n", (long)L, --states);
	return orig_lua_close(L);
}

static const char *add_hook() {
	MH_STATUS status;
	HMODULE lua = GetModuleHandleA("lua51.dll");
	if (lua == NULL) {
		return "Lua module didn't exist";
	}
	// The symbols _we_ have are local to our module. I'm not sure quite
	// how dynamic linking works, but it looks like the local symbol probably
	// binds to a stub which calls onto the dynamically loaded symbol,
	// which we need here.
	void *module_lua_newstate = GetProcAddress(lua, "lua_newstate");
	void *module_luaL_newstate = GetProcAddress(lua, "luaL_newstate");
	void *module_luaopen_math = GetProcAddress(lua, "luaopen_math");
	void *module_lua_close = GetProcAddress(lua, "lua_close");
	if ((module_luaL_newstate == NULL) || (module_luaL_newstate == NULL) || (module_luaopen_math == NULL) || (module_lua_close == NULL)) {
		return "Couldn't find C function to hook";
	}
	if ((status = (
		MH_Initialize() ||
		MH_CreateHook(module_lua_newstate, our_lua_newstate, (LPVOID*)&orig_lua_newstate) ||
		MH_CreateHook(module_luaL_newstate, our_luaL_newstate, (LPVOID*)&orig_luaL_newstate) ||
		MH_CreateHook(module_luaopen_math, our_luaopen_math, (LPVOID*)&orig_luaopen_math) ||
		MH_CreateHook(module_lua_close, our_lua_close, (LPVOID*)&orig_lua_close) ||
		MH_EnableHook(MH_ALL_HOOKS)
	))) {
		fprintf(stderr, "Hook failed\n");
		return MH_StatusToString(status);
	}
	fprintf(stderr, "Hook succeeded!\n");
	return NULL;
}

static int cleanup(lua_State *L) {
	if (ApiMod_registry_state != NULL) {
		if (DEBUG) fprintf(stderr, "cleanup in %lx\n", (long)L);
		lua_close(ApiMod_registry_state);
		ApiMod_registry_state = NULL;
		if (DEBUG) fprintf(stderr, "cleanup done %lx\n", (long)L);
		return 0;
	}
	if (DEBUG) fprintf(stderr, "skip cleanup in %lx\n", (long)L);
	return 0;
}

static void make_finaliser(lua_State *L) {
	// Make a blank userdata that will be destroyed when the thread is destroyed.
	// (We store it referencing itself in the registry.)
	lua_newuserdata(L, 1);
	lua_newtable(L);
	lua_pushstring(L, "__gc");
	lua_pushcfunction(L, cleanup);
	lua_rawset(L, -3);
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -1);
	lua_rawset(L, LUA_REGISTRYINDEX); // Keep our userdata alive forever.
}

/**
 * Add our hooks to the running process, to allow new states to use them.
 */
//static int INITIALISED_KEY; // Never observed; just used as a key in the initialising state's registry.
//static lua_State *main_state = NULL;
static int initialise(lua_State *L) {
	create_registry(L);
	handle_charstar_error(L, "initialise", ensure_not_called_again());
	make_finaliser(L);
	handle_charstar_error(L, "pin_ourself", pin_ourself());
	handle_charstar_error(L, "add_hook", add_hook());
	return 0;
}

static const luaL_Reg funcs[] = { {"initialise", initialise}, {NULL, NULL} };

__declspec(dllexport) int luaopen_api_mod(lua_State *L) {
	luaL_register(L, "ApiMod", ApiMod_funcs);
	lua_pop(L, 1);
	lua_newtable(L);
	luaL_register(L, NULL, funcs);
	return 1;
}

#include <stdio.h>
#include <lauxlib.h>
#include <lua.h>
#include "proxy.h"
#include "destroy_proxy.h"
#include "proxy_metatables.h"
#include "util.h"
#include "transfer.h"

static RemoteProxy *check_remote_proxy(lua_State *raise_errors_in, lua_State *L, int idx) {
	RemoteProxy *rp = (RemoteProxy *)check_userdata(raise_errors_in, L, idx, &REMOTE_PROXY_METATABLE_KEY);
	if (rp->dead) {
		if (DEBUG) fprintf(stderr, "Lua error raising: check_remote_proxy: Proxy is dead (host lua_State has been finalised).\n");
		lua_pushstring(raise_errors_in, "check_remote_proxy: Proxy is dead (host lua_State has been finalised).");
		lua_error(raise_errors_in);
	}
	return rp;
}

// Common code for remote proxy metatable methods.
// We're in "to"; the stack is [rp for obj, arg1, ..., argn]. After return, the stack is [ret1, ..., retn].
static void remote_proxy_inner(lua_State *to, lua_CFunction func, int nargs, int nret) {
	RemoteProxy *rp = check_remote_proxy(to, to, 1);

	lua_checkstack(to, 1);
	// [remote_proxy__?_inner, rp for obj, arg1, ..., argn]

	pcall_transfer(to, rp->owner, func, nargs, nret);
}

// In "from", we're doing table[key] for a table proxied from "to". Proxying is handled by pcall_transfer.
// Params: [table, key]
// Return: [value]
static int remote_proxy__index_inner(lua_State *from) {
	if (DEBUG) printf("remote_proxy__index_inner %i\n", lua_gettop(from));
	lua_gettable(from, 1);
	lua_remove(from, 1); // Remove the table from the stack, just to be clean.
	return 1;
}

// In "to", we're doing rp[key] on a RemoteProxy userdata.
// Params: [rp for table, key]
// Return: [transfer(table[transfer(key)])]
static int remote_proxy__index(lua_State *to) {
	if (DEBUG) printf("remote_proxy__index\n");
	if (lua_gettop(to) != 2) {
		lua_checkstack(to, 1);
		if (DEBUG) fprintf(stderr, "Lua error raising: remote_proxy__index: Wrong number of args\n");
		lua_pushstring(to, "remote_proxy__index: Wrong number of args");
		lua_error(to);
	}
	remote_proxy_inner(to, remote_proxy__index_inner, 2, 1);
	return 1;
}

// In "from", we're doing rp[key] = val on a RemoteProxy userdata. Proxying is handled by pcall_transfer.
// Params: [table, key, val]
// Return: []
static int remote_proxy__newindex_inner(lua_State *from) {
	if (DEBUG) printf("remote_proxy__newindex_inner\n");
	// Reasonably, we shouldn't get a call except from
	// remote_proxy__newindex, which guarantees a stack of exactly 3.
	// We're using absolute indexes though, so let's just be safe.
	luaL_checkany(from, 3);
	lua_settop(from, 3);
	lua_getmetatable(from, 1);
	if (lua_isnil(from, -1)) {
		lua_pop(from, 1); // The metatable
	} else {
		lua_pushstring(from, "__apimod_settable");
		lua_gettable(from, -2);
		switch(lua_type(from, -1)) {
		case LUA_TNIL:
			break;
		case LUA_TFUNCTION:
			lua_insert(from, 1);
			lua_pop(from, 1); // The metatable
			lua_call(from, 3, 0);
			return 0;
		default:
			// If it's a table or a userdata, run the set on that.
			// If it's something else, this will just error.
			lua_replace(from, 1);
			lua_pop(from, 1); // The metatable
			lua_settable(from, 1);
			return 0;
		}
		lua_pop(from, 2); // The metatable, and the nil __apimod_settable value.
	}
	lua_settable(from, 1);
	return 0;
}

// In "to", we're doing rp[key] on a RemoteProxy userdata.
// Params: [rp for func, arg1, ..., argn]
// Return: [transfer(table[transfer(key)])]
static int remote_proxy__newindex(lua_State *to) {
	if (DEBUG) printf("remote_proxy__newindex\n");
	int nargs = lua_gettop(to) - 1;
	remote_proxy_inner(to, remote_proxy__newindex_inner, nargs + 1, LUA_MULTRET);
	return lua_gettop(to);
}

// In "from", we're doing func(arg1, ..., argn) for a function proxied from "to". Proxying is handled by pcall_transfer.
// Params: [func, arg1, ..., argn]
// Return: [ret1, ..., retn]
static int remote_proxy__call_inner(lua_State *from) {
	if (DEBUG) printf("remote_proxy__call_inner\n");
	int nargs = lua_gettop(from) - 1;
	lua_call(from, nargs, LUA_MULTRET);
	return lua_gettop(from); // return everything
}

// In "to", we're doing rp[key] on a RemoteProxy userdata.
// Params: [rp for func, arg1, ..., argn]
// Return: [transfer(table[transfer(key)])]
static int remote_proxy__call(lua_State *to) {
	if (DEBUG) printf("remote_proxy__call\n");
	int nargs = lua_gettop(to) - 1;
	remote_proxy_inner(to, remote_proxy__call_inner, nargs + 1, LUA_MULTRET);
	return lua_gettop(to);
}

int no_metatable_access(lua_State *L) {
	lua_pushstring(L, "no_metatable_access: metatable of ApiMod internals cannot be accessed from Lua");
	lua_error(L);
	return 0; // Unreachable
}

// Either make a new RemoteProxy metatable, or use the existing one if it's there. Leave it on the stack.
void ensure_remote_proxy_metatable(lua_State *to) {
	lua_pushlightuserdata(to, &REMOTE_PROXY_METATABLE_KEY);
	lua_rawget(to, LUA_REGISTRYINDEX);
	if (lua_isnil(to, -1)) {
		lua_pop(to, 1);
		lua_newtable(to);
		lua_pushstring(to, "__gc");
		lua_pushcfunction(to, remote_proxy__gc);
		lua_rawset(to, -3);
		lua_pushstring(to, "__call");
		lua_pushcfunction(to, remote_proxy__call);
		lua_rawset(to, -3);
		lua_pushstring(to, "__index");
		lua_pushcfunction(to, remote_proxy__index);
		lua_rawset(to, -3);
		lua_pushstring(to, "__newindex");
		lua_pushcfunction(to, remote_proxy__newindex);
		lua_rawset(to, -3);
		// It's not the end of the world if a mod gets ahold of the
		// metatable, as we always check that the first parameter
		// is a RemoteProxy userdata anyway.
		// Still, let's just not let too many shenanigans happen;
		// there's only one of these per state and if it's edited,
		// it'll affect all RemoteProxy objects there, which is probably
		// not what they wanted.
		lua_pushstring(to, "__metatable");
		lua_pushcfunction(to, no_metatable_access);
		lua_rawset(to, -3);
		lua_pushlightuserdata(to, &REMOTE_PROXY_METATABLE_KEY);
		lua_pushvalue(to, -2);
		lua_rawset(to, LUA_REGISTRYINDEX);
	}
}

// Either make a new ProxyFinaliser metatable, or use the existing one if it's there. Leave it on the stack.
void ensure_proxy_finaliser_metatable(lua_State *from) {
	lua_pushlightuserdata(from, &PROXY_FINALISER_METATABLE_KEY);
	lua_rawget(from, LUA_REGISTRYINDEX);
	if (lua_isnil(from, -1)) {
		lua_pop(from, 1);
		lua_newtable(from);
		lua_pushstring(from, "__gc");
		lua_pushcfunction(from, proxy_finaliser__gc);
		lua_rawset(from, -3);
		// We hide the metatable, though since ProxyFinalisers never
		// leave the registry, there is no way for Lua to query it
		// anyway.
		lua_pushstring(from, "__metatable");
		lua_pushcfunction(from, no_metatable_access);
		lua_rawset(from, -3);
		lua_pushlightuserdata(from, &PROXY_FINALISER_METATABLE_KEY);
		lua_pushvalue(from, -2);
		lua_rawset(from, LUA_REGISTRYINDEX);
	}
}

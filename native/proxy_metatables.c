#include <stdio.h>
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
	lua_pushlightuserdata(to, func);
	lua_insert(to, 1);
	// [remote_proxy__?_inner, rp for obj, arg1, ..., argn]

	pcall_transfer(to, rp->owner, nargs, nret);
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
		lua_pushstring(to, "__metatable");
		lua_pushnil(to);
		lua_rawset(to, -3); // DISABLES fetching metatable
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
		lua_pushstring(from, "__metatable");
		lua_pushnil(from);
		lua_rawset(from, -3); // DISABLES fetching metatable
		lua_pushlightuserdata(from, &PROXY_FINALISER_METATABLE_KEY);
		lua_pushvalue(from, -2);
		lua_rawset(from, LUA_REGISTRYINDEX);
	}
}

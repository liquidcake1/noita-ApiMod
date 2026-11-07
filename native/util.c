#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <lua.h>
#include "util.h"

void *check_userdata(lua_State *raise_errors_in, lua_State *L, int idx, void *table_key) {
	void *ud = lua_touserdata(L, idx);
	if (ud == NULL) {
		if (DEBUG) fprintf(stderr, "Lua error raising: check_userdata: Param must be lua_State *\n");
		lua_pushstring(raise_errors_in, "check_userdata: Param must be lua_State *");
		lua_error(raise_errors_in);
	}
	if (lua_getmetatable(L, idx) == 0) {
		if (DEBUG) fprintf(stderr, "Lua error raising: check_userdata: No metatable\n");
		lua_pushstring(raise_errors_in, "check_userdata: No metatable");
		lua_error(raise_errors_in);
	}
	lua_pushlightuserdata(L, table_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	if (!lua_rawequal(L, -1, -2)) {
		lua_pop(L, 2);
		if (DEBUG) fprintf(stderr, "Lua error raising: check_userdata: Wrong metatable (type is incorrect)\n");
		lua_pushstring(raise_errors_in, "check_userdata: Wrong metatable (type is incorrect)");
		lua_error(raise_errors_in);
	}
	lua_pop(L, 2);
	return ud;
}

// After a cpcall in from, grab and pop its error, clean it out and then raise it as our own error.
// raise_to is the current protected context; ran_in is the one that returned.
void handle_error_from(lua_State *raise_to, lua_State *ran_in, int retval) {
	if (DEBUG) fprintf(stderr, "handle_error_from: retval is %i\n", retval);
	if (retval != 0) {
		// raise_to: [...], ran_in: [..., error_str]
		size_t len;
		// lua_tolstring only raises if the index isn't a string.
		// If we caught an error, it was a string.
		// So it cannot raise.
		char *t;
		if (lua_type(ran_in, -1) == LUA_TSTRING) {
			const char *s = lua_tolstring(ran_in, -1, &len);
			if (DEBUG) fprintf(stderr, "handle_error_from: Error is %s\n", s);

			// Do a little dance so that we don't leak onto ran_in's stack.
			t = malloc(len);
			if (t == NULL) {
				// Well, we tried. Probably this fails too, as we're going to try to allocate that string, but it's Lua's problem at that point.
				lua_pop(ran_in, 1);
				lua_checkstack(raise_to, 1);
				if (DEBUG) fprintf(stderr, "Lua error raising: handle_error_from: Memory error while handling an error\n");
				lua_pushstring(raise_to, "handle_error_from: Memory error while handling an error");
				lua_error(raise_to);
			}
			memcpy(t, s, len);
		} else {
			if (DEBUG) fprintf(stderr, "handle_error_from: Error is of type %i\n", lua_type(ran_in, -1));
			t = "Unknown error return in handle_error_from";
			len = strlen(t);
		}
		lua_pop(ran_in, 1);

		lua_checkstack(raise_to, 1);
		lua_pushlstring(raise_to, t, len);
		if (DEBUG) fprintf(stderr, "Lua error raising: %s\n", t);
		free(t); // If either of the above two raise (which they could if there were a memory error), we leak. We could fix with a pcall, but TBH if we're getting memory errors, it's probably over.
		// raise_to: [..., error_str], ran_in: [...]
		lua_error(raise_to);
	}
}

static int TMP_KEY; // Unique address to passaround return values using lua_cpcall.

// Reinterpret userdata as cfunction and put it in TMP_KEY.
static int pushcfunction_inner(lua_State *L) {
	lua_CFunction func = (lua_CFunction)lua_touserdata(L, 1);
	lua_pop(L, 1);
	lua_pushlightuserdata(L, &TMP_KEY);
	lua_pushcfunction(L, func);
	lua_rawset(L, LUA_REGISTRYINDEX);
	return 0;
}

// Get a lua_cfunction safely onto the stack of L, or raise in raise_errors_in
// Bit of a hack, but it's "safe" in the sense that it cannot error locally.
// Needs 1 stack in L, though lua_cpcall probably guarantees that.
void pushcfunction_from_outside(lua_State *raise_errors_in, lua_State *L, lua_CFunction func) {
	if (DEBUG) fprintf(stderr, "pushcfunction_from_outside\n");
	handle_error_from(raise_errors_in, L, lua_cpcall(L, pushcfunction_inner, func));
	lua_pushlightuserdata(L, &TMP_KEY);
	lua_rawget(L, LUA_REGISTRYINDEX);
}


// cpcall func in call_in with call_from as param, re-raising errors in call_from.
void remote_cpcall(lua_State *call_from, lua_State *call_in, lua_CFunction func) {
	if (DEBUG) fprintf(stderr, "remote_cpcall\n");
	handle_error_from(call_from, call_in, lua_cpcall(call_in, func, call_from));
}

void remote_pcall(lua_State *raise_errors_in, lua_State *call, int nargs, int nresults) {
	if (DEBUG) fprintf(stderr, "remote_pcall\n");
	handle_error_from(raise_errors_in, call, lua_pcall(call, nargs, nresults, 0));
}

static int MAIN_THREAD_KEY; // Registry key to store the main thread of a lua_State*.
lua_State *get_main_thread(lua_State *raise_errors_in, lua_State *L) {
	lua_State *main;
	if (lua_pushthread(L)) {
		lua_pop(L, 1);
		return L;
	} else {
		lua_pushlightuserdata(L, &MAIN_THREAD_KEY);
		lua_rawget(L, LUA_REGISTRYINDEX);
		main = lua_tothread(L, -1);
		lua_pop(L, 2);
		if (main == NULL) {
			lua_pushstring(raise_errors_in, "get_main_thread: MAIN_THREAD_KEY does not exist in registry");
			lua_error(raise_errors_in);
		}
		return main;
	}
}

void set_main_thread(lua_State *L) {
	lua_pushlightuserdata(L, &MAIN_THREAD_KEY);
	if (!lua_pushthread(L)) {
		lua_pushstring(L, "set_main_thread called not in main thread");
		lua_error(L);
	}
	lua_rawset(L, LUA_REGISTRYINDEX);
}

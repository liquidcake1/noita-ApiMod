#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include "proxy.h"
#include "create_proxy.h"
#include "util.h"
#include "transfer.h"

// In state to, copy string at idx in from to top of to.
static void copy_string_inner(lua_State *to, lua_State *from, int idx) {
	size_t len;
	// Allegedly this can raise a memory error, but I think only if
	// it's not already a string.
	//
	// Checking the _current_ luajit source, that's the case -- it
	// either straight returns if it's a string, or if it's a
	// number, does a string conversion into the string table,
	// which can throw a memory error.
	//
	// So it can't raise here, as we know it's already a string.
	// Woohoo!
	const char *s = lua_tolstring(from, idx, &len);
	if (DEBUG) fprintf(stderr, "Transferring a string from %lx to %lx: %s\n", (long)from, (long)to, s);
	lua_pushlstring(to, s, len);
}

static int copy_string(lua_State *to) {
	lua_State *from = (lua_State*)lua_touserdata(to, 1);
	copy_string_inner(to, from, -1);
	return 1;
}

// Copy outgoing stack index idx to the top of to's stack.
// We assume there is space in to's stack for the object, but errors must only occur in raise_errors_in, which could be either.
// Assumption:
// * to has enough stack space.
// * errors raised in raise_errors_in will bubble to both to and from, so their stack is ours.
static void transfer_index(lua_State *raise_errors_in, lua_State *to, lua_State *from, int idx) {
	switch(lua_type(from, idx)) {
	case LUA_TNIL:
		if (DEBUG) fprintf(stderr, "Transferring a nil from %lx to %lx\n", (long)from, (long)to);
		lua_pushnil(to);
		break;
	case LUA_TNUMBER:
		lua_pushnumber(to, lua_tonumber(from, idx));
		if (DEBUG) fprintf(stderr, "Transferring a number from %lx to %lx: %f\n", (long)from, (long)to, lua_tonumber(from, idx));
		break;
	case LUA_TBOOLEAN:
		lua_pushboolean(to, lua_toboolean(from, idx));
		if (DEBUG) fprintf(stderr, "Transferring a bool from %lx to %lx: %i\n", (long)from, (long)to, lua_toboolean(from, idx));
		break;
	case LUA_TSTRING:
		if (raise_errors_in == to) {
			copy_string_inner(to, from, idx);
		} else {
			lua_pushvalue(from, idx);
			if (DEBUG) fprintf(stderr, "transfer: pushcfunction_from_outside\n");
			pushcfunction_from_outside(from, to, copy_string);
			lua_pushlightuserdata(to, from);
			if (DEBUG) fprintf(stderr, "transfer: remote_pcall\n");
			remote_pcall(raise_errors_in, to, 1, 1);
			lua_pop(from, 1);
		}
		break;
	case LUA_TTABLE:
		if (DEBUG) fprintf(stderr, "Transferring a table from %lx to %lx.\n", (long)from, (long)to);
		create_proxy(raise_errors_in, to, from, idx);
		break;
	case LUA_TFUNCTION:
		if (DEBUG) fprintf(stderr, "Transferring a function from %lx to %lx.\n", (long)from, (long)to);
		create_proxy(raise_errors_in, to, from, idx);
		break;
	case LUA_TUSERDATA: {
		// If this is a RemoteProxy in from, and the RemoteProxy's real object lives in to, resolve it to the real thing.
		// If the real object lives elsewhere, create a new proxy from _there_ to to.
		lua_getmetatable(from, idx);
		lua_pushlightuserdata(from, &REMOTE_PROXY_METATABLE_KEY);
		lua_rawget(from, LUA_REGISTRYINDEX);
		if (!lua_rawequal(from, -1, -2)) {
			lua_pop(from, 2);
			if (DEBUG) fprintf(stderr, "Transferring a userdata %lx from %lx to %lx.\n", (long)lua_touserdata(from, idx), (long)from, (long)to);
			create_proxy(raise_errors_in, to, from, idx);
			break;
		}
		lua_pop(from, 2);
		if (DEBUG) fprintf(stderr, "Transferring a proxy %lx (%i type %i) from %lx to %lx.\n", (long)lua_touserdata(from, idx), idx, lua_type(from, idx), (long)from, (long)to);
		RemoteProxy *rp = (RemoteProxy*)lua_touserdata(from, idx);
		if (rp->dead) {
			if (DEBUG) fprintf(stderr, "It's dead.\n");
			// It's dead, Fred. So I guess we'll just make another dead one in the target...
			lua_pushlightuserdata(to, new_remote_proxy);
			lua_pushlightuserdata(to, rp->owner);
			if (DEBUG) fprintf(stderr, "transfer: remote_pcall\n");
			remote_pcall(raise_errors_in, to, 1, 1);
		} else if (rp->owner == to) {
			if (DEBUG) fprintf(stderr, "It's going home.\n");
			// Convert it back to the real object.
			lua_pushlightuserdata(to, rp);
			lua_rawget(to, LUA_REGISTRYINDEX);
			if (DEBUG) fprintf(stderr, "Type of return is %i.\n", lua_type(to, -1));
		} else {
			if (DEBUG) fprintf(stderr, "It's some new thing.\n");
			// It's for some third state. We don't own that state's stack, so time for some magic...
			struct ProxyRequest pr = {rp, to};
			if (DEBUG) fprintf(stderr, "transfer: special pcall\n");
			handle_error_from(raise_errors_in, rp->owner, lua_cpcall(rp->owner, create_proxy_for, &pr));
		}
		break;
	}
	case LUA_TLIGHTUSERDATA:
		if (DEBUG) fprintf(stderr, "Lua error raising: Will not proxy a lightuserdata.\n");
		lua_pushstring(raise_errors_in, "Will not proxy a lightuserdata.");
		lua_error(raise_errors_in);
		break;
	case LUA_TTHREAD:
		if (DEBUG) fprintf(stderr, "Lua error raising: Will not proxy a thread.\n");
		lua_pushstring(raise_errors_in, "Will not proxy a thread.");
		lua_error(raise_errors_in);
		break; // Unreachable
	default:
		if (DEBUG) fprintf(stderr, "Lua error raising: Will not proxy an unknown type.\n");
		lua_pushstring(raise_errors_in, "Will not proxy an unknown type.");
		lua_error(raise_errors_in);
		break; // Unreachable
	}
}

// Take over from pcall_transfer, now in "call_in"'s state.
// * call_in's stack must be [call_data_from as userdata].
// * call_data_from's stack must be [lua_CFunction as userdata, arg1, ..., argn, &nargs as userdata, &nret as userdata].
// * We perform the guarantees of lua_cpcall_transfer excepting raising an error in call_data_from:
//   - either call_in's stack is [ret1, ..., retn], or
//   - an error is raised in call_in which should be reraised by lua_cpcall_transfer in call_data_from.
static int pcall_transfer_internal(lua_State *call_in) {
	if (DEBUG) fprintf(stderr, "In pcall_transfer_internal (%lx)\n", (long)call_in);
	luaL_checkany(call_in, 1);
	lua_State *call_data_from = (lua_State *)lua_touserdata(call_in, -1);
	lua_pop(call_in, 1);
	if (DEBUG) fprintf(stderr, "pcall_transfer_internal (call_data_from = %lx)\n", (long)call_data_from);

	// call_in: []; call_data_from: [func as userdata, arg1, ..., argn, nargs as userdata, nret as userdata]
	int *p_nargs = lua_touserdata(call_data_from, -2);
	if (DEBUG) fprintf(stderr, "pcall_transfer_internal: nargs is %lx\n", (long)p_nargs);
	int *p_nret = lua_touserdata(call_data_from, -1);
	if (DEBUG) fprintf(stderr, "pcall_transfer_internal: nret is %lx\n", (long)p_nret);
	lua_pop(call_data_from, 2);
	if (p_nargs == NULL || p_nret == NULL) {
		if (DEBUG) fprintf(stderr, "Lua error raising: Ooops, an args is null?\n");
		lua_pushstring(call_in, "Ooops, an args is null?");
		lua_error(call_in);
	}
	int nargs = *p_nargs;
	int nret = *p_nret;

	lua_checkstack(call_in, nargs + 1);

	// Grab nargs+1 deep as a userdata-encoded cfunction, and push it.
	lua_CFunction func = (lua_CFunction)lua_touserdata(call_data_from, -nargs - 1);
	lua_pushcfunction(call_in, func);
	if (DEBUG) fprintf(stderr, "pcall_transfer_internal: func is %lx at %i\n", (long)func, -nargs - 1);

	// Then push the rest of the args, deepest first.
	for(int i=-nargs; i<0; i++) {
		if (DEBUG) fprintf(stderr, "pcall_transfer_internal: transfer %i\n", i);
		transfer_index(call_in, call_in, call_data_from, i);
	}

	// Pop everything we just transfered or copied.
	lua_pop(call_data_from, nargs + 1);
	size_t old_top = lua_gettop(call_in) - nargs - 1;
	// call_in: [func as cfunction, arg1, ..., argn]; call_data_from: []

	if (DEBUG) fprintf(stderr, "pcall_transfer_internal: Call!\n");
	lua_call(call_in, nargs, nret);
	// call_in: [ret1, ..., retn]; call_data_from: []

	// We already checked call_data_from has the stack space in lua_cpcall_transfer.
	int stop;
	if (nret == LUA_MULTRET) {
		stop = lua_gettop(call_in);
		if (stop - old_top > MAXMULTRET) {
			stop = old_top + MAXMULTRET;
		}
	} else {
		stop = old_top + nret;
	}
	if (DEBUG) fprintf(stderr, "pcall_transfer_internal: nret is %i (LUA_MULTRET=%i); transfer %i to %i\n", nret, LUA_MULTRET, old_top + 1, stop);
	for(int i=old_top + 1; i <= stop; i++) {
		if (DEBUG) fprintf(stderr, "pcall_transfer_internal: transfer back %i\n", i);
		transfer_index(call_in, call_data_from, call_in, i);
	}
	lua_settop(call_in, 0);
	// call_in: []; call_data_from: [ret1, ..., retn]
	return 0;
}

// Call a lua function in call_in with data from call_data_from.
// call_data_from: [lua_CFunction as lightuserdata, arg1, ..., argn]
// call_data_from stack must have enough room for 2 more elements right now, and for nret after the above is popped
// Either:
// * The whole listed stack (nargs + 3) of call_data_from will be popped, and nret items will be pushed, as with a pcall, or
// * An error will be raised in call_data_from.
// Either way, the stack of call_in will be left alone.
int pcall_transfer(lua_State *call_data_from, lua_State *call_in, int nargs, int nret) {
	const int already_there = lua_gettop(call_data_from);
	if (nret == LUA_MULTRET) {
		lua_checkstack(call_data_from, already_there + 2 > MAXMULTRET ? 2 : MAXMULTRET - already_there);
	} else {
		lua_checkstack(call_data_from, already_there + 1 > nret ? 2 : nret - already_there);
	}
	lua_pushlightuserdata(call_data_from, &nargs);
	lua_pushlightuserdata(call_data_from, &nret);
	if (DEBUG) fprintf(stderr, "Stack set up, about to remote_cpcall (pcall_transfer)\n");
	remote_cpcall(call_data_from, call_in, pcall_transfer_internal);
	if (DEBUG) fprintf(stderr, "remote_cpcall returned (pcall_transfer)\n");
	return 0;
}

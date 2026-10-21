#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <malloc.h>

static lua_State *my_global;

#define DEBUG 0

// Running in incoming (so don't change outgoing's stack!), copy outgoing stack index idx to the top of incoming's stack.
// Assumption:
// * to has enough stack space.
// * errors raised in to will not corrupt the stack (ie. errors will bubble up to a pcall in to's state)
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
		size_t len;
		const char *s = lua_tolstring(from, idx, &len);
		if (DEBUG) fprintf(stderr, "Transferring a string from %lx to %lx: %s\n", (long)from, (long)to, s);
		lua_pushlstring(to, s, len);
		break;
	case LUA_TTABLE:
		// Need to recurse, or make a proxy. Recurse is horrible because of need to modify both stacks (ie. we need to cpcall the source and yet get data back from it), so proxy is better?
		// Which semantic is actually good, though? I think probably proxy table because if the provider wants a copy, it can just make one itself. If we copy, there's no option!
		lua_pushstring(raise_errors_in, "Cannot yet proxy a table.");
		lua_error(raise_errors_in);
		break; // Unreachable
	case LUA_TFUNCTION:
		// Need to make a proxy (function should always call in to's state).
		lua_pushstring(raise_errors_in, "Cannot yet proxy a function.");
		lua_error(raise_errors_in);
		break; // Unreachable
	case LUA_TLIGHTUSERDATA:
		if (DEBUG) fprintf(stderr, "Transferring a lightuserdata from %lx to %lx. I hope you know what you're doing.\n", (long)from, (long)to);
		lua_pushlightuserdata(to, lua_touserdata(from, idx));
		break;
	case LUA_TTHREAD:
	case LUA_TUSERDATA:
		lua_pushstring(raise_errors_in, "Will not proxy a userdata or thread.");
		lua_error(raise_errors_in);
		break; // Unreachable
	default:
		lua_pushstring(raise_errors_in, "Will not proxy an unknown type.");
		lua_error(raise_errors_in);
		break; // Unreachable
	}
}

// After a cpcall in from, grab and pop its error, clean it out and then raise it as our own error.
// raise_to is the current protected context; ran_in is the one that returned.
static void handle_error_from(lua_State *raise_to, lua_State *ran_in, int retval) {
	if (DEBUG) fprintf(stderr, "handle_error_from: retval is %i\n", retval);
	if (retval != 0) {
		// raise_to: [...], ran_in: [..., error_str]
		size_t len;
		// lua_tolstring only raises if the index isn't a string.
		// If we caught an error, it was a string.
		// So it cannot raise.
		const char *s = lua_tolstring(ran_in, -1, &len);
		if (DEBUG) fprintf(stderr, "handle_error_from: Error is %s\n", s);
		lua_pushlstring(raise_to, s, len);
		lua_pop(ran_in, 1);
		// raise_to: [..., error_str], ran_in: [...]
		lua_error(raise_to);
	}
}

// cpcall func in call_in with call_from as param, re-raising errors in call_from.
void remote_cpcall(lua_State *call_in, lua_State *call_from, lua_CFunction func) {
	handle_error_from(call_from, call_in, lua_cpcall(call_in, func, call_from));
}

static int settable_with_idxptr(lua_State*);

// Take over from lua_cpcall_transfer.
// * call_in's stack must be [call_data_from as userdata].
// * call_in's stack must be [lua_CFunction as userdata, arg1, ..., argn, &nargs as userdata, &nret as userdata].
// * We must be allowed to modify the stack of call_in freely.
// * We perform the guarantees of lua_cpcall_transfer excepting raising an error in call_data_from:
//   - either call_in's stack is [ret1, ..., retn], or
//   - an error is raised in call_in which should be reraised by lua_cpcall_transfer in call_data_from.
static int lua_cpcall_transfer_internal(lua_State *call_in) {
	if (DEBUG) fprintf(stderr, "In lua_cpcall_transfer_internal (%lx)\n", (long)call_in);
	luaL_checkany(call_in, 1);
	lua_State *call_data_from = (lua_State *)lua_touserdata(call_in, -1);

	// call_in: []; call_data_from: [func as userdata, arg1, ..., argn, nargs as userdata, nret as userdata]
	int nargs = *((int*)lua_touserdata(call_data_from, -2));
	if (DEBUG) fprintf(stderr, "lua_cpcall_transfer_internal: nargs is %i\n", nargs);
	int nret = *((int*)lua_touserdata(call_data_from, -1));
	if (DEBUG) fprintf(stderr, "lua_cpcall_transfer_internal: nret is %i\n", nret);

	lua_checkstack(call_in, nargs + 1);

	// Grab nargs+1 deep as a userdata-encoded cfunction, and push it.
	lua_CFunction func = (lua_CFunction)lua_touserdata(call_data_from, -nargs - 3);
	lua_pushcfunction(call_in, func);
	if (DEBUG) fprintf(stderr, "lua_cpcall_transfer_internal: func is %lx; settable_with_idxptr is %lx\n", (long)func, (long)settable_with_idxptr);

	// Then push the rest of the args, deepest first.
	for(int i=-nargs - 2; i<-2; i++) {
		if (DEBUG) fprintf(stderr, "lua_cpcall_transfer_internal: transfer %i\n", i);
		transfer_index(call_in, call_in, call_data_from, i);
	}

	// Pop everything we just transfered or copied.
	lua_pop(call_data_from, nargs + 3);
	// call_in: [func as cfunction, arg1, ..., argn]; call_data_from: []

	if (DEBUG) fprintf(stderr, "lua_cpcall_transfer_internal: Call!\n");
	lua_call(call_in, nargs, nret);
	// call_in: [ret1, ..., retn]; call_data_from: []

	// We already checked call_data_from has the stack space in lua_cpcall_transfer.
	for(int i=-nret; i<0; i++) {
		transfer_index(call_in, call_data_from, call_in, i);
	}
	lua_pop(call_in, nret);
	// call_in: []; call_data_from: [ret1, ..., retn]
	return 0;
}

// Call a lua function in call_in with data from call_data_from.
// call_data_from must have the following stack (left is deepest):
// [lua_CFunction as userdata, arg1, ..., argn]
// call_data_from stack must have enough room for 2 more elements right now, and for nret after the above is popped
// Either:
// * The whole listed stack (nargs + 3) of call_data_from will be popped, and nret items will be pushed, as with a pcall, or
// * An error will be raised in both call_in and call_data_from.
// Either way, the stack of call_in will be left alone.
static int lua_cpcall_transfer(lua_State *call_in, lua_State *call_data_from, int nargs, int nret) {
	lua_checkstack(call_data_from, nargs + 3 + 2 > nret ? 2 : nret - nargs + 3);
	lua_pushlightuserdata(call_data_from, &nargs);
	lua_pushlightuserdata(call_data_from, &nret);
	if (DEBUG) fprintf(stderr, "Stack set up, about to remote_cpcall (lua_cpcall_transfer)\n");
	remote_cpcall(call_in, call_data_from, lua_cpcall_transfer_internal);
	if (DEBUG) fprintf(stderr, "remote_cpcall returned (lua_cpcall_transfer)\n");
	return 0;
}

static int settable_with_idxptr(lua_State *table_owner) {
	// table_owner: [key, value, &idx as userdata]
	if (lua_type(table_owner, -1) != LUA_TLIGHTUSERDATA) {
		// Error is raised in the current execution frame in table_owner, reraised in the calling one by handle_error_from.
		if (DEBUG) fprintf(stderr, "Top of stack type is %i\n", lua_type(table_owner, -1));
		lua_pushstring(table_owner, "Top of the stack must be a lightuserdata (int pointer).");
		lua_error(table_owner);
	}
	int idx = *((int*)lua_touserdata(table_owner, -1));
	lua_pop(table_owner, 1);
	lua_settable(table_owner, idx);
	return 0;
}

static int settable(lua_State *L) {
	if (DEBUG) fprintf(stderr, "In settable\n");
	luaL_checkany(L, 1); // key
	luaL_checkany(L, 2); // value

	lua_pushlightuserdata(L, settable_with_idxptr);
	lua_pushvalue(L, 1); // key
	lua_pushvalue(L, 2); // value
	int idx = LUA_REGISTRYINDEX;
	lua_pushlightuserdata(L, &idx);

	// L: [key, value|settable_with_idxptr, key, value, &LUA_REGISTRYINDEX]
	if (DEBUG) fprintf(stderr, "Stack set up, about to lua_cpcall_transfer\n");
	lua_cpcall_transfer(my_global, L, 3, 0);
	// L: [key, value|]
	return 0;
}

static int gettable_with_idxptr(lua_State *table_owner) {
	// table_owner: [key, &idx as userdata]
	if (lua_type(table_owner, -1) != LUA_TLIGHTUSERDATA) {
		// Error is raised in the current execution frame in table_owner, reraised in the calling one by handle_error_from.
		if (DEBUG) fprintf(stderr, "Top of stack type is %i\n", lua_type(table_owner, -1));
		lua_pushstring(table_owner, "Top of the stack must be a lightuserdata (int pointer).");
		lua_error(table_owner);
	}
	int idx = *((int*)lua_touserdata(table_owner, -1));
	lua_pop(table_owner, 1);
	lua_gettable(table_owner, idx);
	return 1;
}

static int gettable(lua_State *L) {
	luaL_checkany(L, 1);

	lua_pushlightuserdata(L, gettable_with_idxptr);
	lua_pushvalue(L, 1); // key
	int idx = LUA_REGISTRYINDEX;
	lua_pushlightuserdata(L, &idx);

	// L: [key|rettable_with_idxptr, key, &LUA_REGISTRYINDEX]
	lua_cpcall_transfer(my_global, L, 2, 1);
	// L: [key|value]

	return 1;
}

static const luaL_Reg funcs[] = { {"settable", settable}, {"gettable", gettable}, {NULL, NULL} };

__declspec(dllexport) int luaopen_api_mod(lua_State *L) {
	lua_newtable(L);
	luaL_register(L, NULL, funcs);
	return 1;
}

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

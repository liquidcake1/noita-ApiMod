#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <malloc.h>

static lua_State *my_global;

#define DEBUG 0

// userdata held in a "remote" lua_State to facilitate proxy calls
struct RemoteProxy {
	bool dead;  // Unusable if true.
	lua_State *owner;
	bool is_table; // true for table, false for function.
	// We know (lua_State L, RemoteProxy p).
	// If dead, never do anything -- owner is invalid.
	// We __call (not is_table) by proxy call to original = owner:registry[&p]
	// We __index (is_table) by proxy gettable to original = owner:registry[&p]
	// We __newindex (is_table) by proxy settable to original = owner:registry[&p]
	// We __gc by:
	//   Destroy owner:registry[L][original] (this is a lud to us)
	//   Destroy owner:registry[L] (if empty)
	//   Destroy owner:registry[&p] (this will set our dead, but that's OK).
	// proxy_cache is needed else proxies needs a list of finalisers. This is fine, but inefficient.
	// To make a new proxy for original in owner for L, either:
	//   Obtain owner:registry[L][original]
	//     Look it up in caller:registry to return RemoteProxy real
	//   Or do all of:
	//     Create a userdata RemoteProxy p in the calling state.
	//     Proxy call to owner, and:
	//       Create a userdata ProxyFinaliser f
	//       Install owner:registry[&p] = f
	//       Install owner:registry[L][original] = p // keepalive to original plus cache
	//       Install caller:registry[&p] = p // to allow lookup
	//       Set the metatable of f to owner:registry:proxyteardown
	//     Set the metatable of p to caller:registry:proxy
	//     Add caller:registry:proxy_cache[p]
	// When proxying a proxy, go back to the original and proxy that instead.
};

// userdata held in an "owner" lua_State to facilitate safety over death.
struct ProxyFinaliser {
	bool dead;
	RemoteProxy *other;
	// We __gc by (if not dead):
	//   Set other->dead
	// We don't need anything else.
};

static void remote_pcall(lua_State *raise_errors_in, lua_State *call, int nargs, int nresults) {
	if (lua_pcall(call, nargs, nresults, 0)) {
		size_t len;
		const char *s = lua_tolstring(from, -1, &len);
		lua_pushlstring(to, s, len);
		lua_error(to);
	}
}

// In state of from, set a new proxy table for to and leave it top of the stack.
static void setup_outgoing_proxy_cache_for_inner(lua_State *from, lua_State *to) {
	lua_newtable(from); // Push first so we can copy for rawset, and return it.
	lua_pushlightuserdata(from, to);
	lua_pushvalue(from, -2);
	lua_rawset(from, LUA_REGISTRYINDEX);
}

// lua_CFunction.
// Param is just to state. to is not modified; it's treated as pure lightuserdata.
// Returns the table it adds
static int setup_outgoing_proxy_cache_for(lua_State *from) {
	luaL_checkany(from, 1);
	lua_checkstack(from, 3);
	lua_State *to = (lua_State *)lua_touserdata(from, -1);
	if (to == NULL) {
		lua_pushstring(from, "setup_outgoing_proxy_cache_for: Param must be lua_State *");
		lua_error(from);
	}
	setup_outgoing_proxy_cache_for_inner(from, to);
	return 1;
}

// Action: in from, ensure there is proxy cache for to.
// before: from: [] + 3
// after: from: [proxy cache for to]
static void ensure_outgoing_proxy_cache_for(lua_State *raise_errors_in, lua_State *from, lua_State *to) {
	lua_pushlightuserdata(from, to);
	lua_rawget(from, -2);
	// from: [proxy cache for to or nil]
	if (!lua_isnil(from, -1)) {
		return;
	}
	// First outgoing proxy to to, so make a table.
	lua_pop(from, 1); // pop the nil
	if (raise_errors_in == from) {
		setup_outgoing_proxy_cache_for_inner(from, to);
		// from: [proxy cache for to]
	} else {
		lua_pushcfunction(from, setup_outgoing_proxy_cache_for);
		lua_pushlightuserdata(from, to);
		remote_pcall(to, from, 1, 1, 0);
	}
}

// In the state of to, make a userdata RemoteProxy which points at from.
// Before: to: [] + 3, from: [] + 0
// After: to: [RemoteProxy], from: [RemoteProxy*]
static RemoteProxy *new_remote_proxy_inner(lua_State *to, lua_State *from) {
	// lua_checkstack(to, 2);
	RemoteProxy *rp = (RemoteProxy*)lua_newuserdata(to, sizeof(RemoteProxy));
	if (rp == NULL) {
		lua_pushstring(to, "new_remote_proxy_inner: Could not allocate");
		lua_error(to);
	}
	rp->dead = true;
	rp->owner = from;
	// TODD rp->is_table = ??? -- do we even need this? We can just choose the metatable based on it?
	lua_newtable(to); // TODO initialise! // TODO lua_newmetatable?
	lua_setmetatable(to, -2);
	// to: [RemoteProxy], from: []
	lua_pushvalue(to, -1);
	lua_pushvalue(to, -1);
	lua_rawset(to, LUA_REGISTRYINDEX); // to:registry[rp] = rp // TODO defer this and push placeholder then nil? Otherwise need to clean up or leak.
	// to: [RemoteProxy], from: []
	return rp;
}

// lua_CFunction.
// Params: [from]
// Return: [RemoteProxy]
// Before: from: [] + 0
// After: from: []
static int new_remote_proxy(lua_State *to) {
	lua_State *from = (lua_State *)lua_touserdata(to, -1);
	if (from == NULL) {
		lua_pushstring(to, "new_remote_proxy: First parameter must be lua_State*");
		lua_error(to);
	}
	lua_checkstack(to, 3);
	new_remote_proxy_inner(to, from); // pushes lud to from
	return 1;
}

// In the state of from, make a userdata ProxyFinaliser which points at to's RemoteProxy.
// Before: from: [] + 3
// After: from: []
static RemoteProxy *new_proxy_finaliser_inner(lua_State *from, RemoteProxy *rp) {
	// lua_checkstack(to, 2);
	ProxyFinaliser *rp = (ProxyFinaliser*)lua_newuserdata(from, sizeof(ProxyFinaliser));
	if (rp == NULL) {
		lua_pushstring(from, "new_remote_proxy_inner: Could not allocate");
		lua_error(from);
	}
	pf->dead = true;
	pf->other = rp;
	lua_newtable(from); // TODO initialise! // TODO lua_newmetatable?
	lua_setmetatable(from, -2);
	// from: [ProxyFinaliser]
	lua_pushvalue(from, rp);
	lua_pushvalue(from, -2);
	lua_rawset(from, LUA_REGISTRYINDEX); // from:registry[rp] = pf // TODO defer this? Otherwise we can leak?
	// from: [ProxyFinaliser]
	lua_pop(from, 1);
	return pf;
}

// lua_CFunction.
// Params: [proxy_cache for to, obj, RemoteProxy *]
// Return: []
static int new_proxy_finaliser(lua_State *from) {
	luaL_checktype(from, 1, LUA_TTABLE);
	RemoteProxy *rp = (RemoteProxy *)lua_touserdata(from, 3);
	if (rp == NULL) {
		lua_pushstring(from, "new_proxy_finaliser: First parameter must be RemoteProxy*");
		lua_error(from);
	}
	lua_pop(from, 1);
	lua_rawset(from, LUA_REGISTRYINDEX); // from:registry[L][obj] = rp
	new_remote_proxy_inner(from, rp);
	return 0;
}

// Before: from: [obj, ...]; to: []
// After: from: [obj, ...]; to: [RemoteProxy for p]
static void create_proxy(lua_State *raise_errors_in, lua_State *to, lua_State *from, int idx) {
	// TODO type check that obj can be proxied.
	// from: [obj, ...], to: []
	ensure_outgoing_proxy_cache_for(raise_errors_in, from, to);
	// from: [obj, ..., proxy cache for to]; to: []
	lua_pushvalue(from, idx - 1);
	lua_rawget(from, -2);
	// from: [obj, ..., proxy_cache for to, RemoteProxy* for obj or nil], to: []
	if (!lua_isnil(from, -1)) {
		// We do have a proxy, but we need to make a full proxy.
		// from: [obj, ..., proxy_cache for to, RemoteProxy* for obj], to: []
		RemoteProxy *rp = (RemoteProxy*)lua_touserdata(from, -1);
		if (rp == NULL) {
			// Should not happen.
			lua_pushstring(raise_errors_in, "create_proxy: proxy cache was not a lightuserdata");
			lua_error(raise_errors_in);
		} else if (rp->dead) {
			// Should happen only after from dies, so again, not ever if we're here...
			lua_pushstring(raise_errors_in, "create_proxy: proxy cache was dead");
			lua_error(raise_errors_in);
		} else {
			lua_pushlightuserdata(to, rp);
			lua_rawget(to, LUA_REGISTRYINDEX);
			// from: [obj, ..., proxy_cache for to, RemoteProxy* for obj], to: [RemoteProxy for obj]
			lua_pop(from, 2);
			// from: [obj, ...], to: [RemoteProxy for obj]
			return;
		}
	}

	lua_pop(from, 1);
	// from: [obj, ..., proxy cache for to], to: []
	// We don't have a proxy for this object.
	// First, get a RemoteProxy in *to
	if (raise_errors_in == to) {
		// TODO from must have 1 free, to must have 3 free
		RemoteProxy *rp = new_remote_proxy_inner(to, from);
		// from: [obj, ..., proxy cache for to], to: [RemoteProxy for obj]
		lua_pushcfunction(from, new_proxy_finaliser);
		lua_insert(from, -2);
		lua_pushvalue(from, idx - 2);
		lua_pushlightuserdata(from, rp);
		remote_pcall(to, from, 3, 1);
		// from: [obj, ...], to: [RemoteProxy for obj]

		rp->dead = false;
		pf->dead = false;
	} else {
		// We're in from, so need to pcall to.
		// TODO from must have 1 free, to must have 3 free
		lua_pushcfunction(from, new_remote_proxy);
		lua_pushlightuserdata(from, to);
		remote_pcall(from, to, 1, 1);
		RemoteProxy *rp = lua_touserdata(to, -1);
		lua_pushlightuserdata(from, rp);
		// from: [obj, ..., proxy_cache for to, RemoteProxy* for obj], to: [RemoteProxy for obj]

		// TODO we need an extra 1 for from here.
		lua_pushvalue(from, idx - 2);
		lua_insert(from, -2);
		lua_rawset(from, -3); // from:registry[L][obj] = rp
		lua_pop(from, 1);
		// from: [obj, ...]; to: [RemoteProxy for obj]

		ProxyFinaliser* pf = new_proxy_finaliser_inner(from, rp);
		// from: [obj, ...], to: [RemoteProxy for obj]

		rp->dead = false;
		pf->dead = false;
	}
}

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
		// TODO this can raise, so needs pcall proxying if raise_errors_in != to
		// We'll need to push &idx.
		lua_pushlstring(to, s, len);
		break;
	case LUA_TTABLE:
		create_proxy(raise_errors_in, to, from, idx);
		break; // Unreachable
	case LUA_TFUNCTION:
		create_proxy(raise_errors_in, to, from, idx);
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

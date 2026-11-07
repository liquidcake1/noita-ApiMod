#include <lua.h>
#include <lauxlib.h>
#include <stdbool.h>
#include "proxy.h"
#include "proxy_metatables.h"
#include "util.h"

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
		if (DEBUG) fprintf(stderr, "Lua error raising: setup_outgoing_proxy_cache_for: Param must be lua_State *\n");
		lua_pushstring(from, "setup_outgoing_proxy_cache_for: Param must be lua_State *");
		lua_error(from);
	}
	setup_outgoing_proxy_cache_for_inner(from, to);
	return 1;
}

// Action: in from, ensure there is proxy cache for to, and leave it at the top of the stack
// before: from: [] + 3
// after: from: [proxy cache for to]
// We're running in raise_errors_in, but have "some" stack leeway in from (TODO), and raising here will clear from's state.
static void ensure_outgoing_proxy_cache_for(lua_State *raise_errors_in, lua_State *from, lua_State *to_main) {
	lua_pushlightuserdata(from, to_main);
	lua_rawget(from, LUA_REGISTRYINDEX);
	// from: [proxy cache for to or nil]
	if (!lua_isnil(from, -1)) {
		return;
	}
	// First outgoing proxy to to, so make a table.
	lua_pop(from, 1); // pop the nil
	if (raise_errors_in == from) {
		setup_outgoing_proxy_cache_for_inner(from, to_main);
		// from: [proxy cache for to]
	} else {
		if (DEBUG) fprintf(stderr, "ensure_outgoing_proxy_cache_for: pushcfunction_from_outside\n");
		pushcfunction_from_outside(raise_errors_in, from, setup_outgoing_proxy_cache_for);
		lua_pushlightuserdata(from, to_main);
		if (DEBUG) fprintf(stderr, "ensure_outgoing_proxy_cache_for: remote_pcall\n");
		remote_pcall(raise_errors_in, from, 1, 1);
		// from: [proxy cache for to]
	}
}


static int REMOTE_PROXY_LOOKUP_KEY; // Unique address to use as index. Value irrelevant.
// Either make a new weak key cache table, or use the existing one if it's there. Leave it on the stack.
void ensure_weak_remote_proxy_lookup(lua_State *to) {
	lua_pushlightuserdata(to, &REMOTE_PROXY_LOOKUP_KEY);
	lua_rawget(to, LUA_REGISTRYINDEX);
	if (lua_isnil(to, -1)) {
		lua_pop(to, 1);
		lua_newtable(to); // cache
		lua_newtable(to); // mt
		lua_pushstring(to, "__mode");
		lua_pushstring(to, "v");
		lua_rawset(to, -3); // mt["__mode"] = "v"
		lua_setmetatable(to, -2); // cache.mt = mt
		lua_pushlightuserdata(to, &REMOTE_PROXY_LOOKUP_KEY);
		lua_pushvalue(to, -2); // copy cache
		lua_rawset(to, LUA_REGISTRYINDEX);
	}
}

// In the state of to, make a userdata RemoteProxy which points at from, and leave it at the top of the stack.
// Before: to: [...]
// After: to: [..., RemoteProxy]
static RemoteProxy *new_remote_proxy_inner(lua_State *to, lua_State *from) {
	lua_checkstack(to, 3);
	RemoteProxy *rp = (RemoteProxy*)lua_newuserdata(to, sizeof(RemoteProxy));
	if (rp == NULL) {
		if (DEBUG) fprintf(stderr, "Lua error raising: new_remote_proxy_inner: Could not allocate\n");
		lua_pushstring(to, "new_remote_proxy_inner: Could not allocate");
		lua_error(to);
	}
	rp->dead = true;
	rp->owner = get_main_thread(to, from);
	ensure_remote_proxy_metatable(to);
	lua_setmetatable(to, -2);
	// to: [..., RemoteProxy], from: []
	ensure_weak_remote_proxy_lookup(to);
	lua_pushlightuserdata(to, rp);
	lua_pushvalue(to, -3);
	lua_rawset(to, -3); // to:registry:cache[&rp] = rp
	lua_pop(to, 1);
	// to: [..., RemoteProxy], from: []
	return rp;
}

// lua_CFunction.
// Params: [from]
// Return: [RemoteProxy]
int new_remote_proxy(lua_State *to) {
	if (DEBUG) fprintf(stderr, "new_remote_proxy\n");
	lua_State *from = (lua_State *)lua_touserdata(to, -1);
	if (from == NULL) {
		if (DEBUG) fprintf(stderr, "Lua error raising: new_remote_proxy: First parameter must be lua_State*\n");
		lua_pushstring(to, "new_remote_proxy: First parameter must be lua_State*");
		lua_error(to);
	}
	lua_checkstack(to, 3);
	new_remote_proxy_inner(to, from); // pushes lud to from
	return 1;
}

// In the state of from, make a userdata ProxyFinaliser which points at to's RemoteProxy.
// We don't return it, it's needed only to GC.
// Before: from: [..., proxy cache for to_main, obj, RemoteProxy*] + 4
// After: from: [..., ProxyFinaliser]
static ProxyFinaliser *new_proxy_finaliser_inner(lua_State *from) {
	RemoteProxy *rp = (RemoteProxy*)lua_touserdata(from, -1);
	lua_pop(from, 1);
	ProxyFinaliser *pf = (ProxyFinaliser*)lua_newuserdata(from, sizeof(ProxyFinaliser));
	if (pf == NULL) {
		if (DEBUG) fprintf(stderr, "Lua error raising: new_proxy_finaliser_inner: Could not allocate\n");
		lua_pushstring(from, "new_proxy_finaliser_inner: Could not allocate");
		lua_error(from);
	}
	pf->dead = true;
	pf->other = rp;
	ensure_proxy_finaliser_metatable(from); // Needs 3 space
	lua_setmetatable(from, -2);
	// from: [proxy cache for to_main, obj, pf]
	lua_pushlightuserdata(from, rp);
	lua_pushvalue(from, -3);
	if (DEBUG) fprintf(stderr, "new_proxy_finaliser_inner: Setting up from:registry[%lx] to type %i\n", (long)rp, lua_type(from, -1));
	lua_rawset(from, LUA_REGISTRYINDEX); // from:registry[&rp] = obj // TODO defer this? A crash after will leak obj forever...
	// from: [proxy cache for to_main, obj, pf]
	lua_pushvalue(from, -1);
	lua_insert(from, -4);
	// from: [pf, proxy cache for to_main, obj, pf]
	if (DEBUG) fprintf(stderr, "new_proxy_finaliser_inner: Pointing proxy cache for caller for obj to %lx (which hopefully is %lx)\n", (long)lua_touserdata(from, -1), (long)pf);
	lua_rawset(from, -3); // from:registry[to_main][obj] = pf // TODO similarly, this will also leak obj and pf if we crash...
	// from: [pf, proxy cache for to_main]
	lua_pop(from, 1);
	return pf;
}

// lua_CFunction.
// Params: [proxy_cache for to, obj, RemoteProxy*]
// Return: []
static int new_proxy_finaliser(lua_State *from) {
	if (DEBUG) fprintf(stderr, "new_proxy_finaliser\n");
	if (DEBUG) fprintf(stderr, "new_proxy_finaliser: Start with top of stack type %i, size %i\n", lua_type(from, -1), lua_gettop(from));
	new_proxy_finaliser_inner(from);
	return 1;
}

// Before: from: [obj, ...]; to: []
// After: from: [obj, ...]; to: [RemoteProxy for p]
void create_proxy(lua_State *raise_errors_in, lua_State *to, lua_State *from, int idx) {
	if (idx < 0) {
		// Convert to absolute.
		idx = lua_gettop(from) + 1 + idx;
	}
	// from: [obj, ...], to: []
	ensure_outgoing_proxy_cache_for(raise_errors_in, from, get_main_thread(raise_errors_in, to));
	// from: [obj, ..., proxy cache for to_main]; to: []
	lua_pushvalue(from, idx);
	lua_rawget(from, -2);
	// from: [obj, ..., proxy_cache for to_main, ProxyFinaliser for obj or nil], to: []
	if (!lua_isnil(from, -1)) {
		if (DEBUG) fprintf(stderr, "create_proxy: Found a cache %lx!\n", (long)lua_touserdata(from, -1));
		// from: [obj, ..., proxy_cache for to, ProxyFinaliser for obj], to: []
		ProxyFinaliser *pf = (ProxyFinaliser*)check_userdata(raise_errors_in, from, -1, &PROXY_FINALISER_METATABLE_KEY);
		if (pf->dead) {
			// Sanity check: Should happen only after to's RemoteProxy was gc-ed, but that should also delete the lookup!
			// It _could_ happen if the gc crashed midway, perhaps. We could recover by deleting here and then making a new one.
			lua_pushstring(raise_errors_in, "create_proxy: ProxyFinaliser was dead");
			lua_error(raise_errors_in);
		}
		if (DEBUG) fprintf(stderr, "create_proxy: Cache is valid!\n");
		// We do have a proxy; look it up in to to upgrade it to a full one.
		lua_pop(from, 2);
		// from: [obj, ...], to: []
		lua_pushlightuserdata(to, &REMOTE_PROXY_LOOKUP_KEY);
		lua_rawget(to, LUA_REGISTRYINDEX); // Note if the RemoteProxy exists, the cache table must, too.
		if (DEBUG) fprintf(stderr, "create_proxy: REMOTE_PROXY_LOOKUP_KEY yields type %i!\n", lua_type(to, -1));
		lua_pushlightuserdata(to, pf->other);
		lua_rawget(to, -2); // Convert to real userdata
		lua_remove(to, -2); // Remove cache table from stack
		// from: [obj, ...], to: [RemoteProxy for obj]
		return;
	}

	lua_pop(from, 1);
	// from: [obj, ..., proxy cache for to_main], to: []
	// We don't have a proxy for this object.
	// First, get a RemoteProxy in *to
	if (raise_errors_in == to) {
		if (DEBUG) fprintf(stderr, "create_proxy: Running in to state\n");
		lua_pushvalue(from, idx);
		RemoteProxy *rp = new_remote_proxy_inner(to, from);
		// from: [obj, ..., proxy cache for to_main, obj], to: [RemoteProxy for obj]
		if (DEBUG) fprintf(stderr, "create_proxy: pushcfunction_from_outside\n");
		pushcfunction_from_outside(to, from, new_proxy_finaliser);
		lua_insert(from, -3);
		lua_pushlightuserdata(from, rp);
		if (DEBUG) fprintf(stderr, "create_proxy: remote_pcall\n");
		remote_pcall(to, from, 3, 1);
		// from: [obj, ..., ProxyFinaliser], to: [RemoteProxy for obj]
		ProxyFinaliser *pf = lua_touserdata(from, -1);
		lua_pop(from, 1);

		rp->dead = false;
		pf->dead = false;
	} else {
		if (DEBUG) fprintf(stderr, "create_proxy: Running in from state\n");
		// We're in from, so need to pcall to.
		if (DEBUG) fprintf(stderr, "create_proxy: pushcfunction_from_outside\n");
		pushcfunction_from_outside(from, to, new_remote_proxy);
		lua_pushlightuserdata(to, from);
		if (DEBUG) fprintf(stderr, "create_proxy: remote_pcall\n");
		remote_pcall(from, to, 1, 1);
		RemoteProxy *rp = lua_touserdata(to, -1);
		if (DEBUG) fprintf(stderr, "create_proxy: rp is %lx, type is %i\n", (long)rp, lua_type(to, -1));
		lua_pushvalue(from, idx);
		lua_pushlightuserdata(from, rp);
		// from: [obj, ..., proxy_cache for to_main, obj, RemoteProxy* for obj], to: [RemoteProxy for obj]

		ProxyFinaliser* pf = new_proxy_finaliser_inner(from);
		lua_pop(from, 1);
		// from: [obj, ...], to: [RemoteProxy for obj]

		rp->dead = false;
		pf->dead = false;
	}
}

// When we're in "other" and need to re-proxy something from "from" to "to", we call into "from" to get the ball rolling.
int create_proxy_for(lua_State *from) {
	ProxyRequest *pr = (ProxyRequest *)lua_touserdata(from, 1);
	lua_pushlightuserdata(from, pr->rp);
	lua_rawget(from, LUA_REGISTRYINDEX); // Look up to get the real object.
	create_proxy(from, pr->to, from, -1);
	return 0;
}

#include <stdio.h>
#include <stdbool.h>
#include <lua.h>
#include <lauxlib.h>
#include <malloc.h>
#include <string.h>
#include <MinHook.h>
#include <windows.h>

#define DEBUG 0

// TODO can we avoid this?
#define MAXMULTRET 5

/**
 * Userdata structures for a proxy for obj:
 *   In the owner state, a ProxyFinaliser pf. Its job is only to tell the remote that the owner has been destroyed.
 *   In the caller state, a RemoteProxy rp. It's exposed to Lua and has a metatable to allow calls through to owner.
 *
 * Wiring (& = lightuserdata, plain = real userdata or plain object):
 *   owner:registry[&rp] = original        // Acts as a keepalive for original, and lookup to resolve rp
 *   owner:registry[caller][original] = pf // Acts as keepalive for pf and cache for re-proxying (pf has pointer to rp).
 *   caller:registry:cache                 // This (REMOTE_PROXY_LOOKUP_KEY) is a weak _value_ table, so that we can gc when the last reference goes away.
 *   caller:registry:cache[&rp] = rp       // To allow owner to resolve a cached &rp to the real userdata.
 *
 * Call semantics, from caller:
 *   * caller does eg. proxy(foo)
 *   * Lua resolves to rp's metatable __call, does a pcall to
 *   * remote_proxy__call, which checks args and passes (c call) to
 *   * remote_proxy_inner(..., func=remote_proxy__call_inner, ...), which sets up and passes (c call) to
 *   * pcall_transfer, which sets up and passes (Lua cpcall to owner) to...
 *   * In owner (with semantics that errors here error caller, too -- it's OK to meddle with both stacks):
 *     * pcall_transfer_internal, which
 *     * transfers all args into owner using transfer(). This may do further cpcalls back into caller, as for anything that needs a proxy we need to modify both states.
 *     * clears the entire stack of caller.
 *     * Passes to func=remote_proxy__call_inner (in this case) which can be Lua or C.
 *     * transfers all returns back into caller using transfer(). Again, this may do further pcalls.
 *   * Back in caller, assuming no exception is raised, the stack is exactly the transferred return values, so we return.
 *
 * Transfer semantics:
 *   * Basic types (nil, number, boolean, string) are simply copied.
 *   * Tables become RemoteProxy.
 *   * Functions become RemoteProxy.
 *   * RemoteProxy userdata becomes the real object (if sent to owner state) or a RemoteProxy in the dest (otherwise).
 *   * Other userdata is proxied, but still only supports __index and __call.
 *   * You can't copy a lightuserdata.
 *   * You can't copy a thread.
 *
 * Thread wrangling:
 * * If a proxy is created, we look up the main thread and record that in the
 *   state of the owner as the target. This means if the thread then passes the
 *   data elsewhere, when the garbage collector runs in the main thread, it can
 *   pass something that still exists to the owner to help its cleanup.
 * * Similarly, the RemoteProxy records the _main_ thread of the owner, so that
 *   if the owner created a proxy in a return value, we hold a valid pointer to
 *   the owning state. This probably isn't actually needed, as this can only
 *   happen when proxying a call into the state (which by construction of the
 *   proxy will only happen on the main thread anyway).
 * * A weird behaviour is that if we switch state and then call, returned calls
 *   are in the main thread again. If we called into the "known current thread",
 *   we'd have access to the _thread_ environment. It's also a bit naughty because
 *   B "believes" it's in two calls (the stack will be a bit odd; it'll see
 *   nothing inside the coroutine). We should probably record the "current" thread
 *   and call into that with remote calls. TODO.
 *
 * TODO: We're not doing lua_checkstack properly and should make sure to do it in both states before we go on to append randomly to their stacks. LuaJit (master: TODO?) can call lj_state_growstack on _any_ append (even those not claiming a memory error can result!); any out-of-state append can therefore trigger a memory error and unwind the stack in a messed up way. The official Lua docs say this doesn't happen; that applications are responsible for managing the stack, but either way we can't overrun it.
 * TODO double-check we're not pushing any objects onto states we don't own. These are anything other than lightuserdata, nil, integer and number. (pushvalue and insert are OK too, as are rawget, rawset of existing, ....)
 */

// Left to do:
//
// For metatables, can "name" types using luaL_newmetatable. It seems like a PITA to make sure there's no collision, though?

static int TMP_KEY; // Unique address to passaround return values using lua_cpcall.
static int MAIN_THREAD_KEY; // Registry key to store the main thread of a lua_State*.

// userdata held in a "remote" lua_State to facilitate proxy calls
static int REMOTE_PROXY_METATABLE_KEY; // Unique address to use as index. Value irrelevant.
typedef struct RemoteProxy {
	bool dead;  // Unusable if true.
	lua_State *owner;
} RemoteProxy;

// userdata held in an "owner" lua_State to facilitate safety over death.
static int PROXY_FINALISER_METATABLE_KEY; // Unique address to use as index. Value irrelevant.
typedef struct ProxyFinaliser {
	bool dead;
	RemoteProxy *other;
	// We __gc by (if not dead):
	//   Set other->dead
	// We don't need anything else.
} ProxyFinaliser;

typedef struct ProxyRequest {
	RemoteProxy *rp;
	lua_State *to;
} ProxyRequest;

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

RemoteProxy *check_remote_proxy(lua_State *raise_errors_in, lua_State *L, int idx) {
	RemoteProxy *rp = (RemoteProxy *)check_userdata(raise_errors_in, L, idx, &REMOTE_PROXY_METATABLE_KEY);
	if (rp->dead) {
		if (DEBUG) fprintf(stderr, "Lua error raising: check_remote_proxy: Proxy is dead (host lua_State has been finalised).\n");
		lua_pushstring(raise_errors_in, "check_remote_proxy: Proxy is dead (host lua_State has been finalised).");
		lua_error(raise_errors_in);
	}
	return rp;
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
static void pushcfunction_from_outside(lua_State *raise_errors_in, lua_State *L, lua_CFunction func) {
	if (DEBUG) fprintf(stderr, "pushcfunction_from_outside\n");
	handle_error_from(raise_errors_in, L, lua_cpcall(L, pushcfunction_inner, func));
	lua_pushlightuserdata(L, &TMP_KEY);
	lua_rawget(L, LUA_REGISTRYINDEX);
}

// cpcall func in call_in with call_from as param, re-raising errors in call_from.
static void remote_cpcall(lua_State *call_from, lua_State *call_in, lua_CFunction func) {
	if (DEBUG) fprintf(stderr, "remote_cpcall\n");
	handle_error_from(call_from, call_in, lua_cpcall(call_in, func, call_from));
}

static void remote_pcall(lua_State *raise_errors_in, lua_State *call, int nargs, int nresults) {
	if (DEBUG) fprintf(stderr, "remote_pcall\n");
	handle_error_from(raise_errors_in, call, lua_pcall(call, nargs, nresults, 0));
}

// Finalisers: ffi finalisers run first, then userdata finalisers, then once
// all the userdata is gone, the rest of the state is deleted (__gc of non
// userdata does not run as at this point it cannot affect the outside world).

// The owning state is going away, or the RemoteProxy has been garbage collected.
// We don't hold any light userdata here, but we need to break the remote object.
static int proxy_finaliser__gc(lua_State *from) {
	if (DEBUG) fprintf(stderr, "proxy_finaliser__gc in %lx\n", (long)from);
	// The owner has died (or we're in GC after a remote_proxy dropped).
	ProxyFinaliser *pf = (ProxyFinaliser *)check_userdata(from, from, -1, &PROXY_FINALISER_METATABLE_KEY);
	// Congrats, pf _is_ a ProxyFinaliser!
	if (pf->dead) {
		if (DEBUG) fprintf(stderr, "proxy_finaliser__gc: %lx for %lx is dead\n", (long)pf, (long)pf->other);
		return 0;
	}
	if (DEBUG) fprintf(stderr, "proxy_finaliser__gc: %lx for %lx lives!\n", (long)pf, (long)pf->other);
	pf->other->dead = true;
	return 0;
}

static int remote_proxy__gc_from(lua_State *from) {
	lua_State *to = (lua_State*)lua_touserdata(from, 1);
	if (to == NULL) {
		if (DEBUG) fprintf(stderr, "Lua error raising: remote_proxy__gc_from: Param must be lua_State*\n");
		lua_pushstring(from, "remote_proxy__gc_from: Param must be lua_State*");
		lua_error(from);
	}
	if (DEBUG) fprintf(stderr, "remote_proxy__gc_from %lx for %lx\n", (long)from, (long)to);
	RemoteProxy *rp = (RemoteProxy*)lua_touserdata(to, 1);
	// We're just going to trust this one is wired right.
	lua_checkstack(from, 4);
	lua_pushlightuserdata(from, rp);
	lua_rawget(from, LUA_REGISTRYINDEX);
	// from: [obj]
	if (lua_isnil(from, -1)) {
		fprintf(stderr, "RemoteProxy lookup failed for %lx in %lx...\n", (long)to, (long)from);
		lua_pushstring(from, "RemoteProxy lookup failed");
		lua_error(from);
	}
	lua_pushlightuserdata(from, rp);
	lua_pushnil(from);
	lua_rawset(from, LUA_REGISTRYINDEX); // Delete from:registry[&rp]
	// from: [obj]
	lua_pushlightuserdata(from, to);
	lua_rawget(from, LUA_REGISTRYINDEX);
	if (lua_isnil(from, -1)) {
		fprintf(stderr, "Proxy cache lookup failed for %lx in %lx...\n", (long)rp, (long)from);
		lua_pushstring(from, "Proxy cache lookup failed");
		lua_error(from);
	}
	// from: [obj, proxy cache for to]
	lua_pushvalue(from, -2);
	lua_rawget(from, -2);
	if (lua_isnil(from, -1)) {
		fprintf(stderr, "Proxy cache object lookup failed for %lx in %lx...\n", (long)rp, (long)from);
		lua_pushstring(from, "Proxy cache object lookup failed");
		lua_error(from);
	}
	// from: [obj, proxy cache for to, pf]
	ProxyFinaliser *pf = (ProxyFinaliser*)lua_touserdata(from, -1);
	if (DEBUG) fprintf(stderr, "proxy_finaliser is %lx!!!\n", (long)pf);
	pf->dead = true; // Don't let its GC talk to rp, which is about to be freed.
	lua_pop(from, 1);
	// from: [obj, proxy cache for to]
	lua_pushvalue(from, -2);
	lua_pushnil(from);
	lua_rawset(from, -3); // Delete (proxy cache for to)[obj]
	// from: [obj, proxy cache for to]
	lua_pushnil(from);
	if (lua_next(from, -1) == 0) {
		// Unlink the to state entirely if it's going to be empty.
		if (DEBUG) fprintf(stderr, "Removing proxy cache in %lx for %lx\n", (long)from, (long)to);
		lua_pushlightuserdata(from, to);
		lua_pushnil(from);
		lua_rawset(from, LUA_REGISTRYINDEX); // Delete from:registry[to]
		lua_pop(from, 2);
	} else {
		lua_pop(from, 4); // Plus the K/V pair at head of the table.
	}
	if (DEBUG) fprintf(stderr, "remote_proxy__gc_from %lx for %lx COMPLETE\n", (long)from, (long)to);
	return 0;
}

// All references to the RemoteProxy have gone away (or the remote is in lua_close)
// We need to break and remove the finaliser, and remove the upstream cache.
// We also need to remove our local cache.
static int remote_proxy__gc(lua_State *to) {
	if (DEBUG) fprintf(stderr, "remote_proxy__gc in %lx\n", (long)to);
	RemoteProxy *rp = (RemoteProxy *)check_userdata(to, to, -1, &REMOTE_PROXY_METATABLE_KEY);
	// Congrats, pf _is_ a RemoteProxy!
	if (rp->dead) {
		if (DEBUG) fprintf(stderr, "remote_proxy__gc: %lx is dead\n", (long)rp);
		// Other state has died already.
		return 0;
	}
	if (DEBUG) fprintf(stderr, "remote_proxy__gc: %lx lives\n", (long)rp);
	rp->dead = true; // Shouldn't be needed as no references to rp will exist after this, but let's be tidy.
	lua_State *from = rp->owner;
	// We don't need to clean up our cache -- it's a weak value table and we're
	// running gc, so the last reference is gone.
	remote_cpcall(to, from, remote_proxy__gc_from);
	if (DEBUG) fprintf(stderr, "remote_proxy__gc: %lx done\n", (long)rp);
	return 0;
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

static int remote_proxy__call(lua_State*);
static int remote_proxy__index(lua_State*);

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
static int new_remote_proxy(lua_State *to) {
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
static void create_proxy(lua_State *raise_errors_in, lua_State *to, lua_State *from, int idx) {
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

// In state to, copy string at idx in from to top of to.
void copy_string_inner(lua_State *to, lua_State *from, int idx) {
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

int copy_string(lua_State *to) {
	lua_State *from = (lua_State*)lua_touserdata(to, 1);
	copy_string_inner(to, from, -1);
	return 1;
}

// When we're in "other" and need to re-proxy something from "from" to "to", we call into "from" to get the ball rolling.
int create_proxy_for(lua_State *from) {
	ProxyRequest *pr = (ProxyRequest *)lua_touserdata(from, 1);
	lua_pushlightuserdata(from, pr->rp);
	lua_rawget(from, LUA_REGISTRYINDEX); // Look up to get the real object.
	create_proxy(from, pr->to, from, -1);
	return 0;
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
static int pcall_transfer(lua_State *call_data_from, lua_State *call_in, int nargs, int nret) {
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


/**
 * ApiMod_registry_state is used to hold the mod registry, out of the ApiMod
 * initialisation module itself (which has no direct access). There's nothing
 * in here but a K/V table of name -> proxy.
 *
 * We hold proxies in a table named by APIMOD_REGISTRY_KEY in the registry
 * of this state, and register/get will proxy here to set/get values.
 *
 * Because tables are read-only proxies, we could also directly return the
 * table as a proxy ("get_all()"), but that seems more complex (among other
 * things, there doesn't seem to be a natural way to proxy iteration via
 * metatables, so the only actual operation would be a metatable lookup, which
 * is just get() anyway).
 */
static lua_State *ApiMod_registry_state;
static int APIMOD_REGISTRY_KEY; // Unused, just used as a registry key.

static int ApiMod_register_inner(lua_State *global) {
	lua_pushlightuserdata(global, &APIMOD_REGISTRY_KEY);
	lua_rawget(global, LUA_REGISTRYINDEX);
	lua_insert(global, 1);
	lua_rawset(global, 1);
	lua_remove(global, 1);
	return 0;
}

static int ApiMod_register(lua_State *L) {
	if (ApiMod_registry_state == NULL) {
		lua_pushstring(L, "ApiMod_register: ApiMod is not initialised yet.");
		lua_error(L);
	}
	luaL_checkstring(L, 1);
	luaL_checkany(L, 2);
	lua_settop(L, 2);
	lua_checkstack(L, 1);
	lua_pushlightuserdata(L, ApiMod_register_inner);
	lua_insert(L, 1);
	pcall_transfer(L, ApiMod_registry_state, 2, 0);
	return 0;
}

static int ApiMod_get_inner(lua_State *global) {
	lua_pushlightuserdata(global, &APIMOD_REGISTRY_KEY);
	lua_rawget(global, LUA_REGISTRYINDEX);
	lua_insert(global, 1);
	lua_rawget(global, 1);
	lua_remove(global, 1);
	return 1;
}

static int ApiMod_get(lua_State *L) {
	if (ApiMod_registry_state == NULL) {
		lua_pushstring(L, "ApiMod_get: ApiMod is not initialised yet.");
		lua_error(L);
	}
	luaL_checkstring(L, 1);
	lua_settop(L, 1);
	lua_pushlightuserdata(L, ApiMod_get_inner);
	lua_insert(L, 1);
	if (DEBUG) fprintf(stderr, "About to call ApiMod_get!\n");
	pcall_transfer(L, ApiMod_registry_state, 1, 1);
	return 1;
}

static int init_ApiMod_registry(lua_State *global) {
	lua_checkstack(global, 2);
	lua_pushlightuserdata(global, &APIMOD_REGISTRY_KEY);
	lua_newtable(global);
	lua_rawset(global, LUA_REGISTRYINDEX);
	return 0;
}

void create_registry(lua_State* L) {
	if (ApiMod_registry_state != NULL) {
		lua_pushstring(L, "ApiMod registry is already created");
		lua_error(L);
	}
	ApiMod_registry_state = luaL_newstate();
	if (ApiMod_registry_state == NULL) {
		lua_pushstring(L, "Could not create ApiMod registry");
		lua_error(L);
	}
	lua_cpcall(ApiMod_registry_state, init_ApiMod_registry, NULL);
}

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

static const luaL_Reg ApiMod_funcs[] = { {"register", ApiMod_register}, {"get", ApiMod_get}, {NULL, NULL} };

static int states = 0;

static lua_State* (*orig_lua_newstate)(lua_Alloc,void*) = NULL;
static lua_State* (*orig_luaL_newstate)(void) = NULL;
static int (*orig_luaopen_math)(lua_State*) = NULL;
static int (*orig_lua_close)(lua_State*) = NULL;
lua_State *our_lua_newstate(lua_Alloc f, void *ud) {
	lua_State *L = orig_lua_newstate(f, ud);
	fprintf(stderr, "lua_newstate called -> %lx (%i)!\n", (long)L, ++states);
	lua_pushlightuserdata(L, &MAIN_THREAD_KEY);
	lua_pushthread(L);
	lua_rawset(L, LUA_REGISTRYINDEX);
	luaL_register(L, "ApiMod", ApiMod_funcs);
	return L;
}
lua_State *our_luaL_newstate() {
	fprintf(stderr, "luaL_newstate called!\n");
	lua_State *L = orig_luaL_newstate();
	return L;
}
// luaL_register, luaL_loadlibs are _not_ interestingly hookable (never called).
// It looks like Noita calls luaopen_math etc. to get the libs. So it doesn't filter the state but build it. luaopen_os is _not_ called.
int our_luaopen_math(lua_State *L) {
	fprintf(stderr, "luaopen_math called!\n");
	return orig_luaopen_math(L);
}
int our_lua_close(lua_State *L) {
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

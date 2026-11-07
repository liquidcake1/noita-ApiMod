#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include "api_mod.h"
#include "util.h"
#include "transfer.h"

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
lua_State *ApiMod_registry_state;
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

const luaL_Reg ApiMod_funcs[] = { {"register", ApiMod_register}, {"get", ApiMod_get}, {NULL, NULL} };

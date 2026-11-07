#include <lua.h>
#include <lauxlib.h>
#include "util.h"
#include "proxy.h"
#include "destroy_proxy.h"

// Finalisers: ffi finalisers run first, then userdata finalisers, then once
// all the userdata is gone, the rest of the state is deleted (__gc of non
// userdata does not run as at this point it cannot affect the outside world).

// The owning state is going away, or the RemoteProxy has been garbage collected.
// We don't hold any light userdata here, but we need to break the remote object.
int proxy_finaliser__gc(lua_State *from) {
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
int remote_proxy__gc(lua_State *to) {
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

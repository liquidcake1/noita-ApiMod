#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include "proxy.h"
#include "proxy_metatables.h"
#include "util.h"
#include "transfer.h"

// TODO consider not wiring in create_proxy_finaliser, so that we do not leak
// on error. We'll need to do one of:
// * Push placeholder values into the registry, and be prepared to handle them
//   on any future lookup. These can be replaced without invoking gc.
// * Call back in to finalise the allocation, paying for a second pcall.
//
// For now we trust that errors in this code will be extremely rare (they can
// only be allocation failure, or an error in a finaliser of some object which
// is destroyed in a gc step caused by this allocation).

// REPLACE the stack element at idx with a lightuserdata ProxyFinaliser.
static ProxyFinaliser *create_proxy_finaliser(lua_State *from, lua_State *to, int idx) {
	if (DEBUG) fprintf(stderr, "create_proxy_finaliser(%lx, %lx, %i).\n", (long)from, (long)to, idx);
	if (idx < 0) {
		idx = lua_gettop(from) + idx + 1;
	}
	lua_State *to_main = get_main_thread(from, to); // Naughty
	lua_pushlightuserdata(from, to_main);
	lua_rawget(from, LUA_REGISTRYINDEX);
	if (lua_isnil(from, -1)) {
		if (DEBUG) fprintf(stderr, "create_proxy_finaliser: need new table for to.\n");
		lua_pop(from, 1);
		lua_newtable(from);
		lua_pushlightuserdata(from, to_main);
		lua_pushvalue(from, -2);
		lua_rawset(from, LUA_REGISTRYINDEX);
	}
	// [table for to]
	lua_pushvalue(from, idx);
	lua_rawget(from, -2);
	// [table, pf?]
	ProxyFinaliser *pf;
	if (lua_isnil(from, -1)) {
		if (DEBUG) fprintf(stderr, "create_proxy_finaliser: need new finaliser.\n");
		lua_pop(from, 1);
		lua_pushvalue(from, idx); // [table, obj]
		pf = lua_newuserdata(from, sizeof(ProxyFinaliser));
		if (DEBUG) fprintf(stderr, "New ProxyFinaliser is %p.\n", pf);
		ensure_proxy_finaliser_metatable(from);
		lua_setmetatable(from, -2);
		if (pf == NULL) {
			lua_pushstring(from, "create_proxy_finaliser: Could not allocate ProxyFinaliser! Oh no...");
			lua_error(from);
			return NULL; // Unreachable.
		}
		pf->dead = true;
		pf->owner = get_main_thread(from, from);
		pf->proxy = NULL; // Set in claim_proxy_finaliser
		lua_rawset(from, -3);
		lua_pop(from, 1); // Pop the table
	} else {
		if (DEBUG) fprintf(stderr, "create_proxy_finaliser: using existing finaliser.\n");
		pf = lua_touserdata(from, -1);
		lua_pop(from, 2); // Drop the table and pf
	}
	// []
	lua_pushlightuserdata(from, pf);
	lua_pushvalue(from, idx);
	lua_rawset(from, LUA_REGISTRYINDEX);
	// []
	return pf;
}

static void replace_with_proxy_finaliser(lua_State *from, lua_State *to, int idx) {
	if (idx < 0) {
		idx = lua_gettop(from) + idx + 1;
	}
	ProxyFinaliser *pf = create_proxy_finaliser(from, to, idx);
	lua_pushlightuserdata(from, pf);
	lua_replace(from, idx);
}

// REPLACE the stack element at idx with an exportable version.
// This is either a userdata for a RemoteProxy, or a lightuserdata
// for a ProxyFinaliser which can be tied to a RemoteProxy. It's the job of
// claim_index to convert that into a RemoteProxy on the receiving end.
static void export_index(lua_State *from, lua_State *to, int idx) {
	if (DEBUG) fprintf(stderr, "export_index(%lx, %lx, %i)\n", (long)from, (long)to, idx);
	switch(lua_type(from, idx)) {
	// Easy
	case LUA_TNIL:
	case LUA_TNUMBER:
	case LUA_TBOOLEAN:
	case LUA_TSTRING:
		break;
	// Things we must replace with our ProxyFinaliser
	case LUA_TTABLE:
		if (DEBUG) fprintf(stderr, "Exporting a table from %lx to %lx.\n", (long)from, (long)to);
		replace_with_proxy_finaliser(from, to, idx);
		break;
	case LUA_TFUNCTION:
		if (DEBUG) fprintf(stderr, "Exporting a function from %lx to %lx.\n", (long)from, (long)to);
		replace_with_proxy_finaliser(from, to, idx);
		break;
	case LUA_TUSERDATA: {
		lua_getmetatable(from, idx);
		lua_pushlightuserdata(from, &REMOTE_PROXY_METATABLE_KEY);
		lua_rawget(from, LUA_REGISTRYINDEX);
		bool is_RemoteProxy = lua_rawequal(from, -1, -2);
		lua_pop(from, 2);
		if (is_RemoteProxy) {
			// It's a proxy already. Leave it as a userdata. In
			// claim, we must either ask the real owner or resolve
			// back to the real object (if to is the original
			// owner).
		} else {
			// It's not a proxy, so this is some other userdata
			// type. Create a proxy finaliser for it; this will
			// be a lightuserdata on the stack.
			if (DEBUG) fprintf(stderr, "Exporting a userdata %lx from %lx to %lx.\n", (long)lua_touserdata(from, idx), (long)from, (long)to);
			replace_with_proxy_finaliser(from, to, idx);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA:
		lua_pushstring(from, "Will not export a lightuserdata.");
		lua_error(from);
		break; // Unreachable
	case LUA_TTHREAD:
		lua_pushstring(from, "Will not export a thread.");
		lua_error(from);
		break; // Unreachable
	default:
		lua_pushstring(from, "Will not export an unknown type.");
		lua_error(from);
		break; // Unreachable
	}
}

static int REMOTE_PROXY_LOOKUP_KEY; // Unique address to use as index. Value irrelevant.
static void create_weak_proxy_lookup_table(lua_State *to) {
	// Create the weak proxy lookup table; it's not there!
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

// Put on top of stack of to a (real) RemoteProxy pointing to pf
static void claim_proxy_finaliser(lua_State *to, ProxyFinaliser *pf) {
	if (pf->dead && pf->proxy == NULL) {
		if (DEBUG) fprintf(stderr, "Before lua_newuserdata, stack is %i.\n", lua_gettop(to));
		// It's new; make a new RemoteProxy.
		RemoteProxy *rp = lua_newuserdata(to, sizeof(RemoteProxy));
		if (DEBUG) fprintf(stderr, "New RemoteProxy is %p.\n", rp);
		rp->dead = true;
		rp->owner = pf->owner;
		rp->finaliser = pf;
		if (DEBUG) fprintf(stderr, "Setting proxy of %p.\n", pf);
		pf->proxy = rp;
		if (DEBUG) fprintf(stderr, "Before ensure_remote_proxy_metatable, stack is %i.\n", lua_gettop(to));
		ensure_remote_proxy_metatable(to);
		lua_setmetatable(to, -2);
		if (DEBUG) fprintf(stderr, "Before ensure_weak_proxy_lookup_table, stack is %i.\n", lua_gettop(to));
		lua_pushlightuserdata(to, &REMOTE_PROXY_LOOKUP_KEY);
		lua_rawget(to, LUA_REGISTRYINDEX);
		if (lua_isnil(to, -1)) {
			lua_pop(to, 1);
			create_weak_proxy_lookup_table(to);
		}
		if (DEBUG) fprintf(stderr, "Before setting weak val, stack is %i.\n", lua_gettop(to));
		// [rp, table]
		lua_pushlightuserdata(to, rp);
		lua_pushvalue(to, -3); // real rp
		lua_rawset(to, -3);
		// [rp, table]
		if (DEBUG) fprintf(stderr, "Before popping table, stack is %i.\n", lua_gettop(to));
		lua_pop(to, 1);
		rp->dead = false;
		if (DEBUG) fprintf(stderr, "Setting alive of %p.\n", pf);
		pf->dead = false;
		// [rp]
	} else if (pf->dead) {
		lua_pushstring(to, "claim_proxy_finaliser: ProxyFinaliser is dead! This should be impossible; something is super wrong.");
		lua_error(to);
		return; // Unreachable
	} else {
		// It exists already; look up our own.
		lua_pushlightuserdata(to, &REMOTE_PROXY_LOOKUP_KEY);
		lua_rawget(to, LUA_REGISTRYINDEX);
		// Table MUST exist as we made it with the proxy!!
		lua_pushlightuserdata(to, pf->proxy);
		lua_rawget(to, -2);
		lua_remove(to, -2);
		// [rp]
	}
}

static int request_proxy(lua_State *from) {
	ProxyRequest *pr = lua_touserdata(from, -1);
	lua_pushlightuserdata(from, pr->finaliser);
	lua_rawget(from, LUA_REGISTRYINDEX);
	if (lua_isnil(from, -1)) {
		lua_pop(from, 1);
		lua_pushstring(from, "request_proxy: input finaliser is dead!!");
		lua_error(from);
		return 0; // Unreachable
	}
	pr->finaliser = create_proxy_finaliser(from, pr->to, -1);
	return 0;
}

static void claim_index(lua_State *to, lua_State *from, int idx) {
	if (DEBUG) fprintf(stderr, "export_index(%lx, %lx, %i)\n", (long)from, (long)to, idx);
	switch(lua_type(from, idx)) {
	case LUA_TNIL:
		if (DEBUG) fprintf(stderr, "'Copying' a nil from %lx to %lx\n", (long)from, (long)to);
		lua_pushnil(to);
		break;
	case LUA_TNUMBER:
		lua_pushnumber(to, lua_tonumber(from, idx));
		if (DEBUG) fprintf(stderr, "Copying a number from %lx to %lx: %f\n", (long)from, (long)to, lua_tonumber(from, idx));
		break;
	case LUA_TBOOLEAN:
		lua_pushboolean(to, lua_toboolean(from, idx));
		if (DEBUG) fprintf(stderr, "Copying a bool from %lx to %lx: %i\n", (long)from, (long)to, lua_toboolean(from, idx));
		break;
	case LUA_TSTRING:
	{
		size_t len;
		// Allegedly this can raise a memory error, but I think only if
		// it's not already a string (because it implicitly converts
		// numbers).
		//
		// Checking the _current_ luajit source, that's the case -- it
		// either straight returns if it's a string, or if it's a
		// number, does a string conversion into the string table,
		// which can throw a memory error.
		//
		// So it can't raise here, as we know it's already a string.
		// Woohoo!
		const char *s = lua_tolstring(from, idx, &len);
		if (DEBUG) fprintf(stderr, "Copying a string from %lx to %lx: %s\n", (long)from, (long)to, s);
		lua_pushlstring(to, s, len);
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		// This is a pointer to a ProxyFinaliser.
		if (DEBUG) fprintf(stderr, "Claiming a new proxy %lx (%i type %i) from %lx to %lx.\n", (long)lua_touserdata(from, idx), idx, lua_type(from, idx), (long)from, (long)to);
		ProxyFinaliser *pf = (ProxyFinaliser*)lua_touserdata(from, idx);
		claim_proxy_finaliser(to, pf);
		if (DEBUG) fprintf(stderr, "Claimed a new proxy %lx (%i type %i) from %lx to %lx.\n", (long)lua_touserdata(from, idx), idx, lua_type(from, idx), (long)from, (long)to);
		break;
	}
	case LUA_TUSERDATA:
		// This is a real RemoteProxy in the sending state.
		// If this is a RemoteProxy in from, and the RemoteProxy's real object lives in to, resolve it to the real thing.
		// If the real object lives elsewhere, create a new proxy from _there_ to to.
		RemoteProxy *rp = (RemoteProxy*)lua_touserdata(from, idx);
		if (rp->dead) {
			if (DEBUG) fprintf(stderr, "Reclaiming a dead proxy.\n");
			RemoteProxy *rp = lua_newuserdata(from, sizeof(RemoteProxy));
			rp->dead = true;
			rp->owner = NULL;
			rp->finaliser = NULL;
		} else if (rp->owner == to) {
			// Convert it back to the real object.
			lua_pushlightuserdata(to, rp->finaliser);
			lua_rawget(to, LUA_REGISTRYINDEX);
			if (DEBUG) fprintf(stderr, "Reclaiming a proxy to our own object. Type of return is %i.\n", lua_type(to, -1));
		} else {
			// It's in some third state.
			if (DEBUG) fprintf(stderr, "Claiming a proxy to a third party object. Asking for a new finaliser. Finaliser: %p\n", rp->finaliser);
			ProxyRequest pr = {rp->finaliser, get_main_thread(to, to)};
			if (DEBUG) fprintf(stderr, "Old finaliser: %p\n", pr.finaliser);
			handle_error_from(to, rp->owner, lua_cpcall(rp->owner, request_proxy, &pr));
			if (DEBUG) fprintf(stderr, "New finaliser: %p\n", pr.finaliser);
			claim_proxy_finaliser(to, pr.finaliser); // request_proxy sets finaliser
		}
		break;
	// These should have become a userdata/lightuserdata.
	case LUA_TTABLE:
		lua_pushstring(to, "Cannot claim a table.");
		lua_error(to);
		break;
	case LUA_TFUNCTION:
		lua_pushstring(to, "Cannot claim a function.");
		lua_error(to);
		break;
	// These should never have been allowed.
	case LUA_TTHREAD:
		lua_pushstring(to, "Cannot claim a thread.");
		lua_error(to);
		break; // Unreachable
	default:
		if (DEBUG) fprintf(stderr, "Lua error raising: Will not proxy an unknown type.\n");
		lua_pushstring(to, "Will not proxy an unknown type.");
		lua_error(to);
		break; // Unreachable
	}
}

static void export_top_n(lua_State *from, lua_State *to, int count) {
	if (DEBUG) fprintf(stderr, "export_top_n(%lx, %lx, %i)\n", (long)from, (long)to, count);
	for(int i=-count; i<0; i++) {
		export_index(from, to, i);
	}
}

static void claim_from(lua_State *to, lua_State *from, int count) {
	for(int i=-count; i<0; i++) {
		if (DEBUG) fprintf(stderr, "Before claim, stack is length %i/%i.\n", lua_gettop(to), lua_gettop(from));
		claim_index(to, from, i);
	}
	if (DEBUG) fprintf(stderr, "Before pop, stack is length %i/%i.\n", lua_gettop(to), lua_gettop(from));
	lua_pop(from, count);
	if (DEBUG) fprintf(stderr, "After pop, stack is length %i/%i.\n", lua_gettop(to), lua_gettop(from));
}

static void prepare_push_to_registry(lua_State *from, size_t nret) {
	lua_pushlightuserdata(from, &RETURN_TMP_KEY);
	// Called with an empty stack, so this is always index 2.
	lua_createtable(from, nret, 0);
}

static void export_to_registry(lua_State *from, lua_State *to, int index_of_table) {
	if (DEBUG) fprintf(stderr, "export_to_registry: Table is %i; stack is %i.\n", index_of_table, lua_gettop(from));
	size_t offset = index_of_table;
	size_t nret = lua_gettop(from) - index_of_table;
	lua_pushinteger(from, 1);
	lua_pushinteger(from, nret);
	lua_rawset(from, index_of_table);
	for(int i=nret; i>0; i--) {
		export_index(from, to, i + offset);
		lua_pushinteger(from, i + 1);
		lua_insert(from, -2);
		lua_rawset(from, index_of_table);
	}
	if (DEBUG) fprintf(stderr, "export_to_registry: Length is %i.\n", lua_objlen(from, -1));
	// Stack has the stuff from prepare_push_to_registry.
	lua_rawset(from, LUA_REGISTRYINDEX);
}

// Slightly naughty and needs 2 stack space in from.
static void claim_from_registry(lua_State *to, lua_State *from) {
	lua_pushlightuserdata(from, &RETURN_TMP_KEY);
	lua_rawget(from, LUA_REGISTRYINDEX);
	if (lua_type(from, -1) != LUA_TTABLE) {
		lua_pushstring(to, "claim_from_registry: RETURN_TMP_KEY does not hold a table!!");
		lua_error(to);
	}
	lua_pushinteger(from, 1);
	lua_rawget(from, -2);
	int nret = lua_tointeger(from, -1);
	lua_pop(from, 1);
	if (DEBUG) fprintf(stderr, "claim_from_registry: Length is %i.\n", nret);
	for(size_t i=1; i <= nret; i++) {
		lua_pushinteger(from, i + 1);
		lua_rawget(from, -2);
		if (DEBUG) fprintf(stderr, "claim_from_registry: claim_index %i, stack size is %i.\n", i, lua_gettop(to));
		claim_index(to, from, -1);
		if (DEBUG) fprintf(stderr, "claim_from_registry: claim_index %i post, stack size is %i.\n", i, lua_gettop(to));
		lua_pop(from, 1);
	}
	lua_pop(from, 1);
	// And clean up.
	lua_pushlightuserdata(from, &RETURN_TMP_KEY);
	lua_pushnil(from);
	lua_rawset(from, LUA_REGISTRYINDEX);
}

typedef struct RemoteCall {
	int nargs;
	int nret;
	lua_State *call_data_from;
	lua_CFunction func;
} RemoteCall;

static int remote_call(lua_State *call_in) {
	RemoteCall *call = lua_touserdata(call_in, 1);

	if (DEBUG) fprintf(stderr, "On call, stack is length %i.\n", lua_gettop(call_in));
	prepare_push_to_registry(call_in, call->nret);
	if (DEBUG) fprintf(stderr, "After prepare, stack is length %i.\n", lua_gettop(call_in));
	const int original_top = lua_gettop(call_in);
	lua_pushcfunction(call_in, call->func);
	claim_from(call_in, call->call_data_from, call->nargs);
	if (DEBUG) fprintf(stderr, "After claim (of %i), stack is length %i.\n", call->nargs, lua_gettop(call_in));
	lua_call(call_in, call->nargs, call->nret);
	if (DEBUG) fprintf(stderr, "After call, stack is length %i.\n", lua_gettop(call_in));

	export_to_registry(call_in, call->call_data_from, original_top);
	if (DEBUG) fprintf(stderr, "After export, stack is length %i.\n", lua_gettop(call_in));

	// precondition for claim_from_registry; we hope Lua doesn't reduce the stack after we return.
	lua_checkstack(call_in, 2);
	return 0;
}

static int pcall_transfer_exported(lua_State *call_data_from, lua_State *call_in, lua_CFunction func, int nargs, int nret) {
	RemoteCall call = {nargs, nret, call_data_from, func};
	handle_error_from(call_data_from, call_in, lua_cpcall(call_in, remote_call, &call));
	// We can cpcall if this returns via a temp registry key.
	// With only very slight knowledge of internals, we can guarantee that
	// on return it has 2 stack slots, by having Lua discard 2 values.
	claim_from_registry(call_data_from, call_in);
	return nret;
}

int pcall_transfer(lua_State *call_data_from, lua_State *call_in, lua_CFunction func, int nargs, int nret) {
	export_top_n(call_data_from, call_in, nargs);
	// TODO Any crash here will leak any objects (and finalisers for them).
	// We could pcall to try to clear this up.
	// Stack is now free of table/userdata values, and we crashed if it had
	// other naughty stuff.
	return pcall_transfer_exported(call_data_from, call_in, func, nargs, nret);
}

#include <stdbool.h>
#include <lua.h>

// userdata held in a "remote" lua_State to facilitate proxy calls
extern int REMOTE_PROXY_METATABLE_KEY; // Unique address to use as index. Value irrelevant.
typedef struct RemoteProxy {
	bool dead;  // Unusable if true.
	lua_State *owner;
} RemoteProxy;

// userdata held in an "owner" lua_State to facilitate safety over death.
extern int PROXY_FINALISER_METATABLE_KEY; // Unique address to use as index. Value irrelevant.
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

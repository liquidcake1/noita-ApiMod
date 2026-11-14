#include <stdbool.h>
#include <lua.h>

// userdata held in a "remote" lua_State to facilitate proxy calls
extern int REMOTE_PROXY_METATABLE_KEY; // Unique address to use as index. Value irrelevant.
typedef struct ProxyFinaliser ProxyFinaliser;
typedef struct RemoteProxy {
	bool dead;  // Unusable if true.
	lua_State *owner;
	ProxyFinaliser *finaliser; // Within owner, the finaliser for this RemoteProxy.
} RemoteProxy;

// userdata held in an "owner" lua_State to facilitate safety over death.
extern int PROXY_FINALISER_METATABLE_KEY; // Unique address to use as index. Value irrelevant.
typedef struct ProxyFinaliser {
	bool dead; // Unusable if true. This just prevents the gc from setting other->dead.
	lua_State *owner;
	RemoteProxy *proxy;
} ProxyFinaliser;

typedef struct ProxyRequest {
	ProxyFinaliser *finaliser;
	lua_State *to;
} ProxyRequest;

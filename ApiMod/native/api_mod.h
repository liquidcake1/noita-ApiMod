#include <lua.h>
#include <lauxlib.h>

extern lua_State *ApiMod_registry_state;
extern const luaL_Reg ApiMod_funcs[];
extern int MAIN_THREAD_KEY;
void create_registry(lua_State*);

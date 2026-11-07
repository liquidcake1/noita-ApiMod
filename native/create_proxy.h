#include <lua.h>

int new_remote_proxy(lua_State *to);
void create_proxy(lua_State *raise_errors_in, lua_State *to, lua_State *from, int idx);
int create_proxy_for(lua_State *from);

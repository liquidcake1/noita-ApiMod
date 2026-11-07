#include <lua.h>

// TODO can we avoid this?
#define MAXMULTRET 5

int pcall_transfer(lua_State *call_data_from, lua_State *call_in, int nargs, int nret);

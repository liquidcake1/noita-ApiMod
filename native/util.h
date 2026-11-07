#include <lua.h>

#define DEBUG 0

void *check_userdata(lua_State *raise_errors_in, lua_State *L, int idx, void *table_key);
void handle_error_from(lua_State *raise_to, lua_State *ran_in, int retval);
void pushcfunction_from_outside(lua_State *raise_errors_in, lua_State *L, lua_CFunction func);
void remote_cpcall(lua_State *call_from, lua_State *call_in, lua_CFunction func);
void remote_pcall(lua_State *raise_errors_in, lua_State *call, int nargs, int nresults);
lua_State *get_main_thread(lua_State *raise_errors_in, lua_State *L);
void set_main_thread(lua_State *L);

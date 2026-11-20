#include <lua.h>

// TODO can we avoid this?
#define MAXMULTRET 5

// While in a pcall inside call_data_from, safely make a pcall to call_in using
// arguments from call_data_from, transferring the return values to
// call_data_from.
// * call_in must have at least nargs + TODO of stack (else we can raise in call_data_from).
// * Either nargs + 1 is popped from call_data_from and then nret pushed, or we
//   raise in call_data_from.
// * We guarantee to leave call_in's stack as it was.
int pcall_transfer(lua_State *call_data_from, lua_State *call_in, lua_CFunction func, int nargs, int nret);

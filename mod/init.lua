local ffi = require("ffi")
ffi.cdef("void create(); void increment(); int get();")
local ffidll = ffi.load("mods/api_mod/api_mod.dll")
local dll = package.loadlib("mods/api_mod/api_mod.dll", "luaopen_api_mod")()
local io = require("io")
io.write("Loaded!!!\n")
io.write("Output: ", dll.test(10), "!!!\n")
ffidll.create();
ffidll.increment();
ffidll.increment();
ffidll.increment();
ffidll.increment();
io.write("Output: ", ffidll.get(), "!!!\n")

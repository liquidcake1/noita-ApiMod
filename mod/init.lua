local ffi = require("ffi")
ffi.cdef("void create(); void increment(); int get();")
local ffidll = ffi.load("mods/api_mod/api_mod.dll")
local dll = package.loadlib("mods/api_mod/api_mod.dll", "luaopen_api_mod")()
local io = require("io")
io.write("Loaded!!!\n")
ffidll.create();
dll.settable("astring", "stringvalue")
dll.settable("anint", 1)
dll.settable("afloat", 1.0e30)
dll.settable("abool", true)
dll.settable("anil", nil)
io.write("astring: ", dll.gettable("astring"), "!!!\n")
io.write("anint: ", tostring(dll.gettable("anint")), "!!!\n")
io.write("afloat: ", tostring(dll.gettable("afloat")), "!!!\n")
io.write("abool: ", tostring(dll.gettable("abool")), "!!!\n")
io.write("anil: ", tostring(dll.gettable("anil")), "!!!\n")
ffidll.increment();
ffidll.increment();
ffidll.increment();
ffidll.increment();
io.write("Output: ", ffidll.get(), "!!!\n")

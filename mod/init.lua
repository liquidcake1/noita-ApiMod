local ffi = require("ffi")
local dll = ffi.load("mods/api_mod/api_mod.dll")
local io = require("io")
io.write("Loaded!!!")

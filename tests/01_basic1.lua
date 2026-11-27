val = 0
local my_table = {}
local api = {}
function api.add (x)
	val = val + x
	my_table[val] = "foo"
	return val
end
function api.set (k, v)
	my_table[k] = v
end
function api.get (x)
	return my_table[x]
end
function api.print(x)
	--print(ModTextFileGetContent("mods/test/init.lua"))
end
function api.generate_garbage()
	collectgarbage()
	collectgarbage()
	collectgarbage()
	collectgarbage()
	collectgarbage()
end
ApiMod.register("test", api)
print("Registered test")

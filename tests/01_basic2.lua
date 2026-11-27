print("Getting test")
local test_api = ApiMod.get("test")
print("Got test")

ApiMod.get("test")

print("Pre " .. tostring(test_api.get(5)))
print("Add val " .. tostring(test_api.add(5)))
print("Post " .. tostring(test_api.get(5)))
test_api.print("Hello, world!")

function a (x)
   -- stats[L][x] = "proxy thingy"
   test_api.set("bar", x)
   coroutine.yield()
end
thing_to_proxy = {}
thing_to_proxy.foo = "bar"
L = coroutine.create(a)
coroutine.resume(L, thing_to_proxy)
print("Collecting garbage!!")
print("Collecting garbage!!")
print("Collecting garbage!!")
print("Collecting garbage!!")
print("Collecting garbage!!")
-- collectgarbage()
-- collectgarbage()
-- collectgarbage()
-- collectgarbage()
-- L = nil
print("Collected garbage!!")
proxied = test_api.get("bar") -- looks up state[test's proxy of thing_to_proxy], this is still OK
print(proxied.foo)

test_api.set("bar", nil) -- looks up state[L]["bar"] but L is gone!!!!
print("Collecting garbage (remote)!!")
print("Collecting garbage (remote)!!")
print("Collecting garbage (remote)!!")
print("Collecting garbage (remote)!!")
print("Collecting garbage (remote)!!")
print("Collecting garbage (remote)!!")
-- test_api.generate_garbage()
print(proxied.foo)

local pollnet = ApiMod.get("pollnet")
local s = pollnet.open_tcp("localhost:8001")
s:send("Hello, this message came from Noita, proxied via ApiMod.\n")
-- pollnet.init()
-- print(tostring(s))
-- print(tostring(s.send(pollnet)))

local closure = (function ()
  local dll = package.loadlib("mods/ApiMod/api_mod.dll", "luaopen_api_mod")()
  dll.initialise()
  -- Return a closure which closes dll. Nobody can get at it (beyond its
  -- tostring), but it cannot die until the closure does.
  return function () return tostring(dll) end
end)()

-- Destroy all of our globals. Just in case anyone manages to escape into this
-- state (eg. ModLuaFileAppend).
-- The dll is kept alive by the closure, which is a local and so not in _G.
do
  local t = {}
  for varname, _ in pairs(_G) do
    table.insert(t, varname)
  end
  table.sort(t)
  -- save some globals
  local print = print
  local collectgarbage = collectgarbage
  for _, varname in ipairs(t) do
    if varname ~= "_G" then
      _G[varname] = nil
    end
  end
  t = nil
  collectgarbage()
end

-- Ne'er may ye achieve nowt again.

do
  local pollnet_binding = function ()
-- pollnet bindings for luajit+ffi

-- Change this as necessary to point to where [lib?]pollnet.dll|.so|.dylib
-- is actually located.
local LIBDIR = "mods/ApiMod-pollnet/"
local API_VERSION = "1.0.0"

local ffi = require("ffi")
ffi.cdef[[
typedef struct pollnet_ctx pollnet_ctx;
typedef uint64_t sockethandle_t;
typedef uint32_t socketstatus_t;

const char* pollnet_version();

pollnet_ctx* pollnet_init();
void pollnet_shutdown(pollnet_ctx* ctx);
sockethandle_t pollnet_open_tcp(pollnet_ctx* ctx, const char* addr);
sockethandle_t pollnet_listen_tcp(pollnet_ctx* ctx, const char* addr);
sockethandle_t pollnet_open_ws(pollnet_ctx* ctx, const char* url);
sockethandle_t pollnet_simple_http_get(pollnet_ctx* ctx, const char* url, const char* headers, bool ret_body_only);
sockethandle_t pollnet_simple_http_post(pollnet_ctx* ctx, const char* url, const char* headers, const char* data, uint32_t datasize, bool ret_body_only);
void pollnet_close(pollnet_ctx* ctx, sockethandle_t handle);
void pollnet_send(pollnet_ctx* ctx, sockethandle_t handle, const char* msg);
void pollnet_send_binary(pollnet_ctx* ctx, sockethandle_t handle, const unsigned char* msg, uint32_t msgsize);
socketstatus_t pollnet_update(pollnet_ctx* ctx, sockethandle_t handle);
uint32_t pollnet_get_data_size(pollnet_ctx* ctx, sockethandle_t handle);
const uint8_t* pollnet_unsafe_get_data_ptr(pollnet_ctx* ctx, sockethandle_t handle);
sockethandle_t pollnet_get_connected_client_handle(pollnet_ctx* ctx, sockethandle_t handle);
sockethandle_t pollnet_listen_ws(pollnet_ctx* ctx, const char* addr);
sockethandle_t pollnet_serve_static_http(pollnet_ctx* ctx, const char* addr, const char* serve_dir);
sockethandle_t pollnet_serve_dynamic_http(pollnet_ctx* ctx, const char* addr, bool keep_alive);
sockethandle_t pollnet_serve_http(pollnet_ctx* ctx, const char* addr);
void pollnet_add_virtual_file(pollnet_ctx* ctx, sockethandle_t handle, const char* filename, const char* filedata, uint32_t filesize);
void pollnet_remove_virtual_file(pollnet_ctx* ctx, sockethandle_t handle, const char* filename);
uint32_t pollnet_get_nanoid(char* dest, uint32_t dest_size);
]]

local pollnet
if jit.os == 'Windows' then
  pollnet = ffi.load(LIBDIR .. "pollnet.dll")
elseif jit.os == 'OSX' or jit.os == 'Darwin' then
  pollnet = ffi.load(LIBDIR .. "libpollnet.dylib")
else
  pollnet = ffi.load(LIBDIR .. "libpollnet.so")
end
local POLLNET_VERSION = ffi.string(pollnet.pollnet_version())

do
  local function split_version(v)
    local major, minor, patch = v:match("(%d+)%.(%d+)%.(%d+)")
    return tonumber(major), tonumber(minor), tonumber(patch)
  end

  local maj_req, min_req, pat_req = split_version(API_VERSION)
  local maj_dll, min_dll, pat_dll = split_version(POLLNET_VERSION)
  if maj_dll ~= maj_req then
    error("Incompatible Pollnet binary: expected " .. API_VERSION
          .. " got " .. POLLNET_VERSION)
  end
  if (min_dll < min_req) or (min_dll == min_req and pat_dll < pat_req) then
    error("Incompatible Pollnet binary: expected " .. API_VERSION
      .. " got " .. POLLNET_VERSION)
  end
end

-- Save any globals we need, so the clear later doesn't break us.
local assert = assert
local error = error
local ipairs = ipairs
local pairs = pairs
local print = print
local setmetatable = setmetatable
local table = table
local tostring = tostring
local type = type

local POLLNET_RESULT_CODES = {
  [0] = "invalid_handle",
  [1] = "error",
  [2] = "closed",
  [3] = "opening",
  [4] = "nodata",
  [5] = "hasdata",
  [6] = "newclient"
}

local _ctx = nil

local function init_ctx()
  if _ctx then return end
  _ctx = ffi.gc(pollnet.pollnet_init(), pollnet.pollnet_shutdown)
  assert(_ctx ~= nil)
end

-- Perhaps we don't have to do this. ffi will check for exact pointer
-- compatibility, as best as I can tell. Can we trust that this is well-formed?
-- sockethandle_t is just a uint64_t which sounds non-type-safe...

-- Make a metatable for fake_obj to turn transform fake_obj:foo(...) into
-- obj:foo(...)
-- This allows us to export fake_obj to other mods without them being able
-- to do anything naughty to obj, which is our "real" socket: no access to
-- _socket and no ability to use any method on anything else.
-- Baking the metatable like this means that the function has a stable
-- proxy, so can still be captured for a bit of speed with
--   f = obj.foo
--   f(obj, ...)  -- Same as obj:foo(...) but no proxy call to resolve foo
-- We raise an error if we're called as obj.foo(somthing_else)
local function force_obj(obj, mt)
  local idx = nil
  local forced_mt = {}
  local fake_obj = {}
  for name, fun in pairs(mt) do
    if name:byte(1) ~= 95 then -- skip _ functions
      forced_mt[name] = function (in_obj, ...)
        if in_obj ~= fake_obj then error("metatable function called with the wrong object; probably you did sock." .. name .. "(...) instead of sock:" .. name .. "(...). For safety, the functions are different for each sock, so you cannot call sock1.read(sock2). Perhaps you are attempting such shenanigans...?") end
        return fun(obj, ...)
      end
    end
  end
  setmetatable(obj, {__index = mt})  -- Allow metatable functions to call each other
  -- We don't really need to disable __apimod_settable, as we never use
  -- fake_obj, but it's less confusing to just get an error. If we allowed it,
  -- a remote mod could add methods of their own to the socket and store data
  -- in it, but everything would need proxying twice to run them.
  return setmetatable(fake_obj, {__index = forced_mt, __apimod_settable = false})
end

local socket_mt = {}
local function Socket()
  return force_obj({}, socket_mt)
end

function socket_mt:_from_handle(handle)
  init_ctx()
  if self._socket then self:close() end
  self._socket = handle
  self._status = "unpolled"
  return self
end

function socket_mt:_open(opener, ...)
  init_ctx()
  if self._socket then self:close() end
  self._socket = opener(_ctx, ...)
  self._status = "unpolled"
  return self
end

local function format_headers(headers)
  if type(headers) == 'string' then return headers end
  if type(headers) ~= 'table' then
    error("HTTP headers must be table|string, got: " .. tostring(headers))
  end
  local keys = {}
  for name, _ in pairs(headers) do
    table.insert(keys, name)
  end
  table.sort(keys)
  local frags = {}
  for idx, name in ipairs(keys) do
    local val = headers[name]
    if type(val) == 'string' then
      table.insert(frags, ("%s:%s"):format(name, val))
    else -- assume table representing a duplicated header
      for _, subval in ipairs(val) do
        table.insert(frags, ("%s:%s"):format(name, subval))
      end
    end
  end
  return table.concat(frags, "\n")
end

function socket_mt:http_get(url, headers, ret_body_only)
  headers = format_headers(headers or "")
  ret_body_only = not not ret_body_only
  return self:_open(
    pollnet.pollnet_simple_http_get,
    url,
    headers,
    ret_body_only
  )
end

function socket_mt:http_post(url, headers, body, ret_body_only)
  body = body or ""
  headers = format_headers(headers or {
    ["content-type"] = "application/x-www-form-urlencoded"
  })
  ret_body_only = not not ret_body_only
  return self:_open(
    pollnet.pollnet_simple_http_post,
    url,
    headers,
    body,
    #body,
    ret_body_only
  )
end

function socket_mt:open_ws(url)
  return self:_open(pollnet.pollnet_open_ws, url)
end

function socket_mt:open_tcp(addr)
  return self:_open(pollnet.pollnet_open_tcp, addr)
end

function socket_mt:serve_http(addr, dir)
  self.is_http_server = true
  if dir and dir ~= "" then
    return self:_open(pollnet.pollnet_serve_static_http, addr, dir)
  else
    return self:_open(pollnet.pollnet_serve_http, addr)
  end
end

function socket_mt:add_virtual_file(filename, filedata)
  assert(filedata and type(filedata) == 'string', "filedata must be provided as string!")
  if filename:sub(1,1) ~= "/" then
    -- url paths start from root at "/"
    filename = "/" .. filename
  end
  local dsize = #filedata
  pollnet.pollnet_add_virtual_file(_ctx, self._socket, filename, filedata, dsize)
end

function socket_mt:remove_virtual_file(filename)
  pollnet.pollnet_remove_virtual_file(_ctx, self._socket, filename)
end

function socket_mt:listen_ws(addr, callback)
  if callback then self:on_connection(callback) end
  return self:_open(pollnet.pollnet_listen_ws, addr)
end

function socket_mt:listen_tcp(addr, callback)
  if callback then self:on_connection(callback) end
  return self:_open(pollnet.pollnet_listen_tcp, addr)
end

function socket_mt:serve_dynamic_http(addr, keep_alive, callback)
  if callback then self:on_connection(callback) end
  return self:_open(pollnet.pollnet_serve_dynamic_http, addr, keep_alive or false)
end

function socket_mt:on_connection(f)
  self._on_connection = f
  return self
end

function socket_mt:_get_message()
  local msg_size = pollnet.pollnet_get_data_size(_ctx, self._socket)
  if msg_size > 0 then
    -- Note: unsafe_get_data_ptr requires careful consideration to use safely!
    -- Here we are OK because ffi.string copies the data to a new Lua string,
    -- so we only hang on to the pointer long enough for the copy.
    local raw_pointer = pollnet.pollnet_unsafe_get_data_ptr(_ctx, self._socket)
    if raw_pointer == nil then
      error("Impossible situation: msg_size > 0 but null data pointer")
    end
    return ffi.string(raw_pointer, msg_size)
  else
    return ""
  end
end

function socket_mt:poll()
  self._last_message = nil
  if not self._socket then
    self._status = "invalid"
    return false, "invalid"
  end
  local res = POLLNET_RESULT_CODES[pollnet.pollnet_update(_ctx, self._socket)] or "error"
  self._status = res
  if res == "hasdata" then
    self._status = "open"
    self._last_message = self:_get_message()
    return true, self._last_message
  elseif res == "nodata" then
    self._status = "open"
    return true
  elseif res == "opening" then
    self._status = "opening"
    return true
  elseif res == "error" then
    self._status = "error"
    self._last_message = self:_get_message()
    return false, self._last_message
  elseif res == "closed" then
    self._status = "closed"
    self._last_message = "closed"
    return false, "closed"
  elseif res == "newclient" then
    self._status = "open"
    local client_addr = self:_get_message()
    local client_handle = pollnet.pollnet_get_connected_client_handle(_ctx, self._socket)
    assert(client_handle > 0)
    local client_sock = Socket():_from_handle(client_handle)
    client_sock.parent = self
    client_sock.remote_addr = client_addr
    if self._on_connection then
      self._on_connection(client_sock, client_addr)
    else
      print("Incoming connection but no :on_connection handler! Just closing it!")
      client_sock:close()
    end
    return true
  end
end

function socket_mt:await()
  local yield_count = 0
  while true do
    if self.timeout and (yield_count > self.timeout) then
      return false, "timeout"
    end
    local happy, msg = self:poll()
    if not happy then
      self:close()
      return false, "error: " .. tostring(msg)
    end
    if msg then return msg end
    yield_count = yield_count + 1
    coroutine.yield()
  end
end

function socket_mt:await_n(count)
  local parts = {}
  for idx = 1, count do
    local part, err = self:await()
    if not part then return false, err end
    parts[idx] = part
  end
  return parts
end

function socket_mt:last_message()
  return self._last_message
end

function socket_mt:status()
  return self._status
end

function socket_mt:send(msg)
  assert(self._socket)
  assert(type(msg) == 'string', "Argument to send must be a string")
  pollnet.pollnet_send(_ctx, self._socket, msg)
end

function socket_mt:send_binary(msg)
  assert(self._socket)
  assert(type(msg) == 'string', "Argument to send must be a string")
  pollnet.pollnet_send_binary(_ctx, self._socket, msg, #msg)
end

function socket_mt:close()
  if not self._socket then return end
  pollnet.pollnet_close(_ctx, self._socket)
  self._socket = nil
end

local function get_nanoid()
  local _id_scratch = ffi.new("int8_t[?]", 128)
  local msg_size = pollnet.pollnet_get_nanoid(_id_scratch, 128)
  return ffi.string(_id_scratch, msg_size)
end

-- exports are gutted: anything that's pure, mods can just define themselves:
-- it'll be faster that way. The Reactor is highly unlikely to work via ApiMod
-- (it involves changing coroutines, which you can't do over a C boundary), and
-- is pure anyway. We don't expose init as that happens automatically, or
-- shutdown because we never want to (let ffi.gc handle that).
local exports = {
  VERSION = POLLNET_VERSION,
  Socket = Socket,
  nanoid = get_nanoid,  -- Not useful, but harmless to expose.
}

local fnames = {
  "open_ws", "listen_ws", "open_tcp", "listen_tcp",
  "serve_http", "serve_dynamic_http", "http_get", "http_post"
}
for _, name in ipairs(fnames) do
  exports[name] = function(...)
    local sock = Socket()
    return sock[name](sock, ...)
  end
end

return exports
  end -- end pollnet wrapper function
  ApiMod.register("pollnet", pollnet_binding())
end

-- Destroy all of our globals. Just in case anyone manages to escape into this
-- state (eg. ModLuaFileAppend).
-- The ffi is kept alive by the ApiMod state holding the bindings.
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

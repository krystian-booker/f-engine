-- Deep recursion through metamethods, error handlers and the bindings ends
-- in a Lua error (probes deep, errobj, errobj2). Lua allows 200 nested C
-- calls; sanitizer builds run with a larger C stack so those calls fit there
-- too (CMakeLists.txt).
local function forget_errors(t, pattern)
  local found = false
  for _, e in ipairs(fe.dev.errors()) do
    if e.message:find(pattern, 1, true) then found = true end
  end
  t.ok(found, "a reported error mentions '" .. pattern .. "'")
  fe.dev.clear_errors()
end

return {
  ["metamethod and tostring recursion raise stack overflow"] = function(t)
    local mt = {}
    mt.__index = function(self, k) return self[k] end
    local ok, err = pcall(function() return setmetatable({}, mt).x end)
    t.ok(not ok and tostring(err):find("stack overflow", 1, true), tostring(err))
    local s = setmetatable({}, { __tostring = function(x) return tostring(x) end })
    ok, err = pcall(tostring, s)
    t.ok(not ok and tostring(err):find("stack overflow", 1, true), tostring(err))
    local co = 0
    local function nest() co = co + 1; return coroutine.wrap(nest)() end
    t.ok(not pcall(nest))
  end,

  ["error objects whose tostring recurses or fails are survivable"] = function(t)
    local recursive = setmetatable({}, { __tostring = function(x) return tostring(x) end })
    t.ok(not xpcall(function() error(recursive) end, function(m) return tostring(m) end))
    t.ok(not xpcall(function() error(recursive) end, fe.dev.traceback))
    local failing = setmetatable({}, { __tostring = function() error("boom in tostring") end })
    t.ok(not xpcall(function() error(failing) end, function(m) return tostring(m) end))
    local output, ok = fe.dev.console("error(setmetatable({}, {__tostring = function(s) return tostring(s) end}))")
    t.eq(ok, false)
    t.eq(type(output), "string")
  end,

  ["a path cost function that searches again nests until Lua stops it"] = function(t)
    local w = fe.world.load("recursion_path", "arena")
    local depth = 0
    local function cost()
      depth = depth + 1
      w:path({ x = 1, z = 1 }, { x = 2, z = 1 }, { cost = cost })
      return 1
    end
    local ok, err = pcall(w.path, w, { x = 1, z = 1 }, { x = 2, z = 1 }, { cost = cost })
    t.ok(not ok)
    t.ok(tostring(err):find("stack overflow", 1, true), tostring(err):sub(-200))
    t.ok(depth > 20, "nested " .. depth .. " searches")
    fe.world.unload("recursion_path")
  end,

  ["a tick whose update ticks again nests until Lua stops it"] = function(t)
    fe.harness.start()
    local depth = 0
    _G.SANDBOX_UPDATE = function()
      depth = depth + 1
      fe.harness.tick(1)
    end
    fe.harness.tick(1)
    _G.SANDBOX_UPDATE = nil
    t.ok(depth > 20, "nested " .. depth .. " ticks")
    forget_errors(t, "stack overflow")
    fe.harness.tick(1)
  end,

  ["a module whose __reload reloads itself nests until Lua stops it"] = function(t)
    local m = require("recursive_reload")
    t.ok(fe.harness.reload("recursive_reload"))
    t.ok(m.reloads > 20, "nested " .. m.reloads .. " reloads")
    forget_errors(t, "stack overflow")
  end,
}

-- f-engine Lua standard library.
--
-- `require("fe")` returns this table of lazily loaded modules, so
-- `local fe = require("fe")` then `fe.task`, `fe.ui`, ... works without
-- listing requires. The C++ services live in the global `fe` table
-- (fe.world, fe.canvas, ...); this module adds the library on top of them.
local services = _G.fe
local library = setmetatable({}, {
  __index = function(self, name)
    local service = services[name]
    if service ~= nil then
      return service
    end
    local ok, module = pcall(require, "fe." .. name)
    if not ok then
      error(module, 2)
    end
    rawset(self, name, module)
    return module
  end,
})
return library

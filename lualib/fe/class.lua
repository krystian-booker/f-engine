--- Minimal classes with single inheritance.
---
--- ```lua
--- local Class = require("fe.class")
--- local Animal = Class("Animal")
--- function Animal:init(name) self.name = name end
--- local Dog = Class("Dog", Animal)
--- function Dog:init(name) Animal.init(self, name); self.sound = "woof" end
--- local rex = Dog("Rex")
--- ```
local function class(name, parent)
  local cls = {}
  cls.__index = cls
  cls.__name = name
  cls.super = parent
  setmetatable(cls, {
    __index = parent,
    __call = function(c, ...)
      local instance = setmetatable({}, c)
      if instance.init then
        instance:init(...)
      end
      return instance
    end,
    __tostring = function() return "class " .. name end,
  })
  function cls.is(object, other)
    local mt = getmetatable(object)
    while mt do
      if mt == other then
        return true
      end
      mt = mt.super
    end
    return false
  end
  return cls
end

return class

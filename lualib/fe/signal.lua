--- Signals: lists of listeners that can be emitted to.
---
--- ```lua
--- local on_hit = signal.new()
--- local disconnect = on_hit:connect(function(amount) print(amount) end)
--- on_hit:emit(12)
--- disconnect()
--- on_hit:connect(show_damage, 0, "hud.damage")   -- keyed: connecting again replaces it
--- ```
--- Module top-level code runs again on hot reload, so kits connect their
--- listeners with a key; a second connect with the same key replaces the
--- first instead of adding another.
local signal = {}

---@class Signal
local Signal = {}
Signal.__index = Signal

---@return Signal
function signal.new()
  return setmetatable({ listeners = {} }, Signal)
end

--- Add a listener; returns a function that removes it. With a `key`, an
--- earlier listener connected with the same key is removed first.
---@param fn function
---@param priority? number listeners with higher priority run first
---@param key? any
---@return fun()
function Signal:connect(fn, priority, key)
  local entry = { fn = fn, priority = priority or 0, key = key }
  local list = self.listeners
  if key ~= nil then
    for i, other in ipairs(list) do
      if other.key == key then
        other.removed = true
        table.remove(list, i)
        break
      end
    end
  end
  local index = #list + 1
  for i, other in ipairs(list) do
    if entry.priority > other.priority then
      index = i
      break
    end
  end
  table.insert(list, index, entry)
  return function()
    entry.removed = true
    for i, other in ipairs(self.listeners) do
      if other == entry then
        table.remove(self.listeners, i)
        return
      end
    end
  end
end

--- Add a listener that runs once.
function Signal:once(fn, priority)
  local disconnect
  disconnect = self:connect(function(...)
    disconnect()
    return fn(...)
  end, priority)
  return disconnect
end

--- Call every listener. A listener returning `true` stops the emission
--- (handled); `emit` then returns true.
function Signal:emit(...)
  local snapshot = { table.unpack(self.listeners) }
  for _, entry in ipairs(snapshot) do
    if not entry.removed and entry.fn(...) == true then
      return true
    end
  end
  return false
end

function Signal:clear()
  self.listeners = {}
end

function Signal:count()
  return #self.listeners
end

return signal

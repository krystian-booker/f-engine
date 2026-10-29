--- The game's persistent state.
---
--- State is split into named sections. Kits register the sections they own
--- (`party`, `inventory`, ...) and keep the returned table; saving snapshots
--- every section and loading replaces their contents in place, so references
--- held by kits stay valid. Flags and variables are built-in sections.
---
--- Everything in state must survive a JSON round trip: booleans, finite
--- numbers, strings and plain tables that are either lists (keys 1..n) or
--- string-keyed records. `set_var` rejects anything else immediately with
--- the offending path; `save.write` checks whole sections the same way.
local signal = require("fe.signal")
local tablex = require("fe.tablex")

local state = {
  --- Emitted as (kind, name, value) when something event conditions can
  --- read changes: "flag" and "var" from this module, "reload" after
  --- `restore`/`reset`, and kit kinds such as "quest" or "hour".
  changed = signal.new(),
  --- Emitted after `restore` or `reset`.
  reloaded = signal.new(),
}

local sections = {}
local order = {}

local function check(value, path, open)
  local kind = type(value)
  if kind == "string" or kind == "boolean" then
    return
  elseif kind == "number" then
    if value ~= value or value == math.huge or value == -math.huge then
      error(path .. ": numbers in state must be finite", 0)
    end
    return
  elseif kind ~= "table" then
    error(path .. ": " .. kind .. " values cannot be stored in state", 0)
  end
  if getmetatable(value) ~= nil then
    error(path .. ": tables with metatables (objects) cannot be stored in state", 0)
  end
  if open[value] then
    error(path .. ": state tables cannot contain themselves", 0)
  end
  open[value] = true
  local count, strings = 0, 0
  for key, child in pairs(value) do
    count = count + 1
    if type(key) == "string" then
      strings = strings + 1
      check(child, path .. "." .. key, open)
    elseif math.type(key) == "integer" and key >= 1 then
      check(child, path .. "[" .. key .. "]", open)
    else
      error(path .. ": state keys must be strings or list indices, got " .. tostring(key), 0)
    end
  end
  if strings > 0 and strings < count then
    error(path .. ": a state table cannot mix list entries and named fields", 0)
  end
  if strings == 0 and count ~= #value then
    error(path .. ": state lists cannot have holes", 0)
  end
  open[value] = nil
end

--- Raise a descriptive error unless `value` can be saved.
---@param value any
---@param path? string shown in the error
function state.check(value, path)
  check(value, path or "value", {})
end

--- Register (or fetch) a persistent section.
---@param name string
---@param default? fun(): table builds the initial contents
---@return table
function state.section(name, default)
  local existing = sections[name]
  if existing then
    if existing.orphan then
      existing.orphan = nil
      existing.default = default
      order[#order + 1] = name
    end
    return existing.value
  end
  local value = default and default() or {}
  sections[name] = { default = default, value = value }
  order[#order + 1] = name
  return value
end

local flags = state.section("flags")
local vars = state.section("vars")

---@param name string
---@return boolean
function state.flag(name)
  return flags[name] == true
end

---@param name string
---@param value? boolean defaults to true
function state.set_flag(name, value)
  if value == nil then value = true end
  if type(value) ~= "boolean" then
    error("state.set_flag('" .. tostring(name) .. "'): value must be a boolean", 2)
  end
  if (flags[name] == true) ~= value then
    flags[name] = value or nil
    state.changed:emit("flag", name, value)
  end
end

--- A variable (any JSON-compatible value).
function state.var(name, default)
  local value = vars[name]
  if value == nil then return default end
  return value
end

--- Set a variable. Raises if the value could not be saved.
function state.set_var(name, value)
  check(value, "vars." .. tostring(name), {})
  if vars[name] ~= value then
    vars[name] = value
    state.changed:emit("var", name, value)
  end
end

--- Add to a numeric variable; returns the new value.
function state.add(name, amount)
  local value = (vars[name] or 0) + amount
  state.set_var(name, value)
  return value
end

local function replace_contents(target, source)
  for key in pairs(target) do target[key] = nil end
  for key, value in pairs(source) do target[key] = value end
end

local function fresh(section)
  return section.default and section.default() or {}
end

--- Deep copy of every section (what a save stores), including sections a
--- save brought in that no kit has registered yet.
function state.snapshot()
  local out = {}
  for name, section in pairs(sections) do
    out[name] = tablex.deep_copy(section.value)
  end
  return out
end

--- Replace every section from a snapshot. Registered sections missing from
--- the snapshot go back to their defaults. Unknown sections are kept as
--- data (and written back by the next save) until a kit registers them;
--- they never become defaults for a new game.
function state.restore(snapshot)
  for name, section in pairs(sections) do
    if section.orphan and snapshot[name] == nil then sections[name] = nil end
  end
  for name, value in pairs(snapshot) do
    local section = sections[name]
    if section then
      replace_contents(section.value, tablex.deep_copy(value))
    else
      sections[name] = { value = tablex.deep_copy(value), orphan = true }
    end
  end
  for _, name in ipairs(order) do
    if snapshot[name] == nil then
      local section = sections[name]
      replace_contents(section.value, fresh(section))
    end
  end
  state.reloaded:emit()
  state.changed:emit("reload")
end

--- Back to a new game: registered sections take their defaults and data
--- from unregistered sections is dropped.
function state.reset()
  for name, section in pairs(sections) do
    if section.orphan then sections[name] = nil end
  end
  for _, name in ipairs(order) do
    local section = sections[name]
    replace_contents(section.value, fresh(section))
  end
  state.reloaded:emit()
  state.changed:emit("reload")
end

return state

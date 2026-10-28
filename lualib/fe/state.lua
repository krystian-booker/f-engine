--- The game's persistent state.
---
--- State is split into named sections. Kits register the sections they own
--- (`party`, `inventory`, ...) and keep the returned table; saving snapshots
--- every section and loading replaces their contents in place, so references
--- held by kits stay valid. Flags and variables are built-in sections.
local signal = require("fe.signal")
local tablex = require("fe.tablex")

local state = {
  --- Emitted as (kind, name, value) when a flag or variable changes.
  changed = signal.new(),
  --- Emitted after `restore` or `reset`.
  reloaded = signal.new(),
}

local sections = {}
local order = {}

--- Register (or fetch) a persistent section.
---@param name string
---@param default? fun(): table builds the initial contents
---@return table
function state.section(name, default)
  local existing = sections[name]
  if existing then
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
  if flags[name] ~= value then
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

function state.set_var(name, value)
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

--- Deep copy of every section (what a save stores).
function state.snapshot()
  local out = {}
  for _, name in ipairs(order) do
    out[name] = tablex.deep_copy(sections[name].value)
  end
  return out
end

--- Replace every section from a snapshot. Unknown sections are kept so a
--- save written by a newer version of a kit does not lose data.
function state.restore(snapshot)
  for name, value in pairs(snapshot) do
    local section = sections[name]
    if section then
      replace_contents(section.value, tablex.deep_copy(value))
    else
      state.section(name, function() return tablex.deep_copy(value) end)
    end
  end
  for _, name in ipairs(order) do
    if snapshot[name] == nil then
      local section = sections[name]
      replace_contents(section.value, section.default and section.default() or {})
    end
  end
  state.reloaded:emit()
end

--- Back to a new game.
function state.reset()
  for _, name in ipairs(order) do
    local section = sections[name]
    replace_contents(section.value, section.default and section.default() or {})
  end
  state.reloaded:emit()
end

return state

--- Save slots built on fe.save. A save holds `state.snapshot()` plus a
--- version number; metadata (location, party, playtime) is collected from
--- providers so slot lists can be shown without loading the whole save.
---
--- ```lua
--- save.version = 2
--- save.migrations[1] = function(data) data.vars.chapter = data.vars.chapter or 1 return data end
--- save.add_meta(function(meta) meta.location = field.current_map_name() end)
--- save.write("slot1")
--- ```
local signal = require("fe.signal")
local state = require("fe.state")

local save = {
  version = 1,
  --- migrations[n] upgrades data written by version n to version n + 1.
  migrations = {},
  loaded = signal.new(),
  saved = signal.new(),
  autosave_slot = "auto",
}

local providers = {}

--- Add a metadata provider: fn(meta) fills fields shown on save screens.
function save.add_meta(fn)
  providers[#providers + 1] = fn
end

--- Write the current state to a slot.
---@param slot string
---@param options? {thumbnail?: boolean, meta?: table}
function save.write(slot, options)
  options = options or {}
  local meta = { version = save.version, saved_at = fe.time.wall(), playtime = state.var("playtime", 0) }
  for _, provider in ipairs(providers) do
    provider(meta)
  end
  for key, value in pairs(options.meta or {}) do
    meta[key] = value
  end
  fe.save.write(slot, { version = save.version, state = state.snapshot() }, meta)
  if options.thumbnail then
    fe.save.thumbnail(slot)
  end
  save.saved:emit(slot, meta)
  return meta
end

--- Load a slot into the state. Returns the metadata, or nil and a reason.
---@param slot string
function save.load(slot)
  local data, meta = fe.save.read(slot)
  if not data then
    return nil, "empty slot"
  end
  local version = data.version or 1
  if version > save.version then
    return nil, "this save was made by a newer version of the game"
  end
  while version < save.version do
    local migrate = save.migrations[version]
    if not migrate then
      return nil, "no migration from save version " .. version
    end
    data = migrate(data) or data
    version = version + 1
  end
  state.restore(data.state or {})
  save.loaded:emit(slot, meta)
  return meta
end

--- Slots with metadata, sorted by name.
function save.list()
  return fe.save.list()
end

function save.exists(slot)
  for _, info in ipairs(fe.save.list()) do
    if info.slot == slot then return true end
  end
  return false
end

function save.delete(slot)
  fe.save.delete(slot)
end

--- Save to the autosave slot (call on map changes or after key events).
function save.autosave()
  return save.write(save.autosave_slot, { meta = { autosave = true } })
end

return save

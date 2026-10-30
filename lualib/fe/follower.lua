--- NPC followers: townsfolk who join the party on the field for a while
--- (Octopath's Allure and Guide). A follower walks in the field trail behind
--- the party and may carry battle-assist data for the battle kit.
---
--- ```lua
--- follower.join_entity(npc, {assist = {enemy = "old_man", skill = "kick", chance = 0.5}})
--- if follower.has("old_man") then ... end
--- follower.leave("old_man")
--- ```
--- Followers are saved in the "followers" state section. While an NPC
--- placement follows, the field does not spawn it on its map; when it
--- leaves it is back at its placement the next time the map loads.
--- `follower.max` (default 1) followers travel at once: a new one sends the
--- longest-serving one home.
---
--- Battle side: a follower with an `assist` table ({enemy = enemy data id,
--- skill = skill id, chance? = 0..1, name?}) joins the battle kit's
--- companions (`fe.battle.assist.join(id, assist)`) and leaves them when it
--- stops following. `follower.assist_module` names that module; when no
--- such module exists (no battle kit), followers only walk the field. A
--- module that exists but fails to load raises its error.
local signal = require("fe.signal")
local state = require("fe.state")
local tablex = require("fe.tablex")

local follower = {
  max = 1,
  --- Module whose `join(id, assist)` and `leave(id)` receive battle assists.
  assist_module = "fe.battle.assist",
  --- Emitted as (id, entry) when someone starts following.
  joined = signal.new(),
  --- Emitted as (id, entry) when a follower leaves.
  left = signal.new(),
}

--- The file's values of the settings above, for `__reload`.
follower.file_defaults = { max = follower.max, assist_module = follower.assist_module }

local record = state.section("followers", function() return { list = {} } end)

--- Adopt edited settings on hot reload (values the game changed at runtime
--- are kept).
function follower.__reload(live, fresh)
  live.file_defaults = tablex.adopt(live, { max = fresh.max, assist_module = fresh.assist_module }, live.file_defaults)
end

-- Whether a module file exists, in the game folder or in lualib (a
-- `fe.*` module), as a file or a folder's init.lua.
local function module_exists(name)
  local path = (name:gsub("%.", "/"))
  local candidates = { path .. ".lua", path .. "/init.lua" }
  if name:sub(1, 3) == "fe." then
    local inside = path:sub(4)
    candidates[#candidates + 1] = "@fe/" .. inside .. ".lua"
    candidates[#candidates + 1] = "@fe/" .. inside .. "/init.lua"
  end
  for _, candidate in ipairs(candidates) do
    if fe.fs.exists(candidate) then return true end
  end
  return false
end

local function battle_assist()
  local name = follower.assist_module
  if not module_exists(name) then return nil end
  local module = require(name)
  return type(module) == "table" and module or nil
end

local function index_of(id)
  for i, entry in ipairs(record.list) do
    if entry.id == id then return i end
  end
  return nil
end

--- Current followers, longest-serving first: {id, name, sprite, assist?}.
---@return table[]
function follower.list()
  return record.list
end

---@param id string
---@return table?
function follower.get(id)
  local index = index_of(id)
  return index and record.list[index]
end

---@param id string
---@return boolean
function follower.has(id)
  return index_of(id) ~= nil
end

--- Remove a follower; returns false when `id` was not following.
---@param id string
---@return boolean
function follower.leave(id)
  local index = index_of(id)
  if not index then return false end
  local entry = table.remove(record.list, index)
  local assist = entry.assist and battle_assist()
  if assist then assist.leave(id) end
  follower.left:emit(id, entry)
  return true
end

--- Start following (or update a follower). `spec.sprite` is the field
--- sprite; `assist` goes to the battle kit. Returns the entry.
---@param id string
---@param spec {name?: string, sprite: string, assist?: {enemy: string, skill: string, chance?: number, name?: string}}
---@return table
function follower.join(id, spec)
  local entry = { id = id, name = spec.name or id, sprite = spec.sprite, assist = spec.assist }
  state.check(entry, "followers." .. id)
  local assist = entry.assist and battle_assist()
  local index = index_of(id)
  if index then
    record.list[index] = entry
  else
    while #record.list >= math.max(1, follower.max) do
      follower.leave(record.list[1].id)
    end
    record.list[#record.list + 1] = entry
  end
  if assist then
    local settings = { name = entry.name }
    for key, value in pairs(entry.assist) do settings[key] = value end
    assist.join(id, settings)
  end
  follower.joined:emit(id, entry)
  return entry
end

--- An NPC entity joins: its id, `props.name` and sprite make the follower
--- (fields in `spec` override them) and the entity is removed from the map.
---@param entity Entity
---@param spec? table
---@return table
function follower.join_entity(entity, spec)
  local sprite = entity:sprite()
  local settings = { name = entity:props().name, sprite = sprite and sprite.animation }
  for key, value in pairs(spec or {}) do settings[key] = value end
  local entry = follower.join(entity.id, settings)
  entity:destroy()
  return entry
end

return follower

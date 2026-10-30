--- Game-defined stats, resources and modifiers.
---
--- Nothing about HP or attack is built into the engine: a game declares its
--- stats and resources (the defaults below suit a classic JRPG) and every
--- formula reads them by id.
---
--- ```lua
--- stats.define({
---   {id = "max_hp", name = "Max HP", min = 1, max = 99999},
---   {id = "attack", name = "Attack"}, ...
--- })
--- stats.resources({
---   hp = {max = "max_hp", name = "HP"},
---   sp = {max = "max_sp", name = "SP"},
---   bp = {max = 5, start = 1, name = "BP", battle_only = true},
--- })
--- ```
--- Modifiers are {stat = "attack", add = 10}, {stat = "speed", mul = 1.5} or
--- {stat = "move", set = 0} (optionally `source` and `priority`);
--- `stats.compute` folds them in.
local tablex = require("fe.tablex")

local stats = { list = {}, by_id = {}, resource_list = {}, resource_by_id = {},
  --- Bumped by every define/resources call; combatants key their stat caches by it.
  version = 0 }

--- Declare the stats (replaces previous definitions).
---@param list {id: string, name?: string, short?: string, min?: number, max?: number, integer?: boolean}[]
function stats.define(list)
  stats.list, stats.by_id = {}, {}
  stats.version = stats.version + 1
  for _, def in ipairs(list) do
    def.name = def.name or def.id
    def.min = def.min or 0
    if def.integer == nil then def.integer = true end
    stats.list[#stats.list + 1] = def
    stats.by_id[def.id] = def
  end
end

--- Declare resources: current values bounded by a stat or a constant.
---@param map table<string, {max: string|number, name?: string, start?: number|"max", battle_only?: boolean}>
function stats.resources(map)
  stats.resource_list, stats.resource_by_id = {}, {}
  stats.version = stats.version + 1
  local ids = {}
  for id in pairs(map) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local def = map[id]
    def.id = id
    def.name = def.name or id:upper()
    if def.start == nil then def.start = "max" end
    stats.resource_list[#stats.resource_list + 1] = def
    stats.resource_by_id[id] = def
  end
end

function stats.get(id)
  return stats.by_id[id]
end

function stats.resource(id)
  return stats.resource_by_id[id]
end

--- Apply modifiers to base values. Order: additions, then multipliers, then
--- `set` overrides; results are clamped and rounded per stat definition.
--- When several `set` modifiers touch a stat, the highest `priority`
--- (default 0) wins and ties go to the later modifier in the list
--- (combatants list equipment and passives, then buffs, then states in
--- sorted id order, so the result never depends on table iteration).
---@param base table<string, number>
---@param modifiers table[]
---@return table<string, number>
function stats.compute(base, modifiers)
  local add, mul, set, set_priority = {}, {}, {}, {}
  for _, m in ipairs(modifiers or {}) do
    if m.add then add[m.stat] = (add[m.stat] or 0) + m.add end
    if m.mul then mul[m.stat] = (mul[m.stat] or 1) * m.mul end
    if m.set then
      local priority = m.priority or 0
      if set_priority[m.stat] == nil or priority >= set_priority[m.stat] then
        set[m.stat], set_priority[m.stat] = m.set, priority
      end
    end
  end
  local out = {}
  for _, def in ipairs(stats.list) do
    local value = ((base[def.id] or 0) + (add[def.id] or 0)) * (mul[def.id] or 1)
    if set[def.id] then value = set[def.id] end
    value = tablex.clamp(value, def.min, def.max or math.huge)
    if def.integer then value = math.floor(value + 0.5) end
    out[def.id] = value
  end
  -- Keep values for stats the game did not declare (custom data).
  for key, value in pairs(base) do
    if out[key] == nil then out[key] = value end
  end
  return out
end

--- Maximum of a resource given computed stats.
function stats.resource_max(id, values)
  local def = stats.resource_by_id[id]
  if not def then error("unknown resource '" .. tostring(id) .. "'", 2) end
  if type(def.max) == "number" then return def.max end
  return values[def.max] or 0
end

-- Defaults: a classic JRPG stat block. Games call define/resources to replace.
stats.define({
  { id = "max_hp", name = "Max HP", short = "HP", min = 1, max = 99999 },
  { id = "max_mp", name = "Max MP", short = "MP", min = 0, max = 9999 },
  { id = "attack", name = "Attack", short = "Atk", max = 9999 },
  { id = "defense", name = "Defense", short = "Def", max = 9999 },
  { id = "magic", name = "Magic", short = "Mag", max = 9999 },
  { id = "spirit", name = "Spirit", short = "Spr", max = 9999 },
  { id = "speed", name = "Speed", short = "Spd", min = 1, max = 999 },
  { id = "accuracy", name = "Accuracy", short = "Acc", max = 999 },
  { id = "evasion", name = "Evasion", short = "Eva", max = 999 },
  { id = "critical", name = "Critical", short = "Crt", max = 100 },
  { id = "move", name = "Move", short = "Mov", max = 20 },
  { id = "jump", name = "Jump", short = "Jmp", max = 20 },
})
stats.resources({
  hp = { max = "max_hp", name = "HP" },
  mp = { max = "max_mp", name = "MP" },
})

return stats

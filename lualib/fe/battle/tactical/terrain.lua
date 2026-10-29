--- Terrain reactions and hazards (data in tactical.reactions and
--- tactical.hazards). Cell changes go through terrain.set_cell, which
--- remembers each cell as it was before the battle first changed it;
--- terrain.revert puts every such cell back (tags and walkability) and
--- drops the timers when the battle ends or is aborted, so a battle on the
--- field world (or a mock battle) leaves the map as it found it. Damage goes through
--- fe.effects as environmental effects, so resistances apply. Presenters
--- get "terrain" ({cell, reaction}) and "terrain_changed" events. A
--- destroyed obstacle changes its cell with the `destroyed` reaction from
--- its enemy data (e.g. {"add": ["rubble"]}). A unit standing on a cell
--- that a reaction makes unwalkable (ice melting back into deep water) is
--- washed to the nearest free cell (event "pushed") and takes
--- settings.sink_damage of its max HP.
local effects = require("fe.effects")
local grid = require("fe.battle.tactical.grid")
local config = require("fe.battle.tactical.settings")
local i18n = require("fe.i18n")
local tablex = require("fe.tablex")

local terrain = {}

i18n.defaults({ fe = { tactics = { sink = "Sinking" } } })

--- Change a cell (world:set_cell changes) and remember how it was before
--- the battle first changed it, for terrain.revert.
function terrain.set_cell(battle, cell, changes)
  local t = battle.tactical
  local k = grid.key(cell)
  if not t.original[k] then
    local info = t.world:cell(cell)
    t.original[k] = { cell = { x = cell.x, z = cell.z, layer = cell.layer or 0 }, tags = tablex.copy(info.tags),
      walkable = info.walkable }
  end
  t.world:set_cell(cell, changes)
end

--- Put every cell the battle changed back as it was and drop the terrain
--- timers (the tactical rule layer does this when the battle ends).
function terrain.revert(battle)
  local t = battle.tactical
  local keys = tablex.keys(t.original)
  table.sort(keys)
  for _, k in ipairs(keys) do
    local before = t.original[k]
    local now = t.world:cell(before.cell)
    t.world:set_cell(before.cell, {
      add_tags = tablex.filter(before.tags, function(tag) return not tablex.contains(now.tags, tag) end),
      remove_tags = tablex.filter(now.tags, function(tag) return not tablex.contains(before.tags, tag) end),
      walkable = before.walkable,
    })
  end
  t.original, t.cell_timers, t.walkable_before = {}, {}, {}
end

local neighbours = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }

local function has_tag(world, cell, tag)
  local info = world:cell(cell)
  return info ~= nil and tablex.contains(info.tags, tag)
end

--- Hurt `units` with a terrain source ({name, element, effects}).
function terrain.harm(battle, units, source)
  if #units == 0 then return {} end
  local results = {}
  for _, u in ipairs(units) do
    for _, r in ipairs(effects.resolve({ user = u, targets = { u }, source = source, battle = battle,
      environmental = true })) do
      results[#results + 1] = r
    end
  end
  battle:present("results", { action = { source = source }, results = results })
  return results
end

--- A unit on a cell that stopped being walkable is washed to the nearest
-- free cell and hurt.
local function strand(battle, cell)
  local u = grid.unit_at(battle, cell)
  if not u or u.obstacle or not u:alive() then return end
  local to = grid.nearest_free(battle, cell)
  if not to then return end
  local from = u.cell
  grid.place(battle, u, to)
  battle:present("pushed", { battler = u, from = from, to = u.cell })
  local damage = config.settings.sink_damage
  if damage > 0 then
    terrain.harm(battle, { u }, { name = i18n.t("fe.tactics.sink"),
      effects = { { type = "damage", formula = "percent", power = damage * 100, variance = 0 } } })
  end
end

-- Change a cell with a reaction table ({add, remove, walkable, turns}).
local function change_cell(battle, cell, reaction)
  local t = battle.tactical
  local world = t.world
  local k = grid.key(cell)
  local changes = { add_tags = reaction.add or {}, remove_tags = reaction.remove or {} }
  if reaction.walkable == "restore" then
    local before = t.walkable_before[k]
    changes.walkable = before == true
    t.walkable_before[k] = nil
  elseif reaction.walkable ~= nil then
    if t.walkable_before[k] == nil then
      local info = world:cell(cell)
      t.walkable_before[k] = info.walkable
    end
    changes.walkable = reaction.walkable
  end
  terrain.set_cell(battle, cell, changes)
  if changes.walkable == false then strand(battle, cell) end
  if reaction.turns then
    t.cell_timers[k] = { cell = { x = cell.x, z = cell.z, layer = cell.layer or 0 }, turns = reaction.turns,
      tags = reaction.add, reaction = reaction }
  end
  battle:present("terrain", { cell = cell, reaction = reaction })
end

-- Every unit standing in the region of `reaction.tag` cells connected to
-- any of `origins` is hurt once.
local function conduct(battle, origins, reaction)
  local world = battle.tactical.world
  local seen, queue, units = {}, {}, {}
  for _, c in ipairs(origins) do queue[#queue + 1] = c end
  while #queue > 0 do
    local cell = table.remove(queue, 1)
    local k = grid.key(cell)
    if not seen[k] and has_tag(world, cell, reaction.tag) then
      seen[k] = true
      local u = grid.unit_at(battle, cell)
      if u and u:alive() then units[#units + 1] = u end
      for _, d in ipairs(neighbours) do
        queue[#queue + 1] = { x = cell.x + d[1], z = cell.z + d[2], layer = cell.layer or 0 }
      end
    end
  end
  battle:present("terrain", { cell = origins[1], reaction = reaction })
  terrain.harm(battle, units, { name = reaction.name or reaction.element, element = reaction.element,
    effects = reaction.effects or {} })
end

terrain.change = change_cell

--- An obstacle was destroyed: its `destroyed` reaction changes its cell.
--- Presents "destroyed" ({battler, cell}).
function terrain.destroyed(battle, b)
  local reaction = b.def and b.def.destroyed
  battle:present("destroyed", { battler = b, cell = b.cell })
  if reaction and b.cell then
    change_cell(battle, b.cell, reaction)
    battle:present("terrain_changed", battle)
  end
end

--- Apply terrain reactions for an element landing on cells.
function terrain.react(battle, element, cells)
  if not element then return end
  local world = battle.tactical.world
  for _, reaction in ipairs(config.reactions) do
    if reaction.element == element then
      local hits = tablex.filter(cells, function(cell) return has_tag(world, cell, reaction.tag) end)
      if #hits > 0 then
        if reaction.conduct then
          conduct(battle, hits, reaction)
        else
          for _, cell in ipairs(hits) do change_cell(battle, cell, reaction) end
        end
      end
    end
  end
  battle:present("terrain_changed", battle)
end

--- A new round: timed cells count down (and wear off), and spreading
--- reactions (burning grass) may catch on adjacent cells.
function terrain.tick(battle)
  local t = battle.tactical
  local world = t.world
  local spreading = {}
  for _, k in ipairs(tablex.keys(t.cell_timers)) do
    local timer = t.cell_timers[k]
    timer.turns = timer.turns - 1
    if timer.turns <= 0 then
      terrain.set_cell(battle, timer.cell, { remove_tags = timer.tags })
      t.cell_timers[k] = nil
    elseif timer.reaction.spread_chance then
      spreading[#spreading + 1] = timer
    end
  end
  for _, timer in ipairs(spreading) do
    for _, d in ipairs(neighbours) do
      local cell = { x = timer.cell.x + d[1], z = timer.cell.z + d[2], layer = timer.cell.layer }
      if has_tag(world, cell, timer.reaction.tag) and battle.random:float() < timer.reaction.spread_chance then
        change_cell(battle, cell, timer.reaction)
      end
    end
  end
  battle:present("terrain_changed", battle)
end

--- Hazards on the unit's cell at the start of its turn (burning ground).
function terrain.hazards(battle, b)
  if not b.cell or not b:alive() then return end
  local world = battle.tactical.world
  for _, hazard in ipairs(config.hazards) do
    if b:alive() and has_tag(world, b.cell, hazard.tag) then
      terrain.harm(battle, { b }, hazard)
    end
  end
  battle:settle()
end

return terrain

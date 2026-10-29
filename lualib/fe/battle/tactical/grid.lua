--- The tactical board: occupancy, facing, movement ranges (with zones of
--- control), skill ranges and areas. Pure battle state: nothing here reads
--- or moves entities; presenters follow the "step", "pushed", "placed" and
--- "facing" events.
---
--- battle.tactical holds {world, occupied = {[cell key] = living unit},
--- corpses = {[cell key] = {fallen units, oldest first}}, cell_timers,
--- walkable_before, original (cells as they were before the battle changed
--- them), reinforcements = {pending specs}}. Several fallen units can lie
--- on one cell; each stays there until revived or removed from the battle.
--- Obstacles occupy their cell like units (nobody walks through them) and
--- block line of sight; destroyed ones leave no corpse.
local data = require("fe.data")
local tablex = require("fe.tablex")
local config = require("fe.battle.tactical.settings")
local vec = require("fe.vec")

local grid = {}

local settings = config.settings

function grid.key(cell) return vec.cell_key(cell) end

--- Cardinal direction from cell a towards cell b.
function grid.direction_between(a, b)
  local dx, dz = b.x - a.x, b.z - a.z
  if math.abs(dx) >= math.abs(dz) then return { x = dx >= 0 and 1 or -1, z = 0 } end
  return { x = 0, z = dz >= 0 and 1 or -1 }
end

function grid.cell_height(world, cell)
  local c = world:cell(cell)
  return c and c.height or 0
end

function grid.range_of(skill)
  return skill.range or settings.attack_range
end

function grid.area_of(skill)
  return skill.area or { shape = "diamond", min = 0, max = 0 }
end

--- The cell entry in `cells` matching `cell` (x, z and layer), if any.
function grid.contains_cell(cells, cell)
  for _, c in ipairs(cells) do
    if c.x == cell.x and c.z == cell.z and (c.layer or 0) == (cell.layer or 0) then return c end
  end
end

--- The living unit on a cell.
function grid.unit_at(battle, cell)
  return battle.tactical.occupied[grid.key(cell)]
end

--- The fallen units lying on a cell, oldest first.
function grid.corpses_at(battle, cell)
  return tablex.filter(battle.tactical.corpses[grid.key(cell)] or {}, function(u) return u.battle == battle end)
end

--- The fallen unit on top of a cell (the latest to fall there), if any.
function grid.corpse_at(battle, cell)
  local list = grid.corpses_at(battle, cell)
  return list[#list]
end

-- Take a unit's corpse off its cell.
local function clear_corpse(t, b)
  local k = grid.key(b.cell)
  local list = t.corpses[k]
  if list and tablex.remove_value(list, b) and #list == 0 then t.corpses[k] = nil end
end

--- Put a unit on a cell (occupancy only).
function grid.place(battle, b, cell)
  local t = battle.tactical
  if b.cell and t.occupied[grid.key(b.cell)] == b then t.occupied[grid.key(b.cell)] = nil end
  b.cell = { x = cell.x, z = cell.z, layer = cell.layer or 0 }
  t.occupied[grid.key(b.cell)] = b
end

--- Take a unit off the board (deployment changes).
function grid.unplace(battle, b)
  local t = battle.tactical
  if b.cell then
    if t.occupied[grid.key(b.cell)] == b then t.occupied[grid.key(b.cell)] = nil end
    clear_corpse(t, b)
  end
end

--- A unit falls: its cell frees up, and it stays there as a corpse
--- (destroyed obstacles simply leave).
function grid.lay_down(battle, b)
  if not b.cell then return end
  local t = battle.tactical
  local k = grid.key(b.cell)
  if t.occupied[k] == b then t.occupied[k] = nil end
  if not b.obstacle then
    t.corpses[k] = t.corpses[k] or {}
    table.insert(t.corpses[k], b)
  end
end

--- The standing obstacle on a cell, if any.
function grid.obstacle_at(battle, cell)
  local u = grid.unit_at(battle, cell)
  return u and u.obstacle and u or nil
end

--- Whether no standing obstacle lies on the straight line between two
--- cells' centres (the end cells themselves do not count).
function grid.clear_line(battle, from, to)
  local dx, dz = to.x - from.x, to.z - from.z
  local steps = math.max(math.abs(dx), math.abs(dz)) * 4
  for i = 1, steps - 1 do
    local t = i / steps
    local cell = { x = math.floor(from.x + 0.5 + dx * t), z = math.floor(from.z + 0.5 + dz * t), layer = from.layer or 0 }
    if not (cell.x == from.x and cell.z == from.z) and not (cell.x == to.x and cell.z == to.z)
      and grid.obstacle_at(battle, cell) then
      return false
    end
  end
  return true
end

local function free(battle, cell)
  local c = battle.tactical.world:cell(cell)
  if not c or not c.walkable or grid.unit_at(battle, cell) then return false end
  for _, tag in ipairs(settings.blocked_tags) do
    if tablex.contains(c.tags, tag) then return false end
  end
  return true
end

--- The free cell nearest to `cell` (itself when free), or nil.
function grid.nearest_free(battle, cell)
  if free(battle, cell) then return { x = cell.x, z = cell.z, layer = cell.layer or 0 } end
  local world = battle.tactical.world
  local gw, gd = world:grid_size()
  for radius = 1, gw + gd do
    for _, c in ipairs(world:area(cell, "ring", radius, radius)) do
      if free(battle, c) then return c end
    end
  end
end

--- A revived unit stands up on its cell, or the nearest free one (other
--- fallen units stay where they lie).
function grid.stand_up(battle, b)
  if not b.cell then return end
  clear_corpse(battle.tactical, b)
  local cell = grid.nearest_free(battle, b.cell)
  if cell then grid.place(battle, b, cell) end
end

--- Turn a unit to face a direction ({x, z}, snapped to the grid axes).
function grid.face(battle, b, direction)
  if not direction or (direction.x == 0 and direction.z == 0) then return end
  b.facing = grid.direction_between({ x = 0, z = 0 }, direction)
  battle:present("facing", b)
end

--- Walk a unit along a path of cells (grid.reachable paths: a step may be
--- marked `jump` or `climb`, which "step" passes on in `to`); it ends
--- facing its last step.
function grid.walk(battle, b, path)
  for _, cell in ipairs(path) do
    local from = b.cell
    battle:present("step", { battler = b, from = from, to = cell })
    grid.place(battle, b, cell)
    if cell.x ~= from.x or cell.z ~= from.z then b.facing = grid.direction_between(from, cell) end
  end
end

--- Movement rules for world:reachable (`blocked` adds cells nobody may
--- enter). The unit's jump stat (settings.default_jump without one) limits
--- how far it climbs up (and one more down), steps and gap jumps alike;
--- settings.gap_jump allows gap jumps and settings.ladders ladders.
function grid.step_rules(battle, b, blocked)
  local stop_blocked, no_stop = {}, {}
  for _, other in ipairs(battle:living()) do
    if other ~= b and other.cell then
      if other.side == b.side then no_stop[#no_stop + 1] = other.cell else stop_blocked[#stop_blocked + 1] = other.cell end
    end
  end
  for _, c in ipairs(blocked or {}) do stop_blocked[#stop_blocked + 1] = c end
  local jump = b:stat("jump")
  if jump <= 0 then jump = settings.default_jump end
  local gap = settings.gap_jump
  return { max_up = jump, max_down = jump + 1, blocked = stop_blocked, no_stop = no_stop,
    blocked_tags = settings.blocked_tags, tag_costs = settings.tag_costs, ladders = settings.ladders,
    jump = gap and { distance = gap.distance, diagonal = gap.diagonal } or nil }
end

function grid.move_points(b)
  local move = b:stat("move")
  return move > 0 and move or settings.default_move
end

-- Cells next to any foe of `b`: entering one ends movement.
local function control_zone(battle, b)
  local zone = {}
  for _, foe in ipairs(battle:opponents(b)) do
    if foe.cell then
      for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
        zone[#zone + 1] = { x = foe.cell.x + d[1], z = foe.cell.z + d[2], layer = foe.cell.layer or 0 }
      end
    end
  end
  return zone
end

local function with_paths(cells)
  for _, c in ipairs(cells) do
    local path, node = {}, c
    while node.parent do
      table.insert(path, 1, { x = node.x, z = node.z, layer = node.layer, jump = node.jump, climb = node.climb })
      node = cells[node.parent]
    end
    c.path = path
  end
  return cells
end

--- Cells a unit can move to this turn. Each entry carries `path`, the
--- cells walked to reach it, each marked `jump` or `climb` when the step
--- into it jumps a gap or climbs a ladder. With zones of control, a unit may enter a
--- cell next to a foe but not move on from it (except out of its starting
--- cell); world:reachable's `zoc` rule reaches every such cell by its
--- cheapest path through other cells.
function grid.reachable(battle, b)
  local rules = grid.step_rules(battle, b)
  if settings.zone_of_control then rules.zoc = control_zone(battle, b) end
  return tablex.filter(with_paths(battle.tactical.world:reachable(b.cell, grid.move_points(b), rules)),
    function(c) return c.stop end)
end

--- Path (list of cells) to a reachable entry.
function grid.path_to(cells, target)
  local c = grid.contains_cell(cells, target)
  return c and c.path or {}
end

local directions = { { x = 0, z = -1 }, { x = 1, z = 0 }, { x = 0, z = 1 }, { x = -1, z = 0 } }

--- Cells a skill can target from a cell (default: the user's). Line and
--- cone ranges cover all four directions: units turn to face their target
--- when they act. Range `height` limits the height difference; `los`
--- skills need a clear line of sight, past the terrain and obstacles.
function grid.range_cells(battle, user, skill, from)
  from = from or user.cell
  local world = battle.tactical.world
  local r = grid.range_of(skill)
  local shape = r.shape or "diamond"
  local cells
  if shape == "line" or shape == "cone" then
    cells = {}
    for _, d in ipairs(directions) do
      for _, c in ipairs(world:area(from, shape, r.min or 0, r.max or 0, d)) do
        if not grid.contains_cell(cells, c) then cells[#cells + 1] = c end
      end
    end
  else
    cells = world:area(from, shape, r.min or 0, r.max or 0)
  end
  local h0 = grid.cell_height(world, from)
  return tablex.filter(cells, function(c)
    if r.height and math.abs(grid.cell_height(world, c) - h0) > r.height then return false end
    if skill.los then
      local eye = { x = from.x + 0.5, y = h0 + 1.2, z = from.z + 0.5 }
      local goal = { x = c.x + 0.5, y = grid.cell_height(world, c) + 0.8, z = c.z + 0.5 }
      if not world:line_of_sight(eye, goal, from.layer or 0) or not grid.clear_line(battle, from, c) then return false end
    end
    return true
  end)
end

--- Units a skill aimed at `cell` would affect, and the cells it covers.
--- Line and cone areas point from the user towards the cell. `dead_ally`
--- skills find the latest ally to fall on each cell of the area.
function grid.affected(battle, user, skill, cell)
  local world = battle.tactical.world
  local a = grid.area_of(skill)
  local cells = world:area(cell, a.shape or "diamond", a.min or 0, a.max or 0, grid.direction_between(user.cell, cell))
  local wants_allies = skill.target == "ally" or skill.target == "all_allies" or skill.target == "self"
  local units = {}
  for _, c in ipairs(cells) do
    if skill.target == "dead_ally" then
      local fallen = tablex.filter(grid.corpses_at(battle, c), function(u) return u.side == user.side and not u:alive() end)
      if #fallen > 0 then units[#units + 1] = fallen[#fallen] end
    else
      local u = grid.unit_at(battle, c)
      if u then
        local friendly = u.side == user.side
        if (skill.target == "any" or skill.target == "everyone") or (wants_allies and friendly)
          or (not wants_allies and not friendly) then
          units[#units + 1] = u
        end
      end
    end
  end
  return units, cells
end

--- The skill for an action table, if it names one.
function grid.skill_of(action)
  return action.skill and data.find("skills", action.skill)
end

return grid

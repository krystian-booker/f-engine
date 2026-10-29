--- Tactical presentation: unit sprites kept in sync with battle events,
--- the grid scene and HUD, and player input (move / act / wait with undo,
--- target forecasts with hit chance, facing, CT turn-order previews).
--- Text comes from fe.tactics.* i18n keys.
---
--- Units walk their paths a step at a time with
--- `entity:move_to(nil, {path = {cell}})`: steps marked `jump` (gap
--- jumps) or `climb` (ladders) are jumped or climbed by the engine, and
--- other steps up or down more than 0.6 are hopped. While the board is
--- shown the scene reads the world's events: a unit's jump_start, landed,
--- climb_start and climb_end call `Presenter:motion(battler, kind)`, which
--- plays settings.motion_clips[kind] when the unit's sprite has that clip.
local Battle = require("fe.battle.core")
local camera = require("fe.camera")
local effects = require("fe.effects")
local grid = require("fe.battle.tactical.grid")
local i18n = require("fe.i18n")
local rules = require("fe.battle.tactical.rules")
local scene = require("fe.scene")
local config = require("fe.battle.tactical.settings")
local sound = require("fe.sound")
local tablex = require("fe.tablex")
local task = require("fe.task")
local transition = require("fe.transition")
local tween = require("fe.tween")
local ui = require("fe.ui")
local vec = require("fe.vec")

local settings = config.settings

i18n.defaults({ fe = { tactics = {
  turn = "{name}'s turn", follow_up = "Follow-up: {name}", counters = "{name} counters!",
  reinforcements = "Reinforcements!", destroyed = "{name} is destroyed", miss = "Miss",
  victory = "Victory!", defeat = "Defeat...", over = "Battle over",
  command = { move = "Move", act = "Act", wait = "Wait", undo = "Undo move" },
  facing = "Choose a facing, then {button}", ct = "CT {cost}",
} } })

local Presenter = {}
Presenter.__index = Presenter

---@param options {map?: string, music?: string}
function Presenter.new(battle, options)
  return setmetatable({ battle = battle, options = options or {}, popups = {}, units = {} }, Presenter)
end

--- Recolour terrain overlays (burning, ice) from the grid.
function Presenter:update_overlays()
  local world = self.world
  world:overlay("terrain:burning", world:cells_with_tag("burning"), { color = 0xff7a2a66, pulse = 2, order = -1 })
  world:overlay("terrain:ice", world:cells_with_tag("ice"), { color = 0xbfe8ff55, order = -1 })
end

local TacticalScene = { name = "tactical", opaque = true }
TacticalScene.__index = TacticalScene

function TacticalScene:update(dt)
  local p = self.presenter
  if fe.input.pressed("camera_left") then p.rig:rotate(-90, true) end
  if fe.input.pressed("camera_right") then p.rig:rotate(90, true) end
  if fe.input.pressed("info") then p.top_down = not p.top_down p.rig:set_top_down(p.top_down) end
  p.rig:update(dt)
  for _, popup in ipairs(p.popups) do popup.age = popup.age + dt end
  p.popups = tablex.filter(p.popups, function(popup) return popup.age < 1.2 end)
end

-- After the world moved: units' jumps and climbs.
function TacticalScene:post_update()
  local p = self.presenter
  for _, e in ipairs(p.world:events()) do
    local b = e.a and p.units[e.a.key]
    if b and (e.type == "jump_start" or e.type == "landed" or e.type == "climb_start" or e.type == "climb_end") then
      p:motion(b, e.type)
    end
  end
end

function TacticalScene:draw()
  local p = self.presenter
  p.world:draw()
  require("fe.battle.tactical").draw_hud(p)
end

--- A sprite for a unit at its cell.
function Presenter:spawn(b)
  b.entity = self.world:spawn({
    position = vec.cell_centre(b.cell), layer = b.cell.layer, facing = b.facing,
    sprite = { animation = b:sprite(), move_clip = "walk", idle_clip = "idle", visible = b:alive() },
    mover = { speed = 4, collide = false }, tags = { "unit", b.side },
  })
  self.units[b.entity.key] = b
  return b.entity
end

function Presenter:despawn(b)
  if b.entity then
    self.units[b.entity.key] = nil
    if b.entity:valid() then b.entity:destroy() end
  end
  b.entity = nil
end

--- A unit's sprite started or finished a jump or a ladder climb (`kind`
--- is the world event: jump_start, landed, climb_start or climb_end).
function Presenter:motion(b, kind)
  local clip = settings.motion_clips[kind]
  local entity = b.entity
  if clip and entity and entity:valid() and fe.app.sprite_clips(entity:sprite().animation)[clip] then
    entity:clip(clip)
  end
end

-- Snap a unit's sprite to its cell.
local function sync(b)
  if b.entity and b.entity:valid() and b.cell then
    b.entity:place(vec.cell_centre(b.cell), b.cell.layer)
    if b.facing then b.entity:face(b.facing) end
  end
end
function Presenter:sync(b) sync(b) end

--- Show the board (deployment may start it early).
function Presenter:start(battle)
  if self.scene then return end
  self.world = battle.tactical.world
  if self.options.music then sound.music(self.options.music) end
  for _, b in ipairs(battle.battlers) do
    if b.cell and not b.entity then self:spawn(b) end
  end
  self.rig = camera.rig(self.world, settings.camera)
  local first = battle.battlers[1]
  self.rig:look_at(vec.cell_centre(first and first.cell or { x = 0, z = 0 }))
  self.rig:snap()
  self.scene = setmetatable({ presenter = self }, TacticalScene)
  scene.push(self.scene)
  self:update_overlays()
  if self.options.map then transition.into("fade", 0.3) end
end

function Presenter:turn(payload)
  self.order = payload.order
  self.active = payload.battler
  self.rig:follow(payload.battler.entity)
  self.ct_cost = nil
  self:banner(i18n.t("fe.tactics.turn", { name = payload.battler:name() }), 0.8)
  task.wait(0.3)
end

function Presenter:banner(text, duration)
  self.banner_text = text
  task.spawn(function()
    task.wait(duration or 1)
    if self.banner_text == text then self.banner_text = nil end
  end)
end

function Presenter:popup(b, text, color)
  if not b.entity or not b.entity:valid() then return end
  local x, y = b.entity:screen_position(1.4)
  self.popups[#self.popups + 1] = { x = x, y = y, text = text, color = color or 0xffffffff, age = 0 }
end

function Presenter:step(payload)
  local b, world, to = payload.battler, self.world, payload.to
  local entity = b.entity
  if not entity or not entity:valid() then return end
  local from_h = grid.cell_height(world, payload.from)
  local to_h = grid.cell_height(world, to)
  local cell = { x = to.x, z = to.z, layer = to.layer or 0, jump = to.jump, climb = to.climb }
  if not (to.jump or to.climb) and math.abs(to_h - from_h) > 0.6 then
    entity:jump(vec.cell_centre(to, to_h), { height = math.max(0.6, to_h - from_h + 0.6), duration = 0.35, layer = cell.layer })
  else
    entity:move_to(nil, { path = { cell } })
  end
  task.wait_until(function() return not entity:valid() or not entity:moving() end, 3)
end

function Presenter:pushed(payload)
  local b = payload.battler
  local entity = b.entity
  if not entity or not entity:valid() then return end
  entity:move_to(vec.cell_centre(payload.to, grid.cell_height(self.world, payload.to)), { path = false, speed = 8 })
  task.wait_until(function() return not entity:valid() or not entity:moving() end, 1)
  sync(b)
end

function Presenter:facing(b)
  if b.entity and b.entity:valid() then b.entity:face(b.facing) end
end

function Presenter:action(action)
  self:banner(action.source.name, 0.9)
  local entity = action.user.entity
  if entity and entity:valid() and fe.app.sprite_clips(entity:sprite().animation).attack then entity:clip("attack") end
  task.wait(0.35)
end

function Presenter:results(payload)
  for _, r in ipairs(payload.results) do
    local entity = r.target.entity
    if r.missed then
      self:popup(r.target, i18n.t("fe.tactics.miss"), ui.theme.muted)
    elseif r.type == "damage" then
      self:popup(r.target, tostring(r.amount), r.critical and 0xffd060ff or 0xffffffff)
      if entity and entity:valid() then
        entity:sprite({ flash = { 1, 1, 1, 0.8 } })
        task.spawn(function()
          tween.run(0.25, function(k) if entity:valid() then entity:sprite({ flash = { 1, 1, 1, 0.8 * (1 - k) } }) end end)
        end)
      end
    elseif (r.type == "heal" or r.type == "revive") and (r.amount or 0) > 0 then
      self:popup(r.target, tostring(r.amount), ui.theme.good)
    end
  end
  task.wait(0.4)
end

function Presenter:knocked_out(b)
  local entity = b.entity
  if not entity or not entity:valid() then return end
  task.spawn(function()
    tween.run(0.4, function(k) if entity:valid() then entity:sprite({ tint = { 1, 1, 1, 1 - k } }) end end)
    if entity:valid() and not b:alive() then entity:sprite({ visible = false }) end
  end)
end

function Presenter:revived(b)
  if not b.entity or not b.entity:valid() then return end
  b.entity:sprite({ visible = true, tint = { 1, 1, 1, 1 } })
  sync(b)
end

function Presenter:battler_added(b)
  if b.cell and not b.entity then self:spawn(b) end
end

function Presenter:battler_removed(b)
  self:despawn(b)
end

function Presenter:follow_up(payload)
  self:banner(i18n.t("fe.tactics.follow_up", { name = payload.user:name() }), 0.8)
end

function Presenter:reaction(payload)
  self:banner(i18n.t("fe.tactics.counters", { name = payload.battler:name() }), 0.8)
end

function Presenter:reinforcements(payload)
  self:banner(i18n.t("fe.tactics.reinforcements"), 1.2)
  local first = payload.units[1]
  if first.entity then self.rig:follow(first.entity) end
  sound.play("reinforcements")
  task.wait(0.6)
end

function Presenter:destroyed(payload)
  self:banner(i18n.t("fe.tactics.destroyed", { name = payload.battler:name() }), 0.8)
end

--- Terrain changes play the reaction's `sound`, else "terrain_<element>".
function Presenter:terrain(payload)
  local reaction = payload.reaction
  local id = reaction.sound or (reaction.element and "terrain_" .. reaction.element)
  if id then sound.play(id) end
end

function Presenter:terrain_changed()
  self:update_overlays()
end

-- Whether `world` is still loaded (an unloaded handle raises on access).
local function loaded(world)
  if not world then return false end
  local ok, name = pcall(function() return world.name end)
  return ok and fe.world.get(name) == world
end

--- Tear the board down at once: remove the scene, overlays and cursor;
--- on the field world the unit sprites are removed, a battle map is
--- unloaded. Runs once (finish and abort both end here).
function Presenter:close()
  if self.closed then return end
  self.closed = true
  if self.scene then scene.remove(self.scene) end
  local world = self.battle.tactical and self.battle.tactical.world
  if loaded(world) then
    world:clear_overlay()
    world:cursor(nil)
    if self.options.map then
      if fe.world.get(settings.world_name) == world then fe.world.unload(settings.world_name) end
    else
      for _, b in ipairs(self.battle.battlers) do self:despawn(b) end
    end
  end
  for _, b in ipairs(self.battle.battlers) do b.entity = nil end
  self.units = {}
  local tactical = require("fe.battle.tactical")
  if tactical.active == self.battle then tactical.active = nil end
end

--- Victory/defeat banner, then tear down (see Presenter:close).
function Presenter:finish(payload)
  local key = payload.outcome == "victory" and "victory" or payload.outcome == "defeat" and "defeat" or "over"
  self:banner(i18n.t("fe.tactics." .. key), 2)
  if payload.outcome == "victory" and payload.rewards then
    task.wait(0.5)
  end
  task.wait(1.5)
  self:close()
end

--- The battle raised or was cancelled before it finished (fe.battle.start
--- calls this after Battle:abort, whose tactical `on_abort` reverted the
--- terrain): tear down at once and uncover a battle map's fade.
function Presenter:abort()
  self:close()
  if self.options.map then transition.clear() end
end

-------------------------------------------------------------------------------
-- Player input

-- Cursor control on the grid: arrows move relative to the camera; the mouse picks.
local function move_cursor(p, cursor)
  local dx, dz = 0, 0
  if fe.input.repeating("left") then dx = -1 elseif fe.input.repeating("right") then dx = 1 end
  if fe.input.repeating("up") then dz = -1 elseif fe.input.repeating("down") then dz = 1 end
  if dx ~= 0 or dz ~= 0 then
    local yaw = math.floor((p.rig:yaw() % 360) / 90 + 0.5) % 4
    for _ = 1, yaw do dx, dz = dz, -dx end
    local gw, gd = p.world:grid_size()
    cursor.x = tablex.clamp(cursor.x + dx, 0, gw - 1)
    cursor.z = tablex.clamp(cursor.z + dz, 0, gd - 1)
    ui.sound("move")
  end
  if fe.input.mouse_moved() then
    local cell = p.world:pick(fe.input.mouse())
    if cell then cursor.x, cursor.z, cursor.layer = cell.x, cell.z, cell.layer end
  end
  p.world:cursor(cursor)
  p.hover = grid.unit_at(p.battle, cursor) or grid.corpse_at(p.battle, cursor)
  return cursor
end

--- Pick a cell from `cells`; returns it, or nil on cancel. Pressing the
--- optional `finish` action returns nil, true.
function Presenter:pick_cell(cells, start, on_hover, finish)
  local cursor = { x = start.x, z = start.z, layer = start.layer or 0 }
  task.frame()
  while true do
    move_cursor(self, cursor)
    if on_hover then on_hover(cursor) end
    if fe.input.pressed("confirm") then
      local c = grid.contains_cell(cells, cursor)
      if c then ui.sound("confirm") return c end
      ui.sound("buzzer")
    elseif fe.input.pressed("cancel") then
      ui.sound("cancel")
      return nil
    elseif finish and fe.input.pressed(finish) then
      ui.sound("confirm")
      return nil, true
    end
    task.frame()
  end
end

local function choose_skill(battle, b)
  local entries = {}
  local attack = Battle.attack_skill()
  if attack then entries[#entries + 1] = { label = attack.name, value = attack } end
  for _, skill in ipairs(b:skills()) do
    if (not attack or skill.id ~= attack.id) and skill.scope ~= "field" then
      local cost = {}
      for resource, amount in pairs(skill.cost) do cost[#cost + 1] = amount .. resource:upper() end
      table.sort(cost)
      entries[#entries + 1] = { label = skill.name, right = table.concat(cost, " "), value = skill,
        enabled = battle:usable(b, skill) }
    end
  end
  local _, item = ui.choose(entries, { title = i18n.t("fe.tactics.command.act"), anchor = "bottom-left", width = 150,
    rows = 6, cancel = true })
  return item and item.value
end

-- Turn order if the active unit ended its turn now with this choice's CT
-- cost (shown with the cost in the HUD).
local function preview_order(p, battle, b, moved, acted)
  local scheduler = battle.scheduler
  if not scheduler.preview then return end
  p.ct_cost = rules.ct_cost(moved, acted)
  p.order = scheduler.preview(battle, 10, { battler = b, cost = p.ct_cost })
end

--- Player turn: Move / Act / Wait, with undoable movement. After acting
--- the unit chooses a facing (plan.choose_facing); Wait chooses it now.
function Presenter:command(b, battle)
  local world = self.world
  local plan = { move = {} }
  local origin = { x = b.cell.x, z = b.cell.z, layer = b.cell.layer }
  local origin_facing = b.facing
  local moved = false
  while true do
    preview_order(self, battle, b, moved, true)
    local options = {
      { label = i18n.t("fe.tactics.command.move"), value = "move", enabled = not moved },
      { label = i18n.t("fe.tactics.command.act"), value = "act" },
      { label = i18n.t("fe.tactics.command.wait"), value = "wait" },
    }
    if moved then options[#options + 1] = { label = i18n.t("fe.tactics.command.undo"), value = "undo" } end
    local _, choice = ui.choose(options, { anchor = "bottom-left", width = 90, rows = 4 })
    choice = choice and choice.value
    if choice == "move" then
      local cells = grid.reachable(battle, b)
      world:overlay("move", cells, { color = settings.colors.move, pulse = 1.5 })
      local dest = self:pick_cell(cells, b.cell, function(cursor)
        local path = grid.path_to(cells, cursor)
        table.insert(path, 1, b.cell)
        world:overlay_path("path", #path > 1 and path or {}, {color = settings.colors.path})
      end)
      world:clear_overlay("move")
      world:clear_overlay("path")
      if dest then
        grid.walk(battle, b, grid.path_to(cells, dest))
        moved = true
        plan.moved = true
      end
    elseif choice == "undo" then
      grid.place(battle, b, origin)
      b.facing = origin_facing
      sync(b)
      moved = false
      plan.moved = false
    elseif choice == "act" then
      local skill = choose_skill(battle, b)
      if skill then
        local cells = grid.range_cells(battle, b, skill)
        world:overlay("range", cells, { color = settings.colors.attack })
        local target = self:pick_cell(cells, cells[1] or b.cell, function(cursor)
          if grid.contains_cell(cells, cursor) then
            local units, area = grid.affected(battle, b, skill, cursor)
            world:overlay("area", area, { color = settings.colors.area, order = 1 })
            self.forecast = { skill = skill, units = units,
              results = #units > 0 and effects.resolve({ user = b, targets = units, source = skill, battle = battle, preview = true }) or {} }
          else
            world:clear_overlay("area")
            self.forecast = nil
          end
        end)
        world:clear_overlay("range")
        world:clear_overlay("area")
        self.forecast = nil
        if target then
          local units = grid.affected(battle, b, skill, target)
          plan.action = { kind = "skill", user = b, skill = skill.id, targets = units, cell = { x = target.x, z = target.z, layer = target.layer } }
          plan.choose_facing = true
          world:cursor(nil)
          return plan
        end
      end
    elseif choice == "wait" then
      preview_order(self, battle, b, moved, false)
      world:cursor(nil)
      plan.facing = self:choose_facing(b)
      return plan
    end
  end
end

--- Arrow keys pick a facing direction; Confirm ends the turn.
function Presenter:choose_facing(b)
  local directions = { { x = 0, z = -1 }, { x = 1, z = 0 }, { x = 0, z = 1 }, { x = -1, z = 0 } }
  local index = 1
  for i, d in ipairs(directions) do
    if b.facing and d.x == b.facing.x and d.z == b.facing.z then index = i end
  end
  self.choosing_facing = true
  task.frame()
  while not fe.input.pressed("confirm") do
    for i, action in ipairs({ "up", "right", "down", "left" }) do
      if fe.input.pressed(action) then index = i end
    end
    b.entity:face(directions[index])
    task.frame()
  end
  self.choosing_facing = false
  return directions[index]
end

return Presenter

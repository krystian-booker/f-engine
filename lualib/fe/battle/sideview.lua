--- Side-view battle presentation (party on one side, enemies on the other).
---
--- The stage is a map (maps/<stage>) loaded as a second world, so the field
--- stays loaded underneath. Stage points "party1".."party8" and
--- "enemy1".."enemy16" place battlers (settings.formation otherwise).
--- Actions play timelines from data/timelines.json (built-in defaults
--- below); a timeline's "effect" command marks when results appear.
--- Timeline times and every pause (settings.waits) are divided by the
--- battle speed (Battle.speed: Battle.settings.speed or the player's
--- battle_speed setting).
---
--- Customise with `sideview.settings`, `sideview.commands` (new battle
--- commands), `sideview.modifier` (e.g. a boost gauge), `sideview.overlays`
--- (extra HUD drawing) and by replacing `sideview.draw_hud`. The module is
--- split in three: this file (scene, sprites and events),
--- fe.battle.sideview_hud (the HUD and the victory window) and
--- fe.battle.sideview_commands (the command menu, target picking); the
--- shared popup, banner and fade helpers are fe.battle.presenter_common.
---
--- The HUD's turn bar shows the scheduler's preview: round_order's remaining turns this round plus the predicted
--- next round (the acting battler highlighted); it is recomputed after
--- each battle event, not every frame. Battlers added mid-battle
--- (`battle:add`, `battle:summon`) get a sprite at their formation slot;
--- swapped-in back-row battlers (`battle:swap`) take the leaver's place, and
--- a party member with a living back-row partner gets a Swap command.
--- Battlers that already left (a dismissed summon) have no sprite: their
--- late popups, fades and clips are skipped. Cancelled (nullified) results
--- show as such. Item commands use the battle's inventory
--- (Battle:inventory). Text comes from `fe.battle.*` i18n keys;
--- `settings.command_names[name]` overrides a command's label.
local Battle = require("fe.battle.core")
local camera = require("fe.camera")
local class = require("fe.class")
local common = require("fe.battle.presenter_common")
local commands = require("fe.battle.sideview_commands")
local hud = require("fe.battle.sideview_hud")
local i18n = require("fe.i18n")
local scene = require("fe.scene")
local sound = require("fe.sound")
local stats = require("fe.rpg.stats")
local task = require("fe.task")
local tablex = require("fe.tablex")
local timeline = require("fe.timeline")
local transition = require("fe.transition")
local tween = require("fe.tween")
local ui = require("fe.ui")

local sideview = {
  settings = {
    world_name = "battle",
    camera = { pitch = 24, yaw = 0, distance = 15, fov = 32 },
    victory_music = "victory",
    transition = "swirl",
    --- Labels by command name, overriding the fe.battle.command.* texts.
    command_names = {},
    --- Default positions when the stage has no partyN / enemyN points:
    --- the party in a column stepping back per slot, the enemies in rows.
    formation = {
      party = { x = 11, step = 0.4, z = 7.8, spacing = 1.3 },
      enemy = { x = 4.5, column = 1.4, rows = 4, z = 7.8, spacing = 1.35 },
    },
    --- Pauses in seconds (divided by the battle speed).
    waits = { results = 0.15, state_tick = 0.4, defend = 0.3, flee = 0.4, escape_failed = 0.6, assist = 0.3,
      opening = 0.8, level_up = 0.8, defeat = 2, finish = 0.4,
      timeline = 0.2 }, -- a timeline `wait` step without a duration
  },
  --- Extra commands: commands[name] = {label = "Steal", run = function(presenter, battler, battle) -> action|nil}
  commands = {},
  --- Optional command modifier (a boost/charge gauge): {label (text or
  --- function returning it), max(battler) -> integer, apply(action, value)}
  modifier = nil,
  --- Extra HUD layers: overlays[name] = function(presenter), drawn after draw_hud.
  overlays = {},
  timelines = {
    attack = {
      { time = 0, command = "step", distance = 1.0 },
      { time = 0.12, command = "anim", clip = "attack" },
      { time = 0.3, command = "effect" },
      { time = 0.3, command = "sound", id = "hit" },
      { time = 0.65, command = "return" },
    },
    skill = {
      { time = 0, command = "anim", clip = "cast" },
      { time = 0.15, command = "flash", color = { 0.5, 0.7, 1, 0.35 } },
      { time = 0.45, command = "effect" },
      { time = 0.45, command = "particles", color = { 1.4, 1.1, 0.6, 1 } },
      { time = 0.8, command = "return" },
    },
    item = {
      { time = 0, command = "anim", clip = "item" },
      { time = 0.3, command = "effect" },
      { time = 0.3, command = "particles", color = { 0.6, 1.6, 0.8, 1 } },
      { time = 0.6, command = "return" },
    },
  },
}

i18n.defaults({ fe = { battle = {
  command = { attack = "Attack", skill = "Skills", item = "Items", defend = "Defend", flee = "Flee", swap = "Swap" },
  swap_with = "Swap: {name}", defends = "{name} defends", escape = "Escape!",
  escape_failed = "Couldn't escape!", fallen = "The party has fallen...", next = "Next",
  assists = "{name} lends a hand!", leaves = "{name} leaves",
} } })

--- Label of a battle command: `commands[name].label`, settings.command_names,
--- the fe.battle.command.<name> text, else the skill type's name.
function sideview.command_label(name, user)
  local custom = sideview.commands[name]
  if custom and custom.label then return custom.label end
  if sideview.settings.command_names[name] then return sideview.settings.command_names[name] end
  local key = "fe.battle.command." .. name
  local text = i18n.t(key)
  if text ~= key then return text end
  local types = user and user:skills(name) or {}
  return #types > 0 and types[1].type:gsub("^%l", string.upper) or name
end

local function formation(stage_points, side, slot, count)
  local name = (side == "party" and "party" or "enemy") .. slot
  local point = stage_points[name]
  if point then return point.position end
  local f = sideview.settings.formation
  if side == "party" then
    return { x = f.party.x + slot * f.party.step, z = f.party.z - (slot - (count + 1) / 2) * f.party.spacing }
  end
  local column = math.floor((slot - 1) / f.enemy.rows)
  local row = (slot - 1) % f.enemy.rows
  return { x = f.enemy.x - column * f.enemy.column, z = f.enemy.z - (row - (f.enemy.rows - 1) / 2) * f.enemy.spacing }
end

---@class SideviewPresenter
local Presenter = class("SideviewPresenter")

function Presenter:init(battle, options)
  local timelines = tablex.copy(sideview.timelines)
  if fe.fs.exists("data/timelines.json") then
    for name, entries in pairs(fe.json.read("data/timelines.json")) do timelines[name] = entries end
  end
  self.battle = battle
  self.options = options
  self.timelines = timelines
  self.popups = {}
  self.banner = nil
  self.shown = {}
  self.stamp = 0 -- bumped by every battle event; the HUD's turn preview is cached against it
  battle.events:connect(function() self.stamp = self.stamp + 1 end)
end

function sideview.new(battle, options)
  return Presenter(battle, options)
end

-------------------------------------------------------------------------------
-- Scene

local BattleScene = { name = "battle", opaque = true }
BattleScene.__index = BattleScene

function BattleScene:update(dt)
  local p = self.presenter
  if p.rig then p.rig:update(dt) end
  common.age_popups(p, dt)
  for b, shown in pairs(p.shown) do
    for _, def in ipairs(stats.resource_list) do
      shown[def.id] = tween.approach(shown[def.id] or b:resource(def.id), b:resource(def.id), 10, dt)
    end
  end
end

function BattleScene:draw()
  local p = self.presenter
  p.world:draw()
  sideview.draw_hud(p)
  for _, name in ipairs(tablex.keys(sideview.overlays)) do sideview.overlays[name](p) end
end

-------------------------------------------------------------------------------
-- Presenter events (called by the battle core; may wait)

function Presenter:spawn(b, slot, count)
  local side = b:is_party() and "party" or "enemy"
  slot = b.position and b.position.slot or slot
  local home = b.position and b.position.x and b.position or formation(self.world:points(), side, slot, count)
  b.entity = self.world:spawn({
    position = { x = home.x, z = home.z }, facing = { x = b:is_party() and -1 or 1, z = 0 },
    sprite = { animation = b:sprite(), clip = b:alive() and "idle" or "ko", scale = b.def and b.def.scale or 1,
      visible = b:alive() or b:is_party() },
    mover = { speed = 7, collide = false },
  })
  b.home = b.entity.position
  self.shown[b] = {}
end

function Presenter:start(battle)
  self.field_music = fe.audio.current_music()
  transition.out(sideview.settings.transition, 0.5)
  self.world = fe.world.load(sideview.settings.world_name, self.options.stage)
  local counts = { party = #battle:side("party", true), enemy = #battle:side("enemy", true) }
  local slots = { party = 0, enemy = 0 }
  for _, b in ipairs(battle.battlers) do
    local side = b:is_party() and "party" or "enemy"
    slots[side] = slots[side] + 1
    self:spawn(b, slots[side], counts[side])
  end
  local cam = self.world:camera()
  local settings = sideview.settings.camera
  self.rig = camera.rig(self.world, settings)
  local centre = self.world:points().centre
  self.rig:look_at(centre and centre.position or { x = 8, y = 0, z = 7.5 })
  self.rig:snap()
  cam.yaw = settings.yaw
  self.scene = setmetatable({ presenter = self }, BattleScene)
  scene.push(self.scene)
  if self.options.music then sound.music(self.options.music) end
  transition.into(sideview.settings.transition, 0.5)
end

function Presenter:show_banner(text, duration)
  common.banner(self, text, duration or 0.9, "banner")
end

--- Wait `seconds` of battle time (scaled by Battle.speed).
function Presenter:wait(seconds)
  common.wait(seconds)
end

function Presenter:popup(b, text, color)
  common.popup(self, b, text, color)
end

-- The battler's sprite while it still exists.
local sprite_of = common.sprite_of

-- Timeline commands for side-view actions.
local handlers = {}

function handlers.step(ctx, params)
  local user = ctx.action.user
  local entity = sprite_of(user)
  if not entity then return end
  local direction = user:is_party() and -1 or 1
  local home = user.home
  entity:move_to({ x = home.x + direction * (params.distance or 0.8), z = home.z }, { path = false })
  task.wait_until(function() return not entity:valid() or not entity:moving() end, 1)
end

function handlers.anim(ctx, params)
  local entity = sprite_of(ctx.action.user)
  common.play_clip(entity, params.clip)
end

function handlers.effect(ctx)
  ctx.reached_effect = true
  task.wait_until(function() return ctx.results ~= nil end, 10)
end

handlers["return"] = function(ctx)
  local user = ctx.action.user
  local entity = sprite_of(user)
  if not entity then return end
  entity:clip("idle")
  entity:move_to(user.home, { path = false })
  task.wait_until(function() return not entity:valid() or not entity:moving() end, 1)
end

function handlers.sound(_, params)
  sound.play(params.id or params.file)
end

function handlers.flash(_, params)
  transition.flash(params.color, params.duration)
end

function handlers.shake(ctx, params)
  ctx.presenter.rig:shake(params.amplitude or 0.12, params.duration or 0.3)
end

function handlers.wait(_, params)
  common.wait(params.duration or sideview.settings.waits.timeline)
end

function handlers.particles(ctx, params)
  for _, target in ipairs(ctx.action.targets or {}) do
    local entity = sprite_of(target)
    local p = entity and entity.position
    if p then ctx.presenter.world:burst({ x = p.x, y = p.y + 0.7, z = p.z }, {
      sprite = params.sprite or "@fe/assets/ui/spark.anim.json", lifetime = 0.6, size = 0.2, size_end = 0.05,
      velocity = { 0, 1, 0 }, velocity_jitter = { 1.2, 1, 1.2 }, color = params.color or { 1.5, 1.2, 0.6, 1 },
      color_end = { 1, 1, 1, 0 }, drag = 2,
    }, params.count or 16) end
  end
end

--- Register a timeline command usable from data/timelines.json.
function sideview.timeline_command(name, fn)
  handlers[name] = fn
end

-- A timeline's entries with their times divided by the battle speed.
local function paced(entries)
  local speed = Battle.speed()
  if speed == 1 then return entries end
  return tablex.map(entries, function(entry)
    local copy = tablex.copy(entry)
    copy.time = (entry.time or 0) / speed
    return copy
  end)
end

function Presenter:opening(payload)
  self:show_banner(i18n.t("fe.battle." .. payload.kind), 1.2)
  common.wait(sideview.settings.waits.opening)
end

function Presenter:action(action)
  local source = action.source
  self:show_banner(source.name, 1.0)
  local name = source.timeline or (action.kind == "item" and "item")
    or (source.id == Battle.settings.attack_skill and "attack") or "skill"
  local entries = self.timelines[name] or self.timelines.skill
  local ctx = { presenter = self, action = action }
  self.current = ctx
  ctx.task = task.spawn(timeline.play, paced(entries), handlers, ctx)
  task.wait_until(function() return ctx.reached_effect or ctx.task.status ~= "running" end, 10)
end

function Presenter:results(payload)
  local ctx = self.current or { presenter = self, action = payload.action }
  common.result_popups(self, payload.results, { on_damage = function(r)
    if r.critical then self.rig:shake(0.12, 0.3) end
  end })
  ctx.results = payload.results
  if ctx.task then task.join(ctx.task) end
  self.current = nil
  common.wait(sideview.settings.waits.results)
end

function Presenter:knocked_out(b)
  local entity = sprite_of(b)
  if not entity then return end
  if b:is_party() then
    entity:clip("ko")
  else
    sound.play("defeat_enemy")
    common.fade_out(entity, b, { 1, 0.4, 0.4 })
  end
end

function Presenter:revived(b)
  local entity = sprite_of(b)
  if not entity then return end
  common.restore(entity)
  entity:clip("idle")
end

--- Reinforcements, summons and swapped-in battlers: a sprite at the
--- battler's formation slot.
function Presenter:battler_added(b)
  local side = b:is_party() and "party" or "enemy"
  local list = self.battle:side(side, true)
  self:spawn(b, #list, #list)
end

function Presenter:dismissed(b)
  self:popup(b, i18n.t("fe.battle.leaves", { name = b:name() }), ui.theme.muted)
end

function Presenter:reaction(payload)
  self:show_banner(i18n.t("fe.battle.counters", { name = payload.battler:name() }), 0.8)
end

function Presenter:assist(payload)
  self:show_banner(i18n.t("fe.battle.assists", { name = payload.battler:name() }), 0.9)
  common.wait(sideview.settings.waits.assist)
end

function Presenter:battler_removed(b)
  local entity = sprite_of(b)
  if entity then entity:destroy() end
  self.shown[b] = nil
end

function Presenter:state_tick(payload)
  for _, r in ipairs(payload.results) do
    if r.amount and r.amount ~= 0 then
      self:popup(payload.battler, tostring(math.abs(r.amount)),
        r.type == "damage" and common.settings.colors.state_tick or ui.theme.good)
    end
  end
  common.wait(sideview.settings.waits.state_tick)
end

--- Experience gained for an action (Battle.settings.exp_per_action): a
--- popup, and a banner per level gained.
function Presenter:exp(payload)
  self:popup(payload.battler, i18n.t("fe.battle.exp_gain", { exp = payload.amount }), ui.theme.good)
  for _, line in ipairs(common.level_up_lines({ { battler = payload.battler, gains = payload.gains } })) do
    self:show_banner(line, sideview.settings.waits.level_up)
    common.wait(sideview.settings.waits.level_up)
  end
end

function Presenter:defend(action)
  self:show_banner(i18n.t("fe.battle.defends", { name = action.user:name() }))
  common.wait(sideview.settings.waits.defend)
end
function Presenter:flee() self:show_banner(i18n.t("fe.battle.escape")) common.wait(sideview.settings.waits.flee) end
function Presenter:escape_failed()
  self:show_banner(i18n.t("fe.battle.escape_failed"))
  common.wait(sideview.settings.waits.escape_failed)
end

function Presenter:finish(payload)
  local outcome = payload.outcome
  local waits = sideview.settings.waits
  if outcome == "victory" then
    for _, b in ipairs(self.battle:side("party")) do
      local entity = sprite_of(b)
      common.play_clip(entity, "victory")
    end
    sound.music(sideview.settings.victory_music)
    sideview.show_rewards(self, payload.rewards)
  elseif outcome == "defeat" then
    self:show_banner(i18n.t("fe.battle.fallen"), waits.defeat)
    common.wait(waits.defeat)
  else
    common.wait(waits.finish)
  end
  transition.out("fade", 0.4)
  self:close()
  transition.into("fade", 0.4)
end

--- Remove the battle scene and stage world and bring the field music back,
--- at once. Runs once (finish and abort both end here).
function Presenter:close()
  if not common.once(self, "closed") then return end
  if self.scene then scene.remove(self.scene) end
  local name = sideview.settings.world_name
  if self.world and fe.world.get(name) == self.world then fe.world.unload(name) end
  if self.field_music and self.field_music ~= "" then
    fe.audio.music(self.field_music)
  end
end

--- The battle raised or was cancelled before it finished: tear down at once
--- and uncover the screen.
function Presenter:abort()
  self:close()
  transition.clear()
end

--- Command input for a party battler (fe.battle.sideview_commands);
--- returns an action or nil. Choosing Swap trades places with the back-row
--- partner, who then chooses.
function Presenter:command(user, battle)
  return commands.command(sideview, self, user, battle)
end

--- Victory window: experience, money, items, level ups. Waits for Confirm.
function sideview.show_rewards(presenter, rewards)
  return hud.show_rewards(sideview, presenter, rewards)
end

--- Draw the battle HUD (fe.battle.sideview_hud); replace for a different look.
function sideview.draw_hud(p)
  return hud.draw(sideview, p)
end

sideview.Presenter = Presenter
sideview.handlers = handlers
return sideview

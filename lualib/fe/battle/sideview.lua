--- Side-view battle presentation (party on one side, enemies on the other).
---
--- The stage is a map (maps/<stage>) loaded as a second world, so the field
--- stays loaded underneath. Stage points "party1".."party8" and
--- "enemy1".."enemy16" place battlers (defaults are used otherwise).
--- Actions play timelines from data/timelines.json (built-in defaults
--- below); a timeline's "effect" command marks when results appear.
---
--- Customise with `sideview.settings`, `sideview.commands` (new battle
--- commands), `sideview.modifier` (e.g. a boost gauge) and by replacing
--- `sideview.draw_hud`.
local Battle = require("fe.battle.core")
local camera = require("fe.camera")
local data = require("fe.data")
local inventory = require("fe.rpg.inventory")
local scene = require("fe.scene")
local sound = require("fe.sound")
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
    command_names = { attack = "Attack", skill = "Skills", item = "Items", defend = "Defend", flee = "Flee" },
  },
  --- Extra commands: commands[name] = {label = "Steal", run = function(presenter, battler, battle) -> action|nil}
  commands = {},
  --- Optional command modifier (a boost/charge gauge): {label, max(battler) -> integer, apply(action, value)}
  modifier = nil,
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

local function formation(stage_points, side, slot, count)
  local name = (side == "party" and "party" or "enemy") .. slot
  local point = stage_points[name]
  if point then return point.position end
  if side == "party" then
    return { x = 11 + slot * 0.4, z = 7.8 - (slot - (count + 1) / 2) * 1.3 }
  end
  local column = math.floor((slot - 1) / 4)
  local row = (slot - 1) % 4
  return { x = 4.5 - column * 1.4, z = 7.8 - (row - 1.5) * 1.35 }
end

local Presenter = {}
Presenter.__index = Presenter

function sideview.new(battle, options)
  local timelines = tablex.copy(sideview.timelines)
  if fe.fs.exists("data/timelines.json") then
    for name, entries in pairs(fe.json.read("data/timelines.json")) do timelines[name] = entries end
  end
  return setmetatable({ battle = battle, options = options, timelines = timelines, popups = {}, banner = nil,
    shown = {} }, Presenter)
end

-------------------------------------------------------------------------------
-- Scene

local BattleScene = { name = "battle", opaque = true }
BattleScene.__index = BattleScene

function BattleScene:update(dt)
  local p = self.presenter
  if p.rig then p.rig:update(dt) end
  for _, popup in ipairs(p.popups) do popup.age = popup.age + dt end
  p.popups = tablex.filter(p.popups, function(popup) return popup.age < 1.2 end)
  for b, shown in pairs(p.shown) do
    for _, def in ipairs(require("fe.rpg.stats").resource_list) do
      shown[def.id] = tween.approach(shown[def.id] or b:resource(def.id), b:resource(def.id), 10, dt)
    end
  end
end

function BattleScene:draw()
  local p = self.presenter
  p.world:draw()
  sideview.draw_hud(p)
end

-------------------------------------------------------------------------------
-- Presenter events (called by the battle core; may wait)

function Presenter:start(battle)
  self.field_music = fe.audio.current_music()
  transition.out(sideview.settings.transition, 0.5)
  self.world = fe.world.load(sideview.settings.world_name, self.options.stage)
  local points = self.world:points()
  local counts = { party = #battle:side("party", true), enemy = #battle:side("enemy", true) }
  local slots = { party = 0, enemy = 0 }
  for _, b in ipairs(battle.battlers) do
    local side = b:is_party() and "party" or "enemy"
    slots[side] = slots[side] + 1
    local slot = b.position and b.position.slot or slots[side]
    local home = b.position and b.position.x and b.position or formation(points, side, slot, counts[side])
    b.entity = self.world:spawn({
      position = { x = home.x, z = home.z }, facing = { x = b:is_party() and -1 or 1, z = 0 },
      sprite = { animation = b:sprite(), clip = b:alive() and "idle" or "ko", scale = b.def and b.def.scale or 1 },
      mover = { speed = 7, collide = false },
    })
    b.home = b.entity.position
    self.shown[b] = {}
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
  self.banner = text
  task.spawn(function()
    task.wait(duration or 0.9)
    if self.banner == text then self.banner = nil end
  end)
end

local function entity_of(b) return b.entity end

function Presenter:popup(b, text, color)
  local x, y = b.entity:screen_position(1.4)
  self.popups[#self.popups + 1] = { x = x, y = y, text = text, color = color or ui.theme.text, age = 0 }
end

-- Timeline commands for side-view actions.
local handlers = {}

function handlers.step(ctx, params)
  local user = ctx.action.user
  local direction = user:is_party() and -1 or 1
  local home = user.home
  user.entity:move_to({ x = home.x + direction * (params.distance or 0.8), z = home.z }, { path = false })
  task.wait_until(function() return not user.entity:moving() end, 1)
end

function handlers.anim(ctx, params)
  local user = ctx.action.user
  local clips = fe.app.sprite_clips(user.entity:sprite().animation)
  if clips[params.clip] then user.entity:clip(params.clip) end
end

function handlers.effect(ctx)
  ctx.reached_effect = true
  task.wait_until(function() return ctx.results ~= nil end, 10)
end

handlers["return"] = function(ctx)
  local user = ctx.action.user
  user.entity:clip("idle")
  user.entity:move_to(user.home, { path = false })
  task.wait_until(function() return not user.entity:moving() end, 1)
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
  task.wait(params.duration or 0.2)
end

function handlers.particles(ctx, params)
  for _, target in ipairs(ctx.action.targets or {}) do
    local p = target.entity.position
    ctx.presenter.world:burst({ x = p.x, y = p.y + 0.7, z = p.z }, {
      sprite = params.sprite or "@fe/assets/ui/spark.anim.json", lifetime = 0.6, size = 0.2, size_end = 0.05,
      velocity = { 0, 1, 0 }, velocity_jitter = { 1.2, 1, 1.2 }, color = params.color or { 1.5, 1.2, 0.6, 1 },
      color_end = { 1, 1, 1, 0 }, drag = 2,
    }, params.count or 16)
  end
end

--- Register a timeline command usable from data/timelines.json.
function sideview.timeline_command(name, fn)
  handlers[name] = fn
end

function Presenter:action(action)
  local source = action.source
  self:show_banner(source.name, 1.0)
  local name = source.timeline or (action.kind == "item" and "item") or (source.id == "attack" and "attack") or "skill"
  local entries = self.timelines[name] or self.timelines.skill
  local ctx = { presenter = self, action = action }
  self.current = ctx
  ctx.task = task.spawn(timeline.play, entries, handlers, ctx)
  task.wait_until(function() return ctx.reached_effect or ctx.task.status ~= "running" end, 10)
end

function Presenter:results(payload)
  local ctx = self.current or { presenter = self, action = payload.action }
  for _, r in ipairs(payload.results) do
    local b = r.target
    if r.missed then
      self:popup(b, "Miss", ui.theme.muted)
    elseif r.type == "damage" then
      local color = r.critical and 0xffd060ff or 0xffffffff
      if r.affinity and r.affinity > 1 then color = 0xff9070ff end
      self:popup(b, tostring(r.amount) .. (r.critical and "!" or ""), color)
      if r.amount > 0 then
        b.entity:sprite({ flash = { 1, 1, 1, 0.8 } })
        task.spawn(function()
          tween.run(0.25, function(t) b.entity:sprite({ flash = { 1, 1, 1, 0.8 * (1 - t) } }) end)
        end)
        if r.critical then self.rig:shake(0.12, 0.3) end
      end
    elseif r.type == "heal" or r.type == "resource" or r.type == "revive" then
      if (r.amount or 0) ~= 0 then self:popup(b, tostring(r.amount), ui.theme.good) end
      if r.revived then b.entity:clip("idle") end
    elseif r.type == "add_state" and r.applied then
      self:popup(b, data.get("states", r.state).name, ui.theme.heading)
    end
  end
  ctx.results = payload.results
  if ctx.task then task.join(ctx.task) end
  self.current = nil
  task.wait(0.15)
end

function Presenter:knocked_out(b)
  if b:is_party() then
    b.entity:clip("ko")
  else
    sound.play("defeat_enemy")
    task.spawn(function()
      tween.run(0.4, function(t) b.entity:sprite({ tint = { 1, 0.4, 0.4, 1 - t } }) end)
      b.entity:sprite({ visible = false })
    end)
  end
end

function Presenter:state_tick(payload)
  for _, r in ipairs(payload.results) do
    if r.amount and r.amount ~= 0 then
      self:popup(payload.battler, tostring(math.abs(r.amount)), r.type == "damage" and 0xc890ffff or ui.theme.good)
    end
  end
  task.wait(0.4)
end

function Presenter:defend(action) self:show_banner(action.user:name() .. " defends") task.wait(0.3) end
function Presenter:flee() self:show_banner("Escape!") task.wait(0.4) end
function Presenter:escape_failed() self:show_banner("Couldn't escape!") task.wait(0.6) end

function Presenter:finish(payload)
  local outcome = payload.outcome
  if outcome == "victory" then
    for _, b in ipairs(self.battle:side("party")) do
      local clips = fe.app.sprite_clips(b.entity:sprite().animation)
      if clips.victory then b.entity:clip("victory") end
    end
    sound.music(sideview.settings.victory_music)
    sideview.show_rewards(self, payload.rewards)
  elseif outcome == "defeat" then
    self:show_banner("The party has fallen...", 2)
    task.wait(2)
  else
    task.wait(0.4)
  end
  transition.out("fade", 0.4)
  scene.remove(self.scene)
  fe.world.unload(sideview.settings.world_name)
  if self.field_music and self.field_music ~= "" then
    fe.audio.music(self.field_music)
  end
  transition.into("fade", 0.4)
end

--- Victory window: experience, money, items, level ups. Waits for Confirm.
function sideview.show_rewards(presenter, rewards)
  local lines = { "{color:heading}Victory!{color:reset}" }
  lines[#lines + 1] = string.format("%d EXP   %d %s", rewards.exp, rewards.money, inventory.settings.currency)
  if rewards.jp and rewards.jp > 0 then lines[#lines + 1] = rewards.jp .. " JP" end
  for _, id in ipairs(rewards.items) do
    lines[#lines + 1] = "Found " .. data.get("items", id).name
  end
  for _, entry in ipairs(rewards.levels or {}) do
    for _, gain in ipairs(entry.gains) do
      lines[#lines + 1] = string.format("{color:good}%s reached level %d!{color:reset}", entry.actor:name(), gain.level)
      for _, skill in ipairs(gain.skills) do
        lines[#lines + 1] = "  Learned " .. data.get("skills", skill).name
      end
    end
  end
  presenter.rewards_text = table.concat(lines, "\n")
  task.frame()
  task.wait_until(function() return fe.input.pressed("confirm") end)
  presenter.rewards_text = nil
end

-------------------------------------------------------------------------------
-- Player input

local function choose_targets(presenter, battle, user, kind)
  local candidates = battle:targets_for(user, kind)
  if #candidates == 0 then return nil end
  if kind:find("^all") or kind == "everyone" or kind == "self" then
    presenter.targeting = candidates
    task.frame()
    while true do
      if fe.input.pressed("confirm") then presenter.targeting = nil return candidates end
      if fe.input.pressed("cancel") then presenter.targeting = nil return nil end
      task.frame()
    end
  end
  local index = 1
  -- Start on the side the skill is meant for.
  presenter.targeting = { candidates[index] }
  task.frame()
  while true do
    local step = (fe.input.repeating("down") or fe.input.repeating("right")) and 1
      or (fe.input.repeating("up") or fe.input.repeating("left")) and -1 or 0
    if step ~= 0 then
      index = (index - 1 + step) % #candidates + 1
      presenter.targeting = { candidates[index] }
      ui.sound("move")
    end
    if fe.input.pressed("confirm") then
      ui.sound("confirm")
      presenter.targeting = nil
      return { candidates[index] }
    end
    if fe.input.pressed("cancel") then
      ui.sound("cancel")
      presenter.targeting = nil
      return nil
    end
    task.frame()
  end
end

local function pick_from(presenter, title, entries)
  local cw, ch = fe.canvas.size()
  local index, item = ui.choose(entries, { title = title, x = 8, y = ch - 112, width = math.floor(cw * 0.55),
    rows = 6, columns = 2, cancel = true })
  return index and item
end

local function skill_entries(user, skill_type)
  return tablex.map(user:skills(skill_type), function(skill)
    local cost = {}
    for resource, amount in pairs(skill.cost) do cost[#cost + 1] = amount .. " " .. resource:upper() end
    return { label = skill.name, right = table.concat(cost, " "), value = skill,
      enabled = user:can_pay(skill.cost) and skill.scope ~= "field" }
  end)
end

local function item_entries()
  return tablex.map(inventory.list(function(item) return item.use and item.use.scope ~= "field" end), function(entry)
    return { label = entry.item.name, right = "x" .. entry.count, value = entry.item, icon = entry.item.icon }
  end)
end

--- Command input for a party battler; returns an action or nil.
function Presenter:command(user, battle)
  self.active = user
  self.modifier_value = 0
  local menu
  -- The battle can end while this player is choosing (active time).
  local forget = task.current():finally(function()
    if menu then scene.remove(menu) end
    self.active, self.targeting = nil, nil
  end)
  while true do
    local commands = user:commands()
    local entries = tablex.map(commands, function(name)
      local custom = sideview.commands[name]
      local label = custom and custom.label or sideview.settings.command_names[name]
      if not label then
        local types = user:skills(name)
        label = #types > 0 and types[1].type:gsub("^%l", string.upper) or name
      end
      return { label = label, value = name, enabled = name ~= "flee" or battle.escapable }
    end)
    local cw, ch = fe.canvas.size()
    local done, choice = false, nil
    menu = ui.Menu({ items = entries, x = 8, y = ch - 98, width = 92, rows = 6,
      on_confirm = function(item) choice, done = item.value, true end })
    scene.push(menu)
    while not done do
      if sideview.modifier then
        local max = sideview.modifier.max(user)
        if fe.input.pressed("page_right") then self.modifier_value = math.min(max, self.modifier_value + 1) end
        if fe.input.pressed("page_left") then self.modifier_value = math.max(0, self.modifier_value - 1) end
      end
      task.frame()
    end
    scene.remove(menu)
    local action
    if choice == "attack" then
      local targets = choose_targets(self, battle, user, "enemy")
      if targets then action = { kind = "skill", user = user, skill = "attack", targets = targets } end
    elseif choice == "defend" then
      action = { kind = "defend", user = user }
    elseif choice == "flee" then
      action = { kind = "flee", user = user }
    elseif choice == "item" then
      local item = pick_from(self, "Items", item_entries())
      if item then
        local targets = choose_targets(self, battle, user, item.value.use.target)
        if targets then action = { kind = "item", user = user, item = item.value.id, targets = targets } end
      end
    elseif sideview.commands[choice] then
      action = sideview.commands[choice].run(self, user, battle)
    else
      local skill_type = choice ~= "skill" and choice or nil
      local picked = pick_from(self, sideview.settings.command_names[choice] or "Skills", skill_entries(user, skill_type))
      if picked then
        local targets = choose_targets(self, battle, user, picked.value.target)
        if targets then action = { kind = "skill", user = user, skill = picked.value.id, targets = targets } end
      end
    end
    if action then
      if sideview.modifier and self.modifier_value > 0 then
        sideview.modifier.apply(action, self.modifier_value)
      end
      self.active = nil
      forget()
      return action
    end
  end
end

-------------------------------------------------------------------------------
-- HUD

--- Draw the battle HUD; replace for a different look.
function sideview.draw_hud(p)
  local battle = p.battle
  local cw, ch = fe.canvas.size()
  local life = require("fe.rpg.combatant").life
  -- Party status (bottom right)
  local members = battle:side("party", true)
  local h = #members * 16 + 12
  local x, y, w = math.floor(cw * 0.42), ch - h - 6, math.floor(cw * 0.58) - 8
  ui.window(x, y, w, h)
  for i, b in ipairs(members) do
    local row = y + 6 + (i - 1) * 16
    local shown = p.shown[b] or {}
    local color = b == p.active and ui.theme.highlight or (b:alive() and ui.theme.text or ui.theme.bad)
    ui.text(b:name(), x + 8, row, { color = color })
    local hp = math.floor((shown[life] or b:resource(life)) + 0.5)
    ui.text(string.format("%d/%d", hp, b:max_resource(life)), x + 66, row, { width = 70, align = "right" })
    ui.gauge(x + 66, row + 9, 70, 3, hp, b:max_resource(life), ui.theme.gauges.hp)
    -- The first resource after life (MP, SP...) is shown as a number.
    for _, def in ipairs(require("fe.rpg.stats").resource_list) do
      if def.id ~= life and not def.battle_only and b:max_resource(def.id) > 0 then
        ui.text(tostring(b:resource(def.id)), x + 140, row, { width = 30, align = "right", color = ui.theme.gauges[def.id] or ui.theme.gauges.mp })
        break
      end
    end
    local pips = ""
    for _, def in ipairs(require("fe.rpg.stats").resource_list) do
      if def.battle_only then
        local n = b:resource(def.id)
        pips = pips .. def.name .. " " .. string.rep("◆", n) .. string.rep("·", math.max(0, b:max_resource(def.id) - n)) .. " "
      end
    end
    if pips ~= "" then ui.text(pips, x + 176, row, { color = ui.theme.heading }) end
    if battle.scheduler.gauge then
      local fill = battle.scheduler.gauge(b)
      ui.gauge(x + w - 52, row + 3, 44, 4, fill, 1, fill >= 1 and ui.theme.highlight or ui.theme.gauges.mp)
    end
  end
  -- Enemy names (bottom left) when no menu is open
  if not p.active then
    local enemies = battle:side("enemy")
    local eh = math.max(1, #enemies) * 11 + 12
    ui.window(8, ch - eh - 6, math.floor(cw * 0.42) - 12, eh)
    for i, b in ipairs(enemies) do
      ui.text(b:name(), 16, ch - eh + (i - 1) * 11)
    end
  end
  -- Modifier gauge (boost)
  if sideview.modifier and p.active then
    local max = sideview.modifier.max(p.active)
    ui.text(sideview.modifier.label .. " " .. string.rep("◆", p.modifier_value or 0) .. string.rep("·", max - (p.modifier_value or 0)),
      104, ch - 112, { color = ui.theme.heading })
  end
  -- Turn order (if the scheduler can preview)
  if battle.scheduler.preview then
    local order = battle.scheduler.preview(battle, 8)
    local bx = 8
    for i, b in ipairs(order) do
      if i > 8 then break end
      local label = b:name():sub(1, 3)
      ui.window(bx, 6, 26, 14, { alpha = b:is_party() and 1 or 0.8 })
      ui.text(label, bx + 4, 9, { color = b:is_party() and ui.theme.good or ui.theme.bad })
      bx = bx + 28
    end
  end
  -- Targeting cursor
  for _, b in ipairs(p.targeting or {}) do
    local tx, ty = b.entity:screen_position(1.5)
    ui.text("▼", tx - 3, ty - 10 + math.floor(task.time() * 4) % 2, { color = ui.theme.highlight })
  end
  -- Banner
  if p.banner then
    local bw = ui.measure(p.banner) + 24
    local bx = math.floor((cw - bw) / 2)
    ui.window(bx, 24, bw, 18)
    ui.text(p.banner, bx + 12, 29, { color = ui.theme.heading })
  end
  -- Popups
  for _, popup in ipairs(p.popups) do
    local rise = math.min(1, popup.age * 4) * 10
    local alpha = popup.age > 0.9 and 1 - (popup.age - 0.9) / 0.3 or 1
    fe.canvas.push({ alpha = alpha })
    fe.canvas.text(popup.text, popup.x - 40, popup.y - rise, { color = popup.color, outline = 0x000000ff, width = 80, align = "center" })
    fe.canvas.pop()
  end
  if p.rewards_text then
    local rw, rh = 220, select(2, ui.measure(p.rewards_text, { width = 200 })) + 18
    local rx, ry = ui.anchor(rw, rh, "center")
    ui.window(rx, ry, rw, rh)
    ui.text(p.rewards_text, rx + 10, ry + 8, { width = 200 })
  end
end

sideview.Presenter = Presenter
sideview.handlers = handlers
return sideview

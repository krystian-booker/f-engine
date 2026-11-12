--- A battle on screen. It is fought where the party stands: the heroes run
--- to the map's formation points, the camera swings to the side, and the
--- rules (game/battle/rules.lua) decide what every command does while this
--- file shows it: the gauges, the command window, the blows and the numbers.
---
--- ```lua
--- local outcome = battle.start {
---   world = field.world, troop = "gate", prefix = "enc1_",
---   heroes = field.actors,                 -- hero id -> entity
---   enemies = { guard_a, guard_b },        -- entities, in the troop's order
---   camera = { target = ..., yaw = 0, pitch = 16, distance = 13 },
--- }                                        -- "victory", or what a script ended it with
--- ```
--- `battle.start` blocks the task that calls it. A lost battle offers to try
--- again and starts over, so it only returns once the fight is won or a
--- script ends it (`script.outcome`).
local actors = require("game.actors")
local audio = require("game.audio")
local director = require("game.director")
local fx = require("game.fx")
local input = require("game.input")
local party = require("game.party")
local rules = require("game.battle.rules")
local scenes = require("game.scenes")
local task = require("game.task")
local ui = require("game.ui")
local vfx = require("game.vfx")

local battle = { active = nil }

local Scene = {}
Scene.__index = Scene

-- ---------------------------------------------------------------- helpers

local function is_mesh(unit)
  return unit.record.model ~= nil
end

local function lift_of(unit)
  return is_mesh(unit) and 4.6 or 1.7
end

--- Where blows land on a unit (its chest), in world space.
local function body(self, unit)
  local e = self.entities[unit]
  local p = e.position
  return { x = p.x, y = p.y + (is_mesh(unit) and 3.2 or 0.8), z = p.z }
end

local function float(self, unit, text, color, big)
  self.floats[#self.floats + 1] = { unit = unit, text = text, color = color, age = 0, big = big,
    drift = (#self.floats % 3 - 1) * 26 }
end

local function banner(self, text, seconds, color)
  self.banner = { text = text, age = 0, life = seconds or 1.6, color = color }
end

--- Play a clip on a unit's entity, sprite or skeleton alike.
local function animate(self, unit, clip)
  local e = self.entities[unit]
  if not e or not e:valid() then return end
  if is_mesh(unit) then
    e:play(clip, clip == "idle" or clip == "overload", 0.18)
  else
    actors.pose(e, clip)
  end
end

--- The clip a unit rests in: ready, on one knee when badly hurt, or down.
local function rest(self, unit)
  local e = self.entities[unit]
  if not e or not e:valid() or is_mesh(unit) then return end
  if unit.ko then
    actors.pose(e, "ko")
  elseif unit.status.guard then
    actors.pose(e, "guard")
  elseif unit.hero and unit.hp <= unit.max_hp * 0.25 then
    actors.pose(e, "critical", false)
  else
    actors.pose(e, self.poses[unit] or "ready", false)
  end
end

local function flash(self, unit, color)
  local e = self.entities[unit]
  if not e or not e:valid() then return end
  task.spawn(function()
    task.over(0.22, function(t)
      if not e:valid() then return end
      if is_mesh(unit) then
        local k = 1 + (1 - t) * 1.6
        e:mesh({ tint = { k, k, k, 1 } })
      else
        e:sprite({ flash = { color[1], color[2], color[3], 1 - t } })
      end
    end)
  end, "flash unit")
end

--- An enemy that fell: it dissolves and is gone. A boss stays for its scene.
local function fall(self, unit)
  local e = self.entities[unit]
  if not e or not e:valid() or is_mesh(unit) then return end
  audio.sfx("enemy_fall", { volume = 0.7 })
  actors.pose(e, "ko")
  task.spawn(function()
    task.wait(0.25)
    task.over(0.5, function(t)
      if e:valid() then e:sprite({ tint = { 1, 0.6, 0.6, 1 - t }, blend = "alpha" }) end
    end)
    if e:valid() then e:sprite({ visible = false }) end
  end, "fall")
end

--- Show the result events of an action that land on targets.
local function land(self, events, from)
  for index = from, #events do
    local event = events[index]
    local unit = event.unit
    if event.type == "damage" then
      if event.ineffective then
        float(self, unit, "Ineffective", ui.colors.dim)
        audio.sfx("guard", { volume = 0.5, pitch = 0.7 })
      else
        float(self, unit, tostring(event.amount), event.crit and ui.colors.gold or 0xffffffff, event.crit)
        flash(self, unit, { 1, 1, 1 })
        if is_mesh(unit) then
          if not self.busy_boss then animate(self, unit, "hit") end
        elseif not event.ko then
          actors.pose(self.entities[unit], unit.status.guard and "guard" or "hurt")
        end
        if event.ko then
          if unit.hero then actors.pose(self.entities[unit], "ko") else fall(self, unit) end
        end
      end
    elseif event.type == "heal" or event.type == "revive" then
      float(self, unit, "+" .. event.amount, ui.colors.good)
      if event.type == "revive" then rest(self, unit) end
    elseif event.type == "mp" then
      float(self, unit, "+" .. event.amount .. " MP", ui.colors.mp)
    elseif event.type == "cover" then
      float(self, event.unit, "Cover!", ui.colors.gold)
    elseif event.type == "delay" then
      float(self, unit, "Slowed", ui.colors.mp)
    elseif event.type == "status" and event.on then
      local words = { guard = "Guard", ward = "Ward", cover = "Bulwark" }
      float(self, unit, words[event.status] or event.status, ui.colors.aether_hot)
    elseif event.type == "corruption" then
      float(self, unit, "Corruption " .. event.value .. "%", ui.colors.aether)
    elseif event.type == "message" and event.text then
      banner(self, event.text, 2.2)
    end
  end
end

local function centre_of(list, self)
  local x, z, n = 0, 0, 0
  for _, unit in ipairs(list) do
    local p = self.home[unit]
    x, z, n = x + p.x, z + p.z, n + 1
  end
  return { x = x / n, z = z / n }
end

--- A point `distance` in front of a unit, towards the other side.
local function in_front(self, unit, distance)
  local p = self.home[unit]
  local dir = unit.side == "party" and self.to_enemies or { x = -self.to_enemies.x, z = -self.to_enemies.z }
  return { x = p.x + dir.x * distance, z = p.z + dir.z * distance }
end

-- ----------------------------------------------------------- choreography

local function hero_strike(self, unit, use, events)
  local e = self.entities[unit]
  local target = use.targets[1]
  local reach = is_mesh(target) and 3.4 or 1.1
  local spot = in_front(self, target, reach)
  spot.z = spot.z + (self.home[unit].z - self.home[target].z) * 0.15
  local resonance = use.skill == "resonance"
  if resonance then
    -- The Aetherite answers: a breath of violet light before the blow.
    audio.sfx("aether_charge", { volume = 0.9 })
    actors.pose(e, "cast")
    vfx.light(self.world, e.position, { color = { 0.7, 0.35, 1.0 }, intensity = 5, radius = 7, y = 1 }, 1.6)
    vfx.aura(self.world, e, { color = { 0.75, 0.45, 1.0 }, seconds = 1.0 })
    e:sprite({ outline = { 0.75, 0.45, 1.0, 1 }, outline_width = 1 })
    fx.camera_async(self.world, { distance = self.camera.distance - 3 }, 0.9)
    task.wait(0.9)
  end
  actors.pose(e, "run")
  director.slide(e, spot, resonance and 0.16 or 0.22, { ease = fx.ease_out })
  actors.pose(e, "attack")
  task.wait(0.24) -- the frame the blade lands on
  local at = body(self, target)
  if resonance then
    audio.sfx("resonance", { volume = 1 })
    vfx.sprite(self.world, "fx/battle/resonance.anim.json", at, { scale = is_mesh(target) and 3.2 or 1.8, life = 0.5 })
    vfx.ring(self.world, at, { color = { 0.8, 0.5, 1.0 }, scale = 5, seconds = 0.5 })
    vfx.burst(self.world, at, "shard", 26)
    fx.flash({ 0.85, 0.7, 1.0 }, 0.35, 0.7)
    fx.shake(self.world, 0.35, 0.6)
    fx.hitstop(self.world, 0.12)
    fe.input.rumble(0.8, 0.6, 0.35)
  else
    audio.sfx(use.skill == "bash" and "slash_heavy" or "slash", { volume = 0.8 })
    vfx.sprite(self.world, "fx/battle/slash.anim.json", at, { scale = is_mesh(target) and 1.8 or 1.1, life = 0.3,
      flip = self.to_enemies.x > 0 })
    audio.sfx("hit", { volume = 0.7 })
    fx.shake(self.world, 0.06, 0.2)
  end
  land(self, events, 2)
  task.wait(resonance and 0.55 or 0.3)
  if resonance then
    e:sprite({ outline = { 0, 0, 0, 0 } })
    fx.camera_async(self.world, { distance = self.camera.distance }, 0.5)
  end
  actors.pose(e, "run")
  director.slide(e, self.home[unit], 0.24, { ease = fx.ease })
  e:face(self.to_enemies)
end

local SPELL = {
  ember = { sprite = "fx/battle/ember.anim.json", sound = "ember", color = { 1.0, 0.55, 0.2 }, scale = 1.3 },
  frost = { sprite = "fx/battle/frost.anim.json", sound = "frost", color = { 0.5, 0.8, 1.0 }, scale = 1.3 },
  surge = { sprite = "fx/battle/surge.anim.json", sound = "aether_pulse", color = { 0.7, 0.4, 1.0 }, scale = 1.5 },
  mend = { sprite = "fx/battle/mend.anim.json", sound = "mend", color = { 0.5, 1.0, 0.6 }, scale = 1.2 },
  ward = { sprite = "fx/battle/ward.anim.json", sound = "ward", color = { 0.7, 0.6, 1.0 }, scale = 1.2 },
}

local function hero_cast(self, unit, use, events)
  local e = self.entities[unit]
  local spell = SPELL[use.fx] or SPELL.ward
  director.slide(e, in_front(self, unit, 0.6), 0.15, { ease = fx.ease_out })
  actors.pose(e, "cast")
  audio.sfx(use.item and "item" or "spell_cast", { volume = 0.7 })
  vfx.circle(self.world, e.position, { color = spell.color, seconds = 0.7 })
  vfx.light(self.world, e.position, { color = spell.color, intensity = 3, radius = 5, y = 1 }, 0.8)
  task.wait(0.55)
  audio.sfx(spell.sound, { volume = 0.85 })
  for _, target in ipairs(use.targets) do
    if target then
      local at = body(self, target)
      local big = is_mesh(target)
      vfx.sprite(self.world, spell.sprite, at, { scale = spell.scale * (big and 2.2 or 1), life = 0.55 })
      vfx.light(self.world, at, { color = spell.color, intensity = 4, radius = 6 }, 0.5)
    end
  end
  task.wait(0.22)
  if use.kind == "magic" then fx.shake(self.world, 0.08, 0.25) end
  land(self, events, 2)
  task.wait(0.5)
  director.slide(e, self.home[unit], 0.18, { ease = fx.ease })
end

local function soldier_strike(self, unit, use, events)
  local e = self.entities[unit]
  local target = use.targets[1]
  if not target then return end
  local spot = in_front(self, target, 1.0)
  actors.pose(e, "run")
  director.slide(e, spot, 0.3, { ease = fx.ease_out })
  actors.pose(e, "attack")
  task.wait(0.24)
  audio.sfx(use.skill == "lunge" and "slash_heavy" or "slash", { volume = 0.7 })
  vfx.sprite(self.world, "fx/battle/slash.anim.json", body(self, target), { scale = 1.0, life = 0.3,
    tint = { 1.0, 0.7, 0.6, 1 }, flip = self.to_enemies.x < 0 })
  audio.sfx("hit", { volume = 0.6 })
  fx.shake(self.world, 0.08, 0.2)
  land(self, events, 2)
  task.wait(0.3)
  actors.pose(e, "run")
  director.slide(e, self.home[unit], 0.3, { ease = fx.ease })
  e:face({ x = -self.to_enemies.x, z = -self.to_enemies.z })
end

local function boss_act(self, unit, use, events)
  local e = self.entities[unit]
  self.busy_boss = true
  if use.kind == "charge" then
    audio.sfx("aether_charge", { volume = 1 })
    vfx.aura(self.world, e, { color = { 0.7, 0.4, 1.0 }, seconds = 1.4, y = 3.2, scale = 3 })
    vfx.light(self.world, body(self, unit), { color = { 0.7, 0.35, 1.0 }, intensity = 7, radius = 12 }, 1.6)
    land(self, events, 2)
    task.wait(1.4)
    self.busy_boss = false
    return
  end
  local magic = use.kind == "magic"
  e:play(use.animation or "slam", false, 0.15)
  local sound = use.animation == "sweep" and "colossus_sweep" or (magic and "aether_charge" or "colossus_slam")
  if magic then audio.sfx(sound, { volume = 0.9 }) end
  task.wait(use.impact or 0.9)
  if magic then
    audio.sfx("aether_pulse", { volume = 1 })
    vfx.ring(self.world, body(self, unit), { color = { 0.75, 0.45, 1.0 }, scale = 14, seconds = 0.7 })
    vfx.shockwave(self.world, body(self, unit), { scale = 12, seconds = 0.7 })
    fx.flash({ 0.8, 0.6, 1.0 }, 0.4, 0.6)
    fx.shake(self.world, 0.4, 0.7)
  else
    audio.sfx(sound, { volume = 1 })
    fx.shake(self.world, 0.45, 0.6)
    fx.hitstop(self.world, 0.08)
  end
  fe.input.rumble(0.9, 0.5, 0.4)
  for _, target in ipairs(use.targets) do
    if target then
      local at = body(self, target)
      vfx.sprite(self.world, magic and "fx/battle/surge.anim.json" or "fx/battle/impact.anim.json", at,
        { scale = 1.6, life = 0.45 })
      vfx.burst(self.world, { x = at.x, y = 0.1, z = at.z }, magic and "mote" or "dust", 10)
    end
  end
  land(self, events, 2)
  task.wait(0.9)
  e:play(self.boss_idle or "idle", true, 0.3)
  self.busy_boss = false
end

--- Show one action: its banner, the actor's move, and what it did.
local function show(self, events)
  local use = events[1]
  if not use or use.type ~= "use" then
    land(self, events, 1)
    return
  end
  local unit = use.unit
  self.acting = unit
  if use.banner or unit.side == "enemy" or use.item then banner(self, use.name, 1.5) end
  if use.kind == "script" then
    self.script.on_script(self, unit, use.skill)
  elseif is_mesh(unit) then
    boss_act(self, unit, use, events)
  elseif use.kind == "guard" then
    audio.sfx("guard", { volume = 0.6 })
    land(self, events, 2)
    task.wait(0.35)
  elseif unit.hero and (use.kind == "physical") then
    hero_strike(self, unit, use, events)
  elseif unit.hero then
    hero_cast(self, unit, use, events)
  else
    soldier_strike(self, unit, use, events)
  end
  for _, each in ipairs(self.rules.units) do rest(self, each) end
  self.acting = nil
  task.wait(0.12)
end

-- ---------------------------------------------------------------- commands

local function wait_frame(self)
  task.frame()
end

--- Point at one of `choices` (units); returns the unit, or nil on cancel.
local function pick_target(self, choices, start)
  if #choices == 0 then
    audio.sfx("buzzer")
    return nil
  end
  local index = start or 1
  self.menu.target = choices[index]
  while true do
    wait_frame(self)
    local step = input.step("up", "down") + input.step("left", "right")
    if step ~= 0 then
      index = (index - 1 + step) % #choices + 1
      self.menu.target = choices[index]
      audio.sfx("cursor", { volume = 0.5 })
    end
    if fe.input.pressed("confirm") then
      audio.sfx("confirm", { volume = 0.6 })
      local chosen = choices[index]
      self.menu.target = nil
      return chosen
    end
    if fe.input.pressed("cancel") then
      audio.sfx("cancel", { volume = 0.6 })
      self.menu.target = nil
      return nil
    end
  end
end

--- A list in the side window: rows {label, right, disabled, help, value}.
local function pick_row(self, rows)
  self.menu.sub = { rows = rows, index = 1 }
  local sub = self.menu.sub
  while true do
    wait_frame(self)
    local step = input.step("up", "down")
    if step ~= 0 then
      sub.index = (sub.index - 1 + step) % #rows + 1
      audio.sfx("cursor", { volume = 0.5 })
    end
    if fe.input.pressed("confirm") then
      local row = rows[sub.index]
      if row.disabled then
        audio.sfx("buzzer", { volume = 0.6 })
      else
        audio.sfx("confirm", { volume = 0.6 })
        return row
      end
    end
    if fe.input.pressed("cancel") then
      audio.sfx("cancel", { volume = 0.6 })
      self.menu.sub = nil
      return nil
    end
  end
end

local function target_for(self, unit, kind)
  if kind == "self" or kind == "enemies" or kind == "allies" then return true end
  local choices = self.rules:choices(unit, kind)
  local start = 1
  if kind == "ally" then
    for i, ally in ipairs(choices) do
      if ally == unit then start = i end
    end
  end
  return pick_target(self, choices, start)
end

--- The command window for a hero whose gauge is full. Blocks until an
--- action is chosen (time stands still meanwhile).
local function choose(self, unit)
  local commands = unit.record.commands
  if self.script.commands then commands = self.script.commands(self, unit) or commands end
  self.menu = { unit = unit, commands = commands, index = self.cursor[unit] or 1 }
  local menu = self.menu
  if menu.index > #commands then menu.index = 1 end
  audio.sfx("atb_ready", { volume = 0.6 })
  while true do
    wait_frame(self)
    local step = input.step("up", "down")
    if step ~= 0 then
      menu.index = (menu.index - 1 + step) % #commands + 1
      audio.sfx("cursor", { volume = 0.5 })
    end
    if fe.input.pressed("confirm") then
      local command = commands[menu.index]
      local action
      if command.skill then
        local blocked = self.rules:blocked(unit, command.skill)
        if blocked then
          audio.sfx("buzzer", { volume = 0.6 })
          banner(self, blocked, 1.2)
        else
          audio.sfx("confirm", { volume = 0.6 })
          local target = target_for(self, unit, rules.skill(command.skill).target)
          if target then action = { skill = command.skill, target = target ~= true and target or nil } end
        end
      elseif command.skills then
        audio.sfx("confirm", { volume = 0.6 })
        local rows = {}
        for _, id in ipairs(command.skills) do
          local skill = rules.skill(id)
          rows[#rows + 1] = { label = skill.name, right = skill.mp and (skill.mp .. " MP") or "", value = id,
            disabled = self.rules:blocked(unit, id) ~= nil, help = skill.help }
        end
        while not action do
          local row = pick_row(self, rows)
          if not row then break end
          local target = target_for(self, unit, rules.skill(row.value).target)
          if target then action = { skill = row.value, target = target ~= true and target or nil } end
        end
      elseif command.items then
        audio.sfx("confirm", { volume = 0.6 })
        local rows = {}
        for _, id in ipairs { "tonic", "vial", "kindle" } do
          local item = rules.item(id)
          local count = self.rules.items[id] or 0
          rows[#rows + 1] = { label = item.name, right = "x" .. count, value = id, disabled = count <= 0,
            help = item.help }
        end
        while not action do
          local row = pick_row(self, rows)
          if not row then break end
          local target = target_for(self, unit, rules.item(row.value).target)
          if target then action = { item = row.value, target = target } end
        end
      end
      self.menu.sub = nil
      if action then
        self.cursor[unit] = menu.index
        self.menu = nil
        return action
      end
    end
  end
end

-- ------------------------------------------------------------------ scene

function Scene:update(dt)
  for i = #self.floats, 1, -1 do
    local f = self.floats[i]
    f.age = f.age + dt
    if f.age > 1.1 then table.remove(self.floats, i) end
  end
  if self.banner then
    self.banner.age = self.banner.age + dt
    if self.banner.age > self.banner.life then self.banner = nil end
  end
  self.time = self.time + dt
  -- Drawn gauges ease towards the real ones.
  for _, unit in ipairs(self.rules.units) do
    local shown = self.shown[unit] or { hp = unit.hp, gauge = unit.gauge }
    shown.hp = shown.hp + (unit.hp - shown.hp) * math.min(1, dt * 8)
    shown.gauge = unit.gauge
    self.shown[unit] = shown
  end
  self.world:events() -- nobody else reads them while the battle is on top
end

local PANEL = { x = 1010, y = 792, w = 860, h = 248 }
local COMMAND = { x = 50, y = 792, w = 400, h = 248 }

local function draw_party(self)
  ui.panel(PANEL.x, PANEL.y, PANEL.w, PANEL.h)
  for row, unit in ipairs(self.rules.heroes) do
    local y = PANEL.y + 22 + (row - 1) * 52
    local active = self.menu and self.menu.unit == unit
    local color = unit.ko and ui.colors.bad or (active and ui.colors.gold or ui.colors.text)
    if active then
      fe.canvas.rect(PANEL.x + 12, y - 4, PANEL.w - 24, 48, 0xe7c36b22)
      fe.canvas.rect(PANEL.x + 12, y - 4, 4, 48, ui.colors.gold)
    end
    ui.text(unit.name, PANEL.x + 34, y + 2, { font = "label", color = color })
    local shown = self.shown[unit] or unit
    local hp = math.floor(shown.hp + 0.5)
    local low = unit.hp <= unit.max_hp * 0.25
    ui.text(string.format("%d", hp), PANEL.x + 190, y - 2, { font = "small", width = 90, align = "right",
      color = low and ui.colors.hp_low or ui.colors.text })
    ui.text("/ " .. unit.max_hp, PANEL.x + 288, y + 4, { font = "small", color = ui.colors.faint, size = 24 })
    ui.bar(PANEL.x + 196, y + 34, 180, 6, shown.hp / unit.max_hp, low and ui.colors.hp_low or ui.colors.hp)
    if unit.max_mp > 0 then
      ui.text(string.format("%d", unit.mp), PANEL.x + 400, y - 2, { font = "small", width = 70, align = "right",
        color = ui.colors.mp })
      ui.bar(PANEL.x + 410, y + 34, 100, 6, unit.mp / unit.max_mp, ui.colors.mp)
    elseif unit.corruption then
      local pulse = 0.7 + 0.3 * math.sin(self.time * (2 + unit.corruption / 20))
      ui.text(unit.corruption .. "%", PANEL.x + 400, y - 2, { font = "small", width = 70, align = "right",
        color = ui.fade(ui.colors.aether, unit.corruption > 0 and pulse or 0.5) })
      ui.bar(PANEL.x + 410, y + 34, 100, 6, unit.corruption / 100, ui.colors.aether)
    end
    local full = unit.gauge >= 1 and not unit.ko
    local glow = full and (0.75 + 0.25 * math.sin(self.time * 9)) or 1
    ui.bar(PANEL.x + 560, y + 14, 250, 14, unit.ko and 0 or unit.gauge,
      ui.fade(full and ui.colors.atb_full or ui.colors.atb, glow))
  end
end

local function draw_commands(self)
  local menu = self.menu
  if not menu then return end
  ui.panel(COMMAND.x, COMMAND.y, COMMAND.w, COMMAND.h)
  for i, command in ipairs(menu.commands) do
    local y = COMMAND.y + 24 + (i - 1) * 50
    local blocked = command.skill and self.rules:blocked(menu.unit, command.skill)
    local color = blocked and ui.colors.faint or (command.color and ui.colors[command.color] or ui.colors.text)
    if i == menu.index then
      fe.canvas.rect(COMMAND.x + 14, y - 4, COMMAND.w - 28, 46, 0xffffff14)
      if not menu.sub and not menu.target then ui.cursor(COMMAND.x + 62, y + 20) end
    end
    ui.text(command.label, COMMAND.x + 78, y, { font = "body", color = color, size = 40 })
  end
  local help
  local selected = menu.commands[menu.index]
  if selected and selected.skill then help = rules.skill(selected.skill).help end
  if menu.sub then
    local x, w = COMMAND.x + COMMAND.w + 14, 520
    ui.panel(x, COMMAND.y, w, COMMAND.h)
    for i, row in ipairs(menu.sub.rows) do
      local y = COMMAND.y + 24 + (i - 1) * 50
      local color = row.disabled and ui.colors.faint or ui.colors.text
      if i == menu.sub.index then
        fe.canvas.rect(x + 14, y - 4, w - 28, 46, 0xffffff14)
        if not menu.target then ui.cursor(x + 62, y + 20) end
      end
      ui.text(row.label, x + 78, y, { font = "body", color = color, size = 40 })
      ui.text(row.right or "", x + 30, y + 6, { font = "small", color = row.disabled and ui.colors.faint or ui.colors.mp,
        width = w - 70, align = "right" })
    end
    help = menu.sub.rows[menu.sub.index].help
  end
  if help and help ~= "" then
    local tw = fe.text.measure(help, { font = "small" })
    ui.panel(COMMAND.x, COMMAND.y - 66, tw + 64, 58, "plate")
    ui.text(help, COMMAND.x + 32, COMMAND.y - 54, { font = "small", color = ui.colors.dim })
  end
  if menu.target then
    local unit = menu.target
    local e = self.entities[unit]
    local x, y, visible = e:screen_position(lift_of(unit) * 0.55)
    if visible then
      local side = unit.side == "party" and 1 or -1
      if is_mesh(unit) then
        x = x + 170
        side = 1
      end
      local bob = math.sin(self.time * 7) * 5
      fe.canvas.image("ui/cursor.png", side > 0 and (x - 110 + bob) or (x + 110 - bob - 64), y - 20,
        { linear = true, flip_x = side < 0 })
      local tw = fe.text.measure(unit.name, { font = "label" })
      local nx = math.max(20, math.min(1900 - tw - 60, x - tw / 2 - 30))
      local _, ty = e:screen_position(lift_of(unit) + 0.5)
      ui.panel(nx, ty - 60, tw + 60, 56, "plate")
      ui.text(unit.name, nx + 30, ty - 50, { font = "label" })
    end
  end
end

local function draw_boss(self)
  local boss = self.boss
  if not boss then return end
  local w = 820
  local x = (1920 - w) / 2
  ui.centred(boss.name:upper(), 960, 44, { font = "label", color = ui.colors.text })
  local shown = self.shown[boss] or boss
  ui.bar(x, 90, w, 12, shown.hp / boss.max_hp, self.boss_color or 0xd9534fff)
  if self.boss_note then
    ui.centred(self.boss_note, 960, 112, { font = "small", color = ui.colors.aether_hot })
  end
end

local function draw_floats(self)
  for _, f in ipairs(self.floats) do
    local e = self.entities[f.unit]
    if e and e:valid() then
      local x, y, visible = e:screen_position(lift_of(f.unit))
      if visible then
        local t = f.age / 1.1
        local pop = f.age < 0.12 and (1.5 - f.age / 0.12 * 0.5) or 1
        local alpha = t > 0.7 and (1 - (t - 0.7) / 0.3) or 1
        local number = tonumber(f.text) ~= nil or f.text:sub(1, 1) == "+"
        local size = (number and (f.big and 80 or 58) or 34) * pop
        fe.canvas.push({ alpha = alpha })
        ui.centred(f.text, x + f.drift, y - 20 - t * 70, { font = "digits", size = size, color = f.color,
          outline = 0x000000e0 })
        fe.canvas.pop()
      end
    end
  end
end

local function draw_banner(self)
  local b = self.banner
  if not b then return end
  local alpha = math.min(1, b.age * 8, (b.life - b.age) * 4)
  local tw = fe.text.measure(b.text, { font = "label" })
  fe.canvas.push({ alpha = alpha })
  ui.panel(960 - tw / 2 - 60, 168, tw + 120, 64, "plate")
  ui.centred(b.text, 960, 182, { font = "label", color = b.color or ui.colors.gold })
  fe.canvas.pop()
end

function Scene:draw()
  self.world:draw()
  if self.hud <= 0 then return end
  fe.canvas.push({ alpha = self.hud })
  draw_boss(self)
  draw_party(self)
  draw_commands(self)
  draw_floats(self)
  draw_banner(self)
  if self.script.draw then self.script.draw(self) end
  if self.menu then
    ui.hints(self.menu.target and { { "confirm", "Select" }, { "cancel", "Back" } }
      or { { "confirm", "Choose" }, { "cancel", "Back" } })
  end
  fe.canvas.pop()
end

--- A boxed hint the player dismisses (the first battle explains itself).
function Scene:tutorial(title, text)
  self.hint = { title = title, text = text }
  local previous = self.script.draw
  self.script.draw = function(scene)
    if previous then previous(scene) end
    local w, h = 1000, 250
    local x, y = (1920 - w) / 2, 300
    ui.panel(x, y, w, h)
    ui.text(title, x + 50, y + 36, { font = "label", color = ui.colors.gold })
    ui.text(text, x + 50, y + 90, { font = "small", width = w - 100, color = ui.colors.text, line_gap = 6 })
    ui.text(input.prompt("confirm"), x + w - 90, y + h - 62, { font = "small" })
  end
  task.wait(0.2)
  task.wait_until(function() return fe.input.pressed("confirm") end)
  audio.sfx("confirm", { volume = 0.6 })
  self.script.draw = previous
  self.hint = nil
  task.frame()
end

-- ------------------------------------------------------------------- flow

local function place_units(self, options)
  local points = self.world:points()
  local prefix = options.prefix or ""
  self.home, self.entities = {}, {}
  for index, unit in ipairs(self.rules.heroes) do
    local e = options.heroes[unit.id]
    self.entities[unit] = e
    local point = points[prefix .. "party" .. index] or error("no point " .. prefix .. "party" .. index)
    self.home[unit] = { x = point.position.x, z = point.position.z }
  end
  for index, unit in ipairs(self.rules.enemies) do
    local e = options.enemies[index]
    self.entities[unit] = e
    local p = e.position
    self.home[unit] = { x = p.x, z = p.z }
  end
  local a, b = centre_of(self.rules.heroes, self), centre_of(self.rules.enemies, self)
  local dx, dz = b.x - a.x, b.z - a.z
  local length = math.max(0.001, math.sqrt(dx * dx + dz * dz))
  self.to_enemies = { x = dx / length, z = dz / length }
  self.middle = { x = (a.x + b.x) / 2, z = (a.z + b.z) / 2 }
end

--- Run the heroes to their places and turn everyone to face the other side.
local function take_positions(self, instant)
  local moves = {}
  for _, unit in ipairs(self.rules.heroes) do
    local e = self.entities[unit]
    if instant then
      e:place(self.home[unit], e.layer)
    else
      actors.pose(e, "run")
      moves[#moves + 1] = director.slide_async(e, self.home[unit], 0.45, { ease = fx.ease })
    end
  end
  task.wait_until(function()
    for _, move in ipairs(moves) do
      if not task.done(move) then return false end
    end
    return true
  end)
  for _, unit in ipairs(self.rules.units) do
    local e = self.entities[unit]
    if not is_mesh(unit) then
      e:face(unit.side == "party" and self.to_enemies or { x = -self.to_enemies.x, z = -self.to_enemies.z })
    end
    rest(self, unit)
  end
end

local function run(self)
  local b = self.rules
  local queue = {}
  local script = self.script
  while true do
    local outcome = (script.outcome and script.outcome(self)) or b:outcome()
    if outcome then return outcome end
    if #queue == 0 then
      local dt = task.frame()
      if script.tick then script.tick(self, dt) end
      for _, unit in ipairs(b:advance(dt)) do queue[#queue + 1] = unit end
    else
      local unit = table.remove(queue, 1)
      if not unit.ko and unit.gauge >= 1 then
        local skip = script.before_turn and script.before_turn(self, unit)
        if not skip then
          local action
          if unit.side == "enemy" then
            action = b:enemy_action(unit)
          else
            if script.first_turn and not self.explained then
              self.explained = true
              script.first_turn(self, unit)
            end
            if script.hero_turn then script.hero_turn(self, unit) end
            action = choose(self, unit)
          end
          show(self, b:act(unit, action))
          if script.after_turn then script.after_turn(self, unit, action) end
        end
      end
    end
  end
end

local function victory(self)
  self.menu = nil
  audio.music("victory", 0.2)
  banner(self, "Victory", 2.6)
  for _, unit in ipairs(self.rules.heroes) do
    if not unit.ko then actors.pose(self.entities[unit], "victory") end
  end
  task.wait(2.6)
  task.over(0.3, function(t) self.hud = 1 - t end)
end

--- "The party has fallen": wait for the player, then set the fight up again.
local function defeat(self)
  self.menu = nil
  audio.music(nil, 1.5)
  fx.fade(1, 1.2)
  self.hud = 0
  local done = false
  local overlay = {
    draw = function()
      ui.centred("The party has fallen", 960, 440, { font = "title", color = ui.colors.text })
      ui.centred(input.prompt("confirm") .. "  Try again", 960, 600, { font = "body", color = ui.colors.dim })
    end,
    update = function()
      if fe.input.pressed("confirm") then done = true end
    end,
  }
  scenes.push(overlay)
  task.wait_until(function() return done end)
  scenes.pop(overlay)
end

--- options: `world`, `troop`, `prefix` (of the formation points), `heroes`
--- (id -> entity), `enemies` (entities in troop order), `camera`, `music`
--- (false keeps what is playing), `transition` (false: no shatter, the scene
--- flows straight in), `instant` (heroes already in place), `script`
--- (hooks: commands, before_turn, hero_turn, first_turn, after_turn, tick,
--- outcome, on_script, draw, setup), `keep_hud` (leave the panels up on
--- return, for a scene that continues the fight).
function battle.start(options)
  local world = options.world
  local snapshot = {}
  for id, saved in pairs(party.state) do
    snapshot[id] = { hp = saved.hp, mp = saved.mp, corruption = saved.corruption }
  end
  local items = {}
  for id, count in pairs(party.items) do items[id] = count end

  local self = setmetatable({ world = world, floats = {}, shown = {}, cursor = {}, poses = {}, time = 0, hud = 0,
    script = options.script or {}, options = options }, Scene)
  battle.active = self
  local outcome
  local first = true
  while true do
    self.rules = rules.new { heroes = party.order, troop = options.troop, state = party.state, items = party.items }
    for _, unit in ipairs(self.rules.enemies) do
      if unit.boss then self.boss = unit end
    end
    self.floats, self.shown, self.menu, self.banner, self.explained = {}, {}, nil, nil, not first
    place_units(self, options)
    local view = options.camera or {}
    self.camera = { target = view.target or { x = self.middle.x, y = 1.3, z = self.middle.z },
      yaw = view.yaw or 0, pitch = view.pitch or 17, distance = view.distance or 13, fov = view.fov or 30 }
    if self.script.setup then self.script.setup(self) end

    if first and options.transition ~= false then
      -- The field freezes and breaks apart into the battle.
      audio.sfx("encounter", { volume = 0.9 })
      fe.screen.freeze()
      if options.music ~= false then audio.music(self.rules.troop.music or "battle", 0.3) end
      scenes.push(self)
      take_positions(self, true)
      fx.camera(world, self.camera, 0)
      task.over(0.7, function(t) fe.screen.set { transition = { kind = "shatter", progress = t } } end)
      fe.screen.unfreeze()
      fe.screen.set { transition = { kind = "none", progress = 0 } }
    else
      if options.music ~= false then audio.music(self.rules.troop.music or "battle", 0.6) end
      if not scenes.contains(self) then scenes.push(self) end
      if not first then
        -- Trying again: everyone is back on their feet where they began.
        for _, unit in ipairs(self.rules.enemies) do
          local e = self.entities[unit]
          if not is_mesh(unit) then e:sprite({ visible = true, tint = { 1, 1, 1, 1 }, blend = "opaque" }) end
        end
        take_positions(self, true)
        fx.camera(world, self.camera, 0)
        fx.fade(0, 0.8)
      else
        fx.camera_async(world, self.camera, 1.0)
        take_positions(self, options.instant)
      end
    end
    task.over(0.3, function(t) self.hud = t end)
    outcome = run(self)
    if outcome ~= "defeat" then break end
    defeat(self)
    first = false
    for id, saved in pairs(snapshot) do
      party.state[id] = { hp = saved.hp, mp = saved.mp, corruption = saved.corruption }
    end
    for id, count in pairs(items) do party.items[id] = count end
    if self.script.reset then self.script.reset(self) end
  end

  self.rules:save()
  if outcome == "victory" and not options.quiet then victory(self) end
  if not options.keep_hud then
    task.over(0.25, function(t) self.hud = math.min(self.hud, 1 - t) end)
    scenes.pop(self)
    for _, unit in ipairs(self.rules.heroes) do
      actors.pose(self.entities[unit], nil)
    end
    battle.active = nil
  end
  return outcome, self
end

--- Take the battle's panels down after a scene that kept them (`keep_hud`).
function battle.finish(self)
  scenes.pop(self)
  for _, unit in ipairs(self.rules.heroes) do
    local e = self.entities[unit]
    if e:valid() then actors.pose(e, nil) end
  end
  if battle.active == self then battle.active = nil end
end

battle.float = float
battle.banner = banner
battle.rest = rest
battle.body = body
battle.is_mesh = is_mesh

return battle

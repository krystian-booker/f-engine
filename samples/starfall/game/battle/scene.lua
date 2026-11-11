--- A battle on screen: the stage, the party's commands and the show of what
--- the rules (game/battle/rules.lua) say happened.
---
--- `battle.start(troop)` blocks the task that calls it and returns "victory",
--- "fled" or "defeat" (after which the game is back at the title).
local audio = require("game.audio")
local data = require("game.data")
local input = require("game.input")
local rules = require("game.battle.rules")
local scenes = require("game.scenes")
local state = require("game.state")
local task = require("game.task")
local ui = require("game.ui")

local battle = { active = nil }

local Scene = { opaque = true }
Scene.__index = Scene

local AFFINITY_WORD = { weak = "Weak", resist = "Resist", null = "Null", absorb = "Absorb" }
local ELEMENTS = { "physical", "fire", "ice", "bolt", "light", "dark" }

-- ------------------------------------------------------------ the stage

local function build(self)
  local world = fe.world.load("battle", self.stage)
  self.world = world
  local points = world:points()
  local camera = world:camera()
  local centre = points.centre and points.centre.position or { x = 8, y = 0, z = 7.5 }
  camera.target = { x = centre.x, y = 0.8, z = centre.z }
  camera.yaw, camera.pitch, camera.distance, camera.fov = 0, 26, 15, 30
  camera:snap()
  -- Both lines of the battle stay in focus; the stage beyond them softens.
  world:set_environment { post = { focus = 0.55, focus_range = 0.5 } }
  self.entities = {}
  local slot = 0
  for _, u in ipairs(self.rules.allies) do
    slot = slot + 1
    local point = points["party" .. slot]
    local e = world:spawn { position = point.position, facing = { x = -1, z = 0 },
      sprite = { animation = u.record.sprite, clip = rules.alive(u) and "idle" or "ko" } }
    self.entities[u] = e
    self.home[u] = point.position
  end
  for index, u in ipairs(self.rules.foes) do
    local point = points["enemy" .. index] or points.enemy1
    local e
    if u.record.model then
      -- A rigged 3D creature among the sprites: its skeleton's clips play like sprite clips.
      local at = { x = point.position.x + 0.4, y = point.position.y, z = centre.z }
      e = world:spawn { position = at, yaw = 90, scale = u.record.scale or 1, mesh = { model = u.record.model } }
      e:play("idle", true)
      self.home[u] = at
    else
      e = world:spawn { position = point.position, facing = { x = 1, z = 0 },
        sprite = { animation = u.record.sprite, clip = "idle" } }
      self.home[u] = point.position
    end
    self.entities[u] = e
  end
end

local function is_model(u)
  return u.record.model ~= nil
end

--- Play a clip on a unit's entity, sprite or skeleton alike.
local function animate(self, u, clip, loop)
  local e = self.entities[u]
  if not e or not e:valid() then return end
  if is_model(u) then
    local names = { attack = "attack", cast = "cast", hurt = "hit", ko = "die", idle = "idle", item = "cast" }
    e:play(names[clip] or clip, loop or clip == "idle", 0.12)
  else
    e:clip(clip, true)
  end
end

local function flash(self, u, color)
  local e = self.entities[u]
  if not e or not e:valid() then return end
  if is_model(u) then
    e:mesh { tint = color and { 1 + color[1], 1 + color[2], 1 + color[3], 1 } or { 1, 1, 1, 1 } }
  else
    e:sprite { flash = color and { color[1], color[2], color[3], 0.85 } or { 1, 1, 1, 0 } }
  end
end

local function float(self, u, text, color)
  local count = 0
  for _, f in ipairs(self.floats) do
    if f.unit == u and f.age < 0.25 then count = count + 1 end
  end
  self.floats[#self.floats + 1] = { unit = u, text = text, color = color or ui.colors.text, age = 0, row = count }
end

local function banner(self, text, seconds)
  self.banner = { text = text, left = seconds or 1.0 }
end

-- ------------------------------------------------------------ showing events

local function show(self, events)
  for _, event in ipairs(events or {}) do
    local u = event.unit
    if event.type == "opening" then
      if event.opening == "preemptive" then banner(self, "Preemptive strike!", 1.4) end
      if event.opening == "ambush" then banner(self, "Ambushed!", 1.4) end
      if event.opening ~= "normal" then task.wait(0.9) end
    elseif event.type == "use" then
      local name = event.skill and event.skill.name or event.item.name
      if not (event.skill and event.skill.name == "Attack") then banner(self, name, 0.9) end
      animate(self, u, event.skill and event.skill.clip or "item")
      if u.side == rules.ALLIES and not u.summoned then
        local home = self.home[u]
        self.entities[u]:place({ x = home.x - 0.5, y = home.y, z = home.z })
      end
      task.wait(0.32)
    elseif event.type == "damage" then
      local color = event.poison and 0xc38cffff or ui.colors.text
      float(self, u, tostring(event.amount) .. (event.crit and "!" or ""), color)
      if event.affinity and AFFINITY_WORD[event.affinity] then
        float(self, u, AFFINITY_WORD[event.affinity], event.affinity == "weak" and ui.colors.weak or ui.colors.dim)
      end
      if event.covered then float(self, u, "Cover", ui.colors.ally) end
      if event.amount > 0 then
        audio.sfx("hit", { volume = 0.7 })
        animate(self, u, "hurt")
        flash(self, u, { 1, 1, 1 })
        if u.side == rules.ALLIES then fe.input.rumble(0.3, 0.5, 0.12) end
      end
      task.wait(0.14)
      flash(self, u, nil)
      task.wait(0.14)
      if rules.alive(u) and not is_model(u) then animate(self, u, "idle") end
      if rules.alive(u) and is_model(u) then task.wait(0.15); animate(self, u, "idle", true) end
    elseif event.type == "heal" then
      float(self, u, "+" .. event.amount, ui.colors.good)
      if event.affinity == "absorb" then float(self, u, "Absorb", ui.colors.dim) end
      audio.sfx("heal", { volume = 0.6 })
      task.wait(0.22)
    elseif event.type == "mp" then
      float(self, u, "+" .. event.amount .. " MP", ui.colors.mp)
      task.wait(0.2)
    elseif event.type == "miss" then
      float(self, u, "Miss", ui.colors.dim)
      task.wait(0.2)
    elseif event.type == "ko" then
      animate(self, u, "ko")
      if u.side == rules.FOES then
        audio.sfx("defeat_enemy", { volume = 0.7 })
        local e = self.entities[u]
        if is_model(u) then
          task.wait(1.2)
        else
          task.over(0.35, function(t) if e:valid() then e:sprite { tint = { 1, 1, 1, 1 - t } } end end)
        end
        if e:valid() then e:destroy() end
        self.entities[u] = nil
      end
    elseif event.type == "break" then
      float(self, u, "BREAK", ui.colors.weak)
      banner(self, u.name .. " is broken!", 1.0)
      audio.sfx("chime", { volume = 0.8 })
      fe.screen.set { flash = { 1, 0.9, 0.6, 0.35 } }
      task.wait(0.08)
      fe.screen.set { flash = { 1, 1, 1, 0 } }
      task.wait(0.3)
    elseif event.type == "sweep" then
      banner(self, "SWEEP! The whole party acts again.", 1.6)
      task.wait(0.8)
    elseif event.type == "recover" then
      float(self, u, "Recovered", ui.colors.dim)
    elseif event.type == "reveal" then
      float(self, u, AFFINITY_WORD[event.affinity] .. ": " .. event.element, ui.colors.accent)
    elseif event.type == "status" then
      local status = data.get("statuses", event.id)
      float(self, u, event.applied and status.name or "Resisted", event.applied and ui.colors.accent or ui.colors.dim)
      task.wait(0.2)
    elseif event.type == "status_end" then
      float(self, u, data.get("statuses", event.id).name .. " ends", ui.colors.dim)
    elseif event.type == "cure" then
      float(self, u, "Cured", ui.colors.good)
      task.wait(0.15)
    elseif event.type == "revive" then
      animate(self, u, "idle")
      float(self, u, "Revived", ui.colors.good)
      audio.sfx("heal")
      task.wait(0.3)
    elseif event.type == "defend" then
      float(self, u, "Defend", ui.colors.ally)
      task.wait(0.2)
    elseif event.type == "summon" then
      local centre = self.world:points().centre.position
      local at = { x = centre.x + 2.2, y = centre.y, z = centre.z + 2.9 }
      self.entities[u] = self.world:spawn { position = at, facing = { x = -1, z = 0 },
        sprite = { animation = u.record.sprite, clip = "idle" } }
      self.home[u] = at
      self.world:burst({ x = at.x, y = 0.8, z = at.z }, { color = { 0.6, 0.9, 1, 1 }, color_end = { 0.6, 0.9, 1, 0 },
        lifetime = 0.7, size = 0.08, velocity = { 0, 1.5, 0 }, velocity_jitter = { 1, 1, 1 } }, 24)
      banner(self, u.name .. " answers the call!", 1.2)
      task.wait(0.5)
    elseif event.type == "leave" then
      local e = self.entities[u]
      if e and e:valid() then e:destroy() end
      self.entities[u] = nil
      banner(self, u.name .. " departs.", 1.0)
      task.wait(0.4)
    elseif event.type == "unison" then
      banner(self, "UNISON: " .. event.echo.name, 1.6)
      fe.screen.set { flash = { 1, 1, 1, 0.6 } }
      task.over(0.4, function(t) fe.screen.set { flash = { 1, 1, 1, 0.6 * (1 - t) } } end)
    elseif event.type == "flee" then
      banner(self, event.success and "Got away!" or (event.boss and "There is no escape!" or "Could not escape!"), 1.0)
      task.wait(0.6)
    elseif event.type == "nothing" then
      float(self, u, "No effect", ui.colors.dim)
      task.wait(0.2)
    end
  end
  -- Everyone back to their place.
  for u, e in pairs(self.entities) do
    if e:valid() and u.side == rules.ALLIES and not u.summoned then
      e:place(self.home[u])
      if rules.alive(u) and e:sprite().clip ~= "idle" then e:clip("idle", true) end
    end
  end
end

-- ------------------------------------------------------------ commands

local function skill_rows(self, u)
  local rows = {}
  for _, id in ipairs(u.skills) do
    local skill = data.get("skills", id)
    local usable = self.rules:usable(u, id)
    rows[#rows + 1] = { skill.name, right = (skill.cost or 0) > 0 and (skill.cost .. " MP") or "", disabled = not usable,
      id = id, about = skill.about }
  end
  return rows
end

local function item_rows()
  local rows = {}
  for _, item in ipairs(data.list("items")) do
    local count = state.item_count(item.id)
    if count > 0 and not item.field_only then
      rows[#rows + 1] = { item.name, right = "x" .. count, id = item.id, about = item.about }
    end
  end
  return rows
end

local function candidates(self, u, target_kind, include_fallen)
  local side = (target_kind == "foe") and rules.FOES or rules.ALLIES
  local out = {}
  for _, other in ipairs(self.rules:side(side)) do
    if not other.gone and (rules.alive(other) or include_fallen) then out[#out + 1] = other end
  end
  return out
end

--- Open the command menu for a hero; the task waits in `choose` for the result.
local function open_commands(self, u, bonus)
  local rows = { { "Attack", id = "attack" }, { "Skills", id = "skills" } }
  if u.surge >= 100 then rows[#rows + 1] = { "Surge", id = "surge", right = "!" } end
  rows[#rows + 1] = { "Items", id = "items" }
  rows[#rows + 1] = { "Defend", id = "defend" }
  if not bonus then rows[#rows + 1] = { "Flee", id = "flee" } end
  self.menu = { mode = "command", unit = u, rows = rows, cursor = ui.cursor(), bonus = bonus }
end

local function pick_target(self, action, target_kind, include_fallen)
  local list = candidates(self, self.menu.unit, target_kind, include_fallen)
  if #list == 0 then return end
  self.menu.back = self.menu.mode
  self.menu.mode, self.menu.action, self.menu.targets, self.menu.target = "target", action, list, 1
end

local function decide(self, action)
  audio.sfx("confirm", { volume = 0.6 })
  self.choice = action
  self.menu = nil
end

local function choose_skill(self, action, skill)
  if skill.target == "foe" or skill.target == "ally" then
    pick_target(self, action, skill.target, skill.revive ~= nil)
  else
    decide(self, action)
  end
end

local function menu_input(self)
  local menu = self.menu
  local u = menu.unit
  if menu.mode == "command" then
    ui.list_input(menu.cursor, #menu.rows)
    if fe.input.pressed("confirm") then
      local id = menu.rows[menu.cursor.index].id
      if id == "attack" then
        choose_skill(self, { skill = u.attack }, data.get("skills", u.attack))
      elseif id == "skills" then
        menu.mode, menu.list, menu.sub = "skills", skill_rows(self, u), ui.cursor()
      elseif id == "surge" then
        choose_skill(self, { surge = true }, data.get("skills", u.surge_skill))
      elseif id == "items" then
        local rows = item_rows()
        if #rows == 0 then
          audio.sfx("buzzer")
        else
          menu.mode, menu.list, menu.sub = "items", rows, ui.cursor()
        end
      elseif id == "defend" then
        decide(self, { defend = true })
      elseif id == "flee" then
        decide(self, { flee = true })
      end
    end
  elseif menu.mode == "skills" or menu.mode == "items" then
    ui.list_input(menu.sub, #menu.list)
    if fe.input.pressed("cancel") then
      menu.mode = "command"
    elseif fe.input.pressed("confirm") then
      local row = menu.list[menu.sub.index]
      if row.disabled then
        audio.sfx("buzzer")
      elseif menu.mode == "skills" then
        choose_skill(self, { skill = row.id }, data.get("skills", row.id))
      else
        local item = data.get("items", row.id)
        pick_target(self, { item = row.id }, "ally", item.effect.kind == "revive")
      end
    end
  elseif menu.mode == "target" then
    local step = input.step("up", "down")
    if step == 0 then step = input.step("left", "right") end
    if step ~= 0 then
      menu.target = (menu.target - 1 + step) % #menu.targets + 1
      audio.sfx("cursor", { volume = 0.5 })
    end
    if fe.input.pressed("cancel") then
      menu.mode = menu.back
    elseif fe.input.pressed("confirm") then
      menu.action.target = menu.targets[menu.target]
      decide(self, menu.action)
    end
  elseif menu.mode == "unison" then
    ui.list_input(menu.cursor, #menu.rows)
    if fe.input.pressed("confirm") then decide(self, { echo = menu.rows[menu.cursor.index].id }) end
  end
end

local function choose(self, u, bonus)
  self.choice = nil
  open_commands(self, u, bonus)
  task.wait_until(function() return self.choice ~= nil end)
  return self.choice
end

local function choose_unison(self, echoes)
  local rows = {}
  for _, id in ipairs(echoes) do
    local echo = data.get("echoes", id)
    rows[#rows + 1] = { echo.name, id = id, about = data.get("skills", echo.unison).about }
  end
  self.choice = nil
  self.menu = { mode = "unison", rows = rows, cursor = ui.cursor() }
  task.wait_until(function() return self.choice ~= nil end)
  return self.choice.echo
end

-- ------------------------------------------------------------ the scene

function Scene:update(dt)
  for i = #self.floats, 1, -1 do
    self.floats[i].age = self.floats[i].age + dt
    if self.floats[i].age > 1.1 then table.remove(self.floats, i) end
  end
  if self.banner then
    self.banner.left = self.banner.left - dt
    if self.banner.left <= 0 then self.banner = nil end
  end
  if self.menu then menu_input(self) end
  if self.waiting_confirm and fe.input.pressed("confirm") then self.waiting_confirm = false end
end

function Scene:post_update()
  if self.world then self.world:events() end -- nothing here listens; keep the queue empty
end

local function draw_timeline(self)
  local now, later = self.rules:timeline()
  local x = 6
  local function chip(u, dim)
    local color = u.side == rules.ALLIES and ui.colors.ally or ui.colors.foe
    if u.broken then color = ui.colors.dim end
    fe.canvas.push({ alpha = dim and 0.55 or 1 })
    fe.canvas.rect(x, 4, 46, 11, 0x10141fe0)
    fe.canvas.rect(x, 4, 3, 11, color)
    fe.canvas.push_clip(x + 4, 4, 41, 11)
    ui.text(u.name, x + 5, 5, { color = u == self.acting and ui.colors.accent or ui.colors.text })
    fe.canvas.pop_clip()
    fe.canvas.pop()
    x = x + 48
  end
  if self.acting and rules.alive(self.acting) then chip(self.acting) end
  for _, u in ipairs(now) do chip(u) end
  ui.text("|", x, 5, { color = ui.colors.dim })
  x = x + 6
  local w = ui.size()
  for _, u in ipairs(later) do
    if x + 48 > w - 4 then break end
    chip(u, true)
  end
end

local function draw_party(self)
  local w, h = ui.size()
  local heroes = {}
  for _, u in ipairs(self.rules.allies) do
    if not u.gone then heroes[#heroes + 1] = u end
  end
  local panel_h = #heroes * 15 + 10
  local x, y = w - 196, h - panel_h - 16
  ui.window(x, y, 192, panel_h)
  for i, u in ipairs(heroes) do
    local ry = y + 6 + (i - 1) * 15
    local color = not rules.alive(u) and ui.colors.bad or (u == self.acting and ui.colors.accent or ui.colors.text)
    ui.text(u.name, x + 8, ry, { color = color })
    ui.text(tostring(u.hp), x + 52, ry, { width = 26, align = "right" })
    ui.bar(x + 82, ry + 2, 40, 4, u.hp / u.max_hp, ui.colors.hp)
    ui.bar(x + 82, ry + 7, 40, 2, u.max_mp > 0 and u.mp / u.max_mp or 0, ui.colors.mp)
    if u.hero then
      ui.bar(x + 128, ry + 2, 30, 4, u.surge / 100, u.surge >= 100 and ui.colors.accent or ui.colors.weak)
      if u.surge >= 100 then ui.text("!", x + 160, ry, { color = ui.colors.accent }) end
    else
      ui.text(tostring(math.max(0, u.turns_left + 1)) .. "t", x + 128, ry, { color = ui.colors.dim })
    end
    local marks = ""
    for id in pairs(u.status) do marks = marks .. data.get("statuses", id).name:sub(1, 1) end
    if u.defending then marks = marks .. "D" end
    ui.text(marks, x + 168, ry, { color = ui.colors.weak })
  end
end

local function draw_foes(self, alpha)
  for _, u in ipairs(self.rules.foes) do
    local e = self.entities[u]
    if e and e:valid() and rules.alive(u) then
      local sx, sy, visible = e:screen_position(is_model(u) and -0.1 or -0.05, alpha)
      if visible then
        ui.bar(sx - 16, sy + 2, 32, 3, u.hp / u.max_hp, ui.colors.foe)
        if u.break_max > 0 then
          -- The break gauge: one pip per point, emptied as it is chipped.
          local pip = math.min(4, math.floor(32 / u.break_max))
          for i = 1, u.break_max do
            local filled = i <= u.break_left
            fe.canvas.rect(sx - 16 + (i - 1) * pip, sy + 7, pip - 1, 2,
              u.broken and ui.colors.bad or (filled and ui.colors.weak or ui.colors.gauge))
          end
          if u.broken then ui.text("BROKEN", sx - 16, sy + 11, { color = ui.colors.bad }) end
        end
      end
    end
  end
end

local function draw_floats(self, alpha)
  for _, f in ipairs(self.floats) do
    local e = self.entities[f.unit]
    if e and e:valid() then
      local sx, sy, visible = e:screen_position(1.2, alpha)
      if visible then
        local tw = fe.text.measure(f.text)
        fe.canvas.push({ alpha = math.min(1, (1.1 - f.age) * 3) })
        ui.text(f.text, sx - tw / 2, sy - f.age * 18 - f.row * 10, { color = f.color, outline = 0x000000ff })
        fe.canvas.pop()
      end
    end
  end
end

local function draw_menu(self, alpha)
  local menu = self.menu
  local _, h = ui.size()
  local x = 6
  if menu.mode == "unison" then
    ui.window(x, h - 100, 150, 14)
    ui.text("Choose a Unison", x + 8, h - 97, { color = ui.colors.accent })
    ui.list(x, h - 84, 150, menu.rows, menu.cursor)
    ui.text(menu.rows[menu.cursor.index].about or "", x + 160, h - 80, { width = 150, color = ui.colors.dim })
    ui.hints({ { "confirm", "Unleash" } })
    return
  end
  local list_h = #menu.rows * 12 + 12
  local y = h - list_h - 16
  ui.window(x, y - 15, 96, 14)
  ui.text(menu.unit.name .. (menu.bonus and "  BONUS" or ""), x + 8, y - 12, { color = ui.colors.accent })
  ui.list(x, y, 96, menu.rows, menu.cursor)
  if menu.mode == "skills" or menu.mode == "items" or (menu.mode == "target" and menu.list and menu.back ~= "command") then
    local visible = math.min(6, #menu.list)
    ui.list(x + 100, h - visible * 12 - 28, 150, menu.list, menu.sub, 6)
    local row = menu.list[menu.sub.index]
    if row and row.about then ui.text(row.about, x + 104, h - visible * 12 - 40, { color = ui.colors.dim }) end
  end
  if menu.mode == "target" then
    local target = menu.targets[menu.target]
    local e = self.entities[target]
    if e and e:valid() then
      local sx, sy = e:screen_position(1.5, alpha)
      fe.canvas.image("@fe/assets/ui/cursor.png", sx - 4, sy - 6 + math.sin(fe.time.now() * 8) * 1.5)
      local label = target.name
      if target.side == rules.FOES then
        -- What the party has learned of this foe; unseen elements stay hidden.
        local known = {}
        for _, element in ipairs(ELEMENTS) do
          local affinity = self.rules:known_affinity(target, element)
          if affinity and AFFINITY_WORD[affinity] then known[#known + 1] = AFFINITY_WORD[affinity] .. " " .. element end
        end
        label = label .. "   " .. (#known > 0 and table.concat(known, ", ") or "Affinities unknown")
      end
      local tw = fe.text.measure(label)
      ui.window(x + 100, y - 15, tw + 16, 14)
      ui.text(label, x + 108, y - 12)
    end
    ui.hints({ { "confirm", "Target" }, { "cancel", "Back" } })
  else
    ui.hints({ { "confirm", "Choose" }, { "cancel", "Back" } })
  end
end

function Scene:draw(alpha)
  if not self.world then return end
  self.world:draw()
  draw_timeline(self)
  draw_foes(self, alpha)
  draw_party(self)
  draw_floats(self, alpha)
  local w, h = ui.size()
  if self.banner then
    local tw = fe.text.measure(self.banner.text)
    ui.window((w - tw) / 2 - 10, 22, tw + 20, 18)
    ui.text(self.banner.text, (w - tw) / 2, 27, { color = ui.colors.accent })
  end
  if self.results then
    local r = self.results
    local lines = { "Victory!", "EXP " .. r.exp .. "    Gil " .. r.gold .. "    Mastery " .. r.mastery }
    for _, line in ipairs(r.bonuses) do lines[#lines + 1] = line end
    for _, line in ipairs(r.notes) do lines[#lines + 1] = line end
    local bh = #lines * 12 + 16
    ui.window(w / 2 - 110, h / 2 - bh / 2, 220, bh)
    for i, line in ipairs(lines) do
      ui.text(line, w / 2 - 100, h / 2 - bh / 2 + 8 + (i - 1) * 12, { color = i == 1 and ui.colors.accent or ui.colors.text })
    end
    ui.hints({ { "confirm", "Continue" } })
  elseif self.menu then
    draw_menu(self, alpha)
  end
end

-- ------------------------------------------------------------ running one

local function pay_out(self)
  local rewards = self.rules:rewards()
  rewards.notes = {}
  state.data.gold = state.data.gold + rewards.gold
  for _, u in ipairs(self.rules.allies) do
    if u.hero then
      local record = state.data.heroes[u.id]
      if rules.alive(u) then
        local gained = state.gain_exp(u.id, rewards.exp)
        if gained > 0 then
          rewards.notes[#rewards.notes + 1] = u.name .. " reached level " .. record.level .. "!"
          u.leveled = true
        end
      end
      if u.echo then
        local before = state.echo_rank(u.echo)
        state.data.echoes[u.echo].points = state.data.echoes[u.echo].points + rewards.mastery
        if state.echo_rank(u.echo) > before then
          rewards.notes[#rewards.notes + 1] = data.get("echoes", u.echo).name .. " reached rank " ..
            state.echo_rank(u.echo) .. "!"
        end
      end
    end
  end
  return rewards
end

local function write_back(self)
  for _, u in ipairs(self.rules.allies) do
    if u.hero then
      local record = state.data.heroes[u.id]
      if not u.leveled then
        -- The fallen get up with 1 HP once the battle is over.
        record.hp, record.mp = math.max(1, u.hp), u.mp
      end
      record.surge = u.surge
    end
  end
end

--- Fight `troop`. options: opening, stage. Returns the outcome.
function battle.start(troop_id, options)
  options = options or {}
  local field = require("game.field")
  local troop = data.get("troops", troop_id)
  local party = {}
  for _, id in ipairs(state.data.party) do
    local saved = state.data.heroes[id]
    party[#party + 1] = rules.hero(data.get("heroes", id), state.stats(id), saved, state.echo_rank(saved.echo))
  end
  local self = setmetatable({
    rules = rules.new { party = party, troop = troop_id, opening = options.opening, known = state.data.bestiary,
      consume = function(id)
        if state.item_count(id) <= 0 then return false end
        state.add_item(id, -1)
        return true
      end },
    stage = options.stage or troop.stage or (field.world and field.world:props().battle_stage) or "battle_grass",
    floats = {}, home = {}, entities = {},
  }, Scene)
  battle.active = self
  state.data.battles = state.data.battles + 1
  local music_before = audio.playing

  -- The field freezes on screen and swirls away into the battle.
  fe.screen.freeze()
  audio.music(troop.music or "battle", 0.3)
  build(self)
  scenes.push(self)
  task.over(0.6, function(t) fe.screen.set { transition = { kind = "swirl", progress = t } } end)
  fe.screen.unfreeze()
  fe.screen.set { transition = { kind = "none", progress = 0 } }

  while true do
    local turn = self.rules:advance()
    show(self, turn.events)
    if turn.type == "over" then break end
    if turn.type == "act" then
      self.acting = turn.unit
      local action
      if turn.unit.hero then
        action = choose(self, turn.unit, turn.bonus)
      else
        task.wait(0.35)
        action = self.rules:ai(turn.unit)
      end
      show(self, self.rules:perform(turn.unit, action))
      self.acting = nil
    elseif turn.type == "skip" then
      if turn.reason == "broken" then float(self, turn.unit, "Loses its turn", ui.colors.dim) end
      if turn.reason == "sleep" then float(self, turn.unit, "Asleep", ui.colors.dim) end
      task.wait(0.45)
    elseif turn.type == "unison" then
      show(self, self.rules:unison(choose_unison(self, turn.echoes)))
    elseif turn.type == "round" then
      task.wait(0.15)
    end
    if self.rules.outcome then
      show(self, {})
    end
  end

  local outcome = self.rules.outcome
  if outcome == "victory" then
    audio.music("victory", 0.2)
    for _, u in ipairs(self.rules.allies) do
      if u.hero and rules.alive(u) then animate(self, u, "victory") end
    end
    self.results = pay_out(self)
    self.waiting_confirm = true
    task.wait(0.4)
    task.wait_until(function() return not self.waiting_confirm end)
  end
  write_back(self)
  task.over(0.3, function(t) fe.screen.set { fade = { 0, 0, 0, t } } end)
  scenes.pop(self)
  fe.world.unload("battle")
  battle.active = nil
  if outcome == "defeat" then
    fe.screen.set { fade = { 0, 0, 0, 0 } }
    require("game.title").game_over()
    return outcome
  end
  audio.music(music_before, 0.4)
  task.over(0.3, function(t) fe.screen.set { fade = { 0, 0, 0, 1 - t } } end)
  return outcome
end

return battle

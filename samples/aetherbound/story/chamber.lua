--- Scenes 2 and 3 and the boss battle between and after them: The Weapon
--- Awakens, the Aetherbound Colossus, The Breaking Point, The Last
--- Resonance. Ends when the player chooses LAST LIGHT.
local actors = require("game.actors")
local audio = require("game.audio")
local battle = require("game.battle.scene")
local common = require("story.common")
local dialogue = require("game.dialogue")
local director = require("game.director")
local field = require("game.field")
local fx = require("game.fx")
local party = require("game.party")
local task = require("game.task")
local ui = require("game.ui")
local vfx = require("game.vfx")

local say, system, wait = common.say, common.system, common.wait
local chamber = { card = nil, countdown = nil }

local BATTLE_CAMERA = { target = { x = 16, y = 1.7, z = 16.4 }, yaw = 0, pitch = 19, distance = 25, fov = 30 }

--- What the Colossus's model file says about itself (eye and core
--- positions, which way is east), with defaults while it is not there.
local function colossus_info()
  local info = fe.fs.exists("models/colossus.json") and fe.json.read("models/colossus.json") or {}
  info.yaw_east = info.yaw_east or info.yaw_plus_x or (info.yaw and (info.yaw.east or info.yaw["+x"])) or 90
  info.core = info.core or { 0, 3.4, 0.6 }
  info.head = info.head or { 0, 5.6, 0.8 }
  return info
end

--- A model-space point of the Colossus in the world (it faces east).
local function boss_point(boss, offset)
  local p = boss.position
  -- Facing +x: the model's forward (+z) lies along world +x, its right (+x) along world -z.
  return { x = p.x + (offset[3] or 0), y = p.y + (offset[2] or 0), z = p.z - (offset[1] or 0) }
end

local function draw_card()
  local card = chamber.card
  if not card then return end
  local alpha = math.min(1, card.age * 2.5, (card.life - card.age) * 2)
  fe.canvas.push({ alpha = math.max(0, alpha) })
  fe.canvas.rect(0, 380, 1920, 320, 0x05020ccc)
  fe.canvas.rect(0, 380, 1920, 3, ui.colors.aether)
  fe.canvas.rect(0, 697, 1920, 3, ui.colors.aether)
  ui.centred(card.small, 960, 410, { font = "label", color = ui.colors.dim })
  ui.centred(card.title, 960, 458, { font = "title", size = 120, color = card.color or ui.colors.aether_hot })
  ui.centred(card.note, 960, 620, { font = "small", color = ui.colors.text })
  fe.canvas.pop()
end

--- A title card across the middle of the screen for `seconds`.
local function show_card(small, title, note, seconds, color)
  chamber.card = { small = small, title = title, note = note, age = 0, life = seconds, color = color }
  task.over(seconds, function(t) chamber.card.age = t * seconds end)
  chamber.card = nil
end

local function draw_countdown()
  local left = chamber.countdown
  if not left then return end
  local text = string.format("%02d:%02d", math.floor(left / 60), math.floor(left % 60))
  local urgent = left < 60 and (0.6 + 0.4 * math.sin(ui.time * 8)) or 1
  ui.panel(1560, 150, 310, 110, "system")
  ui.text("TOTAL RELEASE", 1596, 164, { font = "mono", size = 24, color = ui.colors.system })
  ui.text(text, 1622, 190, { font = "mono", size = 56, color = ui.fade(0xffd0d0ff, urgent) })
end

function chamber.draw()
  draw_card()
end

function chamber.update(dt)
  if chamber.countdown then chamber.countdown = math.max(0, chamber.countdown - dt) end
end

-- ------------------------------------------------------------- the scenes

function chamber.run()
  -- Into the tower.
  fx.fade(1, 0.9)
  audio.music(nil, 2.5)
  audio.ambience(nil, 2.5)
  audio.ambience("hum", 2, "hum")
  audio.volume("hum", 1, 2)
  local world = field.enter("chamber", "start")
  local cael, rowan, lyra, darius = common.cast()
  local points = world:points()
  local info = colossus_info()
  local boss = world:spawn { id = "colossus", position = points.boss.position, yaw = info.yaw_east,
    mesh = { model = "models/colossus.glb", animation = "dormant", loop = true } }
  boss:play("dormant", true)
  local glow = world:spawn { position = boss_point(boss, info.core), keep_height = true,
    light = { color = { 0.6, 0.32, 1.0 }, radius = 10, intensity = 1.6, flicker = 0.3, flicker_speed = 1.2 } }
  fx.letterbox(1, 0)
  field.camera_follow = false
  fx.camera(world, { target = { x = 26, y = 0.5, z = 16.4 }, yaw = 0, pitch = 14, distance = 10, fov = 30 }, 0)
  task.spawn(function() fx.fade(0, 1.4) end, "fade in")

  -- The party enters; the camera lets go of them and finds the machine.
  common.camera_async({ target = { x = 17, y = 1.3, z = 16.4 }, pitch = 18, distance = 27 }, 6.5)
  director.walk_all {
    { cael, points.intro1, { speed = 1.9, face = { x = -1, z = 0 } } },
    { rowan, points.intro2, { speed = 1.9, face = { x = -1, z = 0 } } },
    { lyra, points.intro3, { speed = 1.9, face = { x = -1, z = 0 } } },
    { darius, points.intro4, { speed = 1.9, face = { x = -1, z = 0 } } },
  }
  wait(1.2)
  say("Lyra", "An Aetherbound Colossus...")
  say("Darius", "That thing's supposed to be a myth.")
  common.camera_async({ target = boss_point(boss, { 0, 1.6, 0 }), pitch = 10, distance = 13 }, 3)
  say("Lyra", "They've been feeding it raw Aetherite.")
  director.face(cael, lyra)
  common.camera_async({ target = { x = 23.6, y = 0.45, z = 16.2 }, pitch = 12, distance = 9 }, 1.6)
  say("Cael", "Can we shut it down?")
  director.face(lyra, cael)
  dialogue.say("Lyra", "Perhaps. If we sever its core\u{2014}", { auto = 0.25 })

  -- The machine's eyes ignite.
  common.camera({ target = boss_point(boss, info.head), pitch = 4, distance = 7.5 }, 0.35, fx.ease_out)
  common.all_face({ x = -1, z = 0 })
  boss:play("awaken", false)
  wait(0.6)
  audio.sfx("eyes_ignite", { volume = 1 })
  fx.flash({ 0.7, 0.45, 1.0 }, 0.5, 0.5)
  fx.shake(world, 0.15, 0.8)
  glow:light({ intensity = 4, radius = 14 })
  wait(0.5)
  audio.sfx("system_alert", { volume = 0.8 })
  common.line("SYSTEM", "INTRUDERS DETECTED.", 1.5)
  audio.sfx("system_alert", { volume = 0.8, pitch = 0.9 })
  common.line("SYSTEM", "PURIFICATION PROTOCOL ACTIVE.", 1.7)

  -- The Colossus rises. The chamber begins collapsing.
  local rumble = fx.shake(world, 0.07, 1, true)
  audio.sfx("rumble", { volume = 0.9 })
  local stop_collapse = common.collapse(world, points.centre.position, 10, 2.4)
  common.camera({ target = { x = 15, y = 2.6, z = 16 }, pitch = 7, distance = 21 }, 2.2)
  audio.sfx("colossus_step", { volume = 1 })
  fx.shake(world, 0.3, 0.6)
  wait(0.8)
  fx.stop_shake(rumble)
  boss:play("idle", true, 0.4)
  common.camera_async({ target = { x = 23.6, y = 0.45, z = 16.2 }, pitch = 12, distance = 9 }, 1.2)
  say("Darius", "Of course it talks.")
  dialogue.say("Cael", "Everyone! Weapons ready!", { shout = true })
  for _, hero in pairs(field.actors) do actors.pose(hero, "ready") end
  task.spawn(function() fx.letterbox(0, 0.6) end, "bars")

  -- Phase 1: until half of its HP is gone.
  battle.start { world = world, troop = "colossus", heroes = field.actors, enemies = { boss }, transition = false,
    camera = BATTLE_CAMERA, quiet = true,
    script = {
      setup = function(scene) scene.boss.threshold = math.floor(scene.boss.max_hp * 0.5) end,
    } }

  -- Scene 3: the breaking point.
  fx.letterbox(1, 0.5)
  boss:play("stagger", false, 0.2)
  audio.sfx("colossus_step", { volume = 0.9 })
  fx.shake(world, 0.25, 0.8)
  common.camera_async({ target = { x = 15.5, y = 1.6, z = 16 }, pitch = 12, distance = 20 }, 2.0)
  wait(1.6)
  say("Darius", "We've got it!")
  dialogue.say("Lyra", "No! The core is\u{2014}", { auto = 0.2 })

  -- The crystal explodes with light. The party is thrown backward.
  local core = boss_point(boss, info.core)
  audio.music(nil, 0.2)
  audio.sfx("overload_blast", { volume = 1 })
  fx.flash({ 0.9, 0.8, 1.0 }, 1.6, 1.0)
  fx.shake(world, 0.7, 1.4)
  fe.input.rumble(1, 1, 0.8)
  vfx.ring(world, core, { color = { 0.8, 0.5, 1.0 }, scale = 30, seconds = 0.9 })
  vfx.shockwave(world, core, { scale = 26, seconds = 0.9, strength = 0.9 })
  vfx.burst(world, core, "shard", 60)
  glow:light({ intensity = 9, radius = 22, flicker = 0.6, flicker_speed = 5 })
  local throws = {}
  for index, id in ipairs(party.order) do
    local hero = field.actors[id]
    actors.pose(hero, "thrown")
    throws[#throws + 1] = director.slide_async(hero, points["thrown" .. index], 0.55, { arc = 0.9, ease = fx.ease_out })
  end
  boss:play("overload_enter", false, 0.2)
  task.wait_until(function()
    for _, throw in ipairs(throws) do
      if not task.done(throw) then return false end
    end
    return true
  end)
  for _, hero in pairs(field.actors) do actors.pose(hero, "ko") end
  common.camera_async({ target = { x = 15, y = 2.8, z = 16 }, pitch = 6, distance = 21 }, 2.6)
  wait(2.6)
  boss:play("overload", true, 0.3)
  audio.sfx("klaxon", { volume = 0.7 })
  show_card("AETHERBOUND COLOSSUS", "OVERLOAD", "Core regenerating  \u{00b7}  All attacks ineffective", 3.6)
  audio.sfx("system_alert", { volume = 0.8 })
  common.line("SYSTEM", "CORE INSTABILITY.", 1.6)
  audio.sfx("system_alert", { volume = 0.8, pitch = 0.9 })
  audio.music("overload", 1.0)
  common.line("SYSTEM", "TOTAL RELEASE IN 180 SECONDS.", 2.0)

  -- They get up.
  common.camera_async({ target = { x = 24.4, y = 0.45, z = 16.2 }, pitch = 12, distance = 9.5 }, 1.6)
  for _, hero in pairs(field.actors) do actors.pose(hero, "rise") end
  wait(1.1)
  for _, hero in pairs(field.actors) do actors.pose(hero, "ready") end
  director.face(cael, lyra)
  dialogue.say("Cael", "Lyra! How do we stop it?!", { shout = true })
  director.face(lyra, cael)
  say("Lyra", "We can't! It's drawing Aether from beneath the fortress!")
  director.face(darius, cael)
  say("Darius", "Then we fall back!")
  director.face(cael, darius)
  say("Cael", "And let it destroy everything?!")
  director.face(cael, lyra)
  say("Lyra", "Cael... The blast will reach the city.")

  -- The music drops to silence. Rowan steps forward.
  audio.music(nil, 1.6)
  wait(2.2)
  local r = rowan.position
  actors.pose(rowan, nil)
  common.camera_async({ target = { x = r.x - 0.6, y = 0.45, z = r.z + 0.2 }, pitch = 9, distance = 8.5 }, 2.6)
  director.walk(rowan, { x = r.x - 1.8, z = r.z }, { speed = 0.9, face = { x = -1, z = 0 } })
  wait(0.4)
  say("Rowan", "...There is a way.")
  common.all_face(rowan)
  director.face(rowan, { x = -1, z = 0 })
  say("Cael", "Rowan?")
  say("Rowan", "I can reach the core.")
  say("Lyra", "Without a conduit? That's impossible.")
  say("Rowan", "Not for me.")

  -- Faint violet light appears beneath Rowan's skin.
  audio.sfx("crystal_pulse", { volume = 0.5, pitch = 1.3 })
  common.bind_rowan(rowan, 1)
  vfx.aura(world, rowan, { color = { 0.7, 0.4, 1.0 }, seconds = 2.2, rate = 18 })
  wait(1.8)
  say("Lyra", "You... You're Aether-Bound?")
  say("Cael", "Since when?")
  say("Rowan", "...Since we were children.")
  -- Cael turns toward him.
  local c = cael.position
  director.walk(cael, { x = c.x - 0.8, z = c.z }, { speed = 1.2 })
  director.face(cael, rowan)
  say("Cael", "Why didn't you tell me?")
  director.face(rowan, cael)
  say("Rowan", "Because you would've tried to save me.")
  -- Rowan draws his sword.
  director.face(rowan, { x = -1, z = 0 })
  actors.pose(rowan, "ready")
  audio.sfx("slash", { volume = 0.5, pitch = 0.7 })
  wait(0.6)
  say("Rowan", "And I needed you to let me protect you.")
  say("Cael", "Don't you dare.")
  -- Rowan smiles.
  director.face(rowan, cael)
  wait(0.8)
  say("Rowan", "Still giving orders?")
  director.face(rowan, { x = -1, z = 0 })

  -- The battle resumes: The Last Resonance.
  audio.music("overload", 0.8)
  task.spawn(function() fx.letterbox(0, 0.6) end, "bars")
  local round, chosen = 0, false
  local flicker
  chamber.countdown = 180
  local _, scene = battle.start { world = world, troop = "colossus", heroes = field.actors, enemies = { boss },
    transition = false, music = false, camera = BATTLE_CAMERA, quiet = true, keep_hud = true,
    script = {
      setup = function(s)
        local unit = s.boss
        unit.hp = math.floor(unit.max_hp * 0.5)
        unit.invulnerable = true
        unit.pattern = { "lash", "cascade" }
        unit.avoid = "rowan"
        unit.spd = 30
        s.rules.no_ko = true
        s.boss_note = "OVERLOAD  \u{00b7}  Core regenerating  \u{00b7}  All attacks ineffective"
        s.boss_color = ui.colors.aether
        s.boss_idle = "overload"
        round, chosen = 0, false
        for _, hero in ipairs(s.rules.heroes) do
          if hero.id == "rowan" then
            hero.only, hero.only_reason = "unbound", "Nothing else will reach it."
            hero.haste = 0.62
            hero.gauge = 0.35
            s.poses[hero] = "ready"
          end
        end
      end,
      commands = function(_, unit)
        if unit.id ~= "rowan" then return nil end
        if round >= 2 then return { { label = "LAST LIGHT", skill = "last_light", color = "aether_hot" } } end
        return {
          { label = "Attack", skill = "attack" },
          { label = "Unbound Resonance", skill = "unbound", color = "aether" },
          { label = "Guard", skill = "guard" },
        }
      end,
      before_turn = function(s, unit)
        -- At his third turn the only thing left to him is the last one.
        if unit.id == "rowan" and round >= 2 then unit.only = "last_light" end
        return false
      end,
      on_script = function(s, unit, skill)
        if skill == "last_light" then
          chosen = true
          return
        end
        round = round + 1
        local e = s.entities[unit]
        actors.pose(e, "cast")
        vfx.aura(world, e, { color = { 0.75, 0.45, 1.0 }, seconds = 1.6, rate = 70 })
        vfx.circle(world, e.position, { color = { 0.7, 0.45, 1.0 }, seconds = 1.6, scale = 1.6 })
        fx.camera_async(world, { target = { x = e.position.x - 2, y = 1.0, z = 16.4 }, distance = 16 }, 1.0)
        if round == 1 then
          -- Round 1: Rowan's veins glow. His HP falls to 50%.
          audio.sfx("unbound_1", { volume = 1 })
          common.bind_rowan(e, 2.2)
          fx.flash({ 0.7, 0.45, 1.0 }, 0.6, 0.35)
          fx.shake(world, 0.15, 0.8)
          task.wait(1.0)
          local to = math.floor(unit.max_hp * 0.5)
          battle.float(s, unit, tostring(unit.hp - to), ui.colors.aether, true)
          unit.hp = to
          battle.banner(s, "The Aether floods into him", 2.0, ui.colors.aether_hot)
          dialogue.bark(cael, "Rowan, your arms!", 2.4, "Cael")
        else
          -- Round 2: his sprite flickers. His HP falls to 1.
          audio.sfx("unbound_2", { volume = 1 })
          common.bind_rowan(e, 3.6)
          flicker = common.flicker(e, 600)
          fx.flash({ 0.8, 0.6, 1.0 }, 0.8, 0.55)
          fx.shake(world, 0.3, 1.2)
          vfx.burst(world, battle.body(s, unit), "mote", 40)
          task.wait(1.0)
          battle.float(s, unit, tostring(unit.hp - 1), ui.colors.aether, true)
          unit.hp = 1
          battle.banner(s, "He cannot hold much more", 2.0, ui.colors.aether_hot)
          dialogue.bark(lyra, "No one can carry that much!", 2.4, "Lyra")
        end
        unit.corruption = math.min(100, 50 + round * 25)
        task.wait(0.9)
        fx.camera_async(world, BATTLE_CAMERA, 0.8)
        s.poses[unit] = round >= 2 and "critical" or "ready"
      end,
      tick = function(s, dt)
        -- The clock cannot run out on the player: near the end Rowan is simply ready.
        if chamber.countdown and chamber.countdown < 45 then
          for _, hero in ipairs(s.rules.heroes) do
            if hero.id == "rowan" then hero.haste = 4 end
          end
        end
      end,
      outcome = function() return chosen and "last_light" or nil end,
      draw = draw_countdown,
      reset = function() chamber.countdown = 180 end,
    } }
  chamber.countdown = nil
  stop_collapse()
  return { world = world, boss = boss, glow = glow, info = info, scene = scene, flicker = flicker,
    boss_point = function(offset) return boss_point(boss, offset) end }
end

return chamber

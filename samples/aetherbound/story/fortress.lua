--- Scene 1, The Fallen Fortress: the opening on the shattered bridge, then
--- the walk through the burning courtyard with its two fights.
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

local say, wait = common.say, common.wait
local fortress = {}

--- Guards who have already lost: they wait where the map says a fight is.
local function post_guards(world, prefix, kinds)
  local points = world:points()
  local list = {}
  for index, kind in ipairs(kinds) do
    local point = points[prefix .. "enemy" .. index]
    local enemy = fe.json.read("data/enemies.json")[kind]
    list[index] = actors.spawn(world, prefix .. kind .. index, enemy.sprite, point.position, point.facing,
      { clip = "critical" })
  end
  return list
end

--- The war going on around the party: far explosions, and the crystal's
--- pulse every so often. Runs until the returned function is called.
local function war(world)
  local rng = fe.random.new(3)
  local running = task.spawn(function()
    while true do
      task.wait(rng:range(5, 11))
      if rng:chance(0.3) then
        common.crystal_pulse(0.6)
      else
        audio.sfx("explosion_far", { volume = rng:range(0.3, 0.6), pan = rng:range(-0.8, 0.8),
          pitch = rng:range(0.8, 1.15) })
        fx.shake(world, 0.03, 0.5)
      end
    end
  end, "war")
  return function() task.cancel(running) end
end

--- A fight at one of the map's two spots. `intro` runs before the swords
--- come out, `script` shapes the battle (the tutorials).
local function fight(world, prefix, troop, guards, intro, script)
  field.lock()
  for _, guard in ipairs(guards) do actors.pose(guard, "ready") end
  if intro then intro() end
  local points = world:points()
  local centre = points[prefix .. "centre"].position
  battle.start { world = world, troop = troop, prefix = prefix, heroes = field.actors, enemies = guards,
    camera = { target = { x = centre.x, y = 1.2, z = centre.z }, yaw = 0, pitch = 16, distance = 12.5 },
    script = script }
  for _, guard in ipairs(guards) do actors.remove(guard) end
  audio.music("fortress", 1.5)
  field.unlock()
end

function fortress.run()
  fx.fade(1, 0)
  fx.letterbox(1, 0)
  local world = field.enter("fortress", { cael = "cael", rowan = "rowan", lyra = "lyra", darius = "darius" })
  field.lock()
  local cael, rowan, lyra, darius = common.cast()
  local crystal = common.spawn_crystal(world)
  local guards1 = post_guards(world, "enc1_", { "guard", "guard" })
  local guards2 = post_guards(world, "enc2_", { "guard", "lancer", "guard" })
  local camera = world:camera()
  camera.far = 600

  -- Black screen. Distant explosions. Steel clashing. A low, unnatural hum.
  audio.ambience("war", 3)
  audio.ambience("hum", 4, "hum")
  audio.volume("hum", 0.5, 0)
  wait(1.6)
  audio.sfx("explosion_far", { volume = 0.7, pan = -0.5 })
  wait(1.8)
  audio.sfx("explosion_far", { volume = 0.5, pan = 0.6, pitch = 0.85 })
  wait(1.4)

  -- The screen fades in: the fortress first, burning, the crystal over it;
  -- then down and back to the four on the bridge.
  local crystal_at = common.point("crystal")
  fx.camera(world, { target = { x = 16, y = crystal_at.y - 14, z = 22 }, yaw = 0, pitch = 3, distance = 46, fov = 30 }, 0)
  task.spawn(function() fx.fade(0, 3.5) end, "fade in")
  common.camera({ target = { x = 16, y = 9, z = 40 }, pitch = 6, distance = 34 }, 6.5)
  common.camera({ target = { x = 16, y = 2.4, z = 61.5 }, pitch = 13, distance = 13.5 }, 6.5)

  director.face(darius, cael)
  say("Darius", "That's the last of their defenses.")
  director.face(lyra, { x = 0, z = -1 })
  say("Lyra", "No... Something's wrong.")
  director.face(cael, lyra)
  say("Cael", "What is it?")
  say("Lyra", "The Aether readings. They're still climbing.")

  -- The crystal pulses. The screen shakes.
  common.camera({ target = { x = 16, y = crystal_at.y - 6, z = 20 }, pitch = 2, distance = 40 }, 1.6)
  common.crystal_pulse(1.6)
  common.all_face({ x = 0, z = -1 })
  wait(1.6)
  common.camera({ target = { x = 16, y = 2.2, z = 62 }, pitch = 12, distance = 11 }, 1.8)

  say("Rowan", "Then we destroy it.")
  director.face(cael, rowan)
  say("Cael", "And the soldiers inside?")
  director.face(rowan, cael)
  say("Rowan", "...")
  say("Cael", "We didn't come this far just to bury more people.")
  director.face(darius, cael)
  say("Darius", "Your Highness. We don't have much time.")

  -- Rowan walks toward the fortress.
  local ahead = { x = 16.4, z = 57.2 }
  common.camera_async({ target = { x = 16, y = 2.0, z = 59.5 }, pitch = 14, distance = 11.5 }, 3)
  director.walk(rowan, ahead, { speed = 1.5 })
  say("Rowan", "Then we'd better hurry.")

  -- Cael follows.
  director.walk(cael, { x = 15.4, z = 58.4 }, { speed = 2.2 })
  director.face(cael, rowan)
  say("Cael", "You always this eager to die?")
  director.face(rowan, cael)
  say("Rowan", "Only when you're leading.")
  common.laugh(cael)
  audio.music("fortress", 4)
  say("Cael", "Just like old times.")
  director.face(rowan, { x = 0, z = -1 })
  say("Rowan", "...Just like old times.")

  -- The others catch up, and the player has Cael.
  director.walk_all {
    { lyra, { x = 14.8, z = 59.8 }, { speed = 2.6 } },
    { darius, { x = 16.6, z = 60.2 }, { speed = 2.6 } },
  }
  task.spawn(function() fx.letterbox(0, 1.0) end, "bars")
  field.unlock()
  fortress.prompt = 7 -- seconds the movement prompt stays up
  ui.caption("Aetherite Fortress", 5.5)
  local stop_war = war(world)

  -- Things said on the way, where the party passes them.
  local remarks = {
    { z = 51, who = darius, name = "Darius", text = "The gate was blown out from the inside." },
    { z = 31, who = lyra, name = "Lyra", text = "These readings... the whole fortress is saturated." },
    { z = 21, who = rowan, name = "Rowan", text = "The tower. Stay behind me." },
  }
  local chatter = task.spawn(function()
    while #remarks > 0 do
      task.wait(0.25)
      if field.locked == 0 and cael:valid() and cael.position.z < remarks[1].z then
        local remark = table.remove(remarks, 1)
        dialogue.bark(remark.who, remark.text, 3.4, remark.name)
      end
    end
  end, "remarks")

  -- First fight: two guards who should not still be standing.
  field.on_zone.enc1 = function()
    fight(world, "enc1_", "gate", guards1, function()
      dialogue.bark(guards1[1], "Intruders... hold the line!", 2.2)
      wait(0.9)
      dialogue.bark(darius, "Stragglers. Weapons out!", 2.0, "Darius")
      wait(1.1)
    end, {
      first_turn = function(scene)
        scene:tutorial("Active Time Battle",
          "Each fighter's gauge fills with time. When a hero's gauge is full, choose a command: "
            .. "time waits while you decide. Enemies act when their own gauges fill.")
      end,
    })
    dialogue.bark(lyra, "They could barely stand. What happened in here?", 3.2, "Lyra")
  end

  -- Second fight: Rowan shows what he can do.
  field.on_zone.enc2 = function()
    local explained = false
    fight(world, "enc2_", "yard", guards2, function()
      dialogue.bark(guards2[2], "For the Fortress!", 2.0)
      wait(1.0)
    end, {
      hero_turn = function(scene, unit)
        if unit.id == "rowan" and not explained then
          explained = true
          scene:tutorial("Resonance",
            "Rowan can draw raw Aetherite through his blade. Resonance deals immense damage, "
              .. "but every use raises his corruption.")
        end
      end,
    })
    if (party.state.rowan.corruption or 0) > 0 then
      dialogue.bark(cael, "Rowan. Your hand...", 2.6, "Cael")
      wait(2.0)
      dialogue.bark(rowan, "It's nothing. Keep moving.", 2.6, "Rowan")
    else
      dialogue.bark(darius, "The tower. Move!", 2.4, "Darius")
    end
  end

  -- The tower's door.
  local entered = false
  field.on_zone.to_chamber = function() entered = true end
  task.wait_until(function() return entered end)
  stop_war()
  task.cancel(chatter)
  field.lock()
  if crystal and crystal:valid() then common.crystal = nil end
end

--- The row of movement prompts shown for a few seconds once the player has control.
function fortress.update(dt)
  if fortress.prompt then
    fortress.prompt = fortress.prompt - dt
    if fortress.prompt <= 0 then fortress.prompt = nil end
  end
end

return fortress

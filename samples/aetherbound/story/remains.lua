--- Scene 5, What Remains: the chamber in ruins before dawn, and the end of
--- the slice.
local actors = require("game.actors")
local audio = require("game.audio")
local common = require("story.common")
local dialogue = require("game.dialogue")
local director = require("game.director")
local field = require("game.field")
local fx = require("game.fx")
local task = require("game.task")

local say, wait = common.say, common.wait
local remains = {}

--- A line the player may advance, that also moves on by itself after a
--- pause: this is an ending, not a conversation.
local function line(speaker, text, hold, options)
  options = options or {}
  options.auto = hold or math.max(2.6, #text * 0.085)
  say(speaker, text, options)
end

function remains.run()
  -- Black screen. The sound of wind.
  fx.fade(1, 0, { 0, 0, 0 })
  fx.letterbox(1, 0)
  local world = field.enter("ruins", { cael = "cael_wake", rowan = "sword", lyra = "intro3", darius = "darius_wake" })
  field.lock()
  field.camera_follow = false
  local cael, rowan, lyra, darius = common.cast()
  local points = world:points()
  -- Rowan is not here.
  actors.remove(rowan)
  field.actors.rowan = nil
  local sword = world:spawn { position = points.sword.position,
    sprite = { animation = "sprites/sword_broken.anim.json", mode = "flat", cast_shadow = false, depth_bias = 0.05 } }
  actors.pose(cael, "ko")
  actors.pose(darius, "kneel")
  director.face(darius, cael)
  local camera = world:camera()
  camera.far = 900
  audio.ambience("wind", 4)
  wait(3.5)

  -- Slowly, the screen fades in. Violet particles drift through the air like snow.
  fx.camera(world, { target = { x = 15, y = 5, z = 14 }, yaw = 0, pitch = 16, distance = 30, fov = 30 }, 0)
  audio.music("remains", 0.5)
  task.spawn(function() fx.fade(0, 5) end, "fade in")
  common.camera({ target = { x = 20, y = 0.8, z = 16.8 }, pitch = 18, distance = 15 }, 9)
  common.camera_async({ target = { x = 23.2, y = 0.4, z = 17.3 }, pitch = 12, distance = 8 }, 3)
  wait(1.5)

  line("Darius", "Your Highness! You're alive!", 2.8)
  -- Cael slowly stands.
  cael:sprite({ speed = 0.45 })
  actors.pose(cael, "rise")
  wait(2.4)
  cael:sprite({ speed = 1 })
  actors.pose(cael, nil)
  actors.pose(darius, nil)
  director.face(cael, { x = -1, z = 0 })
  wait(0.8)
  line("Cael", "...Rowan?", 2.4)
  -- No answer.
  common.camera_async({ target = { x = 17, y = 0.8, z = 16.6 }, pitch = 16, distance = 20 }, 3)
  wait(3.4)
  line("Cael", "Rowan!", 2.0)

  -- He runs across the chamber. Lyra is already by the crater.
  director.walk(lyra, points.lyra_kneel, { speed = 2.2, wait = false, timeout = 20 })
  common.camera_async({ target = { x = 15, y = 0.5, z = 16.8 }, pitch = 14, distance = 13 }, 3.2)
  director.walk(cael, points.cael_run_end, { speed = 4.6 })
  director.face(cael, { x = -1, z = 0 })
  dialogue.say("Cael", "ROWAN!!", { shout = true, auto = 2.6 })
  wait(1.4)

  -- Lyra kneels beside a scorched sword.
  task.wait_until(function() return not lyra:moving() end)
  director.face(lyra, points.sword.position and { x = points.sword.position.x - lyra.position.x,
    z = points.sword.position.z - lyra.position.z })
  actors.pose(lyra, "kneel")
  common.camera_async({ target = { x = 13.4, y = 0.35, z = 16.3 }, pitch = 13, distance = 7.5 }, 2.6)
  wait(1.2)
  line("Lyra", "...Cael.", 2.4)
  -- Cael approaches. Rowan's sword lies broken in two.
  director.walk(cael, { x = points.sword.position.x + 0.9, z = points.sword.position.z + 0.5 }, { speed = 1.1 })
  director.face(cael, { x = -1, z = 0 })
  common.camera_async({ target = { x = points.sword.position.x + 0.3, y = 0.1, z = points.sword.position.z + 0.3 },
    pitch = 20, distance = 6 }, 2.4)
  wait(1.4)
  line("Cael", "No...", 2.4)
  -- He picks it up.
  actors.pose(cael, "kneel")
  wait(1.3)
  audio.sfx("sword_fall", { volume = 0.5, pitch = 1.1 })
  sword:destroy()
  local held = world:spawn { position = cael.position,
    sprite = { animation = "sprites/sword_half.anim.json", mode = "standing", cast_shadow = false, depth_bias = 0.2 } }
  held:attach(cael, { x = -0.32, y = 0.42, z = 0.05 })
  actors.pose(cael, nil)
  wait(1.0)
  line("Cael", "He was right here.", 3)
  line("Lyra", "...", 2.2)
  dialogue.say("Cael", "He was RIGHT HERE!", { shout = true, auto = 2.8 })
  -- Long pause.
  common.camera_async({ target = { x = 13.6, y = 0.4, z = 16.4 }, pitch = 14, distance = 9 }, 4)
  wait(3.8)
  line("Lyra", "I couldn't detect his Aether signature.", 4)
  director.face(cael, lyra)
  line("Cael", "Then look again.", 2.6)
  line("Lyra", "Cael...", 2.4)
  dialogue.say("Cael", "LOOK AGAIN!", { shout = true, auto = 2.6 })
  -- Lyra lowers her head. The music stops.
  lyra:sprite({ offset = { 0, 2 } })
  audio.music(nil, 3)
  director.walk(darius, { x = cael.position.x + 1.6, z = cael.position.z + 0.6 }, { speed = 1.2, wait = false })
  wait(4.2)

  director.face(darius, cael)
  line("Darius", "...The city is safe.", 3.2)
  -- Cael doesn't answer.
  director.face(cael, { x = -1, z = 0 })
  wait(3.0)
  line("Darius", "He saved thousands.", 3.2)
  line("Cael", "...", 2.8)
  line("Cael", "He was supposed to see the world.", 4.6)

  -- Cael kneels, holding the broken sword.
  held:destroy()
  actors.pose(cael, "hold")
  wait(3.2)

  -- The camera slowly pans upward, past the ruined tower. High above, a
  -- single violet star flickers.
  local star = world:spawn { position = { x = 15, y = 62, z = -46 }, keep_height = true,
    sprite = { animation = "fx/battle/star.anim.json", mode = "facing", blend = "additive", unlit = true, scale = 2.6,
      tint = { 2.2, 1.5, 3.4, 0 }, cast_shadow = false } }
  local twinkle = task.spawn(function()
    local t = 0
    while star:valid() do
      t = t + task.frame()
      local appear = math.min(1, math.max(0, (t - 7) / 4))
      local shimmer = 0.75 + 0.25 * math.sin(t * 5.1) * math.sin(t * 1.7)
      star:sprite({ tint = { 2.2, 1.5, 3.4, appear * shimmer }, scale = 2.6 * (0.9 + 0.2 * shimmer) })
    end
  end, "star")
  common.camera({ target = { x = 14, y = 16, z = 6 }, pitch = -32, distance = 34 }, 15)
  audio.sfx("star", { volume = 0.7 })
  wait(4.5)

  -- Cut to black. End of vertical slice.
  fx.fade(1, 0)
  task.cancel(twinkle)
  audio.ambience(nil, 3)
  wait(3)
end

return remains

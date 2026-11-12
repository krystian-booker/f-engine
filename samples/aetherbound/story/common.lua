--- What the story's scenes share: the cast, short names for the things
--- every scene does, and the effects that belong to the story rather than
--- to battles in general (the crystal over the tower, Rowan's light, the
--- chamber coming down).
local actors = require("game.actors")
local audio = require("game.audio")
local dialogue = require("game.dialogue")
local director = require("game.director")
local field = require("game.field")
local fx = require("game.fx")
local task = require("game.task")
local vfx = require("game.vfx")

local common = { say = dialogue.say, system = dialogue.system, wait = task.wait }

--- The four heroes' entities, by name, on the current map.
function common.cast()
  return field.actors.cael, field.actors.rowan, field.actors.lyra, field.actors.darius
end

function common.point(name)
  local point = field.world:points()[name] or error("map has no point '" .. tostring(name) .. "'")
  return point.position, point.facing
end

function common.camera(to, seconds, ease)
  fx.camera(field.world, to, seconds, ease)
end

function common.camera_async(to, seconds, ease)
  return fx.camera_async(field.world, to, seconds, ease)
end

--- A clock for a timed scene: `mark(t)` waits until `t` seconds after the
--- clock was made (scenes cut to music count from the bar the track began).
function common.clock()
  local elapsed = 0
  local running = task.spawn(function()
    while true do elapsed = elapsed + task.frame() end
  end, "scene clock")
  return function(t)
    while elapsed < t do task.frame() end
  end, function() task.cancel(running) end
end

--- Say a line that closes by itself: long enough to read, never less than
--- `least` seconds; the player cannot dismiss it (a scene cut to music).
function common.line(speaker, text, hold, options)
  options = options or {}
  options.auto = hold or math.max(1.6, #text * 0.055)
  options.locked = true
  if speaker == "SYSTEM" then
    dialogue.system(text, options)
  else
    dialogue.say(speaker, text, options)
  end
end

--- A quick double hop: someone laughing.
function common.laugh(entity)
  local p = entity.position
  for _ = 1, 3 do
    director.slide(entity, { x = p.x, y = p.y, z = p.z }, 0.16, { arc = 0.12 })
  end
end

-- ---------------------------------------------------------------- crystal

--- The great crystal over the tower: the model, its light, and a pulse
--- that throbs through the whole fortress.
function common.spawn_crystal(world)
  local at = world:points().crystal
  if not at then return nil end
  local p = at.position
  local crystal = world:spawn { id = "crystal", position = p, keep_height = true,
    mesh = { model = "models/aether_crystal.glb", cast_shadow = false },
    light = { color = { 0.62, 0.36, 1.0 }, radius = 60, intensity = 2.2, position = { x = 0, y = 0, z = 6 } } }
  crystal:play("idle", true)
  common.crystal = crystal
  return crystal
end

--- One pulse of the crystal: light, a shudder, a violet wash on the screen.
function common.crystal_pulse(strength)
  strength = strength or 1
  local crystal = common.crystal
  audio.sfx("crystal_pulse", { volume = 0.5 + 0.4 * strength })
  fx.flash({ 0.55, 0.3, 1.0 }, 0.9, 0.22 * strength)
  fx.shake(field.world, 0.12 * strength, 1.0)
  fe.input.rumble(0.4 * strength, 0.2 * strength, 0.5)
  if crystal and crystal:valid() then
    task.spawn(function()
      task.over(1.2, function(t)
        if crystal:valid() then crystal:light({ intensity = 2.2 + 6 * strength * (1 - t) * (1 - t) }) end
      end)
    end, "crystal pulse")
  end
end

-- ------------------------------------------------------------------ Rowan

--- The secret is out: violet light under Rowan's skin, and a glow he casts.
function common.bind_rowan(rowan, strength)
  rowan:sprite({ animation = "sprites/rowan_bound.anim.json" })
  rowan:light({ color = { 0.66, 0.38, 1.0 }, radius = 3 + 2 * (strength or 1), intensity = 1.2 * (strength or 1),
    position = { x = 0, y = 0.9, z = 0 } })
end

--- Rowan flickering in and out, as if the light were all that held him.
function common.flicker(rowan, seconds)
  return task.spawn(function()
    local left = seconds
    while left > 0 and rowan:valid() do
      local on = (math.floor(left * 23) % 3) ~= 0
      rowan:sprite({ tint = { 1.3, 1.1, 1.6, on and 1 or 0.25 }, blend = "alpha" })
      left = left - task.frame()
    end
    if rowan:valid() then rowan:sprite({ tint = { 1, 1, 1, 1 }, blend = "opaque" }) end
  end, "flicker")
end

-- ---------------------------------------------------------------- chamber

--- The chamber shaking itself apart: dust and stones from above, now and
--- then, until the returned function is called.
function common.collapse(world, centre, radius, every)
  local rng = fe.random.new(5)
  local running = task.spawn(function()
    while true do
      task.wait(rng:range(every * 0.6, every * 1.4))
      local angle, r = rng:range(0, math.pi * 2), rng:range(2, radius)
      local at = { x = centre.x + math.cos(angle) * r, y = 9, z = centre.z + math.sin(angle) * r * 0.6 - 2 }
      vfx.burst(world, at, "rubble", rng:int(3, 7), { 0.8, 0.3, 0.8 })
      vfx.burst(world, at, "dust", 4, { 1, 0.5, 1 })
      audio.sfx("collapse", { volume = rng:range(0.25, 0.5), pitch = rng:range(0.8, 1.2) })
      fx.shake(world, rng:range(0.04, 0.1), 0.6)
    end
  end, "collapse")
  return function() task.cancel(running) end
end

--- Everyone turns to look at something.
function common.all_face(target)
  for _, entity in pairs(field.actors) do director.face(entity, target) end
end

common.actors, common.director, common.fx, common.vfx, common.audio = actors, director, fx, vfx, audio

return common

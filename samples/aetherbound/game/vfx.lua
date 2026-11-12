--- Short-lived effects in a world: sprites that play once, lights that
--- flare and die, rings, shock waves, bursts of particles. Each call spawns
--- what it needs and cleans up after itself; none blocks.
local task = require("game.task")

local vfx = {}

local function lerp(a, b, t)
  return a + (b - a) * t
end

--- Run `step(t)` over `seconds` (t from 0 to 1), then `finish`.
local function over(seconds, step, finish)
  return task.spawn(function()
    task.over(seconds, step)
    if finish then finish() end
  end, "vfx")
end

--- A sprite that plays its clip once at `at` and disappears after `life`
--- seconds. options: `scale`, `life`, `tint`, `flip`, `clip`, `blend`
--- (additive by default), `mode` (facing by default), `grow` (scale at the
--- end, relative), `fade` (fade the alpha out over the life).
function vfx.sprite(world, animation, at, options)
  options = options or {}
  local scale = options.scale or 1
  local tint = options.tint or { 1, 1, 1, 1 }
  local e = world:spawn { position = at, keep_height = true, facing = { x = options.flip and -1 or 1, z = 0 },
    sprite = { animation = animation, clip = options.clip or "idle", mode = options.mode or "facing",
      blend = options.blend or "additive", unlit = true, scale = scale, tint = tint, cast_shadow = false,
      depth_bias = options.depth_bias or 0.3 } }
  if options.grow or options.fade then
    over(options.life or 0.5, function(t)
      if not e:valid() then return end
      e:sprite({ scale = scale * lerp(1, options.grow or 1, t),
        tint = { tint[1], tint[2], tint[3], (tint[4] or 1) * (options.fade and (1 - t) or 1) } })
    end, function() if e:valid() then e:destroy() end end)
  else
    over(options.life or 0.5, function() end, function() if e:valid() then e:destroy() end end)
  end
  return e
end

--- A point light that flares at `at` and fades over `seconds`.
function vfx.light(world, at, settings, seconds)
  local e = world:spawn { position = { x = at.x, y = (at.y or 0) + (settings.y or 0), z = at.z }, keep_height = true,
    light = { color = settings.color, radius = settings.radius or 6, intensity = settings.intensity or 3 } }
  over(seconds or 0.5, function(t)
    if e:valid() then e:light({ intensity = (settings.intensity or 3) * (1 - t) * (1 - t) }) end
  end, function() if e:valid() then e:destroy() end end)
  return e
end

--- An expanding ring of light, flat on the ground or facing the camera.
function vfx.ring(world, at, options)
  options = options or {}
  local c = options.color or { 1, 1, 1 }
  return vfx.sprite(world, "fx/battle/ring.anim.json", at, { scale = 0.2, grow = (options.scale or 6) / 0.2,
    life = options.seconds or 0.5, fade = true, tint = { c[1] * 2, c[2] * 2, c[3] * 2, 1 },
    mode = options.flat and "flat" or "facing" })
end

--- A shock wave: it draws nothing, it bends the scene behind it.
function vfx.shockwave(world, at, options)
  options = options or {}
  local e = world:spawn { position = at, keep_height = true,
    sprite = { animation = "fx/battle/ring.anim.json", mode = "facing", distort = options.strength or 0.6,
      scale = 0.3, cast_shadow = false } }
  over(options.seconds or 0.6, function(t)
    if e:valid() then e:sprite({ scale = lerp(0.3, options.scale or 8, t), distort = (options.strength or 0.6) * (1 - t) }) end
  end, function() if e:valid() then e:destroy() end end)
  return e
end

--- A magic circle on the ground under a caster.
function vfx.circle(world, at, options)
  options = options or {}
  local c = options.color or { 0.7, 0.5, 1 }
  local e = world:spawn { position = { x = at.x, y = at.y, z = at.z },
    sprite = { animation = "fx/battle/circle.anim.json", mode = "decal", blend = "additive", unlit = true,
      scale = 0.4, tint = { c[1] * 1.5, c[2] * 1.5, c[3] * 1.5, 1 }, cast_shadow = false, depth_bias = 0.05 } }
  over(options.seconds or 0.7, function(t)
    if not e:valid() then return end
    local k = t < 0.25 and t / 0.25 or 1
    e:sprite({ scale = lerp(0.4, options.scale or 1.2, k),
      tint = { c[1] * 1.5, c[2] * 1.5, c[3] * 1.5, t > 0.7 and (1 - t) / 0.3 or 1 } })
  end, function() if e:valid() then e:destroy() end end)
  return e
end

--- Motes rising around an entity while it gathers power.
function vfx.aura(world, entity, options)
  options = options or {}
  local c = options.color or { 0.7, 0.4, 1 }
  local p = entity.position
  local scale = options.scale or 1
  local e = world:spawn { position = { x = p.x, y = p.y + (options.y or 0.2), z = p.z }, keep_height = true,
    emitter = { sprite = "fx/battle/mote.anim.json", rate = options.rate or 46, lifetime = 0.9,
      lifetime_jitter = 0.3, velocity = { 0, 1.8 * scale, 0 }, velocity_jitter = { 0.5 * scale, 0.6, 0.5 * scale },
      area = { 0.7 * scale, 0.3, 0.7 * scale }, size = 0.16 * scale, size_end = 0.02,
      color = { c[1] * 3, c[2] * 3, c[3] * 3, 1 }, color_end = { c[1], c[2], c[3], 0 }, blend = "additive",
      unlit = true, max = 200 } }
  if options.seconds then
    over(options.seconds, function() end, function()
      if e:valid() then
        e:emitter({ paused = true })
        over(1.2, function() end, function() if e:valid() then e:destroy() end end)
      end
    end)
  end
  return e
end

local BURSTS = {
  shard = { sprite = "fx/battle/shard.anim.json", lifetime = 0.8, lifetime_jitter = 0.3, velocity = { 0, 3.5, 0 },
    velocity_jitter = { 4, 2.5, 4 }, gravity = { 0, -9, 0 }, size = 0.3, size_end = 0.1,
    color = { 2.4, 1.6, 3.2, 1 }, color_end = { 1.2, 0.6, 2, 0 }, blend = "additive", unlit = true },
  mote = { sprite = "fx/battle/mote.anim.json", lifetime = 1.0, lifetime_jitter = 0.4, velocity = { 0, 1.5, 0 },
    velocity_jitter = { 2, 1.5, 2 }, gravity = { 0, 0.6, 0 }, size = 0.2, size_end = 0.02,
    color = { 2.2, 1.4, 3, 1 }, color_end = { 1, 0.5, 2, 0 }, blend = "additive", unlit = true },
  spark = { sprite = "fx/battle/mote.anim.json", lifetime = 0.5, lifetime_jitter = 0.2, velocity = { 0, 3, 0 },
    velocity_jitter = { 4, 3, 4 }, gravity = { 0, -12, 0 }, size = 0.12, size_end = 0.02,
    color = { 3, 2, 1, 1 }, color_end = { 2, 0.6, 0.1, 0 }, blend = "additive", unlit = true },
  dust = { sprite = "fx/battle/dust.anim.json", lifetime = 1.1, lifetime_jitter = 0.4, velocity = { 0, 0.8, 0 },
    velocity_jitter = { 1.6, 0.6, 1.6 }, gravity = { 0, 0.2, 0 }, size = 0.7, size_end = 1.6, drag = 1.5,
    color = { 0.55, 0.5, 0.5, 0.55 }, color_end = { 0.4, 0.38, 0.4, 0 }, blend = "alpha" },
  rubble = { sprite = "fx/battle/rubble.anim.json", lifetime = 1.6, lifetime_jitter = 0.4, velocity = { 0, -1, 0 },
    velocity_jitter = { 0.6, 1, 0.6 }, gravity = { 0, -14, 0 }, size = 0.35, size_end = 0.3,
    color = { 0.8, 0.75, 0.8, 1 }, color_end = { 0.6, 0.55, 0.6, 1 }, blend = "alpha" },
  light = { sprite = "fx/battle/mote.anim.json", lifetime = 2.4, lifetime_jitter = 0.8, velocity = { 0, 2.2, 0 },
    velocity_jitter = { 0.5, 1, 0.5 }, gravity = { 0, 0.8, 0 }, size = 0.14, size_end = 0.01,
    color = { 3, 2.6, 3.4, 1 }, color_end = { 1.6, 1.2, 2.4, 0 }, blend = "additive", unlit = true },
}

--- A one-shot burst of particles: "shard", "mote", "spark", "dust",
--- "rubble" or "light". `area` spreads where they start.
function vfx.burst(world, at, kind, count, area)
  local settings = {}
  for key, value in pairs(BURSTS[kind] or error("no burst '" .. tostring(kind) .. "'")) do settings[key] = value end
  if area then settings.area = area end
  world:burst(at, settings, count or 12)
end

return vfx

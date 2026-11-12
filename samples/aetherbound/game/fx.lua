--- Screen and camera effects that run over time: fades, flashes, shakes,
--- letterbox bars and camera moves. Functions that take `seconds` block the
--- calling task unless they say otherwise; `fx.update(dt)` runs every tick.
local task = require("game.task")

local fx = { shakes = {}, bars = 0, fade_alpha = 0, fade_color = { 0, 0, 0 }, cameras = {} }

function fx.ease(t)
  return t * t * (3 - 2 * t)
end

function fx.ease_out(t)
  return 1 - (1 - t) * (1 - t)
end

function fx.ease_in(t)
  return t * t
end

function fx.lerp(a, b, t)
  return a + (b - a) * t
end

--- Fade the screen to `alpha` of `color` ({r, g, b}, black by default).
function fx.fade(alpha, seconds, color)
  local from = fx.fade_alpha
  if color then fx.fade_color = color end
  local c = fx.fade_color
  if not seconds or seconds <= 0 then
    fx.fade_alpha = alpha
    fe.screen.set { fade = { c[1], c[2], c[3], alpha } }
    return
  end
  task.over(seconds, function(t)
    fx.fade_alpha = fx.lerp(from, alpha, t)
    fe.screen.set { fade = { c[1], c[2], c[3], fx.fade_alpha } }
  end)
end

--- A flash that decays by itself; does not block.
function fx.flash(color, seconds, strength)
  local c = color or { 1, 1, 1 }
  task.spawn(function()
    task.over(seconds or 0.3, function(t)
      fe.screen.set { flash = { c[1], c[2], c[3], (strength or 1) * (1 - t) * (1 - t) } }
    end)
    fe.screen.set { flash = { c[1], c[2], c[3], 0 } }
  end, "flash")
end

--- Cinematic bars: 0 none, 1 full height (8.5% of the screen each).
function fx.letterbox(amount, seconds)
  local from = fx.bars
  if not seconds or seconds <= 0 then
    fx.bars = amount
    fe.screen.set { letterbox = amount, letterbox_height = 0.085 }
    return
  end
  task.over(seconds, function(t)
    fx.bars = fx.lerp(from, amount, fx.ease(t))
    fe.screen.set { letterbox = fx.bars, letterbox_height = 0.085 }
  end)
end

--- Shake a world's camera: `amount` in world units, decaying over `seconds`
--- (or steady until fx.stop_shake with `hold`). Does not block.
function fx.shake(world, amount, seconds, hold)
  local shake = { world = world, amount = amount, left = seconds or 0.4, total = seconds or 0.4, hold = hold }
  fx.shakes[#fx.shakes + 1] = shake
  return shake
end

function fx.stop_shake(shake)
  if shake then shake.left, shake.hold = 0, nil end
end

--- Move a world's camera to the given settings over `seconds`. Fields left
--- out keep their value. Blocks; fx.camera_async does not.
function fx.camera(world, to, seconds, ease)
  local camera = world:camera()
  local from = { target = camera.target, yaw = camera.yaw, pitch = camera.pitch, distance = camera.distance,
    fov = camera.fov }
  ease = ease or fx.ease
  local function apply(t)
    local k = ease(t)
    if to.target then
      local a, b = from.target, to.target
      camera.target = { x = fx.lerp(a.x, b.x, k), y = fx.lerp(a.y or 0, b.y or 0, k), z = fx.lerp(a.z, b.z, k) }
    end
    for _, key in ipairs { "yaw", "pitch", "distance", "fov" } do
      if to[key] then camera[key] = fx.lerp(from[key], to[key], k) end
    end
  end
  if not seconds or seconds <= 0 then
    apply(1)
    camera:snap()
    return
  end
  local token = {}
  fx.cameras[world.name] = token
  local elapsed = 0
  while elapsed < seconds do
    if fx.cameras[world.name] ~= token then return end -- a newer move took over
    apply(elapsed / seconds)
    elapsed = elapsed + task.frame()
  end
  if fx.cameras[world.name] == token then apply(1) end
end

function fx.camera_async(world, to, seconds, ease)
  return task.spawn(function() fx.camera(world, to, seconds, ease) end, "camera")
end

--- Hit-stop: the world slows to `scale` for `seconds` of real time.
function fx.hitstop(world, seconds, scale)
  task.spawn(function()
    world.time_scale = scale or 0.05
    local left = seconds
    while left > 0 do left = left - task.frame() end
    world.time_scale = 1
  end, "hitstop")
end

function fx.update(dt)
  local totals = {}
  for i = #fx.shakes, 1, -1 do
    local shake = fx.shakes[i]
    if not shake.hold then shake.left = shake.left - dt end
    if shake.left <= 0 and not shake.hold then
      table.remove(fx.shakes, i)
      totals[shake.world] = totals[shake.world] or 0
    else
      local strength = shake.hold and shake.amount or shake.amount * (shake.left / shake.total)
      totals[shake.world] = (totals[shake.world] or 0) + strength
    end
  end
  for world, amount in pairs(totals) do
    if fe.world.get(world.name) then
      local t = fe.time.now() * 47
      world:camera().shake = { x = math.sin(t * 1.3) * amount, y = math.cos(t * 1.7) * amount * 0.7, z = 0 }
    end
  end
end

--- Back to a clean screen (title, tests).
function fx.reset()
  fx.shakes, fx.cameras = {}, {}
  fx.bars, fx.fade_alpha = 0, 0
  fe.screen.unfreeze()
  fe.screen.set { fade = { 0, 0, 0, 0 }, flash = { 1, 1, 1, 0 }, letterbox = 0, radial_blur = 0, chromatic = 0,
    transition = { kind = "none", progress = 0 } }
end

return fx

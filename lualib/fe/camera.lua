--- Camera rigs: behaviour on top of the engine's orbit camera.
---
--- A rig follows an entity (or a point), applies the map's camera settings
--- and "camera" zones, clamps to bounds, adds shake, supports player
--- rotation/zoom and runs scripted pans and paths:
---
--- ```lua
--- local rig = camera.rig(world, {follow = hero})
--- rig:update(dt)                     -- every tick (fe.field does this)
--- rig:pan_to({x = 10, z = 4}, 1.5)   -- inside a task
--- rig:shake(0.15, 0.4)
--- ```
--- Map props: `"camera": {"pitch": 40, "yaw": 0, "distance": 18, "fov": 30,
--- "min": [x, z], "max": [x, z], "height": 0.8}`; `camera.defaults` fills
--- what neither the rig options nor the map set. Zones of type "camera"
--- override pitch, yaw, distance, fov, min and max while the followed entity
--- is inside (their props are described for the editor with fe.schema.zone).
---
--- `pan_to` and `path` hold the camera until `release()` or until the task
--- that started them (its owner, see `Task:owner`) ends, so a cancelled
--- cutscene never leaves the camera stuck.
local class = require("fe.class")
local schema = require("fe.schema")
local tablex = require("fe.tablex")
local task = require("fe.task")
local tween = require("fe.tween")

local camera = {}

local Rig = class("CameraRig")
camera.Rig = Rig

camera.defaults = { pitch = 40, yaw = 0, distance = 18, fov = 30, height = 0.8, smoothing = 7 }

schema.zone("camera", {
  pitch = { type = "number", optional = true, doc = "Degrees above the horizon" },
  yaw = { type = "number", optional = true, doc = "Degrees around the target" },
  distance = { type = "number", optional = true, min = 0 },
  fov = { type = "number", optional = true, min = 1, max = 170 },
  min = { type = { list = "number" }, optional = true, doc = "Lower [x, z] bound of the camera target" },
  max = { type = { list = "number" }, optional = true, doc = "Upper [x, z] bound of the camera target" },
}, "Overrides the camera while the followed entity is inside.")

--- Adopt edited defaults on hot reload.
function camera.__reload(live, fresh)
  live.defaults = fresh.defaults
end

local function vec_of(v)
  if not v then return nil end
  return { x = v.x or v[1] or 0, z = v.z or v[2] or 0 }
end

function Rig:init(world, options)
  options = options or {}
  self.world = world
  self.cam = world:camera()
  local props = world:props().camera or {}
  self.base = {}
  for key, value in pairs(camera.defaults) do
    self.base[key] = options[key] or props[key] or value
  end
  self.bounds = { min = vec_of(options.min or props.min), max = vec_of(options.max or props.max) }
  self.wraps = world:wraps() -- a toroidal map has no edges to clamp to
  self.zone_type = options.zone_type or "camera"
  self.current = { pitch = self.base.pitch, yaw = self.base.yaw, distance = self.base.distance, fov = self.base.fov }
  self.yaw_offset = 0
  self.zoom = 1
  self.shakes = {}
  self.override = nil -- the task holding a scripted move
  self.top_down = false
  self.following = options.follow
  self.point = options.point
  self:snap()
end

--- Follow an entity (or nil to hold position).
function Rig:follow(entity)
  self.following = entity
  self.point = nil
end

--- Look at a fixed point.
function Rig:look_at(point)
  self.following = nil
  self.point = point
end

function Rig:focus_point()
  if self.following and self.following:valid() then
    return self.following.position
  end
  return self.point or self.cam.target
end

function Rig:goal()
  local point = self:focus_point()
  local goal = { pitch = self.base.pitch, yaw = self.base.yaw, distance = self.base.distance, fov = self.base.fov }
  local min, max = self.bounds.min, self.bounds.max
  for _, zone in ipairs(self.world:zones_at(point)) do
    if zone.type == self.zone_type then
      local p = zone.props
      for key in pairs(goal) do
        if p[key] then goal[key] = p[key] end
      end
      min, max = vec_of(p.min) or min, vec_of(p.max) or max
    end
  end
  local target = { x = point.x, y = (point.y or 0) + self.base.height, z = point.z }
  if min and not self.wraps then
    target.x = math.max(min.x, target.x)
    target.z = math.max(min.z, target.z)
  end
  if max and not self.wraps then
    target.x = math.min(max.x, target.x)
    target.z = math.min(max.z, target.z)
  end
  goal.yaw = goal.yaw + self.yaw_offset
  goal.distance = goal.distance * self.zoom
  if self.top_down then
    goal.pitch = 89
  end
  return target, goal
end

--- Jump straight to the goal (after warps).
function Rig:snap()
  local target, goal = self:goal()
  self.cam.target = target
  for key, value in pairs(goal) do
    self.current[key] = value
    self.cam[key] = value
  end
  self.cam:snap()
end

local function angle_delta(from, to)
  return ((to - from + 540) % 360) - 180
end

local function approach_angle(current, goal, rate, dt)
  return current + angle_delta(current, goal) * (1 - math.exp(-rate * dt))
end

--- Call once per tick.
function Rig:update(dt)
  if self.override and not self.override:running() then
    self.override = nil
  end
  if self.override then
    return self:apply_shake(dt)
  end
  local target, goal = self:goal()
  local rate = self.base.smoothing
  local t = self.cam.target
  -- Approached the short way round: the engine folds the target back into
  -- the map at the next tick.
  local d = self.world:wrap_delta(t, target)
  self.cam.target = {
    x = tween.approach(t.x, t.x + d.x, rate, dt),
    y = tween.approach(t.y, target.y, rate, dt),
    z = tween.approach(t.z, t.z + d.z, rate, dt),
  }
  self.current.pitch = tween.approach(self.current.pitch, goal.pitch, rate * 0.6, dt)
  self.current.distance = tween.approach(self.current.distance, goal.distance, rate * 0.6, dt)
  self.current.fov = tween.approach(self.current.fov, goal.fov, rate * 0.6, dt)
  self.current.yaw = approach_angle(self.current.yaw, goal.yaw, rate, dt)
  self.cam.pitch, self.cam.distance, self.cam.fov, self.cam.yaw =
    self.current.pitch, self.current.distance, self.current.fov, self.current.yaw
  self:apply_shake(dt)
end

function Rig:apply_shake(dt)
  local offset = { x = 0, y = 0, z = 0 }
  local alive = {}
  for _, s in ipairs(self.shakes) do
    s.time = s.time + dt
    if s.time < s.duration then
      local strength = s.amplitude * (1 - s.time / s.duration)
      local phase = s.time * s.frequency
      offset.x = offset.x + math.sin(phase * 6.283) * strength
      offset.y = offset.y + math.cos(phase * 5.11) * strength * 0.6
      alive[#alive + 1] = s
    end
  end
  self.shakes = alive
  self.cam.shake = offset
end

--- Screen shake that fades out over `duration`.
function Rig:shake(amplitude, duration, frequency)
  self.shakes[#self.shakes + 1] = { amplitude = amplitude or 0.1, duration = duration or 0.3,
    frequency = frequency or 18, time = 0 }
end

--- Rotate the player-controlled offset. `step` in degrees; with
--- `snap` the yaw moves to the next multiple of step.
function Rig:rotate(step, snap)
  if snap then
    local size = math.abs(step)
    self.yaw_offset = (math.floor(self.yaw_offset / size + 0.5) + (step > 0 and 1 or -1)) * size
  else
    self.yaw_offset = self.yaw_offset + step
  end
end

function Rig:set_zoom(zoom)
  self.zoom = tablex.clamp(zoom, 0.3, 3)
end

function Rig:set_top_down(on)
  self.top_down = on
end

--- Current yaw (degrees) for converting stick input to world directions.
function Rig:yaw()
  return self.cam.yaw
end

local function hold(rig)
  local current = task.current()
  if not current then
    error("camera moves wait, so they must run inside a task", 3)
  end
  rig.override = current:owner()
end

--- Scripted move: animate the camera to a target/angles, then hold there
--- until `release()` or the end of the calling task. Waits; call from a
--- task. Yaw turns the short way round.
---@param target table
---@param duration number
---@param options? {pitch?: number, yaw?: number, distance?: number, fov?: number, ease?: string}
function Rig:pan_to(target, duration, options)
  options = options or {}
  hold(self)
  local from = { target = self.cam.target, pitch = self.cam.pitch, yaw = self.cam.yaw,
    distance = self.cam.distance, fov = self.cam.fov }
  local to = { pitch = options.pitch, distance = options.distance, fov = options.fov,
    yaw = options.yaw and from.yaw + angle_delta(from.yaw, options.yaw) }
  local to_target = { x = target.x, y = (target.y or 0) + self.base.height, z = target.z }
  local d = self.world:wrap_delta(from.target, to_target) -- the short way round on a wrapping map
  tween.run(duration, function(t)
    self.cam.target = {
      x = from.target.x + d.x * tween.lerp(0, 1, t),
      y = tween.lerp(from.target.y, to_target.y, t),
      z = from.target.z + d.z * tween.lerp(0, 1, t),
    }
    for _, key in ipairs({ "pitch", "yaw", "distance", "fov" }) do
      if to[key] then
        self.cam[key] = tween.lerp(from[key], to[key], t)
        self.current[key] = self.cam[key]
      end
    end
  end, options.ease or "in_out_cubic")
end

local function catmull(p0, p1, p2, p3, t)
  local t2, t3 = t * t, t * t * t
  return 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3)
end

--- Fly through points (Catmull-Rom spline) over `duration`, then hold
--- like `pan_to`.
---@param points table[] each {x, y?, z}
function Rig:path(points, duration, ease)
  assert(#points >= 2, "a camera path needs at least two points")
  hold(self)
  local pts = { points[1] }
  for _, p in ipairs(points) do pts[#pts + 1] = p end
  pts[#pts + 1] = points[#points]
  local segments = #points - 1
  tween.run(duration, function(t)
    local scaled = math.min(segments - 1e-6, t * segments)
    local i = math.floor(scaled)
    local u = scaled - i
    local a, b, c, d = pts[i + 1], pts[i + 2], pts[i + 3], pts[i + 4]
    self.cam.target = {
      x = catmull(a.x, b.x, c.x, d.x, u),
      y = catmull(a.y or 0, b.y or 0, c.y or 0, d.y or 0, u) + self.base.height,
      z = catmull(a.z, b.z, c.z, d.z, u),
    }
  end, ease or "in_out_quad")
end

--- Return control to following.
function Rig:release()
  self.override = nil
end

--- Create a rig for a world.
function camera.rig(world, options)
  return Rig(world, options)
end

return camera

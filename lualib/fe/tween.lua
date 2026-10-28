--- Tweens animate numbers over time inside the task scheduler.
---
--- ```lua
--- tween.to(panel, {alpha = 1, y = 20}, 0.25, "out_cubic")   -- returns a Task
--- tween.run(0.5, function(t) fe.screen.set{fade = {0, 0, 0, t}} end)  -- waits
--- ```
local task = require("fe.task")

local tween = {}

local function bounce(t)
  if t < 1 / 2.75 then
    return 7.5625 * t * t
  elseif t < 2 / 2.75 then
    t = t - 1.5 / 2.75
    return 7.5625 * t * t + 0.75
  elseif t < 2.5 / 2.75 then
    t = t - 2.25 / 2.75
    return 7.5625 * t * t + 0.9375
  end
  t = t - 2.625 / 2.75
  return 7.5625 * t * t + 0.984375
end

--- Easing functions mapping 0..1 to 0..1.
tween.ease = {
  linear = function(t) return t end,
  in_quad = function(t) return t * t end,
  out_quad = function(t) return t * (2 - t) end,
  in_out_quad = function(t) return t < 0.5 and 2 * t * t or -1 + (4 - 2 * t) * t end,
  in_cubic = function(t) return t * t * t end,
  out_cubic = function(t) t = t - 1; return t * t * t + 1 end,
  in_out_cubic = function(t)
    return t < 0.5 and 4 * t * t * t or (t - 1) * (2 * t - 2) * (2 * t - 2) + 1
  end,
  out_back = function(t)
    local c = 1.70158
    t = t - 1
    return t * t * ((c + 1) * t + c) + 1
  end,
  out_elastic = function(t)
    if t == 0 or t == 1 then return t end
    return 2 ^ (-10 * t) * math.sin((t - 0.075) * (2 * math.pi) / 0.3) + 1
  end,
  out_bounce = bounce,
  sine = function(t) return 0.5 - math.cos(t * math.pi) * 0.5 end,
}

local function easing(name)
  if type(name) == "function" then
    return name
  end
  local fn = tween.ease[name or "linear"]
  assert(fn, "unknown easing '" .. tostring(name) .. "'")
  return fn
end

--- Call `step(eased_t)` every tick for `duration` seconds. Waits (call it
--- from a task); ends with step(1).
---@param duration number
---@param step fun(t: number)
---@param ease? string|fun(t: number): number
function tween.run(duration, step, ease)
  local fn = easing(ease)
  local start = task.time()
  if duration <= 0 then
    step(1)
    return
  end
  step(fn(0))
  while true do
    task.frame()
    local t = math.min(1, (task.time() - start) / duration)
    step(fn(t))
    if t >= 1 then
      return
    end
  end
end

--- Animate numeric fields of `target` to `goals` in a new task.
---@param target table
---@param goals table<string, number>
---@param duration number
---@param ease? string|function
---@return Task
function tween.to(target, goals, duration, ease)
  return task.spawn(function()
    local from = {}
    for key in pairs(goals) do
      from[key] = target[key] or 0
    end
    tween.run(duration, function(t)
      for key, goal in pairs(goals) do
        target[key] = from[key] + (goal - from[key]) * t
      end
    end, ease)
  end)
end

--- Linear interpolation.
function tween.lerp(a, b, t)
  return a + (b - a) * t
end

--- Frame-rate independent smoothing towards a goal (exponential decay).
---@param current number
---@param goal number
---@param rate number larger is snappier (per second)
---@param dt number
function tween.approach(current, goal, rate, dt)
  return goal + (current - goal) * math.exp(-rate * dt)
end

return tween

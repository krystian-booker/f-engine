--- Time of day. Keyframes blend environment values (ambient, sun, fog,
--- sky, LUT blend) as the clock runs, and NPC schedules can read the hour.
---
--- ```lua
--- daynight.start({minutes_per_second = 0.5})   -- a day lasts 48 real minutes
--- daynight.set_hour(18)
--- if daynight.hour() >= 20 then ... end
--- ```
--- Maps opt in with props `"daynight": true`; interiors leave it off.
local game = require("fe.game")
local signal = require("fe.signal")
local state = require("fe.state")

local daynight = {
  minutes_per_second = 0,
  hour_changed = signal.new(),
  --- Keyframes by hour; values blend linearly between neighbours.
  keys = {
    { hour = 0, ambient = { 0.12, 0.14, 0.26 }, sun_color = { 0.25, 0.32, 0.55 }, sun_direction = { 0.3, -1, 0.4 },
      fog_color = { 0.05, 0.07, 0.14 }, sky_top = { 0.02, 0.03, 0.10 }, sky_horizon = { 0.10, 0.12, 0.24 } },
    { hour = 6, ambient = { 0.32, 0.28, 0.30 }, sun_color = { 1.0, 0.62, 0.42 }, sun_direction = { -0.8, -0.5, -0.3 },
      fog_color = { 0.55, 0.42, 0.40 }, sky_top = { 0.30, 0.38, 0.62 }, sky_horizon = { 0.95, 0.62, 0.45 } },
    { hour = 12, ambient = { 0.40, 0.42, 0.46 }, sun_color = { 1.0, 0.92, 0.80 }, sun_direction = { -0.3, -1, -0.4 },
      fog_color = { 0.60, 0.70, 0.82 }, sky_top = { 0.25, 0.45, 0.80 }, sky_horizon = { 0.72, 0.82, 0.92 } },
    { hour = 18, ambient = { 0.34, 0.26, 0.28 }, sun_color = { 1.0, 0.52, 0.30 }, sun_direction = { 0.8, -0.45, -0.2 },
      fog_color = { 0.55, 0.35, 0.32 }, sky_top = { 0.22, 0.24, 0.50 }, sky_horizon = { 0.98, 0.55, 0.35 } },
  },
}

local function minutes()
  return state.var("daynight_minutes", 12 * 60)
end

function daynight.hour()
  return math.floor(minutes() / 60) % 24
end

function daynight.set_hour(hour)
  state.set_var("daynight_minutes", (hour % 24) * 60)
  daynight.apply()
end

local function lerp_list(a, b, t)
  local out = {}
  for i = 1, #a do out[i] = a[i] + (b[i] - a[i]) * t end
  return out
end

--- Environment values for a time (hours, fractional).
function daynight.sample(hours)
  local keys = daynight.keys
  local previous, nxt = keys[#keys], keys[1]
  for i, key in ipairs(keys) do
    if key.hour <= hours then
      previous = key
      nxt = keys[i % #keys + 1]
    end
  end
  local span = (nxt.hour - previous.hour) % 24
  local t = span > 0 and ((hours - previous.hour) % 24) / span or 0
  local out = {}
  for name, value in pairs(previous) do
    if name ~= "hour" then out[name] = lerp_list(value, nxt[name], t) end
  end
  return out
end

--- Push current values to a world (default: the field).
function daynight.apply(world)
  world = world or require("fe.field").world
  if not world or not world:props().daynight then return end
  local v = daynight.sample(minutes() / 60)
  world:set_environment({
    ambient = v.ambient, sun_color = v.sun_color, sun_direction = v.sun_direction, fog_color = v.fog_color,
    sky = { top = v.sky_top, horizon = v.sky_horizon },
  })
end

--- Run the clock (minutes of game time per real second).
function daynight.start(options)
  daynight.minutes_per_second = options and options.minutes_per_second or 1
  if daynight.connection then return end
  daynight.connection = game.updated:connect(function(dt)
    if daynight.minutes_per_second <= 0 then return end
    local before = daynight.hour()
    state.set_var("daynight_minutes", (minutes() + dt * daynight.minutes_per_second) % (24 * 60))
    daynight.apply()
    if daynight.hour() ~= before then daynight.hour_changed:emit(daynight.hour()) end
  end)
end

require("fe.event").condition("hours", function(value)
  local from, to = value[1], value[2]
  local hour = daynight.hour()
  if from <= to then return hour >= from and hour < to end
  return hour >= from or hour < to
end)

return daynight

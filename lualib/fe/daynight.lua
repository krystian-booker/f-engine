--- Time of day. Keyframes blend environment values (ambient, sun, fog,
--- sky and the LUT blend) as the clock runs, and NPC schedules and event
--- pages can read the hour.
---
--- ```lua
--- daynight.start({minutes_per_second = 0.5})   -- a day lasts 48 real minutes
--- daynight.set_hour(18)                        -- also works with the clock stopped
--- if daynight.hour() >= 20 then ... end
--- ```
--- Maps opt in with props `"daynight": true`; interiors leave it off. The
--- time is saved in the "daynight" state section and re-applied whenever
--- the field enters a map (after `install`, which `start` and `set_hour`
--- call). Colour grading: keys carry `lut_blend` (0 = the map's `post.lut`,
--- 1 = its `post.lut_blend_target`); set `daynight.luts = {day = ..., night
--- = ...}` to use the same two LUTs on every opted-in map. Changing hour
--- emits `hour_changed(hour)` and `fe.state.changed("hour", nil, hour)`;
--- the `{"hours": [from, to]}` page condition is provided to fe.event.
--- The clock stands still while the field world is paused (menus).
---
--- Octopath II's day/night switch: `daynight.toggle()` (in a task) fades
--- out, jumps to the other phase's hour (`daynight.phases`) and fades back
--- in; the hour is saved and re-applied on every map like any other.
--- `daynight.phase()` is "day" or "night" (`daynight.day_hours`).
--- Bind it to a button with `field.command`.
local game = require("fe.game")
local signal = require("fe.signal")
local state = require("fe.state")
local task = require("fe.task")
local transition = require("fe.transition")

local daynight = {
  minutes_per_second = 0,
  hour_changed = signal.new(),
  --- Emitted with the new phase after `toggle`.
  toggled = signal.new(),
  --- Hour each phase jumps to on `toggle`.
  phases = { day = 10, night = 22 },
  --- Hours that count as day: [from, to).
  day_hours = { 6, 18 },
  --- Optional {day = path, night = path} LUTs applied to opted-in maps.
  luts = nil,
  --- Keyframes by hour; values blend linearly between neighbours.
  keys = {
    { hour = 0, ambient = { 0.12, 0.14, 0.26 }, sun_color = { 0.25, 0.32, 0.55 }, sun_direction = { 0.3, -1, 0.4 },
      fog_color = { 0.05, 0.07, 0.14 }, sky_top = { 0.02, 0.03, 0.10 }, sky_horizon = { 0.10, 0.12, 0.24 }, lut_blend = 1 },
    { hour = 6, ambient = { 0.32, 0.28, 0.30 }, sun_color = { 1.0, 0.62, 0.42 }, sun_direction = { -0.8, -0.5, -0.3 },
      fog_color = { 0.55, 0.42, 0.40 }, sky_top = { 0.30, 0.38, 0.62 }, sky_horizon = { 0.95, 0.62, 0.45 }, lut_blend = 0.5 },
    { hour = 12, ambient = { 0.40, 0.42, 0.46 }, sun_color = { 1.0, 0.92, 0.80 }, sun_direction = { -0.3, -1, -0.4 },
      fog_color = { 0.60, 0.70, 0.82 }, sky_top = { 0.25, 0.45, 0.80 }, sky_horizon = { 0.72, 0.82, 0.92 }, lut_blend = 0 },
    { hour = 18, ambient = { 0.34, 0.26, 0.28 }, sun_color = { 1.0, 0.52, 0.30 }, sun_direction = { 0.8, -0.45, -0.2 },
      fog_color = { 0.55, 0.35, 0.32 }, sky_top = { 0.22, 0.24, 0.50 }, sky_horizon = { 0.98, 0.55, 0.35 }, lut_blend = 0.6 },
  },
}

local clock = state.section("daynight", function() return { minutes = 12 * 60 } end)
local live = { installed = false }

function daynight.hour()
  return math.floor(clock.minutes / 60) % 24
end

local function set_minutes(minutes)
  local before = daynight.hour()
  clock.minutes = minutes % (24 * 60)
  daynight.apply()
  local hour = daynight.hour()
  if hour ~= before then
    daynight.hour_changed:emit(hour)
    state.changed:emit("hour", nil, hour)
  end
end

local function blend(a, b, t)
  if type(a) == "number" then return a + (b - a) * t end
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
    if name ~= "hour" and nxt[name] ~= nil then out[name] = blend(value, nxt[name], t) end
  end
  return out
end

--- Push current values to a world (default: the field) if its map opts in.
function daynight.apply(world)
  world = world or require("fe.field").world
  if not world or not world:props().daynight then return end
  local v = daynight.sample(clock.minutes / 60)
  local post = { lut_blend = v.lut_blend }
  if daynight.luts then
    post.lut, post.lut_blend_target = daynight.luts.day, daynight.luts.night
  end
  world:set_environment({
    ambient = v.ambient, sun_color = v.sun_color, sun_direction = v.sun_direction, fog_color = v.fog_color,
    sky = { top = v.sky_top, horizon = v.sky_horizon }, post = post,
  })
end

--- Re-apply the time of day whenever the field enters a map (idempotent).
function daynight.install()
  if live.installed then return end
  live.installed = true
  require("fe.field").entered:connect(function() daynight.apply() end, 0, "fe.daynight.apply")
  game.updated:connect(function(dt) daynight.tick(dt) end, 0, "fe.daynight.clock")
end

--- Advance the clock by `dt` real seconds (fe.game does this each tick
--- once installed), unless the field world is paused.
function daynight.tick(dt)
  local world = require("fe.field").world
  if daynight.minutes_per_second > 0 and not (world and world.paused) then
    set_minutes(clock.minutes + dt * daynight.minutes_per_second)
  end
end

--- "day" or "night".
---@return string
function daynight.phase()
  local hour, from, to = daynight.hour(), daynight.day_hours[1], daynight.day_hours[2]
  return hour >= from and hour < to and "day" or "night"
end

--- Switch to the other phase with a fade (call from a task). Returns the
--- new phase. If the task is cancelled meanwhile, the screen is uncovered.
---@param options? {duration?: number, transition?: string}
---@return string
function daynight.toggle(options)
  options = options or {}
  local kind, duration = options.transition or "fade", options.duration or 0.4
  local target = daynight.phase() == "day" and "night" or "day"
  task.ensure(function()
    transition.out(kind, duration)
    daynight.set_hour(daynight.phases[target])
    transition.into(kind, duration)
  end, transition.clear)
  daynight.toggled:emit(target)
  return target
end

--- Adopt edited keyframes and phases on hot reload and show them.
function daynight.__reload(module, fresh)
  module.keys, module.phases, module.day_hours = fresh.keys, fresh.phases, fresh.day_hours
  if live.installed then daynight.apply() end
end

function daynight.set_hour(hour)
  daynight.install()
  set_minutes((hour % 24) * 60)
end

--- Run the clock (minutes of game time per real second); 0 stops it.
function daynight.start(options)
  daynight.install()
  daynight.minutes_per_second = options and options.minutes_per_second or 1
end

--- Event page conditions (fe.event loads them through its providers).
daynight.conditions = {
  hours = function(value)
    local from, to = value[1], value[2]
    local hour = daynight.hour()
    if from <= to then return hour >= from and hour < to end
    return hour >= from or hour < to
  end,
}

return daynight

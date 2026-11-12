--- Staging for cutscenes: tell characters to walk, be thrown, turn and hold
--- a pose, and wait for them. Everything here blocks the calling task
--- unless it says otherwise.
local actors = require("game.actors")
local fx = require("game.fx")
local task = require("game.task")

local director = {}

local function xz(point)
  if point.position then point = point.position end
  return { x = point.x, y = point.y, z = point.z }
end

--- Walk an actor to a position (or a map point) along the grid. options:
--- `speed`, `wait = false` (return at once), `face` (a direction to turn to
--- on arrival). Gives up waiting after `timeout` seconds (default 12).
function director.walk(entity, to, options)
  options = options or {}
  local target = xz(to)
  entity:move_to(target, { speed = options.speed })
  local function finish()
    local left = options.timeout or 12
    task.frame()
    while entity:valid() and entity:moving() and left > 0 do left = left - task.frame() end
    if entity:valid() and options.face then entity:face(options.face) end
  end
  if options.wait == false then return task.spawn(finish, "walk") end
  finish()
end

--- Walk several actors at once: {{entity, to, options}, ...}; waits for all.
function director.walk_all(list)
  local running = {}
  for _, entry in ipairs(list) do
    local options = {}
    for key, value in pairs(entry[3] or {}) do options[key] = value end
    options.wait = false
    running[#running + 1] = director.walk(entry[1], entry[2], options)
  end
  task.wait_until(function()
    for _, entry in ipairs(running) do
      if not task.done(entry) then return false end
    end
    return true
  end)
end

--- Move an actor in a straight line over `seconds`, ignoring the grid: a
--- dash in battle, a body thrown through the air (`arc` = height of the
--- throw). `ease` is one of fx.ease, fx.ease_out, fx.ease_in.
function director.slide(entity, to, seconds, options)
  options = options or {}
  local from, target = entity.position, xz(to)
  local ground_from = from.y
  local ground_to = target.y or entity:world():height_at(target, entity.layer)
  local ease = options.ease or fx.ease
  task.over(seconds, function(t)
    if not entity:valid() then return end
    local k = ease(t)
    local y = fx.lerp(ground_from, ground_to, k) + (options.arc or 0) * 4 * t * (1 - t)
    entity:place({ x = fx.lerp(from.x, target.x, k), y = y, z = fx.lerp(from.z, target.z, k) }, entity.layer, true)
  end)
end

function director.slide_async(entity, to, seconds, options)
  return task.spawn(function() director.slide(entity, to, seconds, options) end, "slide")
end

--- Turn an actor towards another actor, a position or a direction {x, z}.
function director.face(entity, toward)
  if type(toward) == "table" and toward.position then
    local a, b = entity.position, toward.position
    entity:face({ x = b.x - a.x, z = b.z - a.z })
  else
    entity:face(toward)
  end
end

director.pose = actors.pose

--- Fade a sprite's tint alpha (a character dissolving into light).
function director.fade_sprite(entity, from, to, seconds, color)
  local c = color or { 1, 1, 1 }
  task.over(seconds, function(t)
    if entity:valid() then entity:sprite({ tint = { c[1], c[2], c[3], fx.lerp(from, to, t) }, blend = "alpha" }) end
  end)
end

return director

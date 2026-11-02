--- Gentle help for a player who seems stuck: nonmodal hints after idling
--- and a short-lived route line. Nothing here moves the player.
---
--- ```lua
--- local stop = hint.boundary("barrier", "hint.barrier", 20) -- lingering near a blocked road
--- stop()                                                   -- the player left
--- hint.show_route()                                        -- 5 s line to the current objective
--- ```
--- * Idle: `settings.idle` seconds (90) without quest progress (any
---   `fe.quest.changed`) show `hint.next()`, by default the current
---   waypoint's hint (an objective's or stage's `hint` in quests.json, see
---   fe.field.waypoint), then the timer starts over.
--- * Boundaries: `hint.boundary(id, text, seconds?)` starts a linger timer
---   (default `settings.boundary`, 20 s) that shows `text` once when it
---   runs out; it returns a stop function (`hint.leave(id)` does the same).
---   `hint` zones on the field do this for their area.
--- * Timers only run while `hint.enabled()` (the `hints` setting, on by
---   default, a row of the settings screen) and not `hint.paused()`
---   (replaceable; by default whenever the player is not in control:
---   menus, message boxes, cutscenes, locks).
--- * `hint.show(text, options)` presents a hint (replaceable; the default
---   is a strip at the top of the screen for `settings.duration` seconds,
---   drawn by `hint.draw`); `hint.shown` fires (text) for every hint.
--- * `hint.show_route(target?)` draws a path line on walkable ground from
---   the player to `target` (a position, an entity, a waypoint, default
---   the current waypoint) for `settings.route_time` seconds (5) and
---   returns the path cells, or nil when there is no route. It never walks
---   the player.
local config = require("fe.config")
local game = require("fe.game")
local i18n = require("fe.i18n")
local quest = require("fe.quest")
local signal = require("fe.signal")
local tablex = require("fe.tablex")
local task = require("fe.task")

local hint = {
  settings = {
    idle = 90,              -- seconds without quest progress before the next-step hint
    boundary = 20,          -- default linger time of hint.boundary
    duration = 5,           -- seconds a hint stays on screen
    route_time = 5,         -- seconds the route line stays
    route_color = 0xffe08cd0,
    route_width = 2,
    width = 220,            -- hint strip width
  },
  --- Emitted with the text of every hint shown.
  shown = signal.new(),
}

--- The file's settings, for `__reload`.
hint.file_defaults = { settings = tablex.deep_copy(hint.settings) }

function hint.__reload(live, fresh)
  live.file_defaults.settings = tablex.adopt(live.settings, fresh.settings, live.file_defaults.settings)
end

i18n.defaults({ fe = { settings = { hints = "Hints" } } })

config.define("hints", true, nil, { label = "fe.settings.hints" })

-- Kept across hot reload: the idle timer, linger timers, the hint and the
-- route on screen.
local live = { idle = 0, boundaries = {}, current = nil, route = nil }

--- Whether hints are on (the `hints` setting).
---@return boolean
function hint.enabled()
  return config.get("hints") ~= false
end

--- Whether timers stand still: by default while the player is not in
--- control on the field (or there is no field). Replace for other rules.
---@return boolean
function hint.paused()
  local field = require("fe.field")
  if not (field.world and field.player and field.player:valid()) then return true end
  return not field.can_control()
end

--- The text of the next-step hint, or nil: the current waypoint's hint.
---@return string?
function hint.next()
  local target = require("fe.field.waypoint").target()
  return target and target.hint
end

--- Present a hint (see the header); replace to restyle.
---@param text string text or i18n key
---@param options? {duration?: number}
function hint.show(text, options)
  options = options or {}
  live.current = { text = i18n.t(text), until_time = task.time() + (options.duration or hint.settings.duration) }
  hint.shown:emit(live.current.text)
end

--- The hint on screen ({text, until_time}), or nil.
---@return table?
function hint.current()
  if live.current and task.time() >= live.current.until_time then live.current = nil end
  return live.current
end

--- Seconds counted towards the idle hint.
---@return number
function hint.idle_time()
  return live.idle
end

--- Start the idle timer over (quest progress does this).
function hint.progressed()
  live.idle = 0
end

--- Start a linger timer for `id` (replacing one with that id): `text` shows
--- once after `seconds` (default `settings.boundary`) of unpaused time.
--- Returns the function that stops it.
---@param id string
---@param text string text or i18n key
---@param seconds? number
---@return fun()
function hint.boundary(id, text, seconds)
  local entry = { text = text, left = seconds or hint.settings.boundary }
  live.boundaries[id] = entry
  return function()
    if live.boundaries[id] == entry then live.boundaries[id] = nil end
  end
end

--- Stop the linger timer of `id`.
---@param id string
function hint.leave(id)
  live.boundaries[id] = nil
end

--- Drop every timer, the hint and the route on screen.
function hint.reset()
  live.idle = 0
  live.boundaries = {}
  live.current = nil
  live.route = nil
end

--- Advance the timers (fe.game runs this every tick).
---@param dt number
function hint.update(dt)
  if not hint.enabled() or hint.paused() then return end
  for id, entry in pairs(live.boundaries) do
    if not entry.done then
      entry.left = entry.left - dt
      if entry.left <= 0 then
        entry.done = true
        if entry.text then hint.show(entry.text) end
      end
    end
  end
  live.idle = live.idle + dt
  if live.idle >= hint.settings.idle then
    live.idle = 0
    local text = hint.next()
    if text then hint.show(text) end
  end
end

-- A target's ground position: a position, an entity or a waypoint.
local function position_of(target)
  if target == nil then
    local current = require("fe.field.waypoint").current()
    return current and current.position, current and current.layer
  end
  if type(target) == "userdata" then return target.position, target.layer end
  if target.position then return target.position, target.layer end
  return target, target.layer
end

--- Draw a path line to `target` for `settings.route_time` seconds (see
--- the header). Returns the path cells, or nil when there is no route.
---@param target? table|Entity
---@return table[]?
function hint.show_route(target)
  local field = require("fe.field")
  local world, player = field.world, field.player
  if not (world and player and player:valid()) then return nil end
  local goal, layer = position_of(target)
  if not goal then return nil end
  local from = { x = math.floor(player.position.x), z = math.floor(player.position.z), layer = player.layer }
  local to = { x = math.floor(goal.x), z = math.floor(goal.z), layer = layer or player.layer }
  local mover = player:mover() or {}
  local cells = world:path(from, to, { diagonal = true, blocked_tags = mover.blocked_tags,
    required_tags = mover.required_tags })
  if #cells == 0 and not (from.x == to.x and from.z == to.z) then return nil end
  local points = { { x = player.position.x, y = player.position.y, z = player.position.z } }
  for _, cell in ipairs(cells) do
    local info = world:cell(cell)
    points[#points + 1] = { x = cell.x + 0.5, y = info and info.height or 0, z = cell.z + 0.5 }
  end
  live.route = { world = world, points = points, until_time = task.time() + hint.settings.route_time }
  field.hud:connect(function() hint.draw_route() end, 0, "fe.hint.route")
  return cells
end

--- The route on screen ({world, points, until_time}), or nil.
---@return table?
function hint.route()
  local route = live.route
  if route and task.time() >= route.until_time then live.route = nil end
  return live.route
end

--- Draw the route line (the field's `hud`); replace to restyle.
function hint.draw_route()
  local route = hint.route()
  if not route then return end
  if require("fe.field").world ~= route.world then return end
  local camera = route.world:camera()
  local previous
  for _, point in ipairs(route.points) do
    local x, y, visible = camera:project({ x = point.x, y = (point.y or 0) + 0.05, z = point.z })
    local here = visible and { x, y } or nil
    if previous and here then
      fe.canvas.line(previous[1], previous[2], here[1], here[2], hint.settings.route_color, hint.settings.route_width)
    end
    previous = here
  end
end

--- Draw the hint on screen (fe.game's `drawn`); replace to restyle.
function hint.draw()
  local current = hint.current()
  if not current then return end
  local ui = require("fe.ui")
  local cw = fe.canvas.size()
  local w = math.min(hint.settings.width, cw - 16)
  local _, th = ui.measure(current.text, { width = w - ui.theme.padding * 2 })
  local h = th + ui.theme.padding * 2
  local x = math.floor((cw - w) / 2)
  ui.window(x, 8, w, h, { alpha = 0.85 })
  ui.text(current.text, x + ui.theme.padding, 8 + ui.theme.padding, { width = w - ui.theme.padding * 2 })
end

quest.changed:connect(function() hint.progressed() end, 0, "fe.hint.progress")
game.updated:connect(function(dt) hint.update(dt) end, 0, "fe.hint.update")
game.drawn:connect(function() hint.draw() end, 0, "fe.hint.draw")

return hint

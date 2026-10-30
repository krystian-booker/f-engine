--- The standard game loop. A game's main.lua usually ends with:
---
--- ```lua
--- return require("fe.game").main{
---   start = function() scene.push(require("scenes.title")) end,
--- }
--- ```
--- It runs the task scheduler and scene stack, tracks playtime (saved in
--- the "clock" state section, read with `game.playtime()`), reloads data
--- files during development, and gives games hooks.
---
--- Each tick has two phases: `game.update(dt)` before the worlds move
--- (tasks, scenes' `update`, `updated`) and `game.post_update(dt)` after
--- they moved and produced this tick's events (scenes' `post_update`,
--- `post_updated`).
---
--- Pause and time. The game drives every loaded world's `paused` and
--- `time_scale`:
---   * `game.pause()` pauses every world (and worlds loaded meanwhile)
---     until `game.resume()`; `game.pause(world)` pauses one world. Both
---     return a release function, and a pause taken in a task ends with that
---     task at the latest. `game.paused` is true while a pause of every
---     world is held, and also while every loaded world is paused by any
---     means (per-world pauses, or `world.paused` set by hand; checked when
---     the game syncs the worlds, each tick). UI, scenes and tasks that are
---     not bound to a paused world keep running (see `task.bind`), so pause
---     screens work; playtime stops.
---   * `game.time_scale` multiplies gameplay dt and every world's time
---     scale (slow motion). It applies once: a task bound to a world runs
---     on that world's time (which already includes it), any other task on
---     game time (see `fe.task`, Time).
---   * `game.hit_stop(seconds, scale, world?)` slows one world (or all of
---     them) to `scale` (default 0, a freeze) for a moment;
---     `game.scale_time(key, scale, world?)` holds a named scale until it
---     is set to nil (cutscene fast-forward uses it).
--- A world's `paused` and `time_scale` are only written when the game's
--- value for it changes, so a game may still set them by hand.
local data = require("fe.data")
local scene = require("fe.scene")
local signal = require("fe.signal")
local state = require("fe.state")
local task = require("fe.task")

local game = {
  --- Emitted every tick with dt after tasks and scenes update.
  updated = signal.new(),
  --- Emitted every tick with dt after the worlds moved and scenes ran
  --- `post_update`; movement- and event-dependent logic goes here.
  post_updated = signal.new(),
  --- Emitted during draw after scenes; draw overlays here.
  drawn = signal.new(),
  --- Emitted with a game-relative path when a non-Lua file changes.
  file_changed = signal.new(),
  --- Emitted with true or false when `game.paused` changes.
  pause_changed = signal.new(),
  started = false,
  paused = false,
  time_scale = 1,
}

local clock = state.section("clock", function() return { playtime = 0 } end)
local holds = {}     -- token -> world name, or true for every world
-- {key, scale, world?, until_time?} in the order they were first set: a
-- list, not a table keyed by `key` (hit-stop keys are tables), so the scales
-- multiply in the same order on every run and the product is bit-identical.
local scales = {}
local applied = {}   -- world name -> {world, paused, time_scale} last written

--- Seconds played in the current game (saved with it).
function game.playtime()
  return clock.playtime
end

local function world_name(world)
  if world == nil then return nil end
  return type(world) == "string" and world or world.name
end

local function wanted(name)
  local paused = false
  for _, target in pairs(holds) do
    if target == true or target == name then
      paused = true
      break
    end
  end
  local scale = game.time_scale
  for _, entry in ipairs(scales) do
    if entry.world == nil or entry.world == name then scale = scale * entry.scale end
  end
  return paused, scale
end

local function held_all()
  for _, target in pairs(holds) do
    if target == true then return true end
  end
  return false
end

--- Push the game's pause and time scale to every loaded world (the game
--- calls this each tick and whenever they change), then update
--- `game.paused`: a pause of every world is held, or every loaded world is
--- paused by any means.
function game.sync_worlds()
  local seen = {}
  local names = fe.world.list()
  local all_paused = #names > 0
  for _, name in ipairs(names) do
    local world = fe.world.get(name)
    local paused, scale = wanted(name)
    local last = applied[name]
    if not last or last.world ~= world then last = { world = world, paused = false, time_scale = 1 } end
    if last.paused ~= paused then world.paused = paused end
    if last.time_scale ~= scale then world.time_scale = scale end
    applied[name] = { world = world, paused = paused, time_scale = scale }
    seen[name] = true
    if not world.paused then all_paused = false end
  end
  for name in pairs(applied) do
    if not seen[name] then applied[name] = nil end
  end
  local now_paused = held_all() or all_paused
  if now_paused ~= game.paused then
    game.paused = now_paused
    game.pause_changed:emit(now_paused)
  end
end

local function refresh_paused()
  game.sync_worlds()
end

--- Pause every world (no argument) or one world (a World or its name).
--- Returns the release function; inside a task the pause ends with the task
--- at the latest.
---@param world? World|string
---@return fun()
function game.pause(world)
  local token = {}
  holds[token] = world_name(world) or true
  refresh_paused()
  return task.cleanup(function()
    if holds[token] then
      holds[token] = nil
      refresh_paused()
    end
  end)
end

--- Release every pause of all worlds taken with `game.pause()`.
function game.resume()
  for token, target in pairs(holds) do
    if target == true then holds[token] = nil end
  end
  refresh_paused()
end

--- Whether the game holds a world paused (by name or World).
---@param world World|string
---@return boolean
function game.world_paused(world)
  return (wanted(world_name(world)))
end

-- Replace (in place), add (at the end) or with nil remove the scale held
-- under `key`; true if there was one.
local function set_scale(key, entry)
  for i, other in ipairs(scales) do
    if other.key == key then
      if entry then scales[i] = entry else table.remove(scales, i) end
      return true
    end
  end
  if entry then scales[#scales + 1] = entry end
  return false
end

--- Hold a named time scale for one world or all of them; `scale = nil`
--- removes it. Scales multiply each other and `game.time_scale`.
---@param key string
---@param scale number?
---@param world? World|string
function game.scale_time(key, scale, world)
  set_scale(key, scale and { key = key, scale = scale, world = world_name(world) } or nil)
  game.sync_worlds()
end

--- Slow one world (or every world) to `scale` (default 0: freeze) for
--- `seconds` of game time, then restore it. Returns a function that ends
--- it early.
---@param seconds number
---@param scale? number
---@param world? World|string
---@return fun()
function game.hit_stop(seconds, scale, world)
  local key = {}
  set_scale(key, { key = key, scale = scale or 0, world = world_name(world), until_time = task.time() + seconds })
  game.sync_worlds()
  return function()
    if set_scale(key, nil) then game.sync_worlds() end
  end
end

--- Drop every pause and time scale (new game, loading a save).
function game.reset_time()
  holds = {}
  scales = {}
  refresh_paused()
end

local function expire_scales()
  local now = task.time()
  for i = #scales, 1, -1 do
    local entry = scales[i]
    if entry.until_time and now >= entry.until_time then table.remove(scales, i) end
  end
end

--- The update phase (before the worlds move): tasks, scenes, `updated`.
--- `extra(dt)` runs after the scenes (the entry's own update).
---@param dt number
---@param extra? fun(dt: number)
function game.update(dt, extra)
  expire_scales()
  game.sync_worlds()
  local real_dt = dt
  dt = dt * game.time_scale
  if not game.paused then clock.playtime = clock.playtime + dt end
  -- Worlds already carry game.time_scale in their time_scale, so tasks bound
  -- to one scale the unscaled tick by it (no double scaling).
  task.update(dt, real_dt)
  scene.update(dt)
  if extra then extra(dt) end
  game.updated:emit(dt)
end

--- The post-update phase (after the worlds moved): scenes' `post_update`
--- and `post_updated`.
---@param dt number
---@param extra? fun(dt: number)
function game.post_update(dt, extra)
  dt = dt * game.time_scale
  scene.post_update(dt)
  if extra then extra(dt) end
  game.post_updated:emit(dt)
end

--- Build the entry module table the engine calls.
---@param options {start: fun(), update?: fun(dt: number), post_update?: fun(dt: number), draw?: fun(), validate?: fun()}
function game.main(options)
  local entry = {}

  function entry.init()
    game.started = true
    task.named("start", options.start)
  end

  function entry.update(dt)
    game.update(dt, options.update)
  end

  function entry.post_update(dt)
    game.post_update(dt, options.post_update)
  end

  function entry.draw()
    scene.draw()
    if options.draw then options.draw() end
    game.drawn:emit()
  end

  function entry.file_changed(path)
    data.reload(path)
    game.file_changed:emit(path)
  end

  entry.validate = options.validate
  return entry
end

return game

--- Changing maps: `field.warp` (a transition, the map loaded in the
--- background with `field.load_world` and its loading indicator, then an
--- autosave) and the autosave itself. fe.field installs this module
--- (`warp.install(field)`), which gives the field its `warp`, `load_world`,
--- `autosave` and `wants_autosave` functions; replace any of them on
--- fe.field, or this whole module with your own `fe/field/warp.lua`.
---
--- Warps: one at a time. `field.warp` cancels a warp another task still
--- runs (a door's detached warp when a script warps), whose lock and
--- screen cover are removed by its cleanup.
---
--- Autosave (`settings.autosave`: true, false or fn(map_id, point) ->
--- boolean) waits until no event or lock is active, so it never captures
--- the middle of a script.
local i18n = require("fe.i18n")
local scene = require("fe.scene")
local task = require("fe.task")
local transition = require("fe.transition")

local warp = {}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

i18n.defaults({ fe = { field = { loading = "Now loading..." } } })

-- Kept across hot reload: the autosave waiting to run and the task warping.
local live = { pending_autosave = nil, warp = nil }

--- Whether a warp to `map_id`/`point` autosaves: `options.autosave`, else
--- `settings.autosave` (a boolean or a function of map and point).
---@return boolean
function warp.wants_autosave(map_id, point, options)
  if options.autosave ~= nil then return options.autosave end
  local setting = field().settings.autosave
  if type(setting) == "function" then return setting(map_id, point) == true end
  return setting == true
end

--- Write the autosave as soon as no event or lock is active (immediately
--- if none is). Returns the waiting task.
---@return Task
function warp.autosave()
  if live.pending_autosave and live.pending_autosave:running() then return live.pending_autosave end
  local f = field()
  live.pending_autosave = task.detached("autosave", function()
    task.wait_until(f.can_trigger)
    f.remember_location()
    require("fe.save").autosave()
  end)
  return live.pending_autosave
end

local loading_view = { name = "loading", blocks_update = false }

function loading_view.draw()
  local ui = require("fe.ui")
  local text = i18n.t("fe.field.loading")
  local w = ui.measure(text) + 24
  local x, y = ui.anchor(w, 18, "bottom-right")
  local dots = math.floor(task.time() * 4) % 4
  ui.text(text:gsub("%.+$", "") .. string.rep(".", dots), x + 12, y + 4, { color = ui.theme.muted })
end

--- Load a map for `field.start` without stopping the game: the map loads
--- on a worker (`fe.world.load_async`) while this task waits, showing a
--- loading indicator after `settings.loading_delay` seconds. Returns the
--- World. Call from a task. If the task is cancelled while waiting, the
--- load is cancelled (`fe.world.cancel_load`): the engine discards what a
--- worker is still parsing, keeps what it decoded in its asset cache (as it
--- keeps every loaded asset) and never creates the world.
---@param map_id string
---@return World
function warp.load_world(map_id)
  local settings = field().settings
  local ticket = fe.world.load_async(settings.world_name, map_id)
  local started = task.time()
  local shown, ready = false, false
  task.ensure(function()
    task.wait_until(function()
      if not shown and task.time() - started >= settings.loading_delay then
        shown = true
        scene.push(loading_view)
      end
      return fe.world.load_progress(ticket) >= 1
    end)
    ready = true
  end, function()
    if shown then scene.remove(loading_view) end
    if not ready then fe.world.cancel_load(ticket) end
  end)
  return fe.world.finish_load(ticket)
end

-- Whether the current task is `job` or was started by it.
local function inside(job)
  local current = task.current()
  while current do
    if current == job then return true end
    current = current.parent
  end
  return false
end

--- Change maps with a transition, then autosave (see settings.autosave;
--- `options.autosave` overrides it). Call from a task. Control is locked
--- meanwhile; if the task is cancelled the lock and the screen cover are
--- removed. The map loads with `field.load_world` while the screen is
--- covered. In a skipping task (a skipped cutscene, which keeps the screen
--- covered until it ends) the warp leaves the screen alone. A warp another
--- task still runs is cancelled first: one warp owns the field at a time.
---@param options? {transition?: string, duration?: number, autosave?: boolean, music?: boolean}
function warp.warp(map_id, point, options)
  options = options or {}
  local f = field()
  local kind = options.transition or "fade"
  local duration = options.duration or 0.35
  local covered = task.skipping()
  local previous = live.warp
  if previous and previous:running() and not inside(previous) then previous:cancel() end
  local me = task.current()
  live.warp = me
  local unlock = f.lock()
  task.ensure(function()
    if not covered then transition.out(kind, duration) end
    f.start(map_id, point, { music = options.music, world = f.load_world(map_id) })
    if not covered then transition.into(kind, duration) end
  end, function()
    unlock()
    if not covered then transition.clear() end
    if live.warp == me then live.warp = nil end
  end)
  if f.wants_autosave(map_id, point, options) then f.autosave() end
end

-- What this module installed on fe.field, so a reload replaces its own
-- functions and leaves a game's replacements alone.
local installed = {}

--- Give `field` the warp functions it does not have yet (or had from this
--- module).
---@param field table the fe.field module
function warp.install(field)
  local functions = { warp = warp.warp, load_world = warp.load_world, autosave = warp.autosave,
    wants_autosave = warp.wants_autosave }
  for name, fn in pairs(functions) do
    if field[name] == nil or field[name] == installed[name] then
      field[name] = fn
      installed[name] = fn
    end
  end
end

--- Install the fresh functions after a hot reload of this file.
function warp.__reload(live_module)
  live_module.install(require("fe.field"))
end

return warp

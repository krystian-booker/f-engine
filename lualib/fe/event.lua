--- Event pages for map placements (RPG-Maker style).
---
--- A placement's props may contain:
--- ```json
--- "pages": [
---   {"script": "events.elder", "trigger": "action"},
---   {"when": {"flag": "quest_done"}, "script": "events.elder_thanks", "sprite": "sprites/elder.anim.json"}
--- ]
--- ```
--- The last page whose `when` conditions hold is active. Triggers: "action"
--- (Confirm while facing), "touch" (player walks into it), "auto" (runs once
--- when the page becomes active), "parallel" (runs while active, looping,
--- next to the entity's other scripts). A parallel page's task is cancelled
--- when its page stops being active; when its own script turned the page
--- off, the loop ends after that run and a run still waiting is cancelled
--- on the next `event.tick`, so the change that turned it off finishes.
--- Page fields `sprite`, `clip`, `solid`, `hidden` change the entity; with
--- no active page it is hidden and not solid, from the moment the pages
--- are attached. An entity whose placement has a mesh (`defaults.mesh`,
--- which fe.field sets for props) shows and hides the mesh instead of a
--- sprite (see `event.apply`).
--- Scripts are modules returning `function(ev)`; `ev.self` is the entity,
--- `ev.page` the page, `ev.world` its world and `ev.player` the field player.
---
--- An "auto" page starts while `event.auto_allowed()` holds: by default
--- while no blocking event runs; fe.field replaces it with
--- `field.can_trigger`, so no auto page starts under a field lock (a
--- cutscene, a command, a warp) and it starts when the lock lifts.
---
--- Module functions a game may replace: `event.matches`, `event.compare`,
--- `event.apply`, `event.active_index`, `event.stop_parallel`,
--- `event.auto_allowed` and `event.scope`.
---
--- Conditions: `flag`, `not_flag`, `var` and `check` are built in; kits
--- provide `quest`, `quest_stage`, `objective` (fe.quest), `hours`
--- (fe.daynight), `chapter` and `playing` (fe.story), loaded on first use
--- through `event.providers`; games add
--- keys with `event.condition`. A condition that raises is reported with the
--- page and entity and counts as false. `event.matches` and `event.compare`
--- are module functions, so games can replace them.
local state = require("fe.state")
local task = require("fe.task")

local event = {
  --- Condition handlers: conditions[key](value, ev) -> boolean.
  conditions = {},
  --- Modules that supply conditions on demand: providers[key] = module
  --- name; the module's `conditions[key]` is used.
  providers = { quest = "fe.quest", quest_stage = "fe.quest", objective = "fe.quest", hours = "fe.daynight",
    chapter = "fe.story", playing = "fe.story" },
  --- Number of blocking holds (running events; fe.field disables control
  --- while > 0). Take one with `event.block()`.
  blocking = 0,
}

--- The task page scripts start under, or nil for the calling task.
--- fe.field returns its current map's task, so scripts and everything they
--- start end with the map.
---@return Task?
function event.scope()
  return nil
end

--- Compare a value with an expression: a plain value (equality) or a string
--- such as ">= 3", "< 10", "!= 0".
function event.compare(actual, expression)
  if type(expression) ~= "string" then
    return actual == expression
  end
  local op, number = expression:match("^%s*([<>=!~]+)%s*(-?[%d%.]+)%s*$")
  if not op then
    return tostring(actual) == expression
  end
  local value, n = tonumber(actual) or 0, tonumber(number)
  if op == ">=" then return value >= n end
  if op == "<=" then return value <= n end
  if op == ">" then return value > n end
  if op == "<" then return value < n end
  if op == "==" or op == "=" then return value == n end
  if op == "!=" or op == "~=" then return value ~= n end
  error("unknown comparison '" .. op .. "'")
end

event.conditions.flag = function(value)
  for _, name in ipairs(type(value) == "table" and value or { value }) do
    if not state.flag(name) then return false end
  end
  return true
end
event.conditions.not_flag = function(value)
  for _, name in ipairs(type(value) == "table" and value or { value }) do
    if state.flag(name) then return false end
  end
  return true
end
event.conditions.var = function(value)
  for name, expression in pairs(value) do
    if not event.compare(state.var(name), expression) then return false end
  end
  return true
end
--- Escape hatch: {"check": "module.function"} calls a Lua predicate.
event.conditions.check = function(value, ev)
  local module_name, fn = value:match("^(.*)%.([%w_]+)$")
  return require(module_name)[fn](ev) == true
end

--- Register a condition key usable in page `when` tables.
function event.condition(key, fn)
  event.conditions[key] = fn
end

local function condition(key)
  local check = event.conditions[key]
  if check then return check end
  local provider = event.providers[key]
  check = provider and require(provider).conditions[key]
  if not check then
    error("unknown event condition '" .. key .. "'", 0)
  end
  event.conditions[key] = check
  return check
end

--- Whether a page's `when` conditions all hold.
function event.matches(page, ev)
  for key, value in pairs(page.when or {}) do
    if not condition(key)(value, ev) then return false end
  end
  return true
end

local holders = {}

--- Take a blocking hold (field control stays off while any is held).
--- Returns the release function; a hold taken inside a task is released
--- when that task ends at the latest.
---@return fun()
function event.block()
  local token = {}
  holders[token] = true
  event.blocking = event.blocking + 1
  return task.cleanup(function()
    if holders[token] then
      holders[token] = nil
      event.blocking = event.blocking - 1
    end
  end)
end

--- Drop every hold (new game, loading a save).
function event.reset()
  holders = {}
  event.blocking = 0
end

--- True while a blocking event is running.
function event.busy()
  return event.blocking > 0
end

--- Attach pages to an entity (fe.field does this for placements).
--- `defaults` describe the entity without an active page: `sprite`, `clip`,
--- `solid` (default true) and `mesh` (true when the entity shows a mesh
--- instead of a sprite). The active page's appearance (or the no-page
--- state: hidden, not solid) is applied at once.
function event.attach(entity, pages, defaults)
  local d = entity:data()
  d.pages = pages
  d.page_defaults = defaults or {}
  d.page_index = nil
  d.page_applied = false
  d.page_errors = {}
  event.refresh(entity)
end

--- The active page of an entity, or nil.
function event.page(entity)
  local d = entity:data()
  return d.pages and d.page_index and d.pages[d.page_index]
end

--- The index of the last page whose conditions hold, or nil. A condition
--- that raises is reported once per message and counts as false.
---@param entity Entity
---@param d? table the entity's data
---@return integer?
function event.active_index(entity, d)
  d = d or entity:data()
  local ev = { self = entity }
  local index
  for i, page in ipairs(d.pages) do
    local ok, result = pcall(event.matches, page, ev)
    if ok then
      d.page_errors[i] = nil
      if result then index = i end
    elseif d.page_errors[i] ~= tostring(result) then
      d.page_errors[i] = tostring(result)
      fe.log.error(string.format("Event page %d of '%s'%s: %s", i, entity.id,
        page.script and " (" .. page.script .. ")" or "", tostring(result)))
    end
  end
  return index
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

--- Cancel a parallel page's task whose page is no longer active, unless the
--- caller runs inside it (its script turned the page off in the middle of
--- an emit; event.tick cancels it next tick).
---@param entity Entity
---@param d? table the entity's data
function event.stop_parallel(entity, d)
  d = d or entity:data()
  local job = d.parallel
  if job and job:running() and d.parallel_page ~= event.page(entity) and not inside(job) then
    job:cancel()
  end
end

--- Give an entity the appearance of `page` (nil: no active page, so it is
--- hidden and not solid): a sprite from the page or the defaults, or the
--- mesh's visibility when `defaults.mesh` is set (a page `sprite` still
--- adds one). Replace to present pages differently.
---@param entity Entity
---@param page table?
---@param defaults table
function event.apply(entity, page, defaults)
  local visible = page ~= nil and not page.hidden
  local sprite = page and page.sprite or defaults.sprite
  -- `defaults.mesh` rather than asking the entity: on engines before the
  -- component getters, `entity:mesh()` with no settings removes the mesh.
  if defaults.mesh then
    entity:mesh({ visible = visible })
    if sprite then entity:sprite({ animation = sprite, clip = page and page.clip or defaults.clip or "idle", visible = visible }) end
  else
    entity:sprite({ animation = sprite, clip = page and page.clip or defaults.clip or "idle", visible = visible })
  end
  local solid = page and page.solid
  if solid == nil then solid = defaults.solid ~= false end
  entity:collider({ solid = visible and solid })
end

--- Re-evaluate which page is active and apply its appearance.
function event.refresh(entity)
  if not entity:valid() then return end
  local d = entity:data()
  if not d.pages then return end
  local index = event.active_index(entity, d)
  if index == d.page_index and d.page_applied then return end
  d.page_index = index
  d.page_applied = true
  event.stop_parallel(entity, d)
  d.auto_done = false
  event.apply(entity, index and d.pages[index], d.page_defaults)
end

--- Whether an "auto" page may start now (fe.field: `field.can_trigger`).
---@return boolean
function event.auto_allowed()
  return not event.busy()
end

--- Run a page's script in the current task (the caller handles blocking).
--- `extra` fields (such as `player`) are visible on `ev`. A "parallel"
--- page runs again every tick while it is the entity's active page.
function event.execute(entity, page, extra)
  local script = require(page.script)
  local ev = setmetatable({ self = entity, page = page, world = entity:world() }, { __index = extra or {} })
  if page.trigger == "parallel" then
    while true do
      script(ev)
      if not entity:valid() or event.page(entity) ~= page then return end
      task.frame()
    end
  end
  script(ev)
end

local function running(job)
  return job ~= nil and job:running()
end

--- The task running an entity's script or interaction (not its parallel
--- page), or nil.
---@return Task?
function event.running(entity)
  local job = entity:data().running
  return running(job) and job or nil
end

--- Run the active page's script (or a given page) for an entity in a new
--- task under `event.scope()`, bound to the entity's world (see
--- `task.bind`). Pages other than "parallel" hold `event.block()` while
--- running. Returns nil when the entity already runs a script of that kind
--- (a parallel page, or any other).
---@return Task?
function event.run(entity, page, extra)
  page = page or event.page(entity)
  if not page or not page.script then return nil end
  local d = entity:data()
  local parallel = page.trigger == "parallel"
  if running(parallel and d.parallel or d.running) then return nil end
  local function body()
    task.bind(entity:world())
    if not parallel then event.block() end
    event.execute(entity, page, extra)
  end
  local scope = event.scope()
  local job = scope and scope:spawn(page.script, body) or task.named(page.script, body)
  if parallel then
    d.parallel, d.parallel_page = job, page
  else
    d.running = job
  end
  return job
end

--- Called each tick by fe.field for auto/parallel pages; `extra` is passed
--- to the scripts (fe.field passes `{player = ...}`).
function event.tick(entities, extra)
  for _, entity in ipairs(entities) do
    if entity:valid() then
      local d = entity:data()
      event.stop_parallel(entity, d)
      local page = event.page(entity)
      if page and page.trigger == "parallel" then
        event.run(entity, page, extra)
      elseif page and page.trigger == "auto" and not d.auto_done and event.auto_allowed() and not running(d.running) then
        d.auto_done = true
        event.run(entity, page, extra)
      end
    end
  end
end

return event

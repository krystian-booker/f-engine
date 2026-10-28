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
--- when the page becomes active), "parallel" (runs while active, looping).
--- Page fields `sprite`, `clip`, `solid`, `hidden` change the entity.
--- Scripts are modules returning `function(ev)`; `ev.self` is the entity.
local state = require("fe.state")
local task = require("fe.task")

local event = {
  --- Condition handlers: conditions[key](value, ev) -> boolean.
  conditions = {},
  --- Number of blocking events running (fe.field disables control while > 0).
  blocking = 0,
}

local function compare(actual, expression)
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
    if not compare(state.var(name), expression) then return false end
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

local function matches(page, ev)
  for key, value in pairs(page.when or {}) do
    local check = event.conditions[key]
    if not check then
      error("unknown event condition '" .. key .. "'")
    end
    if not check(value, ev) then return false end
  end
  return true
end

--- Attach pages to an entity (fe.field does this for placements).
function event.attach(entity, pages, defaults)
  local d = entity:data()
  d.pages = pages
  d.page_defaults = defaults or {}
  d.page_index = nil
  event.refresh(entity)
end

--- The active page of an entity, or nil.
function event.page(entity)
  local d = entity:data()
  return d.pages and d.page_index and d.pages[d.page_index]
end

--- Re-evaluate which page is active and apply its appearance.
function event.refresh(entity)
  if not entity:valid() then return end
  local d = entity:data()
  if not d.pages then return end
  local ev = { self = entity }
  local index
  for i, page in ipairs(d.pages) do
    if matches(page, ev) then index = i end
  end
  if index == d.page_index then return end
  if d.running and d.running_page and d.pages[d.running_page].trigger == "parallel" then
    d.running:cancel()
  end
  d.page_index = index
  d.auto_done = false
  local page = index and d.pages[index]
  local defaults = d.page_defaults
  local visible = page ~= nil and not page.hidden
  entity:sprite({
    animation = page and page.sprite or defaults.sprite,
    clip = page and page.clip or defaults.clip or "idle",
    visible = visible,
  })
  local solid = page and page.solid
  if solid == nil then solid = defaults.solid ~= false end
  entity:collider({ solid = visible and solid })
end

--- Run the active page's script (or a given script) for an entity.
---@return Task?
function event.run(entity, page, extra)
  page = page or event.page(entity)
  if not page or not page.script then return nil end
  local d = entity:data()
  if d.running and d.running:running() then return nil end
  local script = require(page.script)
  local ev = setmetatable({ self = entity, page = page, world = entity:world() }, { __index = extra or {} })
  local blocking = page.trigger ~= "parallel"
  if blocking then event.blocking = event.blocking + 1 end
  d.running_page = d.page_index
  d.running = task.named(page.script, function()
    if page.trigger == "parallel" then
      while true do
        script(ev)
        task.frame()
      end
    else
      script(ev)
    end
  end)
  d.running:finally(function()
    if blocking then event.blocking = event.blocking - 1 end
    if not blocking then return end
    require("fe.dialogue").close()
  end)
  return d.running
end

--- True while a blocking event is running.
function event.busy()
  return event.blocking > 0
end

--- Called each tick by fe.field for auto/parallel pages.
function event.tick(entities)
  for _, entity in ipairs(entities) do
    if entity:valid() then
      local d = entity:data()
      local page = event.page(entity)
      if page and not (d.running and d.running:running()) then
        if page.trigger == "parallel" then
          event.run(entity, page)
        elseif page.trigger == "auto" and not d.auto_done and not event.busy() then
          d.auto_done = true
          event.run(entity, page)
        end
      end
    end
  end
end

return event

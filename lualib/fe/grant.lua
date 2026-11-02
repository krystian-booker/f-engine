--- Once-only state transactions keyed by event id: item grants,
--- recruitment, evidence, objective changes that must happen exactly once
--- and atomically.
---
--- ```lua
--- grant.run("clinic.supply", function()
---   inventory.add("dressing", 3)
---   state.set_flag("clinic_supplied")
--- end)                                   -- true the first time, then false
--- if grant.done("clinic.supply") then ... end
--- ```
--- `grant.run(id, fn)` runs `fn` unless `id` already ran (the ids are a
--- saved state section, "grants"), then records it. `fn` must not wait: it
--- runs in a coroutine of its own, outside any task, so a yield or a task
--- wait (`task.wait`, a message) is an error. If `fn` raises (or yields),
--- every state section it changed is put back as it was before the call,
--- in place, and the error is raised
--- again; nothing else ran meanwhile, since `fn` cannot wait, so no other
--- change is undone. The ids record is announced with
--- `state.changed("grant", id, "done")` (and "rolled_back" after a
--- failure, so event pages refresh).
local state = require("fe.state")
local tablex = require("fe.tablex")

local grant = {}

local record = state.section("grants", function() return {} end)

--- Whether the grant `id` has run.
---@param id string
---@return boolean
function grant.done(id)
  return record[id] == true
end

--- Forget every grant (tests, a debug menu).
function grant.reset()
  for key in pairs(record) do record[key] = nil end
end

local function replace_contents(target, source)
  for key in pairs(target) do target[key] = nil end
  for key, value in pairs(source) do target[key] = tablex.deep_copy(value) end
end

-- Put back every section that differs from `before`. Sections registered
-- during the call keep whatever they hold (they did not exist before).
local function rollback(before)
  local after = state.snapshot()
  for name, value in pairs(after) do
    local old = before[name]
    if old ~= nil and not tablex.deep_equal(old, value) then
      replace_contents(state.section(name), old)
    end
  end
end

--- Run `fn` once for `id` as a transaction (see the header). Returns true
--- when it ran, false when `id` had already run.
---@param id string
---@param fn fun()
---@return boolean
function grant.run(id, fn)
  if type(id) ~= "string" or id == "" then error("grant.run: the id must be a non-empty string", 2) end
  if record[id] then return false end
  local before = state.snapshot()
  local thread = coroutine.create(fn)
  local ok, err = coroutine.resume(thread)
  local waited = "grant '" .. id .. "' waited: grant functions must not yield (wait outside grant.run)"
  if ok and coroutine.status(thread) ~= "dead" then
    coroutine.close(thread)
    ok, err = false, waited
  elseif not ok and type(err) == "string" and err:find("must run inside a task", 1, true) then
    -- fn runs outside any task, so task waits refuse before they yield.
    err = waited .. ": " .. err
  end
  if not ok then
    rollback(before)
    state.changed:emit("grant", id, "rolled_back")
    error(err, 0)
  end
  record[id] = true
  state.changed:emit("grant", id, "done")
  return true
end

return grant

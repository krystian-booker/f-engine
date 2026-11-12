--- Coroutines for everything that takes time: cutscenes, battle turns, warps.
---
--- ```lua
--- task.spawn(function()
---   task.wait(0.5)                       -- seconds of game time
---   task.wait_until(function() return door_open end)
--- end, "open the door")
--- ```
--- `task.update(dt)` runs once a tick from main.lua. A task that raises is
--- reported and dropped; the others keep running.
local task = { list = {}, current = nil }

--- Start `fn` as a task; it first runs on the next update.
function task.spawn(fn, name)
  local entry = { co = coroutine.create(fn), name = name or "task", age = 0 }
  task.list[#task.list + 1] = entry
  return entry
end

--- Yield until the next tick; returns that tick's dt.
function task.frame()
  return coroutine.yield()
end

function task.wait(seconds)
  local elapsed = 0
  while elapsed < seconds do elapsed = elapsed + coroutine.yield() end
end

function task.wait_until(predicate)
  while not predicate() do coroutine.yield() end
end

--- Run `step(t)` every tick with t rising from 0 to 1 over `seconds`.
function task.over(seconds, step)
  local elapsed = 0
  while elapsed < seconds do
    step(elapsed / seconds)
    elapsed = elapsed + coroutine.yield()
  end
  step(1)
end

function task.cancel(entry)
  if entry then entry.cancelled = true end
end

function task.done(entry)
  return entry == nil or entry.finished == true
end

function task.clear()
  for _, entry in ipairs(task.list) do entry.cancelled = true end
  task.list = {}
end

function task.count()
  return #task.list
end

function task.update(dt)
  local running = task.list
  local kept = {}
  task.list = kept
  for _, entry in ipairs(running) do
    if not entry.cancelled then
      task.current = entry
      local ok, err = coroutine.resume(entry.co, dt)
      task.current = nil
      entry.age = entry.age + dt
      if not ok then
        fe.log.error(fe.dev.traceback(entry.co, "task '" .. entry.name .. "': " .. tostring(err)))
        entry.finished = true
      elseif coroutine.status(entry.co) == "dead" then
        entry.finished = true
      elseif not entry.cancelled then
        kept[#kept + 1] = entry
      end
    else
      entry.finished = true
    end
  end
  -- Tasks spawned during this update were appended to `kept` and first run
  -- next tick; after a task.clear() the list is a new, empty one.
end

return task

--- Cooperative tasks built on coroutines.
---
--- Gameplay code that takes time (cutscenes, battle turns, menus waiting for
--- a choice) is written top to bottom inside a task and waits with
--- `task.wait`, `task.wait_until` or `task.wait_signal`:
---
--- ```lua
--- task.spawn(function()
---   hero:move_to({x = 4, z = 6})
---   task.wait_until(function() return not hero:moving() end)
---   dialogue.say("Mira", "We made it.")
--- end)
--- ```
--- The game loop calls `task.update(dt)` once per tick (fe.game does this).
local task = {}

---@class Task
---@field name string
---@field status "running"|"done"|"cancelled"|"failed"
local Task = {}
Task.__index = Task

local tasks = {}       -- running tasks in start order
local by_thread = setmetatable({}, { __mode = "k" })
local now = 0

local function report(t, message)
  t.status = "failed"
  t.error = message
  local text = "Task '" .. t.name .. "' failed:\n" .. fe.dev.traceback(t.thread, tostring(message))
  fe.log.error(text)
end

local function resume(t, ...)
  if t.status ~= "running" then
    return
  end
  t.wake = nil
  local ok, err = coroutine.resume(t.thread, ...)
  if not ok then
    report(t, err)
  elseif coroutine.status(t.thread) == "dead" then
    t.status = "done"
  end
  if t.status ~= "running" then
    for _, child in ipairs(t.children) do
      child:cancel()
    end
    for _, waiter in ipairs(t.waiters) do
      waiter()
    end
    t.waiters = {}
  end
end

local function start(name, fn, ...)
  local parent = task.current()
  local t = setmetatable({
    name = name,
    status = "running",
    thread = coroutine.create(fn),
    children = {},
    waiters = {},
    parent = parent,
  }, Task)
  by_thread[t.thread] = t
  if parent then
    table.insert(parent.children, t)
  end
  table.insert(tasks, t)
  resume(t, ...)
  return t
end

--- Start a function as a task. Extra arguments are passed to it. A task
--- started from inside another task is cancelled with its parent.
---@param fn function
---@return Task
function task.spawn(fn, ...)
  return start("task", fn, ...)
end

--- Like task.spawn, with a name that appears in error reports.
---@param name string
---@param fn function
---@return Task
function task.named(name, fn, ...)
  return start(name, fn, ...)
end

--- The task running the current code, or nil outside tasks.
---@return Task?
function task.current()
  local thread = coroutine.running()
  return by_thread[thread]
end

local function suspend(condition)
  local t = task.current()
  if not t then
    error("This call waits, so it must run inside a task (task.spawn)", 3)
  end
  t.wake = condition
  return coroutine.yield()
end

--- Wait for game time to pass.
---@param seconds number
function task.wait(seconds)
  local until_time = now + (seconds or 0)
  suspend(function() return now >= until_time end)
end

--- Wait for the next tick.
function task.frame()
  local start = now
  suspend(function() return now > start end)
end

--- Wait until `predicate()` is true. Returns false if `timeout` expires first.
---@param predicate fun(): boolean
---@param timeout? number
---@return boolean
function task.wait_until(predicate, timeout)
  if predicate() then
    return true
  end
  local deadline = timeout and now + timeout
  local reached = false
  suspend(function()
    if predicate() then
      reached = true
      return true
    end
    return deadline ~= nil and now >= deadline
  end)
  return reached
end

--- Wait until a signal fires; returns its arguments.
---@param sig Signal
function task.wait_signal(sig)
  local result
  local disconnect = sig:connect(function(...)
    result = table.pack(...)
  end)
  suspend(function() return result ~= nil end)
  disconnect()
  return table.unpack(result, 1, result.n)
end

--- Wait for another task to finish.
---@param other Task
function task.join(other)
  task.wait_until(function() return other.status ~= "running" end)
  return other.status == "done"
end

--- Run functions concurrently and wait for all of them.
---@param fns function[]
function task.all(fns)
  local children = {}
  for i, fn in ipairs(fns) do
    children[i] = task.spawn(fn)
  end
  task.wait_until(function()
    for _, child in ipairs(children) do
      if child.status == "running" then
        return false
      end
    end
    return true
  end)
end

--- Run functions concurrently; return the index of the first to finish and
--- cancel the rest.
---@param fns function[]
---@return integer
function task.race(fns)
  local children = {}
  for i, fn in ipairs(fns) do
    children[i] = task.spawn(fn)
  end
  local winner
  task.wait_until(function()
    for i, child in ipairs(children) do
      if child.status ~= "running" then
        winner = i
        return true
      end
    end
    return false
  end)
  for _, child in ipairs(children) do
    child:cancel()
  end
  return winner
end

--- Stop this task and its children.
function Task:cancel()
  if self.status ~= "running" then
    return
  end
  self.status = "cancelled"
  for _, child in ipairs(self.children) do
    child:cancel()
  end
  for _, waiter in ipairs(self.waiters) do
    waiter()
  end
  self.waiters = {}
end

function Task:running()
  return self.status == "running"
end

--- Call `fn` when this task stops for any reason (including cancellation,
--- which never resumes the task). Returns a function that removes `fn`.
function Task:finally(fn)
  if self.status ~= "running" then
    fn()
    return function() end
  end
  table.insert(self.waiters, fn)
  return function()
    for i, waiter in ipairs(self.waiters) do
      if waiter == fn then
        table.remove(self.waiters, i)
        return
      end
    end
  end
end

--- Advance time and resume tasks whose wait is over.
---@param dt number
function task.update(dt)
  now = now + dt
  local snapshot = { table.unpack(tasks) }
  for _, t in ipairs(snapshot) do
    if t.status == "running" and t.wake then
      local ok, ready = pcall(t.wake)
      if not ok then
        report(t, ready)
      elseif ready then
        resume(t)
      end
    end
  end
  local alive = {}
  for _, t in ipairs(tasks) do
    if t.status == "running" then
      alive[#alive + 1] = t
    else
      by_thread[t.thread] = nil
    end
  end
  tasks = alive
end

--- Game seconds as seen by the scheduler.
function task.time()
  return now
end

--- Number of running tasks (debugging).
function task.count()
  return #tasks
end

--- Cancel every task except the caller and its ancestors (used when
--- loading a save or starting over from inside a task).
function task.cancel_all()
  local keep = {}
  local current = task.current()
  while current do
    keep[current] = true
    current = current.parent
  end
  for _, t in ipairs(tasks) do
    if not keep[t] then t:cancel() end
  end
end

return task

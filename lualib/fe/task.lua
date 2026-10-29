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
---
--- Lifecycle. A task ends as "done" (its function returned), "failed" (it
--- raised, including inside a `wait_until` predicate) or "cancelled". A task
--- started inside another task is its child:
---   * cancelling or failing a task cancels its running children;
---   * when a task finishes normally, children still running are handed to
---     its own parent (or become top-level), so work started near the end of
---     a script still completes. They keep what they inherited from it: its
---     world binding, speed and skipping (see Time below);
---   * `task.detached` starts a task with no parent, which only stops by
---     itself or when cancelled directly (or by `task.cancel_all`);
---   * `Task:spawn` starts a child of a given task and `Task:set_parent`
---     moves a task under another one (fe.field keeps a map's scripts under
---     one task this way, so they all end with the map).
--- Cleanup registered with `task.finally` / `Task:finally` runs on every
--- terminal state, latest first. `task.ensure(fn, cleanup)` scopes cleanup
--- to a block of code.
---
--- Time. Each task has its own clock (`task.clock()`), which `wait` and
--- `wait_until` timeouts use. One rule decides how fast it runs:
---   * a task bound to a world with `task.bind(world)` (children inherit
---     the binding) runs on that world's time: while the world is paused
---     the task is frozen (its waits are not even checked), and otherwise
---     its clock advances exactly as far as the world did this tick (the
---     tick times the world's `time_scale`, which already includes
---     `game.time_scale`, hit-stop and fast-forward), so field scripts stop
---     with the field and slow down with hit-stop;
---   * any other task (or one bound to a world that is not loaded) runs on
---     game time: the tick times `game.time_scale`, the same as
---     `task.time()`.
--- `Task.speed` (a number or a function) multiplies a task's clock and its
--- children's on top of that. A task marked `skipping` (see `fe.cutscene`)
--- and its children finish every `task.wait` at once.
---
--- `fe.game` calls `task.update(dt, real_dt)` once per tick.
local task = {}

---@class Task
---@field name string
---@field status "running"|"done"|"cancelled"|"failed"
---@field error any the error value when failed
---@field traceback string? the error with a traceback when failed
---@field parent Task?
---@field world string|false|nil the world this task is bound to (nil: its parent's)
---@field speed number|fun(): number|nil clock multiplier for this task and its children
---@field skipping boolean? waits of this task and its children end at once
---@field on_end_skip fun()? run by `task.end_skip` after it clears this task's `skipping`
local Task = {}
Task.__index = Task
task.Task = Task

local tasks = {}       -- running tasks in start order
local by_thread = setmetatable({}, { __mode = "k" })
local now = 0
local ticks = 0
local wake_error = {}  -- resume marker: the wait predicate raised
local finishing = 0    -- > 0 while children and cleanups of an ending task run

local function adopt(parent, child)
  local children = parent.children
  if #children >= 16 then
    local alive = {}
    for _, other in ipairs(children) do
      if other.status == "running" then alive[#alive + 1] = other end
    end
    children = alive
    parent.children = alive
  end
  children[#children + 1] = child
  child.parent = parent
end

local function speed_value(speed)
  return type(speed) == "function" and speed() or speed
end

-- Give `child` what it inherits through the tasks from `from` up to (not
-- including) `stop`: their world binding, speed and skipping. Used before
-- the child leaves them for another parent, so it runs on as it did.
local function carry(child, from, stop)
  local speeds = {}
  local t = from
  while t and t ~= stop do
    if child.world == nil then child.world = t.world end
    if t.skipping then child.skipping = true end
    if t.speed then speeds[#speeds + 1] = t.speed end
    t = t.parent
  end
  if #speeds == 0 then return end
  if child.speed then table.insert(speeds, 1, child.speed) end
  if #speeds == 1 then
    child.speed = speeds[1]
  else
    child.speed = function()
      local factor = 1
      for _, speed in ipairs(speeds) do factor = factor * speed_value(speed) end
      return factor
    end
  end
end

local function finish(t, status, message)
  if t.status ~= "running" then
    return
  end
  t.status = status
  if status == "failed" then
    t.error = message
    t.traceback = fe.dev.traceback(t.thread, tostring(message))
    if not t.quiet then
      fe.log.error("Task '" .. t.name .. "' failed:\n" .. t.traceback)
    end
  end
  if coroutine.status(t.thread) == "suspended" then
    coroutine.close(t.thread)
  end
  finishing = finishing + 1
  local children = t.children
  t.children = {}
  for _, child in ipairs(children) do
    if child.status == "running" then
      if status == "done" then
        carry(child, t, t.parent)
        child.parent = nil
        if t.parent then adopt(t.parent, child) end
      else
        child:cancel()
      end
    end
  end
  local cleanups = t.cleanups
  t.cleanups = {}
  for i = #cleanups, 1, -1 do
    local ok, err = pcall(cleanups[i].fn)
    if not ok then
      fe.log.error("Cleanup of task '" .. t.name .. "' failed: " .. tostring(err))
    end
  end
  finishing = finishing - 1
end

local function resume(t, ...)
  if t.status ~= "running" then
    return
  end
  t.wake = nil
  local ok, err = coroutine.resume(t.thread, ...)
  if not ok then
    finish(t, "failed", err)
  elseif coroutine.status(t.thread) == "dead" then
    finish(t, "done")
  elseif t.status ~= "running" then
    -- The task cancelled itself (or an ancestor) and yielded for good.
    coroutine.close(t.thread)
  end
end

local function start(name, fn, parent, options, ...)
  local t = setmetatable({
    name = name,
    status = "running",
    thread = coroutine.create(fn),
    children = {},
    cleanups = {},
    quiet = options and options.quiet,
    helper = options and options.helper,
    now = 0,
  }, Task)
  by_thread[t.thread] = t
  if parent then adopt(parent, t) end
  tasks[#tasks + 1] = t
  resume(t, ...)
  return t
end

--- The task running the current code, or nil outside tasks.
---@return Task?
function task.current()
  return by_thread[coroutine.running()]
end

local function require_task(level)
  local t = task.current()
  if not t then
    error("This call waits, so it must run inside a task (task.spawn)", level + 1)
  end
  return t
end

--- Start a function as a child of the current task (see the lifecycle notes
--- above). Extra arguments are passed to it.
---@param fn function
---@return Task
function task.spawn(fn, ...)
  return start("task", fn, task.current(), nil, ...)
end

--- Like task.spawn, with a name that appears in error reports.
---@param name string
---@param fn function
---@return Task
function task.named(name, fn, ...)
  return start(name, fn, task.current(), nil, ...)
end

--- Start a task with no parent: it is not cancelled when the task that
--- started it ends or is cancelled. Use it for work that must outlive the
--- caller, such as a map transition started by an interaction.
---@param name string
---@param fn function
---@return Task
function task.detached(name, fn, ...)
  return start(name, fn, nil, nil, ...)
end

local function suspend(condition)
  local t = require_task(3)
  if t.status ~= "running" then
    coroutine.yield() -- cancelled while still executing: never resumed
  end
  t.wake = condition
  local marker, err = coroutine.yield()
  if marker == wake_error then
    error(err, 0)
  end
end

local function skipping(t)
  while t do
    if t.skipping then return true end
    t = t.parent
  end
  return false
end

--- Whether a task (default: the current one) or a task it works for is
--- skipping: waits end at once and presentation should jump to its end
--- state.
---@param t? Task
---@return boolean
function task.skipping(t)
  return skipping(t or task.current())
end

--- Stop skipping: clears `skipping` on the current task and every task it
--- works for (a choice in a skipped cutscene asks the player and the scene
--- plays on from there). Then each of those tasks' `on_end_skip`, set by
--- whoever made it skip, runs in the current task (fe.cutscene uncovers the
--- screen this way).
function task.end_skip()
  local hooks = {}
  local t = task.current()
  while t do
    if t.skipping then
      t.skipping = nil
      hooks[#hooks + 1] = t.on_end_skip
      t.on_end_skip = nil
    end
    t = t.parent
  end
  for _, hook in ipairs(hooks) do hook() end
end

--- Seconds on the current task's clock (see Time above); outside tasks,
--- the scheduler's time.
---@return number
function task.clock()
  local t = task.current()
  return t and t.now or now
end

--- Wait for time to pass on the task's clock.
---@param seconds number
function task.wait(seconds)
  local t = require_task(2)
  if skipping(t) then return end
  local until_time = t.now + (seconds or 0)
  suspend(function() return t.now >= until_time or skipping(t) end)
end

--- Wait for the next tick.
function task.frame()
  local start_tick = ticks
  suspend(function() return ticks > start_tick end)
end

--- Wait until `predicate()` is true. Returns false if `timeout` expires first.
--- An error in the predicate is raised in the waiting task.
---@param predicate fun(): boolean
---@param timeout? number
---@return boolean
function task.wait_until(predicate, timeout)
  local t = require_task(2)
  if predicate() then
    return true
  end
  local deadline = timeout and t.now + timeout
  local reached = false
  suspend(function()
    if predicate() then
      reached = true
      return true
    end
    return deadline ~= nil and t.now >= deadline
  end)
  return reached
end

--- Call `fn` when the current task ends for any reason. Returns a function
--- that unregisters it.
---@param fn fun()
---@return fun()
function task.finally(fn)
  return require_task(2):finally(fn)
end

--- Make `fn` run exactly once: when the returned function is called, or when
--- the current task ends first (outside tasks only the returned function runs
--- it). Locks and holds use this so they never outlive their task.
---@param fn fun()
---@return fun()
function task.cleanup(fn)
  local done = false
  local forget
  local function run()
    if done then return end
    done = true
    if forget then forget() end
    fn()
  end
  local current = task.current()
  if current then forget = current:finally(run) end
  return run
end

--- Run `fn()` and then `cleanup()`, also when `fn` raises or the task is
--- cancelled meanwhile. Returns what `fn` returns; errors propagate after
--- the cleanup.
---@param fn function
---@param cleanup fun()
function task.ensure(fn, cleanup)
  local run = task.cleanup(cleanup)
  local results = table.pack(pcall(fn))
  run()
  if not results[1] then
    error(results[2], 0)
  end
  return table.unpack(results, 2, results.n)
end

--- Wait until a signal fires; returns its arguments. The listener is removed
--- even when the waiting task is cancelled.
---@param sig Signal
function task.wait_signal(sig)
  require_task(2)
  local result
  local stop = task.cleanup(sig:connect(function(...)
    if not result then result = table.pack(...) end
  end))
  suspend(function() return result ~= nil end)
  stop()
  return table.unpack(result, 1, result.n)
end

--- Wait for another task to finish; true if it finished normally.
---@param other Task
---@return boolean
function task.join(other)
  task.wait_until(function() return other.status ~= "running" end)
  return other.status == "done"
end

local function spawn_group(fns, kind)
  local parent = require_task(3)
  local children = {}
  for i, fn in ipairs(fns) do
    children[i] = start(kind .. "[" .. i .. "]", fn, parent, { quiet = true, helper = true })
  end
  return children
end

local function raise_failure(children, failed)
  for _, child in ipairs(children) do child:cancel() end
  error("task '" .. failed.name .. "' failed: " .. failed.traceback, 0)
end

--- Run functions concurrently and wait for all of them. If one fails, the
--- others are cancelled and the error is raised in the caller.
---@param fns function[]
function task.all(fns)
  local children = spawn_group(fns, "all")
  local failed
  task.wait_until(function()
    local running = false
    for _, child in ipairs(children) do
      if child.status == "failed" then
        failed = child
        return true
      end
      running = running or child.status == "running"
    end
    return not running
  end)
  if failed then raise_failure(children, failed) end
end

--- Run functions concurrently; return the index of the first to finish
--- normally and cancel the rest. If one fails first, the rest are cancelled
--- and the error is raised in the caller. Returns nil if every function was
--- cancelled from outside.
---@param fns function[]
---@return integer?
function task.race(fns)
  local children = spawn_group(fns, "race")
  local winner, failed
  task.wait_until(function()
    local running = false
    for _, child in ipairs(children) do
      if child.status == "failed" then
        failed = child
        return true
      end
      running = running or child.status == "running"
    end
    for i, child in ipairs(children) do
      if child.status == "done" then
        winner = i
        return true
      end
    end
    return not running
  end)
  if failed then raise_failure(children, failed) end
  for _, child in ipairs(children) do child:cancel() end
  return winner
end

--- Stop this task and its children. Cleanup runs immediately; the task is
--- never resumed. A task may cancel itself.
function Task:cancel()
  finish(self, "cancelled")
  local current = finishing == 0 and task.current()
  if current and current.status ~= "running" then
    coroutine.yield()
  end
end

function Task:running()
  return self.status == "running"
end

--- Start a named child of this task, wherever the call is made from
--- (`task.named` starts a child of the current task). Raises when this
--- task has ended.
---@param name string
---@param fn function
---@return Task
function Task:spawn(name, fn, ...)
  if self.status ~= "running" then
    error("task '" .. self.name .. "' has ended, so it cannot start '" .. name .. "'", 2)
  end
  return start(name, fn, self, nil, ...)
end

--- Move this task (with its children) under `parent`, or make it
--- top-level with nil: from now on it ends with `parent` instead of its old
--- parent. It keeps the world binding, speed and skipping it inherited.
---@param parent Task?
function Task:set_parent(parent)
  local old = self.parent
  if old == parent then return end
  local t = parent
  while t do
    if t == self then error("a task cannot be moved under itself or its children", 2) end
    t = t.parent
  end
  if parent and parent.status ~= "running" then
    error("task '" .. parent.name .. "' has ended, so it cannot take '" .. self.name .. "'", 2)
  end
  if old then
    carry(self, old)
    for i, other in ipairs(old.children) do
      if other == self then
        table.remove(old.children, i)
        break
      end
    end
  end
  self.parent = nil
  if parent then adopt(parent, self) end
end

--- Call `fn` when this task ends for any reason (done, failed or
--- cancelled); cleanups run latest first. If the task already ended, `fn`
--- runs now. Returns a function that unregisters `fn`.
---@param fn fun()
---@return fun()
function Task:finally(fn)
  if self.status ~= "running" then
    fn()
    return function() end
  end
  local entry = { fn = fn }
  local cleanups = self.cleanups
  cleanups[#cleanups + 1] = entry
  return function()
    for i, other in ipairs(self.cleanups) do
      if other == entry then
        table.remove(self.cleanups, i)
        return
      end
    end
  end
end

--- The task this one works for: helpers started by `task.all` and
--- `task.race` (and so `cutscene.parallel`) answer their caller; any other
--- task answers itself. Dialogue boxes and camera overrides belong to owners.
---@return Task
function Task:owner()
  local t = self
  while t.helper and t.parent do
    t = t.parent
  end
  return t
end

--- Bind the current task (and the tasks it starts) to a world, by World or
--- name; `false` unbinds it from its parent's world. Returns a function
--- that restores the previous binding.
---@param world World|string|false
---@return fun()
function task.bind(world)
  return require_task(2):bind(world)
end

--- See `task.bind`.
---@param world World|string|false
---@return fun()
function Task:bind(world)
  local previous = self.world
  if world and type(world) ~= "string" then world = world.name end
  self.world = world
  return function() self.world = previous end
end

--- The name of the world this task is bound to, or nil.
---@return string?
function Task:bound_world()
  local t = self
  while t do
    if t.world ~= nil then return t.world or nil end
    t = t.parent
  end
  return nil
end

local function world_rates()
  local rates = {}
  for _, name in ipairs(fe.world.list()) do
    local world = fe.world.get(name)
    if world.paused then
      rates[name] = false
    else
      rates[name] = world.time_scale
    end
  end
  return rates
end

-- How far a task's clock moves this tick (see Time above): false while its
-- world is paused. `dt` is game time, `real_dt` the unscaled tick that
-- worlds multiply by their time scale.
local function advance(t, rates, dt, real_dt)
  local factor = 1
  local world
  local current = t
  while current do
    local speed = current.speed
    if speed then
      factor = factor * (type(speed) == "function" and speed() or speed)
    end
    if world == nil and current.world ~= nil then world = current.world end
    current = current.parent
  end
  if world then
    local world_rate = rates[world]
    if world_rate == false then return false end
    if world_rate then return real_dt * world_rate * factor end
  end
  return dt * factor
end

--- Advance time and resume tasks whose wait is over. `dt` is game time
--- (it advances `task.time()` and unbound tasks); `real_dt` (default `dt`)
--- is the unscaled tick, which a world-bound task multiplies by its world's
--- `time_scale` so its clock moves exactly as far as the world.
---@param dt number
---@param real_dt? number
function task.update(dt, real_dt)
  real_dt = real_dt or dt
  now = now + dt
  ticks = ticks + 1
  local rates = world_rates()
  local snapshot = { table.unpack(tasks) }
  for _, t in ipairs(snapshot) do
    local step = t.status == "running" and advance(t, rates, dt, real_dt)
    if step then t.now = t.now + step end
    if step and t.wake then
      local ok, ready = pcall(t.wake)
      if not ok then
        resume(t, wake_error, ready)
      elseif ready then
        resume(t)
      end
    end
  end
  local count = 0
  for i = 1, #tasks do
    local t = tasks[i]
    tasks[i] = nil
    if t.status == "running" then
      count = count + 1
      tasks[count] = t
    else
      by_thread[t.thread] = nil
    end
  end
end

--- Game seconds as seen by the scheduler (the same for every task; see
--- `task.clock` for the current task's own clock).
function task.time()
  return now
end

--- Number of running tasks (debugging).
function task.count()
  local count = 0
  for _, t in ipairs(tasks) do
    if t.status == "running" then count = count + 1 end
  end
  return count
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
  for _, t in ipairs({ table.unpack(tasks) }) do
    if not keep[t] then t:cancel() end
  end
end

return task

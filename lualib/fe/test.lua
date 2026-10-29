--- A small test framework for game and library code, run headless with
--- `fengine_headless --test <game>`. Test files live in <game>/tests and
--- return a table of named test functions:
---
--- ```lua
--- return {
---   ["potions heal"] = function(t)
---     t.eq(heal(10), 10)
---     t.tick(30)                    -- advance half a second
---   end,
--- }
--- ```
--- Tests run as tasks, so they can wait on gameplay. Tasks still running
--- when a test ends (including ones it started) are cancelled before the
--- next test.
local task = require("fe.task")

local test = {}

local function describe(value)
  if type(value) == "string" then return string.format("%q", value) end
  return tostring(value)
end

local function deep_equal(a, b)
  if a == b then return true end
  if type(a) ~= "table" or type(b) ~= "table" then return false end
  for k, v in pairs(a) do
    if not deep_equal(v, b[k]) then return false end
  end
  for k in pairs(b) do
    if a[k] == nil then return false end
  end
  return true
end

local function helpers()
  local t = {}
  function t.ok(value, message)
    if not value then error(message or "expected a true value", 2) end
  end
  function t.eq(actual, expected, message)
    if not deep_equal(actual, expected) then
      error((message and message .. ": " or "") .. "expected " .. describe(expected) .. ", got " .. describe(actual), 2)
    end
  end
  function t.near(actual, expected, epsilon, message)
    epsilon = epsilon or 1e-6
    if type(actual) ~= "number" or math.abs(actual - expected) > epsilon then
      error((message and message .. ": " or "") .. "expected ~" .. describe(expected) .. ", got " .. describe(actual), 2)
    end
  end
  function t.error(fn, pattern)
    local ok, err = pcall(fn)
    if ok then error("expected an error", 2) end
    if pattern and not tostring(err):find(pattern) then
      error("error '" .. tostring(err) .. "' does not match '" .. pattern .. "'", 2)
    end
  end
  --- Advance the game by fixed ticks.
  function t.tick(count)
    for _ = 1, count or 1 do task.frame() end
  end
  --- Wait (ticking) until predicate is true; fails once `ticks` ticks have
  --- passed and at least `wall_seconds` (default 10) of real time, so work on
  --- engine worker threads (background world loads) gets time to finish
  --- however fast the build ticks.
  function t.wait_until(predicate, ticks, message, wall_seconds)
    local deadline = fe.dev.clock() + (wall_seconds or 10)
    local count = 0
    while not predicate() do
      if count >= (ticks or 600) and fe.dev.clock() >= deadline then
        error(message or "condition not reached in time", 2)
      end
      task.frame()
      count = count + 1
    end
  end
  return t
end

local function step()
  fe.harness.tick(1)
  local game = require("fe.game")
  if not game.started then
    -- Drive both phases the way a running game's entry module does.
    game.update(1 / 60)
    game.post_update(1 / 60)
  end
end

--- Run test modules; returns the number of failures.
---@param modules string[]
---@param filter? string only run tests whose name contains this
function test.run(modules, filter)
  local passed, failed = 0, 0
  for _, module_name in ipairs(modules) do
    local ok, cases = pcall(require, module_name)
    if not ok then
      print("FAIL " .. module_name .. " (load): " .. tostring(cases))
      failed = failed + 1
    else
      local names = {}
      for name in pairs(cases) do names[#names + 1] = name end
      table.sort(names)
      for _, name in ipairs(names) do
        if not filter or filter == "" or name:find(filter, 1, true) or module_name:find(filter, 1, true) then
          local errors_before = fe.harness.errors()
          fe.harness.begin_test()
          local result
          local runner = task.named(name, function()
            local success, err = xpcall(cases[name], function(m) return fe.dev.traceback(nil, tostring(m)) end, helpers())
            result = success and true or err
          end)
          local ticks = 0
          while runner.status == "running" and ticks < 36000 do
            step()
            ticks = ticks + 1
          end
          task.cancel_all()
          if result == true and fe.harness.errors() > errors_before then
            result = "script errors were reported during the test (see log)"
          end
          if result == true then
            passed = passed + 1
          else
            failed = failed + 1
            print("FAIL " .. module_name .. " :: " .. name .. "\n  " .. tostring(result or "timed out"))
          end
        end
      end
    end
  end
  print(string.format("%d passed, %d failed", passed, failed))
  return failed
end

return test

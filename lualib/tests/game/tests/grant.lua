-- fe.grant: once-only transactions by event id, rolled back on errors.
local grant = require("fe.grant")
local save = require("fe.save")
local state = require("fe.state")
local task = require("fe.task")

local inventory = state.section("grant_test_inventory", function() return { items = {} } end)

return {
  ["a grant runs once per event id and is saved"] = function(t)
    state.reset()
    local runs = 0
    t.eq(grant.run("clinic.supply", function()
      runs = runs + 1
      inventory.items.dressing = (inventory.items.dressing or 0) + 3
    end), true)
    t.eq(grant.run("clinic.supply", function() runs = runs + 1 end), false, "the second run is skipped")
    t.eq(runs, 1)
    t.ok(grant.done("clinic.supply"))
    t.eq(state.snapshot().grants["clinic.supply"], true, "the ids are a saved section")
    local snapshot = state.snapshot()
    state.reset()
    t.ok(not grant.done("clinic.supply"), "a new game forgets grants")
    state.restore(snapshot)
    t.ok(grant.done("clinic.supply"), "loading a save brings them back")
    t.eq(inventory.items.dressing, 3)
    grant.reset()
    t.ok(not grant.done("clinic.supply"))
    t.error(function() grant.run("", function() end) end, "non%-empty")
  end,

  ["a failing grant rolls back what it changed and raises"] = function(t)
    state.reset()
    state.set_flag("before")
    inventory.items.dressing = 1
    local events = {}
    local disconnect = state.changed:connect(function(kind, id, what)
      if kind == "grant" then events[#events + 1] = what end
    end)
    t.error(function()
      grant.run("locker", function()
        inventory.items.dressing = 4
        inventory.items.stabilizer = 2
        state.set_flag("locker_opened")
        state.set_var("count", 5)
        error("locker jammed")
      end)
    end, "locker jammed")
    disconnect()
    t.eq(inventory.items, { dressing = 1 }, "the section is back as it was, in place")
    t.ok(not state.flag("locker_opened"))
    t.eq(state.var("count"), nil)
    t.ok(state.flag("before"), "earlier state is untouched")
    t.ok(not grant.done("locker"), "a failed grant can run again")
    t.eq(events, { "rolled_back" })
    t.ok(grant.run("locker", function() inventory.items.dressing = 4 end))
    t.eq(inventory.items.dressing, 4)
  end,

  ["a grant that waits is refused and rolled back"] = function(t)
    state.reset()
    t.error(function()
      grant.run("slow", function()
        state.set_flag("slow_started")
        task.frame()
      end)
    end, "must not yield")
    t.ok(not state.flag("slow_started"))
    t.ok(not grant.done("slow"))
    local nested
    t.ok(grant.run("outer", function()
      nested = grant.run("inner", function() state.set_flag("inner") end)
    end))
    t.ok(nested and grant.done("inner") and state.flag("inner"), "grants nest")
    t.error(function()
      grant.run("outer2", function()
        grant.run("inner2", function() state.set_flag("inner2") end)
        error("outer failed")
      end)
    end, "outer failed")
    t.ok(not grant.done("inner2") and not state.flag("inner2"), "a failing outer grant undoes the inner one")
  end,
}

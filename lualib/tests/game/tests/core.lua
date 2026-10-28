local data = require("fe.data")
local save = require("fe.save")
local scene = require("fe.scene")
local schema = require("fe.schema")
local signal = require("fe.signal")
local state = require("fe.state")
local task = require("fe.task")
local tween = require("fe.tween")

return {
  ["tasks wait for time and conditions"] = function(t)
    local log = {}
    local flag = false
    local child = task.spawn(function()
      task.wait(0.1)
      log[#log + 1] = "waited"
      task.wait_until(function() return flag end)
      log[#log + 1] = "flag"
    end)
    t.tick(3)
    t.eq(#log, 0)
    t.tick(5)
    t.eq(log, { "waited" })
    flag = true
    t.tick(1)
    t.eq(log, { "waited", "flag" })
    t.eq(child.status, "done")
  end,

  ["task.race cancels the losers; cancelling a parent stops children"] = function(t)
    local slow_finished = false
    local winner = task.race({
      function() task.wait(0.05) end,
      function() task.wait(1); slow_finished = true end,
    })
    t.eq(winner, 1)
    t.tick(80)
    t.ok(not slow_finished)
    local inner
    local outer = task.spawn(function()
      inner = task.spawn(function() task.wait(10) end)
      task.wait(10)
    end)
    outer:cancel()
    t.eq(inner.status, "cancelled")
  end,

  ["signals pass values and can stop propagation"] = function(t)
    local s = signal.new()
    local seen = {}
    s:connect(function(v) seen[#seen + 1] = "low" .. v end)
    s:connect(function(v) seen[#seen + 1] = "high" .. v; return v == 2 end, 10)
    s:emit(1)
    t.ok(s:emit(2))
    t.eq(seen, { "high1", "low1", "high2" })
    task.spawn(function() task.wait(0.05); s:emit(7) end)
    t.eq(task.wait_signal(s), 7)
  end,

  ["tweens reach their goal"] = function(t)
    local box = { x = 0 }
    task.join(tween.to(box, { x = 10 }, 0.2, "out_cubic"))
    t.eq(box.x, 10)
    t.near(tween.approach(0, 10, 1e9, 1), 10)
  end,

  ["state sections snapshot and restore in place"] = function(t)
    local inventory = state.section("test_inventory", function() return { potion = 1 } end)
    state.set_flag("met_elder")
    state.set_var("chapter", 2)
    local snapshot = state.snapshot()
    inventory.potion = 5
    state.set_flag("met_elder", false)
    state.restore(snapshot)
    t.eq(inventory.potion, 1)
    t.ok(state.flag("met_elder"))
    t.eq(state.var("chapter"), 2)
  end,

  ["saves migrate older versions"] = function(t)
    state.set_var("gold_coins", 3)
    save.version = 1
    save.write("test_slot")
    save.version = 2
    save.migrations[1] = function(d) d.state.vars.migrated = true; return d end
    state.reset()
    t.ok(save.load("test_slot"))
    t.eq(state.var("gold_coins"), 3)
    t.eq(state.var("migrated"), true)
    save.version = 1
    save.migrations = {}
    save.delete("test_slot")
  end,

  ["schemas validate, fill defaults and collect references"] = function(t)
    schema.define("thing", {
      id = "string",
      count = { type = "integer", min = 0, default = 1 },
      kind = { enum = { "a", "b" } },
      friend = { type = { ref = "things" }, optional = true },
      tags = { type = { list = "string" }, default = {} },
    })
    local good = { id = "x", kind = "a", friend = "y" }
    local ok, errors, refs = schema.validate("thing", good)
    t.ok(ok, table.concat(errors, "; "))
    t.eq(good.count, 1)
    t.eq(refs[1].id, "y")
    local bad_ok, bad_errors = schema.validate("thing", { id = 3, kind = "z", count = -1 })
    t.ok(not bad_ok)
    t.eq(#bad_errors, 3)
    t.ok(schema.export().schemas.thing.fields.kind.options[2] == "b")
  end,

  ["scenes stack, defer changes and block updates"] = function(t)
    local calls = {}
    local base = { update = function() calls[#calls + 1] = "base" end }
    local overlay = { blocks_update = false, update = function() calls[#calls + 1] = "overlay" end }
    local modal = {
      update = function(self)
        calls[#calls + 1] = "modal"
        scene.pop()
      end,
    }
    scene.switch(base)
    scene.push(overlay)
    scene.update(0)
    t.eq(calls, { "base", "overlay" })
    calls = {}
    scene.push(modal)
    scene.update(0)
    t.eq(calls, { "modal" })
    t.eq(scene.top(), overlay)
    scene.clear()
  end,

  ["data tables load from JSON"] = function(t)
    t.ok(not data.has_table("nothing"))
    t.error(function() data.get("nothing", "x") end, "not loaded")
  end,
}

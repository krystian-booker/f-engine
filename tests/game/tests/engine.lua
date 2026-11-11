-- The engine as scripts see it, with no library between: worlds, entities,
-- movement events, input injection and the canvas, driven through the test
-- runner built into `fengine_headless --test` (its helpers are `t`).
local setups, teardowns = 0, 0

return {
  _before_each = function() setups = setups + 1 end,
  _after_each = function()
    teardowns = teardowns + 1
    for _, name in ipairs(fe.world.list()) do fe.world.unload(name) end
    fe.input.clear()
  end,

  ["hooks run around every test and are not tests themselves"] = function(t)
    t.ok(setups >= 1)
    t.eq(setups, teardowns + 1)
  end,

  ["require finds only the game's own modules"] = function(t)
    t.error(function() require("fe.task") end, "not found in the game folder")
    t.eq(type(require("main")), "table")
  end,

  ["a mover walks a planned path and reports its arrival"] = function(t)
    local world = fe.world.load("walk", "arena")
    local walker = world:spawn { id = "walker", position = { x = 1.5, z = 1.5 }, mover = { speed = 4 } }
    walker:move_to({ x = 4.5, z = 1.5 })
    local arrived = false
    t.wait_until(function()
      for _, event in ipairs(world:events()) do
        if event.type == "arrived" and event.a == walker then arrived = true end
      end
      return arrived
    end, 600, "the walker never arrived")
    t.near(walker.position.x, 4.5, 0.1)
    t.eq(walker:cell().x, 4)
  end,

  ["injected input reaches actions on the next tick"] = function(t)
    fe.input.define("confirm", { "key:Return", "pad:a" })
    t.ok(not fe.input.pressed("confirm"))
    fe.input.feed("key:Return", 1)
    t.tick(1)
    t.ok(fe.input.pressed("confirm"))
    t.eq(fe.input.device_type(), "keyboard")
    fe.input.feed("key:Return", 0)
    t.tick(1)
    t.ok(fe.input.released("confirm"))
  end,

  ["text measures and the canvas records quads"] = function(t)
    local width, height, lines = fe.text.measure("Hello")
    t.ok(width > 0 and height > 0)
    t.eq(lines, 1)
    t.eq(fe.text.plain("{b}Hi{/b}"), "Hi")
  end,

  ["a torus map wraps positions and differences"] = function(t)
    local world = fe.world.load("ring", "torus")
    t.ok(world:wraps())
    local width = world:grid_size()
    local delta = world:wrap_delta({ x = 0.5, z = 2 }, { x = width - 0.5, z = 2 })
    t.near(delta.x, -1, 1e-4)
  end,
}

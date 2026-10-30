-- Sizes, counts, speeds and positions scripts pass are bounded (probes colr,
-- colp, numeric): a collider of radius 3.4e38 used to hang every tick, and
-- positions at +-3.4e38 overflowed the integer cell and hash coordinates.
-- The wall-clock checks here (and in los, markup and library) are generous:
-- they tell a hang (minutes) from a slow sanitizer or loaded CI machine, not
-- a fast run from a slower one; the watchdog (3 s per test) is the real stop.
local function world(name)
  fe.harness.start()
  return fe.world.load(name, "arena")
end

return {
  ["collider radius and height are bounded and ticks stay fast"] = function(t)
    local w = world("bounds_colliders")
    local e = w:spawn { position = { x = 1, z = 1 }, mover = {}, collider = { radius = 0.4 } }
    e:move_to({ x = 9, z = 7 })
    t.error(function() e:collider { radius = 3.4e38 } end, "collider radius must be from 0 to 64")
    t.error(function() e:collider { radius = 30000 } end, "collider radius must be from 0 to 64")
    t.error(function() e:collider { height = 1e30 } end, "collider height must be from 0 to 1000")
    t.error(function() w:spawn { position = { x = 5, z = 5 }, collider = { radius = -1 } } end, "collider radius")
    local big = w:spawn { position = { x = 5, z = 5 }, collider = { radius = 64, height = 1000 } }
    local start = fe.dev.clock()
    fe.harness.tick(3)
    t.ok(fe.dev.clock() - start < 30, "ticks with a radius-64 collider took " .. (fe.dev.clock() - start) .. " s")
    big:collider { radius = 0.5 }
    big:destroy()
    fe.harness.tick(1)
    fe.world.unload("bounds_colliders")
  end,

  ["positions must be within the world bound"] = function(t)
    local w = world("bounds_positions")
    local e = w:spawn { position = { x = 1, z = 1 }, mover = {}, collider = {} }
    local bound = "must be from %-1000000 to 1000000"
    t.error(function() w:spawn { position = { x = 3.4e38, z = -3.4e38 } } end, bound)
    t.error(function() w:spawn { position = { x = 1e300, z = 1 } } end, "finite number")
    t.error(function() e:place({ x = 1e30, z = 1e30 }) end, bound)
    t.error(function() w:height_at({ x = 1e30, z = -1e30 }) end, bound)
    t.error(function() w:line_of_sight({ x = -1e9, z = 0.5 }, { x = 1e9, z = 0.5 }) end, bound)
    t.error(function() w:camera().target = { x = 1e7, y = 0, z = 0 } end, bound)
    t.error(function() e:attach(w:spawn { position = { x = 2, z = 2 } }, { y = 1e7 }) end, bound)
    t.error(function() e:drive({ x = 1e30, z = 0 }) end, bound)
    -- The bound itself is fine, far outside the grid.
    local far = w:spawn { position = { x = 1e6, z = -1e6 }, mover = {}, collider = { radius = 64 } }
    t.eq(far:cell().x, 1000000)
    t.eq(w:height_at({ x = 1e6, z = 1e6 }), 0)
    far:drive({ x = 1, z = 0 })
    local start = fe.dev.clock()
    fe.harness.tick(3)
    t.ok(fe.dev.clock() - start < 30, "ticks with entities at the world bound took " .. (fe.dev.clock() - start) .. " s")
    fe.world.unload("bounds_positions")
  end,

  ["speeds, sizes and counts are bounded"] = function(t)
    local w = world("bounds_sizes")
    local e = w:spawn { position = { x = 1, z = 1 }, sprite = {}, mover = {} }
    t.error(function() e:mover { speed = 3e38 } end, "mover speed must be from 0 to 1000")
    t.error(function() e:mover { max_step = 3e38 } end, "mover max_step must be from 0 to 1000")
    t.error(function() e:mover { climb_speed = 1e9 } end, "climb_speed must be a positive number up to 1000")
    t.error(function() e:move_to({ x = 9, z = 7 }, { speed = 3e38 }) end, "speed must be from 0 to 1000")
    t.error(function() e:drive({ x = 2000, z = 0 }) end, "drive velocity must be at most 1000")
    t.error(function() e:jump({ x = 9, z = 7 }, { height = 3e38 }) end, "jump height")
    t.error(function() e:jump({ x = 9, z = 7 }, { duration = 1e30 }) end, "jump duration")
    t.error(function() e:platform { size = { x = 1e6, z = 1 } } end, "platform size")
    t.error(function() e:sprite { scale = 1e30 } end, "sprite scale")
    t.error(function() e:sprite { speed = 1e30 } end, "sprite speed")
    t.error(function() e:light { radius = 1e30 } end, "Invalid light radius")
    t.error(function() e:emitter { max = (1 << 32) + 5 } end, "'max' must be an integer from")
    t.error(function() e:emitter { rate = 1e300 } end, "finite number")
    t.error(function() w:set_cell({ x = 1, z = 1 }, { height = 3e38 }) end, "height must be from")
    t.error(function() w:camera().distance = 3e38 end, "distance must be from 0.1 to 10000")
    t.error(function() fe.text.measure("x", { size = 1e30 }) end, "Text size must be 0 to 1024")
    t.error(function() fe.text.measure("x", { width = 1e30 }) end, "Text width must be from 0 to 1000000")
    t.error(function() fe.harness.tick(1 << 30) end, "0 to 36000 ticks")
    t.error(function() w:area({ x = 1, z = 1 }, "square", 0, 2147483647) end, "area max")
    t.error(function() w:burst({ x = 1, z = 1 }, {}, math.maxinteger) end, "count must be an integer")
    local cells = {}
    for i = 1, 65537 do cells[i] = { x = i % 12, z = i % 10 } end
    t.error(function() w:overlay("many", cells) end, "at most 65536 cells")
    cells[65537] = nil
    w:overlay("many", cells)
    for i = 2, 256 do w:overlay("o" .. i, { { x = 1, z = 1 } }) end
    t.error(function() w:overlay("one too many", { { x = 1, z = 1 } }) end, "at most 256 overlays")
    w:overlay("many", { { x = 2, z = 2 } }) -- replacing one is fine
    w:clear_overlay()
    -- Values inside the ranges still work.
    e:mover { speed = 1000, climb_speed = 1000 }
    e:jump({ x = 3, z = 1 }, { height = 2, duration = 60 })
    e:light { radius = 1000 }
    w:camera().distance = 10000
    fe.harness.tick(2)
    fe.world.unload("bounds_sizes")
  end,

  ["numbers beyond a parameter's C++ type raise instead of wrapping"] = function(t)
    -- Probe intwrap2: math.maxinteger reached an int parameter as -1.
    for _, v in ipairs { math.maxinteger, math.mininteger, math.maxinteger - 1024, 1 << 32 } do
      t.error(function() fe.text.paginate("a b", { width = 1 }, v) end,
        "integer from %-2147483648 to 2147483647 expected")
    end
    t.error(function() fe.text.paginate("a b", { width = 1 }, 1.5) end, "integer expected")
    t.error(function() fe.canvas.rect(0, 0, 1e300, 1) end, "finite number expected")
    t.error(function() fe.canvas.rect(0, 0, 0 / 0, 1) end, "finite number expected")
    t.error(function() fe.canvas.set_layer(1 << 31) end, "integer from")
    t.ok(#fe.text.paginate("a b", { width = 1 }, 1) >= 1)
  end,

  ["random streams handle the whole integer range"] = function(t)
    local I, J = math.maxinteger, math.mininteger
    local r = fe.random.new(7)
    for _ = 1, 100 do
      t.ok(math.type(r:int(J, I)) == "integer")
      local a = r:int(J, J + 1)
      t.ok(a == J or a == J + 1)
      local b = r:int(I - 1, I)
      t.ok(b == I - 1 or b == I)
      local c = r:int(-(1 << 62), 1 << 62)
      t.ok(c >= -(1 << 62) and c <= 1 << 62)
      local d = r:range(-1.7e308, 1.7e308)
      t.ok(d >= -1.7e308 and d < 1.7e308)
    end
    t.eq(r:int(I, I), I)
    t.eq(r:int(J, J), J)
    t.error(function() r:int(5, 1) end, "low must not be above high")
    t.error(function() r:range(-1 / 0, 1 / 0) end, "finite number")
  end,
}

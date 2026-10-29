-- Lines of sight are traced only over the grid (probes los, numeric): a line
-- 2e9 units long took 40 seconds, cell by cell, and 1e30 overflowed the count.
return {
  ["line_of_sight is fast and exact for lines as long as the world bound"] = function(t)
    local w = fe.world.load("los", "arena")
    local start = fe.dev.clock()
    -- Cells (8, 2) and (9, 2) of the arena are raised to height 1.
    t.eq(w:line_of_sight({ x = -1e6, y = 0.5, z = 2.5 }, { x = 1e6, y = 0.5, z = 2.5 }), false)
    t.eq(w:line_of_sight({ x = -1e6, y = 5, z = 2.5 }, { x = 1e6, y = 5, z = 2.5 }), true)
    t.eq(w:line_of_sight({ x = -1e6, y = 0.5, z = -1e6 }, { x = 1e6, y = 0.5, z = 1e6 }), true)
    t.eq(w:line_of_sight({ x = -1e6, y = 0.5, z = 50 }, { x = 1e6, y = 0.5, z = 50 }), true)
    t.eq(w:line_of_sight({ x = 1.5, y = 0.5, z = 2.5 }, { x = 11.5, y = 0.5, z = 2.5 }), false)
    t.ok(fe.dev.clock() - start < 0.5, "took " .. (fe.dev.clock() - start) .. " s")
    t.error(function() w:line_of_sight({ x = -1e30, z = -1e30 }, { x = 1e30, z = 1e30 }) end, "must be from")
    t.error(function() w:line_of_sight({ x = 0 / 0, z = 0 }, { x = 1, z = 1 }) end, "finite number")
    local hit = w:raycast({ x = -1e6, y = 0.5, z = 2.5 }, { x = 1, y = 0, z = 0 }, { max = 3e38 })
    t.ok(hit ~= nil, "the ray reaches the arena")
    fe.world.unload("los")
  end,
}

-- Entity handles never come to name a later entity (probe wrap): entt reuses
-- an entity's slot with a 12-bit version, so after 4095 spawn/destroy cycles
-- an old handle used to be valid again and move the new entity.
return {
  ["a destroyed entity's handle stays invalid when its slot is reused"] = function(t)
    local w = fe.world.load("handles", "arena")
    local x = w:spawn { id = "old", position = { x = 1, z = 1 } }
    local key = x.key
    x:destroy()
    local keys = { [key] = true }
    local y
    for i = 1, 5000 do
      y = w:spawn { position = { x = 2, z = 2 } }
      t.ok(not x:valid(), "the old handle stays invalid")
      t.ok(x ~= y, "the old handle never equals a new entity")
      t.ok(not keys[y.key], "keys are never reused")
      keys[y.key] = true
      if i < 5000 then y:destroy() end
    end
    t.eq(tostring(x), "Entity(destroyed)")
    t.eq(x.key, key, "a destroyed entity keeps its key")
    t.error(function() return x.position end, "no longer exists")
    t.error(function() x:place({ x = 7, z = 7 }) end, "no longer exists")
    t.error(function() x:collider { radius = 1 } end, "no longer exists")
    t.eq(y.position.x, 2.0)
    t.ok(y:valid())
    t.eq(w:find("old"), nil)
    fe.world.unload("handles")
    t.ok(not y:valid(), "unloading the world invalidates its handles")
  end,
}

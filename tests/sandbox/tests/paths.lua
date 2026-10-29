-- Paths containing NUL are refused (probe escape2): the file system stops at
-- the NUL, so fe.app.capture("x.lua\0.png") wrote x.lua.
return {
  ["every path argument rejects an embedded NUL"] = function(t)
    for _, path in ipairs { "tests/paths.lua\0.png", "maps/arena/map.json\0", "main.lua\0" } do
      t.error(function() return fe.fs.read(path) end, "NUL")
      t.error(function() return fe.json.read(path) end, "NUL")
      t.error(function() return fe.app.capture(path .. ".png") end, "NUL")
      t.error(function() return fe.app.image_size(path) end, "NUL")
      t.error(function() return fe.app.sprite_clips(path) end, "NUL")
      t.error(function() return fe.audio.sound(path) end, "NUL")
      t.error(function() return fe.audio.music(path) end, "NUL")
      t.eq(fe.harness.reload(path), false, "not a module name")
      t.eq(fe.fs.exists(path), false)
    end
    t.error(function() return fe.harness.reload("main.lua\0.lua") end, "NUL")
    t.error(function() return fe.world.load("nul", "arena\0x") end, "Invalid map id")
    t.error(function() return fe.app.capture("shot\0.lua.png") end, "NUL")
    t.ok(fe.fs.exists("main.lua"))
  end,
}

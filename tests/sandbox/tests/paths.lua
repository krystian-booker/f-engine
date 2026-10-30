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
    for _, path in ipairs { "notes.txt\0", "mods/a\0/mod.json" } do
      t.error(function() return fe.fs.user_read(path) end, "NUL")
      t.error(function() return fe.fs.user_write(path, "x") end, "NUL")
      t.error(function() return fe.fs.user_list(path) end, "NUL")
      t.error(function() return fe.fs.user_remove(path) end, "NUL")
      t.error(function() return fe.save.write_blob("slot1", path, "x") end, "Blob names")
    end
    t.error(function() return fe.harness.reload("main.lua\0.lua") end, "NUL")
    t.error(function() return fe.world.load("nul", "arena\0x") end, "Invalid map id")
    t.error(function() return fe.app.capture("shot\0.lua.png") end, "NUL")
    t.ok(fe.fs.exists("main.lua"))
  end,
  -- User files are confined to <user data>/files; every escape is refused
  -- before anything touches the disk.
  ["user files never leave their folder"] = function(t)
    for _, path in ipairs { "../escape.txt", "/etc/passwd", "mods/../../escape.txt", "" } do
      t.error(function() return fe.fs.user_write(path, "x") end)
      t.error(function() return fe.fs.user_remove(path) end)
    end
    t.error(function() return fe.fs.user_read("../escape.txt") end, "escapes")
    t.error(function() return fe.fs.user_list("/") end, "escapes")
  end,
}

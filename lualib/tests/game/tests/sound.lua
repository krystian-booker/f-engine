local sound = require("fe.sound")
local task = require("fe.task")

-- Replaces fe.audio functions for the duration of `fn`, recording calls.
local function with_audio(overrides, fn)
  local saved = {}
  for name, replacement in pairs(overrides) do
    saved[name] = fe.audio[name]
    fe.audio[name] = replacement
  end
  local ok, err = pcall(fn)
  for name, original in pairs(saved) do fe.audio[name] = original end
  if not ok then error(err, 0) end
end

return {
  ["voice lines play on the voice bus and duck music"] = function(t)
    local played, ducked = {}, {}
    with_audio({
      sound = function(path, options) played[#played + 1] = { path = path, options = options }; return 7 end,
      duck = function(bus, amount, options) ducked[#ducked + 1] = { bus, amount, options and options.attack } end,
    }, function()
      sound.set_ducking({ music = 0.6, sfx = 0.2 }, { attack = 0.1 })
      table.sort(ducked, function(a, b) return a[1] < b[1] or (a[1] == b[1] and a[2] < b[2]) end)
      t.eq(ducked, { { "music", 0, nil }, { "music", 0.6, 0.1 }, { "sfx", 0.2, 0.1 } })
      t.eq(sound.voice("game.json", { volume = 0.8 }), 7)
      t.eq(played[1].path, "game.json")
      t.eq(played[1].options.bus, "voice")
      t.eq(played[1].options.volume, 0.8)
      t.eq(sound.voice("missing.wav"), nil)
      ducked = {}
      sound.set_ducking(false)
      table.sort(ducked, function(a, b) return a[1] < b[1] or (a[1] == b[1] and a[2] < b[2]) end)
      t.eq(ducked, { { "music", 0, nil }, { "sfx", 0, nil } })
      sound.set_ducking({ music = 0.5 })
    end)
  end,

  ["sound.wait waits for a voice to finish"] = function(t)
    local playing = true
    with_audio({ playing = function(voice) return voice == 3 and playing end }, function()
      local finished = false
      task.spawn(function()
        t.ok(sound.wait(3))
        finished = true
      end)
      t.tick(3)
      t.ok(not finished)
      t.ok(sound.playing(3))
      playing = false
      t.tick(1)
      t.ok(finished)
      t.ok(not sound.playing(nil))
      local timed_out
      playing = true
      task.spawn(function() timed_out = sound.wait(3, 0.05) end)
      t.tick(5)
      t.eq(timed_out, false)
    end)
  end,

  ["voice and effect ids resolve through the catalog, paths play as they are, files are looked up once"] = function(t)
    local saved_exists, saved_read = fe.fs.exists, fe.json.read
    local lookups = {}
    fe.fs.exists = function(path)
      lookups[#lookups + 1] = path
      return path == "catalog.json" or path == "game.json" or path == "audio/hello.wav"
    end
    fe.json.read = function() return { voice = { mira_hello = "audio/hello.wav" }, sfx = { ping = { file = "game.json", volume = 0.3 } } } end
    local ok, err = pcall(function()
      sound.load("catalog.json")
      local played = {}
      with_audio({ sound = function(path, options) played[#played + 1] = { path, options.bus, options.volume }; return 1 end }, function()
        t.eq(sound.voice("mira_hello"), 1)
        t.eq(played[1], { "audio/hello.wav", "voice", nil }, "a catalog id")
        t.eq(sound.voice("game.json"), 1)
        t.eq(played[2][1], "game.json", "a path")
        t.eq(sound.voice("nobody_home"), nil, "an unknown id is quiet")
        t.eq(sound.play("ping", { pitch = 1.2 }), 1)
        t.eq(played[3], { "game.json", nil, 0.3 })
        lookups = {}
        for _ = 1, 5 do sound.play("ping") end
        for _ = 1, 5 do sound.play("missing.wav") end
        t.eq(#lookups, 1, "a file is looked up once, also when it is missing")
      end)
    end)
    fe.json.read = function() return { voice = {}, sfx = {} } end
    sound.load("catalog.json")
    fe.fs.exists, fe.json.read = saved_exists, saved_read
    sound.forget_files()
    if not ok then error(err, 0) end
  end,

  ["set_voice checks the ranges fe.audio.sound takes"] = function(t)
    fe.audio.set_voice(1, { volume = 4, pan = -1, pitch = 0.1 })
    t.error(function() fe.audio.set_voice(1, { volume = 4.5 }) end, "volume must be from 0 to 4")
    t.error(function() fe.audio.set_voice(1, { pan = 2 }) end, "pan must be from %-1 to 1")
    t.error(function() fe.audio.set_voice(1, { pitch = 0 }) end, "pitch must be from 0.1 to 4")
  end,

  ["muffle and pause drive the bus effect and global pause"] = function(t)
    sound.muffle(true, { cutoff = 700 })
    t.eq(fe.audio.bus_effect("music").lowpass, 700)
    sound.muffle(false)
    t.eq(fe.audio.bus_effect("music").lowpass, 0)
    sound.pause(true)
    t.ok(fe.audio.paused())
    sound.pause(false)
    t.ok(not fe.audio.paused())
  end,
}

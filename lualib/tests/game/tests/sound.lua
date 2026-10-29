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

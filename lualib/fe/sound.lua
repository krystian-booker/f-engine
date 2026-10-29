--- Named music, sound effects and voice lines. Optional data/audio.json maps
--- ids to files and settings so scripts say `sound.music("battle")`:
--- {"music": {"battle": {"file": "audio/battle.ogg", "loop_start": 3.2}},
---  "sfx": {"cursor": "audio/cursor.wav", "confirm": {"file": "...", "volume": 0.6}},
---  "voice": {"mira_hello": "audio/vo/mira_hello.wav"}}
---
--- Voice lines play on the "voice" bus. While one is audible the mixer ducks
--- music (by half, by default; see `sound.set_ducking`), so dialogue can wait
--- for a line with `sound.wait(voice)` and stop it on advance with
--- `fe.audio.stop(voice, fade)`. Positional effects pass
--- `{position = {x, y, z}, world = w}` to `sound.play`.
local task = require("fe.task")

local sound = {}

local catalog = { music = {}, sfx = {}, voice = {} }

local ducking = { buses = { music = 0.5 }, attack = 0.08, release = 0.5 }
local ducking_applied = false

local function apply_ducking()
  for bus, amount in pairs(ducking.buses) do
    fe.audio.duck(bus, amount, { attack = ducking.attack, release = ducking.release })
  end
  ducking_applied = true
end

function sound.load(path)
  path = path or "data/audio.json"
  if fe.fs.exists(path) then
    local loaded = fe.json.read(path)
    catalog.music = loaded.music or {}
    catalog.sfx = loaded.sfx or {}
    catalog.voice = loaded.voice or {}
  end
  apply_ducking()
end

local function entry(kind, id)
  local value = catalog[kind][id]
  if type(value) == "string" then return { file = value } end
  if value then return value end
  if id:find("[/%.]") then return { file = id } end
  return nil
end

--- Whether the catalog has a music id.
function sound.has_music(id)
  return catalog.music[id] ~= nil
end

--- Play music by id or path (crossfades). Options override the catalog.
function sound.music(id, options)
  local e = entry("music", id)
  if not e then
    fe.log.warn("Unknown music '" .. tostring(id) .. "'")
    return
  end
  local settings = {}
  for k, v in pairs(e) do settings[k] = v end
  for k, v in pairs(options or {}) do settings[k] = v end
  settings.file = nil
  fe.audio.music(e.file, settings)
end

--- Fade out one music layer, or all music (same arguments as fe.audio).
---@param layer? string
---@param fade? number seconds
function sound.stop_music(layer, fade)
  fe.audio.stop_music(layer, fade)
end

--- Play a sound effect by id or path; unknown ids are ignored quietly so
--- placeholder sound names never crash a game. Options are those of
--- fe.audio.sound (position/world for positional one-shots).
function sound.play(id, options)
  local e = entry("sfx", id)
  if not e or not fe.fs.exists(e.file) then return nil end
  local settings = { volume = e.volume, pitch = e.pitch }
  for k, v in pairs(options or {}) do settings[k] = v end
  return fe.audio.sound(e.file, settings)
end

--- Play a voice line by id or path on the "voice" bus; returns its voice id,
--- or nil when the line has no recording (so unvoiced text still works).
---@return integer?
function sound.voice(id, options)
  local e = entry("voice", id)
  if not e or not fe.fs.exists(e.file) then return nil end
  if not ducking_applied then apply_ducking() end
  local settings = { volume = e.volume, bus = "voice" }
  for k, v in pairs(options or {}) do settings[k] = v end
  return fe.audio.sound(e.file, settings)
end

--- Whether a voice (from sound.play or sound.voice) is still playing.
---@param voice? integer
function sound.playing(voice)
  return voice ~= nil and fe.audio.playing(voice)
end

--- Wait inside a task until a voice finishes. Returns false if `timeout`
--- seconds pass first.
---@param voice? integer
---@param timeout? number
function sound.wait(voice, timeout)
  return task.wait_until(function() return not sound.playing(voice) end, timeout)
end

--- Which buses duck while a voice line is audible, as {bus = amount 0-1}
--- (false for none), with optional {attack, release} in seconds.
---@param buses table<string, number>|false
---@param options? {attack?: number, release?: number}
function sound.set_ducking(buses, options)
  for bus in pairs(ducking.buses) do fe.audio.duck(bus, 0) end
  options = options or {}
  ducking = { buses = buses or {}, attack = options.attack or 0.08, release = options.release or 0.5 }
  apply_ducking()
end

--- Pause everything playing (menus); sounds started while paused still play.
---@param paused boolean
function sound.pause(paused)
  fe.audio.set_paused(paused)
end

--- Muffle a bus behind a low-pass, e.g. the music under a menu or underwater.
---@param on boolean
---@param options? {bus?: string, cutoff?: number, fade?: number}
function sound.muffle(on, options)
  options = options or {}
  fe.audio.set_bus_effect(options.bus or "music", {
    lowpass = on and (options.cutoff or 900) or 0,
    fade = options.fade or 0.25,
  })
end

return sound

--- Named music and sound effects. Optional data/audio.json maps ids to
--- files and settings so scripts say `sound.music("battle")`:
--- {"music": {"battle": {"file": "audio/battle.ogg", "loop_start": 3.2}},
---  "sfx": {"cursor": "audio/cursor.wav", "confirm": {"file": "...", "volume": 0.6}}}
local sound = {}

local catalog = { music = {}, sfx = {} }

function sound.load(path)
  path = path or "data/audio.json"
  if fe.fs.exists(path) then
    local loaded = fe.json.read(path)
    catalog.music = loaded.music or {}
    catalog.sfx = loaded.sfx or {}
  end
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

function sound.stop_music(fade, layer)
  fe.audio.stop_music(layer, fade)
end

--- Play a sound effect by id or path; unknown ids are ignored quietly so
--- placeholder sound names never crash a game.
function sound.play(id, options)
  local e = entry("sfx", id)
  if not e or not fe.fs.exists(e.file) then return nil end
  local settings = { volume = e.volume, pitch = e.pitch }
  for k, v in pairs(options or {}) do settings[k] = v end
  return fe.audio.sound(e.file, settings)
end

return sound

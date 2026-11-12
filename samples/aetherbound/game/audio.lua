--- Music, ambiences and sound effects by id, from data/audio.json:
--- `{id = {file, loop, loop_start, loop_end, volume}}`.
local audio = { table = nil, playing = {} }

local warned = {}

local function entry(id)
  if not audio.table then
    audio.table = fe.fs.exists("data/audio.json") and fe.json.read("data/audio.json") or {}
  end
  local found = audio.table[id]
  if not found and not warned[id] then
    warned[id] = true
    fe.log.warn("no audio '" .. tostring(id) .. "' in data/audio.json")
  end
  return found
end

--- Play a music id on a layer ("main" by default), crossfading over `fade`
--- seconds; the id already playing there keeps playing; nil stops the layer.
function audio.music(id, fade, layer)
  layer = layer or "main"
  if audio.playing[layer] == id then return end
  audio.playing[layer] = id
  local found = id and entry(id)
  if not found then
    fe.audio.stop_music(layer, fade or 1)
    return
  end
  fe.audio.music(found.file, { layer = layer, fade = fade or 1, loop = found.loop ~= false,
    loop_start = found.loop_start, loop_end = found.loop_end, volume = found.volume or 1 })
end

--- A looping ambience under the music (wind, the crystal's hum, the war).
function audio.ambience(id, fade, layer)
  audio.music(id, fade or 2, layer or "ambience")
end

function audio.volume(layer, volume, fade)
  fe.audio.set_music_volume(layer or "main", volume, fade or 1)
end

--- Seconds into the music on a layer (cutscenes time themselves to it).
function audio.position(layer)
  return fe.audio.music_position(layer or "main")
end

function audio.sfx(id, options)
  local found = entry(id)
  if not found then return nil end
  options = options or {}
  local volume = (options.volume or 1) * (found.volume or 1)
  return fe.audio.sound(found.file, { volume = volume, pitch = options.pitch, pan = options.pan, loop = options.loop,
    position = options.position, world = options.world, radius = options.radius })
end

function audio.stop_all(fade)
  for layer in pairs(audio.playing) do
    fe.audio.stop_music(layer, fade or 1)
    audio.playing[layer] = nil
  end
end

return audio

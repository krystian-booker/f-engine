--- Music and sound effects by id, from data/audio.json.
local audio = { table = nil, playing = nil }

local function entries()
  audio.table = audio.table or fe.json.read("data/audio.json")
  return audio.table
end

--- Play a music id (crossfading); the same id keeps playing.
function audio.music(id, fade)
  if id == audio.playing then return end
  audio.playing = id
  if not id then
    fe.audio.stop_music(nil, fade or 0.6)
    return
  end
  local entry = entries().music[id]
  if not entry then
    fe.log.warn("no music '" .. tostring(id) .. "' in data/audio.json")
    return
  end
  fe.audio.music(entry.file, { fade = fade or 0.6, loop = entry.loop ~= false })
end

function audio.sfx(id, options)
  local file = entries().sfx[id]
  if file then return fe.audio.sound(file, options) end
end

return audio

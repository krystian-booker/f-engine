--- Spoken lines: a box at the bottom of the screen with the speaker's name,
--- revealed letter by letter; the machine's voice in its own panel at the
--- top; and "barks", short lines floating over a character's head that do
--- not stop play.
---
--- ```lua
--- dialogue.say("Rowan", "Then we'd better hurry.")          -- waits for confirm
--- dialogue.say("Cael", "ROWAN!!", { shout = true, auto = 1.2 }) -- closes by itself
--- dialogue.system("INTRUDERS DETECTED.")
--- dialogue.bark(entity, "Stay close.", 2.5)
--- ```
--- `say` and `system` block the calling task. `dialogue.update` and
--- `dialogue.draw` run from main.lua, over whatever scene is on screen.
local audio = require("game.audio")
local task = require("game.task")
local ui = require("game.ui")

local dialogue = { line = nil, barks = {}, hidden = false }

local SPEED = 46 -- letters per second
local BOX = { x = 300, y = 752, w = 1320, h = 224 }
local SYSTEM_BOX = { x = 340, y = 118, w = 1240, h = 148 }

--- Per speaker: the colour of the name and the pitch of the text tick.
dialogue.voices = {
  Cael = { color = "cael", pitch = 1.08 },
  Rowan = { color = "rowan", pitch = 0.86 },
  Lyra = { color = "lyra", pitch = 1.26 },
  Darius = { color = "darius", pitch = 0.72 },
  SYSTEM = { color = "system", pitch = 1 },
}

local function open(line)
  local options = { font = line.font, width = line.width }
  line.units = fe.text.units(line.markup)
  line.total = line.units.count
  line.shown, line.pause, line.age, line.done_for = 0, 0, 0, 0
  line.options = options
  dialogue.line = line
  -- Blocks until the line is dismissed (by confirm, by `auto`, or by skip).
  task.wait_until(function() return line.closed end)
  if dialogue.line == line then dialogue.line = nil end
end

--- A character speaks. options: `auto` (seconds after the text is complete
--- until the line closes by itself; confirm still closes it sooner unless
--- `locked`), `shout` (larger, trembling), `italic` (thought, memory),
--- `speed` (a multiple of the normal reveal speed).
function dialogue.say(speaker, text, options)
  options = options or {}
  local markup = text
  if options.shout then markup = "{shake}{size:1.2}" .. text .. "{/size}{/shake}" end
  open { speaker = speaker, markup = markup, font = options.italic and "italic" or "body", width = BOX.w - 120,
    auto = options.auto, locked = options.locked, speed = options.speed or (options.shout and 1.6 or 1),
    tick = "text" }
end

--- The machine speaks: a panel at the top, monospaced, no name.
function dialogue.system(text, options)
  options = options or {}
  open { system = true, speaker = "SYSTEM", markup = text, font = "mono", width = SYSTEM_BOX.w - 120,
    auto = options.auto, locked = options.locked, speed = options.speed or 1.4, tick = "text_system" }
end

--- A short line over a character's head for `seconds`; play goes on.
function dialogue.bark(entity, text, seconds, speaker)
  dialogue.barks[#dialogue.barks + 1] = { entity = entity, text = text, left = seconds or 2.5,
    total = seconds or 2.5, speaker = speaker }
end

function dialogue.active()
  return dialogue.line ~= nil
end

--- Drop everything on screen (a scene change, the title).
function dialogue.clear()
  if dialogue.line then dialogue.line.closed = true end
  dialogue.line, dialogue.barks = nil, {}
end

function dialogue.update(dt)
  for i = #dialogue.barks, 1, -1 do
    local bark = dialogue.barks[i]
    bark.left = bark.left - dt
    if bark.left <= 0 or not bark.entity:valid() then table.remove(dialogue.barks, i) end
  end
  local line = dialogue.line
  if not line then return end
  line.age = line.age + dt
  local skipping = fe.input.held("skip")
  if line.shown < line.total then
    if line.pause > 0 then
      line.pause = line.pause - dt
    else
      local before = math.floor(line.shown)
      line.shown = math.min(line.total, line.shown + dt * SPEED * line.speed * (skipping and 6 or 1))
      local now = math.floor(line.shown)
      for unit = before + 1, now do
        local wait = line.units.wait[unit]
        if wait then
          line.pause = wait
          line.shown = unit
          break
        end
      end
      if now > before and now % 2 == 0 then
        local voice = dialogue.voices[line.speaker]
        audio.sfx(line.tick, { volume = 0.35, pitch = voice and voice.pitch or 1 })
      end
    end
    if fe.input.pressed("confirm") and not line.locked and line.age > 0.08 then
      line.shown, line.pause = line.total, 0
    end
    return
  end
  line.done_for = line.done_for + dt
  local auto = line.auto
  if skipping then auto = math.min(auto or 0.15, 0.15) end
  if (auto and line.done_for >= auto) or (fe.input.pressed("confirm") and not line.locked and line.done_for > 0.05) then
    if not line.auto and not skipping then audio.sfx("confirm", { volume = 0.4 }) end
    line.closed = true
    dialogue.line = nil
  end
end

local function draw_line(line)
  local appear = math.min(1, line.age / 0.12)
  local box = line.system and SYSTEM_BOX or BOX
  local y = box.y + (1 - appear) * 12
  fe.canvas.push({ alpha = appear })
  if line.system then
    ui.panel(box.x, y, box.w, box.h, "system")
    local tw = fe.text.measure(line.markup, line.options)
    local flicker = 0.85 + 0.15 * math.sin(ui.time * 31)
    ui.text(line.markup, box.x + (box.w - math.min(tw, line.width)) / 2, y + 48,
      { font = "mono", width = line.width, visible = math.floor(line.shown),
        color = ui.fade(ui.colors.system, flicker), shadow = 0x3a1a66c0 })
  else
    ui.panel(box.x, y, box.w, box.h, "panel")
    if line.speaker then
      local voice = dialogue.voices[line.speaker]
      local color = voice and ui.colors[voice.color] or ui.colors.gold
      local nw = fe.text.measure(line.speaker, { font = "name" })
      ui.panel(box.x + 36, y - 40, nw + 88, 68, "plate")
      ui.text(line.speaker, box.x + 80, y - 30, { font = "name", color = color })
    end
    ui.text(line.markup, box.x + 60, y + 50,
      { font = line.font, width = line.width, visible = math.floor(line.shown), line_gap = 8 })
    if line.shown >= line.total and not line.auto then
      local bob = math.sin(ui.time * 6) * 4
      fe.canvas.image("ui/advance.png", box.x + box.w - 76, y + box.h - 64 + bob, { linear = true })
    end
  end
  fe.canvas.pop()
end

local function draw_bark(bark)
  local x, y, visible = bark.entity:screen_position(1.9)
  if not visible then return end
  local alpha = math.min(1, bark.left * 3, (bark.total - bark.left) * 6)
  local options = { font = "small" }
  local tw = fe.text.measure(bark.text, options)
  fe.canvas.push({ alpha = alpha })
  ui.panel(x - tw / 2 - 28, y - 58, tw + 56, 60, "plate")
  local voice = bark.speaker and dialogue.voices[bark.speaker]
  ui.text(bark.text, x - tw / 2, y - 46, { font = "small", color = voice and ui.colors[voice.color] or ui.colors.text })
  fe.canvas.pop()
end

function dialogue.draw()
  if dialogue.hidden then return end
  for _, bark in ipairs(dialogue.barks) do draw_bark(bark) end
  if dialogue.line then draw_line(dialogue.line) end
end

return dialogue

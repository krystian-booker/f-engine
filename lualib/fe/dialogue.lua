--- Dialogue: message boxes with a typewriter effect, portraits, name plates,
--- choices, speech bubbles above entities and a backlog.
---
--- ```lua
--- dialogue.say("Elder", "The crystal has dimmed, {player}.{wait:0.4} Help us?")
--- if dialogue.choice({"Of course.", "Not yet."}) == 1 then ... end
--- dialogue.bubble(villager, "Lovely weather!")     -- non-blocking
--- ```
--- `say` and `choice` wait for the player, so call them from a task (event
--- scripts and cutscenes run in tasks). Box layout and behaviour are fields
--- of `dialogue.style`; replace `dialogue.draw_box` to restyle completely.
local config = require("fe.config")
local scene = require("fe.scene")
local signal = require("fe.signal")
local sound = require("fe.sound")
local task = require("fe.task")
local ui = require("fe.ui")

local dialogue = {
  style = {
    lines = 3,
    height = 58,
    margin = 6,
    position = "bottom",       -- "bottom" | "top"
    portrait_size = 48,
    voice_every = 3,           -- characters between voice blips
    auto_delay = 1.5,          -- seconds before auto-advance
  },
  auto = false,
  --- Emitted (speaker, text) when a line is shown; game hooks voice-over here.
  line_shown = signal.new(),
  --- Emitted for every revealed character (speaker, character index).
  character = signal.new(),
  backlog = {},
  backlog_size = 200,
}

local Box = { name = "dialogue", blocks_update = true }
Box.__index = Box

local function box_rect(style)
  local cw, ch = fe.canvas.size()
  local w = cw - style.margin * 2
  local y = style.position == "top" and style.margin or ch - style.height - style.margin
  return style.margin, y, w, style.height
end

local function text_width(line, style)
  local _, _, w = box_rect(style)
  local inset = line.portrait and style.portrait_size + 10 or 0
  return w - ui.theme.padding * 2 - inset - 6
end

function Box:show(line)
  self.line = line
  self.pages = fe.text.paginate(line.text, { font = ui.theme.font, width = text_width(line, dialogue.style) }, dialogue.style.lines)
  self.page = 1
  self:start_page()
end

function Box:start_page()
  self.units = fe.text.units(self.pages[self.page])
  self.visible = 0
  self.timer = 0
  self.speed = self.line.speed or config.get("text_speed") or 45
  self.finished = self.speed <= 0
  if self.finished then self.visible = self.units.count end
  self.idle = 0
end

function Box:update(dt)
  if not self.line then return end
  if not self.finished then
    self.timer = self.timer + dt
    while self.visible < self.units.count do
      local index = self.visible + 1
      local delay = self.units.wait[self.visible] or 1 / self.speed
      if self.timer < delay then break end
      self.timer = self.timer - delay
      self.visible = index
      if self.units.speed[index] then self.speed = self.units.speed[index] end
      dialogue.character:emit(self.line.speaker, index)
      if self.line.voice and index % dialogue.style.voice_every == 1 then
        sound.play(self.line.voice, { pitch = 0.95 + (index % 3) * 0.05 })
      end
    end
    self.finished = self.visible >= self.units.count
  else
    self.idle = self.idle + dt
  end
  local advance = fe.input.pressed("confirm") or fe.input.pressed("cancel")
    or (dialogue.auto and self.finished and self.idle >= dialogue.style.auto_delay)
  if not advance then return end
  if not self.finished then
    self.visible = self.units.count
    self.finished = true
  elseif self.page < #self.pages then
    self.page = self.page + 1
    self:start_page()
  else
    self.done = true
  end
end

--- Draw a message box; replace to restyle.
function dialogue.draw_box(box)
  local style = dialogue.style
  local x, y, w, h = box_rect(style)
  ui.window(x, y, w, h)
  local line = box.line
  local tx = x + ui.theme.padding + 2
  if line.portrait then
    fe.canvas.image(line.portrait, x + 5, y + (h - style.portrait_size) / 2, { w = style.portrait_size, h = style.portrait_size })
    tx = tx + style.portrait_size + 8
  end
  if line.speaker and line.speaker ~= "" then
    local nw = ui.measure(line.speaker) + 12
    local ny = style.position == "top" and y + h - 4 or y - 12
    ui.window(x + 6, ny, nw, 16)
    ui.text(line.speaker, x + 12, ny + 4, { color = ui.theme.heading })
  end
  ui.text(box.pages[box.page], tx, y + ui.theme.padding + 2, { visible = box.visible, width = text_width(line, style) })
  if box.finished then
    local bob = math.floor(task.time() * 3) % 2
    ui.text(box.page < #box.pages and "▼" or "▶", x + w - 14, y + h - 12 + bob, { color = ui.theme.highlight })
  end
end

function Box:draw()
  if self.line then dialogue.draw_box(self) end
end

local box

local function open_box()
  if box and scene.contains(box) then return box end
  box = setmetatable({}, Box)
  scene.push(box)
  return box
end

--- Close the message box (after a sequence of lines).
function dialogue.close()
  if box then
    scene.remove(box)
    box = nil
  end
end

local function remember(speaker, text)
  local log = dialogue.backlog
  log[#log + 1] = { speaker = speaker, text = text }
  while #log > dialogue.backlog_size do table.remove(log, 1) end
end

--- Show a line and wait until the player dismisses it. The box stays open
--- for the next line; it closes when the calling task stops talking
--- (`dialogue.close()` or the end of an event).
---@param speaker string|nil
---@param text string markup
---@param options? {portrait?: string, voice?: string, speed?: number, keep_open?: boolean}
function dialogue.say(speaker, text, options)
  options = options or {}
  local b = open_box()
  b.done = false
  b:show({ speaker = speaker, text = text, portrait = options.portrait, voice = options.voice, speed = options.speed })
  remember(speaker, text)
  dialogue.line_shown:emit(speaker, text)
  task.wait_until(function() return b.done end)
  if options.close then dialogue.close() end
end

--- Ask a question; returns the chosen index. `cancel` picks that index when
--- the player presses Cancel (defaults to the last option).
---@param options string[]
---@param settings? {cancel?: integer|false, title?: string}
function dialogue.choice(options, settings)
  settings = settings or {}
  local cancel = settings.cancel
  if cancel == nil then cancel = #options end
  local _, by, _, bh = box_rect(dialogue.style)
  local cw = fe.canvas.size()
  local width = 40
  for _, option in ipairs(options) do width = math.max(width, ui.measure(option) + 30) end
  local height = #options * ui.theme.row_height + ui.theme.padding * 2
  local y = dialogue.style.position == "top" and by + bh + 4 or by - height - 4
  local index = ui.choose(options, {
    x = cw - width - dialogue.style.margin, y = y, width = width, rows = #options,
    cancel = cancel or nil, title = settings.title,
  })
  return index or cancel
end

-------------------------------------------------------------------------------
-- Speech bubbles drawn above entities (barks, ambient chatter).
local bubbles = {}

--- Show text above an entity for `duration` seconds (non-blocking).
---@param entity Entity
---@param text string
---@param options? {duration?: number, lift?: number}
function dialogue.bubble(entity, text, options)
  options = options or {}
  bubbles[#bubbles + 1] = { entity = entity, text = text, until_time = task.time() + (options.duration or 2.5),
    lift = options.lift or 1.8 }
end

--- Draw active bubbles (fe.field draws them over its world).
function dialogue.draw_bubbles()
  local now = task.time()
  local alive = {}
  for _, b in ipairs(bubbles) do
    if now < b.until_time and b.entity:valid() then
      alive[#alive + 1] = b
      local x, y, visible = b.entity:screen_position(b.lift)
      if visible then
        local w, h = ui.measure(b.text, { width = 120 })
        w = math.min(120, w)
        local bx, by = math.floor(x - w / 2 - 5), math.floor(y - h - 10)
        fe.canvas.nine_slice("@fe/assets/ui/bubble.png", bx, by, w + 10, h + 8, 4)
        fe.canvas.text(b.text, bx + 5, by + 4, { width = w + 1, color = 0x282030ff })
      end
    end
  end
  bubbles = alive
end

--- Clear bubbles (on map change).
function dialogue.clear_bubbles()
  bubbles = {}
end

return dialogue

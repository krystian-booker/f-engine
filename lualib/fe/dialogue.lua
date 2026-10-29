--- Dialogue: message boxes with a typewriter effect, portraits, name plates,
--- choices, speech bubbles above entities and a backlog.
---
--- ```lua
--- dialogue.say("Elder", "The crystal has dimmed, {player}.{wait:0.4} Help us?")
--- if dialogue.choice({"Of course.", "Not yet."}) == 1 then ... end
--- dialogue.bubble(villager, "Lovely weather!")     -- non-blocking
--- ```
--- `say` and `choice` wait for the player, so call them from a task (event
--- scripts and cutscenes run in tasks). The message box belongs to the task
--- that opened it (helpers of `task.all`/`cutscene.parallel` share their
--- caller's box): it stays open between lines and closes with
--- `dialogue.close()` or when that task ends. A line waits while another
--- line is on screen, so concurrent lines (from other tasks or parallel
--- helpers) are shown one after another; a task saying a line while another
--- task's box is open between lines takes the box over.
---
--- Box layout and behaviour are fields of `dialogue.style`; replace
--- `dialogue.draw_box` to restyle, or `dialogue.Box` (a scene class with
--- `show`, `update`, `draw`) to change behaviour. `style.backlog_input`
--- (default "info") opens the backlog screen while a box is shown.
---
--- Voice and pacing:
---   * `voice = path` on a line plays voice-over on the "voice" bus. It
---     stops when the player advances; auto-advance waits until it has
---     finished. `blip = id` plays a typing blip every `style.blip_every`
---     characters instead.
---   * `dialogue.speaking(speaker, talking)` fires when a speaker starts and
---     stops talking (voice-over playing, or text still typing without
---     voice). `mouth = {paths}` on a line animates the portrait with those
---     frames while talking (lip-flap).
---   * Auto-advance (`dialogue.auto` or the `auto_advance` setting) moves on
---     `style.auto_delay` seconds after a line is complete.
---   * Holding `style.fast_forward_input` (default "skip") shows lines at
---     once and advances after `style.fast_forward_delay`, cutting
---     voice-over short.
---   * In a skipping task (a skipped cutscene, see fe.cutscene) `say` only
---     records the line in the backlog; `choice` ends the skip and asks.
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
    blip_every = 3,            -- characters between typing blips
    auto_delay = 1.5,          -- seconds before auto-advance
    backlog_input = "info",    -- action that opens the backlog (false: none)
    fast_forward_input = "skip", -- hold to fast-forward (false: none)
    fast_forward_delay = 0.12, -- seconds a complete line stays while fast-forwarding
    mouth_fps = 8,             -- lip-flap frames per second
  },
  auto = false,
  --- Emitted (speaker, text, line) when a line is shown.
  line_shown = signal.new(),
  --- Emitted (speaker, talking: boolean) when a speaker starts or stops
  --- talking (voice-over playing, or typing without voice-over).
  speaking = signal.new(),
  --- Emitted for every revealed character (speaker, character index).
  character = signal.new(),
  backlog = {},
  backlog_size = 200,
}

local Box = { name = "dialogue", blocks_update = true }
Box.__index = Box
dialogue.Box = Box

local function box_rect(style)
  local cw, ch = fe.canvas.size()
  local w = cw - style.margin * 2
  local h = style.height + math.floor(style.lines * ui.theme.text_size * (ui.text_scale - 1) + 0.5)
  local y = style.position == "top" and style.margin or ch - h - style.margin
  return style.margin, y, w, h
end

local function text_width(line, style)
  local _, _, w = box_rect(style)
  local inset = line.portrait and style.portrait_size + 10 or 0
  return w - ui.theme.padding * 2 - inset - 6
end

--- Whether the player holds fast-forward.
function dialogue.fast_forwarding()
  local action = dialogue.style.fast_forward_input
  return action and fe.input.held(action) or false
end

--- Whether auto-advance is on (`dialogue.auto` or the setting).
function dialogue.auto_advancing()
  return dialogue.auto or config.get("auto_advance") == true
end

function Box:show(line)
  self:stop_voice()
  self.line = line
  self.done = false
  self.pages = fe.text.paginate(line.text, { font = ui.theme.font, size = ui.text_size(),
    width = text_width(line, dialogue.style) }, dialogue.style.lines)
  self.page = 1
  self:start_page()
  if line.voice then self.voice = sound.play(line.voice, { bus = "voice" }) end
  self:update_speaking()
end

--- Whether this line's voice-over is still playing.
function Box:voice_playing()
  if not self.voice then return false end
  return fe.audio.playing(self.voice)
end

function Box:stop_voice()
  if self.voice then
    fe.audio.stop(self.voice)
    self.voice = nil
  end
end

--- Whether the speaker is talking: voice-over playing, or typing without one.
function Box:talking()
  if not self.line or self.done then return false end
  if self.line.voice then return self:voice_playing() end
  return not self.finished
end

function Box:update_speaking()
  local talking = self:talking()
  if talking ~= (self.was_talking or false) then
    self.was_talking = talking
    dialogue.speaking:emit(self.line and self.line.speaker, talking)
  end
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

--- Reveal characters for `dt` seconds. A speed of 0 (from options or a
--- {speed:0} tag) shows the rest instantly.
function Box:reveal(dt)
  self.timer = self.timer + dt
  while self.visible < self.units.count do
    local index = self.visible + 1
    local delay = self.units.wait[self.visible] or (self.speed > 0 and 1 / self.speed or 0)
    if self.timer < delay then break end
    self.timer = self.timer - delay
    self.visible = index
    if self.units.speed[index] then self.speed = self.units.speed[index] end
    dialogue.character:emit(self.line.speaker, index)
    if self.line.blip and index % dialogue.style.blip_every == 1 then
      sound.play(self.line.blip, { pitch = 0.95 + (index % 3) * 0.05 })
    end
  end
  self.finished = self.visible >= self.units.count
end

function Box:update(dt)
  if not self.line then return end
  local backlog = dialogue.style.backlog_input
  if backlog and fe.input.pressed(backlog) then
    task.detached("backlog", require("fe.ui.screens.backlog").open)
    return
  end
  local fast = dialogue.fast_forwarding()
  if fast and not self.finished then
    self.visible = self.units.count
    self.finished = true
  end
  if not self.finished then
    self:reveal(dt)
  elseif fast or self.page < #self.pages or not self:voice_playing() then
    self.idle = self.idle + dt
  end
  local advance = fe.input.pressed("confirm") or fe.input.pressed("cancel")
    or (fast and self.finished and self.idle >= dialogue.style.fast_forward_delay)
    or (dialogue.auto_advancing() and self.finished and self.idle >= dialogue.style.auto_delay)
  if advance then
    if not self.finished then
      self.visible = self.units.count
      self.finished = true
    elseif self.page < #self.pages then
      self.page = self.page + 1
      self:start_page()
    else
      self.done = true
      self:stop_voice()
    end
  end
  self:update_speaking()
end

--- Draw a message box; replace to restyle.
function dialogue.draw_box(box)
  local style = dialogue.style
  local x, y, w, h = box_rect(style)
  ui.window(x, y, w, h)
  local line = box.line
  local tx = x + ui.theme.padding + 2
  if line.portrait then
    local image = line.portrait
    if line.mouth and #line.mouth > 0 and box:talking() then
      image = line.mouth[math.floor(task.time() * style.mouth_fps) % #line.mouth + 1]
    end
    fe.canvas.image(image, x + 5, y + (h - style.portrait_size) / 2, { w = style.portrait_size, h = style.portrait_size })
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

-- The open box (owned by box.owner, a Task) and the speech bubbles, in one
-- table that hot reload keeps.
local live = { box = nil, bubbles = {} }

local function close_box(b)
  if live.box == b then
    b:stop_voice()
    b.done = true
    b:update_speaking()
    scene.remove(b)
    live.box = nil
    b.forget()
  end
end

--- Close the message box. Inside a task this only closes a box the task
--- owns; outside tasks it closes any box.
function dialogue.close()
  local current = task.current()
  if live.box and (not current or live.box.owner == current:owner()) then
    close_box(live.box)
  end
end

--- The open box, or nil.
function dialogue.current()
  return live.box
end

--- End the line on screen as if the player advanced it (skipping a
--- cutscene). Only a box owned by `owner` (a Task), when given.
---@param owner? Task
function dialogue.finish_line(owner)
  if live.box and live.box.line and (not owner or live.box.owner == owner) then
    live.box.done = true
    live.box:stop_voice()
  end
end

-- The box for the calling task, once no other line is showing.
local function acquire()
  local owner = task.current() and task.current():owner()
  if not owner then
    error("dialogue.say waits for the player, so it must run inside a task", 3)
  end
  task.wait_until(function()
    return not live.box or not live.box.speaking or not live.box.owner:running()
  end)
  if live.box and live.box.owner ~= owner then close_box(live.box) end
  if not live.box then
    local b = setmetatable({ owner = owner }, dialogue.Box)
    live.box = b
    scene.push(b)
    b.forget = owner:finally(function() close_box(b) end)
  end
  return live.box
end

local function remember(speaker, text)
  local log = dialogue.backlog
  log[#log + 1] = { speaker = speaker, text = text }
  while #log > dialogue.backlog_size do table.remove(log, 1) end
end

--- Show a line and wait until the player dismisses it. The box stays open
--- for the next line; `close = true` closes it afterwards. `voice` is a
--- voice-over file, `blip` a typing sound, `mouth` lip-flap portrait frames.
---@param speaker string|nil
---@param text string markup
---@param options? {portrait?: string, mouth?: string[], voice?: string, blip?: string, speed?: number, close?: boolean}
function dialogue.say(speaker, text, options)
  options = options or {}
  local line = { speaker = speaker, text = text, portrait = options.portrait, mouth = options.mouth,
    voice = options.voice, blip = options.blip, speed = options.speed }
  if task.skipping() then
    remember(speaker, text)
    dialogue.line_shown:emit(speaker, text, line)
    return
  end
  local b = acquire()
  b.speaking = true
  task.ensure(function()
    b:show(line)
    remember(speaker, text)
    dialogue.line_shown:emit(speaker, text, line)
    task.wait_until(function() return b.done end)
  end, function()
    b.speaking = false
    b:stop_voice()
  end)
  if options.close then close_box(b) end
end

--- Ask a question; returns the chosen index. `cancel` picks that index when
--- the player presses Cancel (defaults to the last option; false disables).
---@param options string[]
---@param settings? {cancel?: integer|false, title?: string}
function dialogue.choice(options, settings)
  settings = settings or {}
  task.end_skip()
  local cancel = settings.cancel
  if cancel == nil then cancel = #options end
  local _, by, _, bh = box_rect(dialogue.style)
  local cw = fe.canvas.size()
  local width = 40
  for _, option in ipairs(options) do width = math.max(width, ui.measure(option) + 30) end
  local height = #options * ui.row_height() + ui.theme.padding * 2
  local y = dialogue.style.position == "top" and by + bh + 4 or by - height - 4
  local index = ui.choose(options, {
    x = cw - width - dialogue.style.margin, y = y, width = width, rows = #options,
    cancel = cancel or nil, title = settings.title,
  })
  return index or cancel
end

-------------------------------------------------------------------------------
-- Speech bubbles drawn above entities (barks, ambient chatter).

--- Show text above an entity for `duration` seconds (non-blocking).
---@param entity Entity
---@param text string
---@param options? {duration?: number, lift?: number}
function dialogue.bubble(entity, text, options)
  options = options or {}
  live.bubbles[#live.bubbles + 1] = { entity = entity, text = text, until_time = task.time() + (options.duration or 2.5),
    lift = options.lift or 1.8 }
end

--- Draw active bubbles (fe.field draws them over its world).
function dialogue.draw_bubbles()
  local now = task.time()
  local alive = {}
  for _, b in ipairs(live.bubbles) do
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
  live.bubbles = alive
end

--- Clear bubbles (on map change).
function dialogue.clear_bubbles()
  live.bubbles = {}
end

return dialogue

--- Message boxes and choices. Both block the task that calls them:
---
--- ```lua
--- dialogue.say("Elder Rowan", "The grotto's crystal has gone dark.")
--- if dialogue.choose("Innkeeper", "A night's rest is 20 gil.", { "Stay", "Leave" }) == 1 then ... end
--- ```
local audio = require("game.audio")
local input = require("game.input")
local scenes = require("game.scenes")
local task = require("game.task")
local ui = require("game.ui")

local dialogue = { speed = 45, lines = 3 }

local Box = {}
Box.__index = Box

local function layout()
  local w, h = ui.size()
  return 24, h - 74, w - 48, 56
end

function Box:update(dt)
  local total = fe.text.units(self.pages[self.page]).count
  self.shown = math.min(total, self.shown + dialogue.speed * dt)
  if self.options and self.page == #self.pages and self.shown >= total then
    ui.list_input(self.cursor, #self.options)
    if fe.input.pressed("confirm") then
      audio.sfx("confirm", { volume = 0.6 })
      self.choice, self.closed = self.cursor.index, true
    elseif fe.input.pressed("cancel") and self.cancel then
      self.choice, self.closed = self.cancel, true
    end
    return
  end
  if fe.input.pressed("confirm") or fe.input.pressed("cancel") then
    if self.shown < total then
      self.shown = total
    elseif self.page < #self.pages then
      self.page, self.shown = self.page + 1, 0
    elseif not self.options then
      self.closed = true
    end
  end
end

function Box:draw()
  local x, y, w, h = layout()
  ui.window(x, y, w, h)
  local text_x = x + 10
  if self.portrait then
    fe.canvas.image(self.portrait, x + 8, y + 8, { w = 40, h = 40 })
    text_x = x + 56
  end
  if self.speaker then
    local sw = fe.text.measure(self.speaker)
    ui.window(x + 8, y - 12, sw + 16, 16)
    ui.text(self.speaker, x + 16, y - 8, { color = ui.colors.accent })
  end
  ui.text(self.pages[self.page], text_x, y + 9, { width = x + w - 10 - text_x, visible = math.floor(self.shown) })
  local total = fe.text.units(self.pages[self.page]).count
  if self.shown >= total then
    if self.options and self.page == #self.pages then
      local widest = 0
      for _, option in ipairs(self.options) do widest = math.max(widest, (fe.text.measure(option))) end
      ui.list(x + w - widest - 34, y - #self.options * 12 - 14, widest + 30, self.options, self.cursor)
    else
      ui.text(input.prompt("confirm"), x + w - 18, y + h - 15)
    end
  end
end

local function open(speaker, text, options, settings)
  local x, _, w = layout()
  local portrait = settings and settings.portrait
  local width = w - 20 - (portrait and 46 or 0)
  local box = setmetatable({ speaker = speaker, portrait = portrait, options = options, cursor = ui.cursor(),
    pages = fe.text.paginate(text, { width = width }, dialogue.lines), page = 1, shown = 0,
    cancel = settings and settings.cancel }, Box)
  if #box.pages == 0 then box.pages = { "" } end
  local _ = x
  scenes.push(box)
  task.wait_until(function() return box.closed end)
  scenes.pop(box)
  task.frame() -- the press that closed the box is not also the next one
  return box.choice
end

--- Show a message and wait for the player to read it.
function dialogue.say(speaker, text, settings)
  open(speaker, text, nil, settings)
end

--- Ask; returns the index of the chosen option (`settings.cancel` is the
--- index cancel picks, if any).
function dialogue.choose(speaker, text, options, settings)
  return open(speaker, text, options, settings)
end

return dialogue

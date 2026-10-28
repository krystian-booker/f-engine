--- UI building blocks drawn with fe.canvas: windows, text, gauges, cursors,
--- lists and menus. Screens in fe.ui.screens are built from these and can
--- be replaced by a game's own modules.
local class = require("fe.class")
local input = require("fe.input")
local scene = require("fe.scene")
local sound = require("fe.sound")
local task = require("fe.task")
local theme = require("fe.ui.theme")

local ui = { theme = theme }

--- Canvas size.
function ui.size()
  return fe.canvas.size()
end

--- Position a w*h box on the canvas: anchor is "top-left", "top", "top-right",
--- "left", "center", "right", "bottom-left", "bottom" or "bottom-right".
function ui.anchor(w, h, anchor, margin)
  local cw, ch = fe.canvas.size()
  margin = margin or 8
  anchor = anchor or "center"
  local x = anchor:find("left") and margin or anchor:find("right") and cw - w - margin or math.floor((cw - w) / 2)
  local y = anchor:find("top") and margin or anchor:find("bottom") and ch - h - margin or math.floor((ch - h) / 2)
  return x, y
end

--- Window frame using the theme skin.
function ui.window(x, y, w, h, options)
  options = options or {}
  local skin = options.skin or theme.skin
  local alpha = options.alpha or 1
  if alpha < 1 then fe.canvas.push({ alpha = alpha }) end
  if skin then
    fe.canvas.nine_slice(skin, x, y, w, h, options.border or theme.skin_border, { color = options.color })
  else
    fe.canvas.rect(x, y, w, h, 0x101a34ee)
  end
  if alpha < 1 then fe.canvas.pop() end
end

--- Text with the theme font and a soft shadow.
function ui.text(text, x, y, options)
  options = options or {}
  return fe.canvas.text(text, x, y, {
    font = options.font or theme.font, size = options.size, color = options.color or theme.text,
    width = options.width, align = options.align, visible = options.visible,
    shadow = options.shadow == false and nil or (options.shadow or theme.shadow), line_gap = options.line_gap,
  })
end

function ui.measure(text, options)
  options = options or {}
  return fe.text.measure(text, { font = options.font or theme.font, size = options.size, width = options.width })
end

--- Horizontal gauge filled to value/max.
function ui.gauge(x, y, w, h, value, max, color, options)
  options = options or {}
  local fraction = max and max > 0 and math.max(0, math.min(1, value / max)) or 0
  fe.canvas.rect(x, y, w, h, options.back or theme.gauge_back)
  if fraction > 0 then
    fe.canvas.rect(x + 1, y + 1, math.max(0, (w - 2) * fraction), math.max(1, h - 2), color or theme.gauges.hp)
  end
end

--- Blinking selection cursor at (x, y).
function ui.cursor(x, y)
  local bob = math.floor(task.time() * 4) % 2
  fe.canvas.image(theme.cursor, x - 9 + bob, y + 1)
end

--- A prompt line such as "Z Confirm  X Back" for the current device.
---@param pairs_list {[1]: string, [2]: string}[] {action, label}
function ui.prompts(pairs_list)
  local parts = {}
  for _, p in ipairs(pairs_list) do
    parts[#parts + 1] = "{color:highlight}" .. input.prompt(p[1]) .. "{color:reset} " .. p[2]
  end
  return table.concat(parts, "   ")
end

function ui.sound(name)
  local id = theme.sounds[name]
  if id then sound.play(id) end
end

fe.text.color("highlight", theme.highlight)
fe.text.color("muted", theme.muted)
fe.text.color("good", theme.good)
fe.text.color("bad", theme.bad)
fe.text.color("heading", theme.heading)

-------------------------------------------------------------------------------
--- A scrolling, selectable list. Items are tables:
--- {label = "Potion", right = "x3", enabled = true, icon = "potion", value = any}
--- or plain strings.
---@class List
local List = class("List")
ui.List = List

function List:init(options)
  options = options or {}
  self.items = {}
  self.index = 1
  self.offset = 0
  self.rows = options.rows or 6
  self.columns = options.columns or 1
  self.row_height = options.row_height or theme.row_height
  self.on_confirm = options.on_confirm
  self.on_cancel = options.on_cancel
  self.on_change = options.on_change
  self.wrap = options.wrap ~= false
  self.active = options.active ~= false
  self.render = options.render
  self:set_items(options.items or {})
end

function List:set_items(items)
  self.items = {}
  for i, item in ipairs(items) do
    self.items[i] = type(item) == "table" and item or { label = tostring(item), value = item }
  end
  self:select(self.index)
end

function List:selected()
  return self.items[self.index], self.index
end

function List:select(index)
  local count = #self.items
  if count == 0 then
    self.index = 1
    self.offset = 0
    return
  end
  self.index = math.max(1, math.min(count, index))
  local row = math.floor((self.index - 1) / self.columns)
  if row < self.offset then self.offset = row end
  if row >= self.offset + self.rows then self.offset = row - self.rows + 1 end
end

function List:move(delta)
  local count = #self.items
  if count == 0 then return end
  local target = self.index + delta
  if self.wrap then
    target = (target - 1) % count + 1
  else
    target = math.max(1, math.min(count, target))
  end
  if target ~= self.index then
    self:select(target)
    ui.sound("move")
    if self.on_change then self.on_change(self:selected()) end
  end
end

--- Handle input; call every tick while focused.
function List:update()
  if not self.active then return end
  local vertical = input.navigate("up", "down")
  if vertical ~= 0 then self:move(vertical * self.columns) end
  if self.columns > 1 then
    local horizontal = input.navigate("left", "right")
    if horizontal ~= 0 then self:move(horizontal) end
  end
  if fe.input.pressed("confirm") then
    local item = self:selected()
    if item and item.enabled ~= false then
      ui.sound("confirm")
      if self.on_confirm then self.on_confirm(item, self.index) end
    elseif item then
      ui.sound("buzzer")
    end
  elseif fe.input.pressed("cancel") and self.on_cancel then
    ui.sound("cancel")
    self.on_cancel()
  end
end

--- Draw inside a rectangle.
function List:draw(x, y, w)
  local column_width = math.floor(w / self.columns)
  local first = self.offset * self.columns + 1
  local last = math.min(#self.items, first + self.rows * self.columns - 1)
  for i = first, last do
    local item = self.items[i]
    local slot = i - first
    local cx = x + (slot % self.columns) * column_width
    local cy = y + math.floor(slot / self.columns) * self.row_height
    if self.render then
      self.render(item, cx, cy, column_width, i == self.index)
    else
      local color = item.enabled == false and theme.disabled or (i == self.index and theme.highlight or theme.text)
      if item.icon then
        fe.canvas.text("{icon:" .. item.icon .. "}", cx, cy)
        ui.text(item.label, cx + 11, cy, { color = color })
      else
        ui.text(item.label, cx, cy, { color = color })
      end
      if item.right then
        ui.text(item.right, cx, cy, { color = color, width = column_width - 4, align = "right" })
      end
    end
    if i == self.index and self.active then
      ui.cursor(cx, cy)
    end
  end
  if self.offset > 0 then
    ui.text("▲", x + w - 8, y - 9, { color = theme.muted })
  end
  if last < #self.items then
    ui.text("▼", x + w - 8, y + self.rows * self.row_height - 3, { color = theme.muted })
  end
end

-------------------------------------------------------------------------------
--- A window with an optional title and a list; usable as a scene.
---@class Menu
local Menu = class("Menu")
ui.Menu = Menu

function Menu:init(options)
  self.name = options.name or "menu"
  self.title = options.title
  self.list = List(options)
  self.width = options.width or 120
  self.anchor = options.anchor or "center"
  self.x, self.y = options.x, options.y
  self.opened = 0
  self.blocks_update = true
end

function Menu:height()
  local rows = math.min(self.list.rows, math.max(1, math.ceil(#self.list.items / self.list.columns)))
  return rows * self.list.row_height + theme.padding * 2 + (self.title and 14 or 0) - 2
end

function Menu:enter() self.opened = 0 end

function Menu:update(dt)
  self.opened = math.min(1, self.opened + dt / theme.open_time)
  self.list:update()
end

function Menu:draw()
  local h = self:height()
  local x, y = self.x, self.y
  if not x then x, y = ui.anchor(self.width, h, self.anchor) end
  local scale = 0.85 + 0.15 * self.opened
  fe.canvas.push({ ox = x + self.width / 2, oy = y + h / 2, scale = scale, alpha = self.opened })
  ui.window(x, y, self.width, h)
  local top = y + theme.padding
  if self.title then
    ui.text(self.title, x + theme.padding, top, { color = theme.heading })
    top = top + 14
  end
  self.list:draw(x + theme.padding + 8, top, self.width - theme.padding * 2 - 8)
  fe.canvas.pop()
end

--- Show a menu and wait for a choice. Returns the item index and item, or
--- nil when cancelled (if `cancel` is allowed). Call from a task.
---@param items table[]|string[]
---@param options? table Menu options plus `cancel = true|integer`
function ui.choose(items, options)
  options = options or {}
  local result, done
  local settings = {}
  for k, v in pairs(options) do settings[k] = v end
  settings.items = items
  settings.on_confirm = function(item, index)
    result, done = { index, item }, true
  end
  if options.cancel then
    settings.on_cancel = function()
      if type(options.cancel) == "number" then
        result = { options.cancel, items[options.cancel] }
      end
      done = true
    end
  end
  local menu = Menu(settings)
  scene.push(menu)
  -- If the waiting task is cancelled, the menu must not stay on screen.
  local current = task.current()
  local forget = current and current:finally(function() scene.remove(menu) end)
  task.wait_until(function() return done end)
  if forget then forget() end
  scene.remove(menu)
  if result then
    return result[1], result[2]
  end
  return nil
end

return ui

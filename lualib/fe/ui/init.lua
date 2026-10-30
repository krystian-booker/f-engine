--- UI building blocks drawn with fe.canvas: windows, text, gauges, cursors,
--- lists and menus. Screens in fe.ui.screens are built from these and can
--- be replaced by a game's own modules.
---
--- Screens follow one pattern: `screen.open(...)` runs inside a task, shows
--- a view with `ui.run_screen` and returns a result, or nil when the player
--- backs out with Cancel. `ui.run_screen` removes the view however the
--- waiting task ends (closed, failed or cancelled).
---
--- Also here:
---   * toasts: `ui.toast(text, {icon, duration, sound})` queues a short
---     notification drawn over everything (at most `ui.toast_settings.visible`
---     at once) without waiting;
---   * button glyphs: `ui.prompt(action)` is markup showing the action's
---     button for the device in use (`fe.input.device_type`): keyboard keys,
---     Xbox, PlayStation or Switch buttons from the placeholder sheets in
---     `@fe/assets/ui/glyphs_*.png` (replace `ui.glyph_sets` to ship your
---     own); bindings without a glyph show their name;
---   * accessibility: the `text_scale` and `high_contrast` settings of
---     fe.config scale every `ui.text` and list row and switch the theme
---     (`theme.use("high_contrast")`).
local class = require("fe.class")
local config = require("fe.config")
local game = require("fe.game")
local i18n = require("fe.i18n")
local input = require("fe.input")
local scene = require("fe.scene")
local sound = require("fe.sound")
local tablex = require("fe.tablex")
local task = require("fe.task")
local theme = require("fe.ui.theme")

local ui = {
  theme = theme,
  --- Text size multiplier (the `text_scale` setting).
  text_scale = 1,
  toast_settings = { duration = 2.5, visible = 3, width = 150, fade = 0.2, anchor = "top-right" },
}

--- The file's settings, for `__reload`.
ui.file_defaults = { toast_settings = tablex.deep_copy(ui.toast_settings) }

--- Adopt edited toast settings on hot reload (values the game changed at
--- runtime are kept).
function ui.__reload(live, fresh)
  live.file_defaults.toast_settings = tablex.adopt(live.toast_settings, fresh.toast_settings,
    live.file_defaults.toast_settings)
end

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
  local skin = options.skin
  if skin == nil then skin = theme.skin end
  local alpha = options.alpha or 1
  if alpha < 1 then fe.canvas.push({ alpha = alpha }) end
  if skin then
    fe.canvas.nine_slice(skin, x, y, w, h, options.border or theme.skin_border, { color = options.color })
  else
    fe.canvas.rect(x, y, w, h, theme.window_border)
    fe.canvas.rect(x + 1, y + 1, w - 2, h - 2, options.color or theme.window_fill)
  end
  if alpha < 1 then fe.canvas.pop() end
end

--- The text size for `size` (default `theme.text_size`) after the
--- player's text scale; nil when unscaled so fonts keep their own size.
---@param size? number
---@return number?
function ui.text_size(size)
  if ui.text_scale == 1 then return size end
  return (size or theme.text_size) * ui.text_scale
end

--- List row height after the player's text scale.
---@param base? number default theme.row_height
---@return integer
function ui.row_height(base)
  return math.floor((base or theme.row_height) * ui.text_scale + 0.5)
end

--- Text with the theme font and a soft shadow.
function ui.text(text, x, y, options)
  options = options or {}
  return fe.canvas.text(text, x, y, {
    font = options.font or theme.font, size = ui.text_size(options.size), color = options.color or theme.text,
    width = options.width, align = options.align, visible = options.visible,
    shadow = options.shadow == false and nil or (options.shadow or theme.shadow), line_gap = options.line_gap,
  })
end

function ui.measure(text, options)
  options = options or {}
  return fe.text.measure(text, { font = options.font or theme.font, size = ui.text_size(options.size),
    width = options.width })
end

--- Horizontal gauge filled to value/max.
function ui.gauge(x, y, w, h, value, max, color, options)
  options = options or {}
  local fraction = max and max > 0 and tablex.clamp(value / max, 0, 1) or 0
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

-------------------------------------------------------------------------------
-- Button glyphs

local pad_buttons = { "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "start",
  "back", "dpup", "dpdown", "dpleft", "dpright", "leftstick", "rightstick" }

--- Glyph sheets: 10x10 cells, 8 per row, in `names` order. Pad names are
--- SDL button names (`pad:a` is the bottom face button); keyboard names
--- are raw inputs without the `key:` prefix, plus `mouse:left/right`.
ui.glyph_sets = {
  keyboard = { image = "@fe/assets/ui/glyphs_keyboard.png", names = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Return", "Space", "Escape", "Tab",
    "Left Shift", "Right Shift", "Left Ctrl", "Right Ctrl", "Backspace", "Up", "Down", "Left", "Right", "PageUp",
    "PageDown", "mouse:left", "mouse:right", "=", "-" } },
  xbox = { image = "@fe/assets/ui/glyphs_xbox.png", names = pad_buttons },
  playstation = { image = "@fe/assets/ui/glyphs_playstation.png", names = pad_buttons },
  switch = { image = "@fe/assets/ui/glyphs_switch.png", names = pad_buttons },
}
--- Glyph set per `fe.input.device_type()` value.
ui.glyph_devices = { keyboard = "keyboard", xbox = "xbox", playstation = "playstation", switch = "switch",
  steamdeck = "xbox", generic = "xbox" }

local glyph_ids = {}

local function glyph_id(set, name)
  return "glyph." .. set .. "." .. name:lower():gsub("[^%w]+", "_")
end

--- Register every glyph of `ui.glyph_sets` as an {icon:...} (done on load;
--- call again after changing the sets).
function ui.register_glyphs()
  glyph_ids = {}
  for set, spec in pairs(ui.glyph_sets) do
    glyph_ids[set] = {}
    for i, name in ipairs(spec.names) do
      local id = glyph_id(set, name)
      fe.text.icon(id, { image = spec.image, rect = { ((i - 1) % 8) * 10, ((i - 1) // 8) * 10, 10, 10 } })
      glyph_ids[set][name] = id
    end
  end
end

--- The glyph set for the device the player is using.
---@return string
function ui.device()
  return ui.glyph_devices[fe.input.device_type()] or "xbox"
end

--- Markup for the button bound to `action` on the current device: a glyph
--- icon, or the input's name when there is no glyph for it.
---@param action string
---@return string
function ui.prompt(action)
  local set = ui.device()
  local ids = glyph_ids[set] or {}
  local pad = set ~= "keyboard"
  local fallback
  for _, raw in ipairs(fe.input.bindings(action)) do
    if not raw:find("[%+%-]$") or raw == "key:-" or raw == "key:=" then
      local kind, name = raw:match("^(%w+):(.+)$")
      local fits = (pad and kind == "pad") or (not pad and (kind == "key" or kind == "mouse"))
      if fits then
        local id = ids[kind == "mouse" and raw or name]
        if id then return "{icon:" .. id .. "}" end
        fallback = fallback or name
      end
    end
  end
  return fallback or input.prompt(action)
end

--- A prompt line such as "[Z] Confirm  [X] Back" for the current device.
---@param pairs_list {[1]: string, [2]: string}[] {action, label}
function ui.prompts(pairs_list)
  local parts = {}
  for _, p in ipairs(pairs_list) do
    parts[#parts + 1] = "{color:highlight}" .. ui.prompt(p[1]) .. "{color:reset} " .. p[2]
  end
  return table.concat(parts, "   ")
end

function ui.sound(name)
  local id = theme.sounds[name]
  if id then sound.play(id) end
end

--- Translate a kit string: `ui.t("fe.menu.items")`. Labels that are not
--- keys come back unchanged, so games can pass plain text.
ui.t = i18n.t

--- Push `view` as a scene and wait (in a task) until `ui.close(view, result)`
--- is called; returns the result. The view is removed however the task
--- ends.
---@param view table a scene
---@return any
function ui.run_screen(view)
  view.closed, view.result = false, nil
  scene.push(view)
  task.ensure(function()
    task.wait_until(function() return view.closed end)
  end, function() scene.remove(view) end)
  return view.result
end

--- Finish a view shown with `ui.run_screen`.
function ui.close(view, result)
  view.result = result
  view.closed = true
end

--- Show a non-modal `view` (a side panel, a preview) while `fn` runs, and
--- remove it afterwards even if `fn` raises or the task is cancelled.
function ui.with_view(view, fn, ...)
  local args = table.pack(...)
  scene.push(view)
  return task.ensure(function() return fn(table.unpack(args, 1, args.n)) end, function() scene.remove(view) end)
end

--- Show a message window and wait for Confirm or Cancel.
---@param text string markup
function ui.message(text)
  local cw = fe.canvas.size()
  local w = math.min(cw - 40, 240)
  local _, h = ui.measure(text, { width = w - theme.padding * 2 })
  h = h + theme.padding * 2 + 4
  local view = { name = "message", blocks_update = true }
  function view:update()
    if fe.input.pressed("confirm") or fe.input.pressed("cancel") then
      ui.sound("confirm")
      ui.close(self)
    end
  end
  function view:draw()
    local x, y = ui.anchor(w, h, "center")
    ui.window(x, y, w, h)
    ui.text(text, x + theme.padding, y + theme.padding + 2, { width = w - theme.padding * 2 })
  end
  ui.run_screen(view)
end

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
  self.row_height = options.row_height
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

--- Row height: the `row_height` option, or the theme's scaled by the
--- player's text scale.
function List:row()
  return self.row_height or ui.row_height()
end

function List:select(index)
  local count = #self.items
  if count == 0 then
    self.index = 1
    self.offset = 0
    return
  end
  self.index = tablex.clamp(index, 1, count)
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
    target = tablex.clamp(target, 1, count)
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
    local cy = y + math.floor(slot / self.columns) * self:row()
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
    ui.text("▼", x + w - 8, y + self.rows * self:row() - 3, { color = theme.muted })
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
  self.blocks_update = options.blocks_update ~= false
end

function Menu:height()
  local rows = math.min(self.list.rows, math.max(1, math.ceil(#self.list.items / self.list.columns)))
  return rows * self.list:row() + theme.padding * 2 + (self.title and ui.row_height(14) or 0) - 2
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
    top = top + ui.row_height(14)
  end
  self.list:draw(x + theme.padding + 8, top, self.width - theme.padding * 2 - 8)
  fe.canvas.pop()
end

--- Show a menu and wait for a choice. Returns the item index and item, or
--- nil when cancelled (if `cancel` is allowed). Call from a task. The menu
--- blocks the scenes under it unless `blocks_update = false`.
---@param items table[]|string[]
---@param options? table Menu options plus `cancel = true|integer`
function ui.choose(items, options)
  options = options or {}
  local settings = {}
  for k, v in pairs(options) do settings[k] = v end
  settings.items = items
  local menu
  settings.on_confirm = function(item, index)
    ui.close(menu, { index, item })
  end
  if options.cancel then
    settings.on_cancel = function()
      local index = type(options.cancel) == "number" and options.cancel or nil
      ui.close(menu, index and { index, items[index] })
    end
  end
  menu = Menu(settings)
  local result = ui.run_screen(menu)
  if result then
    return result[1], result[2]
  end
  return nil
end

-------------------------------------------------------------------------------
-- Toasts

local toasts = {}

--- Queue a non-blocking notification (item pickups, quest updates).
---@param text string markup
---@param options? {icon?: string, duration?: number, sound?: string}
function ui.toast(text, options)
  options = options or {}
  toasts[#toasts + 1] = { text = text, icon = options.icon, duration = options.duration or ui.toast_settings.duration,
    sound = options.sound }
end

--- Queued and showing toasts, oldest first ({text, icon, duration, shown_at}).
function ui.toasts()
  return toasts
end

--- Drop every toast.
function ui.clear_toasts()
  for i = #toasts, 1, -1 do toasts[i] = nil end
end

--- Start queued toasts as slots free up and drop finished ones (fe.game
--- runs this every tick).
function ui.update_toasts()
  local now = task.time()
  for i = #toasts, 1, -1 do
    local toast = toasts[i]
    if toast.shown_at and now >= toast.shown_at + toast.duration then table.remove(toasts, i) end
  end
  for i = 1, math.min(#toasts, ui.toast_settings.visible) do
    local toast = toasts[i]
    if not toast.shown_at then
      toast.shown_at = now
      if toast.sound then sound.play(toast.sound) end
    end
  end
end

--- Draw the showing toasts (fe.game runs this after the scenes).
function ui.draw_toasts()
  local settings = ui.toast_settings
  local now = task.time()
  local y_offset = 0
  for i = 1, math.min(#toasts, settings.visible) do
    local toast = toasts[i]
    if toast.shown_at then
      local text = toast.icon and ("{icon:" .. toast.icon .. "} " .. toast.text) or toast.text
      local w = settings.width
      local _, th = ui.measure(text, { width = w - theme.padding * 2 })
      local h = th + theme.padding * 2
      local age = now - toast.shown_at
      local alpha = math.min(1, age / settings.fade, (toast.duration - age) / settings.fade)
      local x, y = ui.anchor(w, h, settings.anchor)
      y = settings.anchor:find("bottom") and y - y_offset or y + y_offset
      fe.canvas.push({ alpha = math.max(0, alpha), x = (1 - math.min(1, age / settings.fade)) * 12 })
      ui.window(x, y, w, h)
      ui.text(text, x + theme.padding, y + theme.padding, { width = w - theme.padding * 2 })
      fe.canvas.pop()
      y_offset = y_offset + h + 3
    end
  end
end

-------------------------------------------------------------------------------
-- Settings

--- Apply the accessibility settings of fe.config (text scale, high
--- contrast); runs whenever settings change.
function ui.apply_settings()
  ui.text_scale = config.get("text_scale") or 1
  local preset = config.get("high_contrast") and "high_contrast" or "default"
  if theme.current ~= preset then theme.use(preset) end
end

ui.register_glyphs()
game.updated:connect(function() ui.update_toasts() end, 0, "fe.ui.toasts")
game.drawn:connect(function() ui.draw_toasts() end, 0, "fe.ui.toasts")
config.changed:connect(function() ui.apply_settings() end, 0, "fe.ui.settings")

return ui

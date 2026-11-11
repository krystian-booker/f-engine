--- A shop: buy items from a data/shops.json record. Blocks the calling task.
local audio = require("game.audio")
local data = require("game.data")
local scenes = require("game.scenes")
local state = require("game.state")
local task = require("game.task")
local ui = require("game.ui")

local shop = {}

local Shop = {}
Shop.__index = Shop

local function rows(record)
  local out = {}
  for _, id in ipairs(record.items) do
    local item = data.get("items", id)
    out[#out + 1] = { item.name, right = item.price .. " gil", id = id, about = item.about,
      disabled = item.price > state.data.gold }
  end
  return out
end

function Shop:update()
  local list = rows(self.record)
  ui.list_input(self.cursor, #list)
  if fe.input.pressed("cancel") then
    self.closed = true
  elseif fe.input.pressed("confirm") then
    local row = list[self.cursor.index]
    local item = data.get("items", row.id)
    if item.price > state.data.gold then
      audio.sfx("buzzer")
    else
      state.data.gold = state.data.gold - item.price
      state.add_item(item.id, 1)
      audio.sfx("buy")
    end
  end
end

function Shop:draw()
  local w, h = ui.size()
  local list = rows(self.record)
  ui.window(20, 16, 200, 18)
  ui.text(self.record.name, 28, 21, { color = ui.colors.accent })
  ui.list(20, 38, 200, list, self.cursor, 8)
  local row = list[self.cursor.index]
  ui.window(226, 16, w - 246, 60)
  ui.text(row.about or "", 234, 22, { width = w - 262 })
  ui.text("Owned: " .. state.item_count(row.id), 234, 46, { color = ui.colors.dim })
  ui.text("Gil " .. state.data.gold, 234, 58, { color = ui.colors.accent })
  local _ = h
  ui.hints({ { "confirm", "Buy" }, { "cancel", "Leave" } })
end

function shop.open(id)
  local screen = scenes.push(setmetatable({ record = data.get("shops", id), cursor = ui.cursor() }, Shop))
  task.wait_until(function() return screen.closed end)
  scenes.pop(screen)
  task.frame()
end

return shop

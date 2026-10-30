--- Shop screen: buy and sell with quantity selection and an equipment
--- comparison for party members. `screen.open(shop_id)` waits until the
--- player leaves (returns nothing). Needs the RPG kit.
---
--- Prices come from `screen.price(item, kind, shop)` (`kind` is "buy" or
--- "sell"): the item's price times the shop's `buy_rate` or `sell_rate`.
--- Replace it for discounts (a town's reputation, an Inquire that haggles).
--- The RPG kit is required inside the functions that use it, so this file
--- (and `fe.ui`) loads without the kit.
local data = require("fe.data")
local i18n = require("fe.i18n")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local screen = {}

i18n.defaults({ fe = { shop = {
  title = "Shop", buy = "Buy", sell = "Sell", leave = "Leave", nothing = "(Nothing)", owned = "Owned: {count}",
  cannot_equip = "{name} can't equip",
} } })

--- The price of an item in a shop: `kind` "buy" (rounded up) or "sell"
--- (rounded down). Replace for discounts and mark-ups.
---@param item table the item record
---@param kind "buy"|"sell"
---@param shop table the shop record
---@return integer
function screen.price(item, kind, shop)
  if kind == "sell" then return math.floor(item.price * shop.sell_rate) end
  return math.ceil(item.price * shop.buy_rate)
end

local function buy_price(shop, item) return screen.price(item, "buy", shop) end
local function sell_price(shop, item) return screen.price(item, "sell", shop) end

local function quantity(max, unit)
  local inventory = require("fe.rpg.inventory")
  local count = 1
  local view = { name = "quantity", blocks_update = true }
  function view:update()
    local step = (fe.input.repeating("right") and 1) or (fe.input.repeating("left") and -1)
      or (fe.input.repeating("up") and 10) or (fe.input.repeating("down") and -10) or 0
    if step ~= 0 then count = tablex.clamp(count + step, 1, max) ui.sound("move") end
    if fe.input.pressed("confirm") then ui.close(view, count) end
    if fe.input.pressed("cancel") then ui.close(view) end
  end
  function view:draw()
    local x, y = ui.anchor(120, 30, "center")
    ui.window(x, y, 120, 30)
    ui.text("x" .. count .. "   " .. count * unit .. " " .. inventory.settings.currency, x + 10, y + 10)
  end
  return ui.run_screen(view)
end

local function equip_hint(item)
  if item.kind ~= "equipment" then return "" end
  local party = require("fe.rpg.party")
  local lines = {}
  for _, actor in ipairs(party.members()) do
    if actor:can_equip(item.id, item.equip.slot) then
      local now, after = actor:stats(), actor:preview_equip(item.equip.slot, item.id)
      local diff = {}
      for stat, value in pairs(after) do
        if now[stat] and value ~= now[stat] then
          diff[#diff + 1] = string.format("{color:%s}%s%+d{color:reset}", value > now[stat] and "good" or "bad", stat, value - now[stat])
        end
      end
      lines[#lines + 1] = actor:name() .. " " .. (#diff > 0 and table.concat(diff, " ") or "=")
    else
      lines[#lines + 1] = "{color:muted}" .. ui.t("fe.shop.cannot_equip", { name = actor:name() }) .. "{color:reset}"
    end
  end
  return table.concat(lines, "\n")
end

function screen.open(shop_id)
  local inventory = require("fe.rpg.inventory")
  local shop = data.get("shops", shop_id)
  while true do
    local _, mode = ui.choose({ { label = ui.t("fe.shop.buy"), value = "buy" }, { label = ui.t("fe.shop.sell"), value = "sell" },
      { label = ui.t("fe.shop.leave"), value = "leave" } },
      { title = shop.name or ui.t("fe.shop.title"), anchor = "top-left", width = 90, rows = 3, cancel = 3 })
    if not mode or mode.value == "leave" then return end
    while true do
      local entries
      if mode.value == "buy" then
        entries = tablex.map(shop.goods, function(id)
          local item = data.get("items", id)
          return { label = item.name, right = tostring(buy_price(shop, item)), value = item, icon = item.icon,
            enabled = inventory.money() >= buy_price(shop, item) }
        end)
      else
        entries = tablex.map(inventory.list(function(item) return item.kind ~= "key" end), function(entry)
          return { label = entry.item.name .. " x" .. entry.count, right = tostring(sell_price(shop, entry.item)),
            value = entry.item, icon = entry.item.icon }
        end)
      end
      if #entries == 0 then entries = { { label = ui.t("fe.shop.nothing"), enabled = false } } end
      local hovered = entries[1]
      local info = { name = "shop_info", blocks_update = false }
      function info:draw()
        local cw, ch = fe.canvas.size()
        ui.window(cw - 168, 8, 160, 18)
        ui.text(inventory.money() .. " " .. inventory.settings.currency, cw - 160, 13)
        if hovered and hovered.value then
          local text = (hovered.value.description or "") .. "\n" .. ui.t("fe.shop.owned", { count = inventory.count(hovered.value.id) })
            .. "\n" .. equip_hint(hovered.value)
          ui.window(cw - 168, 30, 160, ch - 40)
          ui.text(text, cw - 160, 38, { width = 146 })
        end
      end
      local _, item = ui.with_view(info, ui.choose, entries, { title = ui.t("fe.shop." .. mode.value), anchor = "left",
        width = 200, rows = 10, cancel = true, on_change = function(entry) hovered = entry end })
      if not item then break end
      local it = item.value
      if mode.value == "buy" then
        local unit = buy_price(shop, it)
        local max = math.min(it.stack - inventory.count(it.id), unit > 0 and inventory.money() // unit or it.stack)
        local count = max > 0 and quantity(max, unit)
        if count and inventory.spend(unit * count) then
          inventory.add(it.id, count)
          ui.sound("buy")
        end
      else
        local unit = sell_price(shop, it)
        local count = quantity(inventory.count(it.id), unit)
        if count and inventory.remove(it.id, count) then
          inventory.add_money(unit * count)
          ui.sound("buy")
        end
      end
    end
  end
end

return screen

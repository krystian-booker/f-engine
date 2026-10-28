--- Shop screen: buy and sell with quantity selection and an equipment
--- comparison for party members. `screen.open(shop_id)` waits until closed.
local data = require("fe.data")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local scene = require("fe.scene")
local tablex = require("fe.tablex")
local task = require("fe.task")
local ui = require("fe.ui")

local screen = {}

local function buy_price(shop, item) return math.ceil(item.price * shop.buy_rate) end
local function sell_price(shop, item) return math.floor(item.price * shop.sell_rate) end

local function quantity(max, unit)
  local count = 1
  local done, cancelled = false, false
  local view = { name = "quantity", blocks_update = true }
  function view:update()
    local step = (fe.input.repeating("right") and 1) or (fe.input.repeating("left") and -1)
      or (fe.input.repeating("up") and 10) or (fe.input.repeating("down") and -10) or 0
    if step ~= 0 then count = math.max(1, math.min(max, count + step)) ui.sound("move") end
    if fe.input.pressed("confirm") then done = true end
    if fe.input.pressed("cancel") then done, cancelled = true, true end
  end
  function view:draw()
    local x, y = ui.anchor(120, 30, "center")
    ui.window(x, y, 120, 30)
    ui.text("x" .. count .. "   " .. count * unit .. " " .. inventory.settings.currency, x + 10, y + 10)
  end
  scene.push(view)
  task.wait_until(function() return done end)
  scene.remove(view)
  return not cancelled and count or nil
end

local function equip_hint(item)
  if item.kind ~= "equipment" then return "" end
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
      lines[#lines + 1] = "{color:muted}" .. actor:name() .. " can't equip{color:reset}"
    end
  end
  return table.concat(lines, "\n")
end

function screen.open(shop_id)
  local shop = data.get("shops", shop_id)
  while true do
    local _, mode = ui.choose({ { label = "Buy", value = "buy" }, { label = "Sell", value = "sell" }, { label = "Leave", value = "leave" } },
      { title = shop.name or "Shop", anchor = "top-left", width = 90, rows = 3, cancel = 3 })
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
      if #entries == 0 then entries = { { label = "(Nothing)", enabled = false } } end
      local hovered = entries[1]
      local info = { name = "shop_info", blocks_update = false }
      function info:draw()
        local cw, ch = fe.canvas.size()
        ui.window(cw - 168, 8, 160, 18)
        ui.text(inventory.money() .. " " .. inventory.settings.currency, cw - 160, 13)
        if hovered and hovered.value then
          local text = (hovered.value.description or "") .. "\nOwned: " .. inventory.count(hovered.value.id)
            .. "\n" .. equip_hint(hovered.value)
          ui.window(cw - 168, 30, 160, ch - 40)
          ui.text(text, cw - 160, 38, { width = 146 })
        end
      end
      scene.push(info)
      local _, item = ui.choose(entries, { title = mode.value == "buy" and "Buy" or "Sell", anchor = "left", width = 200,
        rows = 10, cancel = true, on_change = function(entry) hovered = entry end })
      scene.remove(info)
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

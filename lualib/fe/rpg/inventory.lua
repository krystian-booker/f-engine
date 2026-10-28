--- Items and money, stored in the "inventory" state section.
local data = require("fe.data")
local signal = require("fe.signal")
local state = require("fe.state")

local inventory = {
  settings = { currency = "Gold", max_money = 9999999 },
  changed = signal.new(), -- (item_id, new_count)
}

local record = state.section("inventory", function() return { items = {}, money = 0 } end)

function inventory.count(id)
  return record.items[id] or 0
end

function inventory.has(id, count)
  return inventory.count(id) >= (count or 1)
end

--- Add items (up to the item's stack size); returns how many fit.
function inventory.add(id, count)
  count = count or 1
  local item = data.get("items", id)
  local current = inventory.count(id)
  local added = math.max(0, math.min(count, item.stack - current))
  if added > 0 then
    record.items[id] = current + added
    inventory.changed:emit(id, record.items[id])
  end
  return added
end

--- Remove items; returns false (and removes nothing) if there are too few.
function inventory.remove(id, count)
  count = count or 1
  local current = inventory.count(id)
  if current < count then return false end
  record.items[id] = current - count > 0 and current - count or nil
  inventory.changed:emit(id, current - count)
  return true
end

--- Items held, in data order, optionally filtered by kind or predicate.
---@param filter? string|fun(item: table): boolean
---@return {item: table, count: integer}[]
function inventory.list(filter)
  local out = {}
  for _, item in ipairs(data.list("items")) do
    local count = record.items[item.id]
    if count and count > 0 then
      local keep = filter == nil or (type(filter) == "string" and item.kind == filter)
        or (type(filter) == "function" and filter(item))
      if keep then out[#out + 1] = { item = item, count = count } end
    end
  end
  return out
end

function inventory.money()
  return record.money
end

function inventory.add_money(amount)
  record.money = math.max(0, math.min(inventory.settings.max_money, record.money + amount))
  inventory.changed:emit(nil, record.money)
  return record.money
end

--- Spend money if there is enough.
function inventory.spend(amount)
  if record.money < amount then return false end
  record.money = record.money - amount
  inventory.changed:emit(nil, record.money)
  return true
end

function inventory.reset()
  record.items, record.money = {}, 0
end

return inventory

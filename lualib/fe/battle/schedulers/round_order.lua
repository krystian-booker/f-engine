--- Round-based turns: every battler chooses an action, then actions resolve
--- in speed order. Supports a turn-order preview of this and the next round
--- (as shown by games with a visible turn bar) and per-action priority.
---
--- ```lua
--- local scheduler = require("fe.battle.schedulers.round_order").new{
---   order_key = function(battle, action) return action.user:stat("speed") end,
--- }
--- ```
local tablex = require("fe.tablex")

local round_order = {}

local function speed_of(battle, battler, settings)
  local base = battler:stat("speed")
  local jitter = settings.jitter and (1 + (battle.random:float() * 2 - 1) * settings.jitter) or 1
  return base * jitter
end

--- Turn order for a set of battlers (fastest first) without randomness.
function round_order.order(battlers)
  local list = tablex.filter(battlers, function(b) return b:alive() end)
  tablex.sort_by(list, function(b) return b:stat("speed") end, true)
  return list
end

---@param settings? {order_key?: fun(battle: table, action: table): number, jitter?: number}
function round_order.new(settings)
  settings = settings or {}
  local scheduler = {}

  --- Predicted order for this round and the next (UI turn bar).
  function scheduler.preview(battle)
    local current = battle.data.round_order or round_order.order(battle.battlers)
    return current, round_order.order(battle.battlers)
  end

  function scheduler.run(battle)
    while not battle:outcome() do
      battle.round = battle.round + 1
      if battle.rules.on_round then battle.rules.on_round(battle, battle.round) end
      battle:present("round", battle.round)
      local actions = {}
      for _, b in ipairs(round_order.order(battle.battlers)) do
        if b:can_act() and not battle:outcome() then
          local action = battle:decide(b)
          if action then actions[#actions + 1] = action end
        end
      end
      for _, action in ipairs(actions) do
        local skill = action.skill and require("fe.data").find("skills", action.skill)
        action.order = (skill and skill.priority or 0) * 10000
          + (settings.order_key and settings.order_key(battle, action) or speed_of(battle, action.user, settings))
      end
      tablex.sort_by(actions, function(a) return a.order end, true)
      battle.data.round_order = tablex.map(actions, function(a) return a.user end)
      battle:present("order", battle.data.round_order)
      for _, action in ipairs(actions) do
        if battle:outcome() then break end
        if action.user:alive() then
          battle:turn(action.user, action)
        end
      end
    end
  end

  return scheduler
end

return round_order

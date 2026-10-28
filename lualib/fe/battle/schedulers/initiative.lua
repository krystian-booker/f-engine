--- Initiative (charge time): every battler gains CT by speed; whoever
--- reaches the threshold first acts, then pays a CT cost (heavier actions
--- can cost more). Deterministic and previewable, which suits tactics games.
local tablex = require("fe.tablex")

local initiative = {}

---@param settings? {threshold?: number, cost?: fun(action: table): number, rate?: fun(b: table): number}
function initiative.new(settings)
  settings = settings or {}
  local threshold = settings.threshold or 100
  local scheduler = { settings = settings }

  local function rate(b)
    return settings.rate and settings.rate(b) or math.max(1, b:stat("speed"))
  end

  local function next_actor(battlers, ct)
    local best, best_time
    for _, b in ipairs(battlers) do
      if b:alive() then
        local time = math.max(0, (threshold - ct[b]) / rate(b))
        if not best or time < best_time - 1e-9 or (math.abs(time - best_time) < 1e-9 and rate(b) > rate(best)) then
          best, best_time = b, time
        end
      end
    end
    return best, best_time
  end

  local function advance(battlers, ct, time)
    for _, b in ipairs(battlers) do
      if b:alive() then ct[b] = ct[b] + rate(b) * time end
    end
  end

  --- The next `count` turns (simulated; does not change the battle).
  function scheduler.preview(battle, count)
    local ct = {}
    for _, b in ipairs(battle.battlers) do ct[b] = b.data.ct or 0 end
    local order = {}
    for _ = 1, count or 8 do
      local b, time = next_actor(battle.battlers, ct)
      if not b then break end
      advance(battle.battlers, ct, time)
      order[#order + 1] = b
      ct[b] = ct[b] - threshold
    end
    return order
  end

  --- Advance to and return the next battler to act.
  function scheduler.next(battle)
    local ct = {}
    for _, b in ipairs(battle.battlers) do ct[b] = b.data.ct or 0 end
    local b, time = next_actor(battle.battlers, ct)
    if not b then return nil end
    advance(battle.battlers, ct, time)
    for _, other in ipairs(battle.battlers) do other.data.ct = ct[other] end
    return b
  end

  function scheduler.spend(b, action)
    local cost = settings.cost and action and settings.cost(action) or threshold
    b.data.ct = (b.data.ct or 0) - cost
  end

  --- Turn function hook: tactics kits replace it to run move/act turns.
  scheduler.take_turn = function(battle, b)
    local action = nil
    battle:turn_start(b)
    if b:can_act() then
      action = battle:decide(b)
      if action then battle:perform(action) end
    end
    battle:turn_end(b)
    return action
  end

  function scheduler.run(battle)
    for _, b in ipairs(battle.battlers) do
      b.data.ct = b.data.ct or battle.random:float() * threshold * 0.25
    end
    while not battle:outcome() do
      local b = scheduler.next(battle)
      if not b then break end
      battle:present("turn", { battler = b, order = scheduler.preview(battle, 8) })
      local action = scheduler.take_turn(battle, b)
      scheduler.spend(b, action)
    end
  end

  return scheduler
end

return initiative

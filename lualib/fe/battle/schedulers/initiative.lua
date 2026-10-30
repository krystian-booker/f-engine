--- Initiative (charge time): every battler gains CT by speed; whoever
--- reaches the threshold first acts, then pays a CT cost (heavier actions
--- can cost more). Deterministic and previewable, which suits tactics games.
---
--- A battler whose turn comes while it cannot act still takes an
--- upkeep-only turn; obstacles take none. A *round* ends once every living
--- battler that was in the battle when it began has taken a turn since;
--- battlers added during a round (reinforcements, summons) act as their CT
--- allows but count from the next round, so they never stretch the round
--- they arrive in. `on_round` hooks and the "round" event mark each new
--- round (round-based timers and objectives count these). A battler
--- swapped in (Battle:swap) keeps the leaver's CT and its place in the
--- round. Battlers added mid-battle start with 0 CT; the battlers present
--- at the start get a random CT up to `start_fill` of the threshold
--- (Battle.settings.initiative.start_fill by default). A turn a rule
--- layer skips (Battle:skips_turn) is upkeep only (Battle:turn).
local Battle = require("fe.battle.core")

local initiative = {}

---@param settings? {threshold?: number, cost?: fun(result: any): number, rate?: fun(b: table): number, start_fill?: number}
function initiative.new(settings)
  settings = settings or {}
  local threshold = settings.threshold or 100
  local scheduler = { settings = settings, threshold = threshold }

  local function rate(b)
    return settings.rate and settings.rate(b) or math.max(1, b:stat("speed"))
  end

  local function next_actor(battlers, ct)
    local best, best_time
    for _, b in ipairs(battlers) do
      if b:takes_turns() then
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
      if b:takes_turns() then ct[b] = ct[b] + rate(b) * time end
    end
  end

  --- CT a turn costs: settings.cost(result) or the threshold.
  function scheduler.cost(result)
    return settings.cost and result and settings.cost(result) or threshold
  end

  --- The next `count` turns (simulated; does not change the battle). With
  --- `pending = {battler, cost}`, the battler first pays `cost` CT: the
  --- order that would follow if the acting battler ended its turn now with
  --- that cost (CT cost previews for move/act/wait choices).
  ---@param pending? {battler: table, cost: number}
  function scheduler.preview(battle, count, pending)
    local ct = {}
    for _, b in ipairs(battle.battlers) do ct[b] = b.data.ct or 0 end
    if pending and ct[pending.battler] then ct[pending.battler] = ct[pending.battler] - pending.cost end
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

  function scheduler.replace(_, old, new)
    new.data.ct = old.data.ct
    local round = scheduler.round
    if round then
      round.members[new], round.acted[new] = round.members[old], round.acted[old]
    end
  end

  function scheduler.spend(b, result)
    b.data.ct = (b.data.ct or 0) - scheduler.cost(result)
  end

  --- Turn function hook: tactics kits replace it to run move/act turns. Its
  --- return value is passed to settings.cost.
  scheduler.take_turn = function(battle, b)
    local _, action = battle:turn(b)
    return action
  end

  -- A new round counts the battlers present now.
  local function begin_round(battle)
    local members = {}
    for _, b in ipairs(battle.battlers) do members[b] = true end
    scheduler.round = { members = members, acted = {} }
    battle.round = battle.round + 1
    battle:notify("on_round", battle.round)
    battle:present("round", battle.round)
  end

  local function round_done(battle)
    local round = scheduler.round
    for _, b in ipairs(battle.battlers) do
      if round.members[b] and b:takes_turns() and not round.acted[b] then return false end
    end
    return true
  end

  function scheduler.run(battle)
    local start_fill = settings.start_fill or Battle.settings.initiative.start_fill
    for _, b in ipairs(battle.battlers) do
      b.data.ct = b.data.ct or battle.random:float() * threshold * start_fill
    end
    begin_round(battle)
    while not battle:outcome() do
      local b = scheduler.next(battle)
      if not b then break end
      battle:present("turn", { battler = b, order = scheduler.preview(battle, 8) })
      local result = scheduler.take_turn(battle, b)
      scheduler.spend(b, result)
      scheduler.round.acted[b] = true
      if not battle:outcome() and round_done(battle) then begin_round(battle) end
    end
    scheduler.round = nil
  end

  return scheduler
end

return initiative

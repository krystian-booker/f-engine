--- Round-based turns: battlers choose actions, then turns resolve in order
--- (skill `priority` first, then `order_key`, by default speed). Every
--- living battler takes a turn each round, including ones that cannot act
--- (asleep, stunned) or whose turn a rule layer skips (Battle:skips_turn):
--- they get upkeep only, so their states still count down and turn hooks
--- still run.
---
--- Rule layers can reorder a round with `order(battle, entries, preview)`
--- (entries are {battler, action, order}, sorted first-to-last; reorder in
--- place). It runs when the round starts, again on the turns still to come
--- before each turn (so a battler disabled mid-round can move back at once),
--- and for previews (`preview = true`), so it must only reorder.
---
--- `jitter` varies the speed order randomly. Each round's variation is
--- rolled when the round before starts, so the next-round preview is the
--- order that will happen unless speeds, states or the line-up change.
---
--- Octopath Traveler's flow is `choose = "turn"`: the round order is
--- known in advance (shown with the next round by the side-view HUD) and
--- each battler chooses its command when its turn comes.
---
--- ```lua
--- local scheduler = require("fe.battle.schedulers.round_order").new{
---   order_key = function(battle, action) return action.user:stat("speed") end,
---   choose = "turn",  -- choose each action when its turn comes ("round": all at round start)
---   jitter = 0.1,
--- }
--- ```
local Battle = require("fe.battle.core")
local data = require("fe.data")
local tablex = require("fe.tablex")

local round_order = {}

--- Turn order for a set of battlers (fastest first) without randomness.
function round_order.order(battlers)
  local list = tablex.filter(battlers, function(b) return b:takes_turns() end)
  tablex.sort_by(list, function(b) return b:stat("speed") end, true)
  return list
end

---@param settings? {order_key?: fun(battle: table, action: table): number, jitter?: number, choose?: "round"|"turn"}
function round_order.new(settings)
  settings = settings or {}
  local scheduler = { settings = settings }

  -- One random factor per battler for a round (1 without jitter).
  local function roll(battle)
    local factors = {}
    if settings.jitter then
      for _, b in ipairs(round_order.order(battle.battlers)) do
        factors[b] = 1 + (battle.random:float() * 2 - 1) * settings.jitter
      end
    end
    return factors
  end

  -- Sort key: priority (times Battle.settings.priority_scale, so it always
  -- outranks speed), then order_key/speed times the round's factor.
  local function order_value(battle, entry, factors)
    local action = entry.action or { user = entry.battler }
    local skill = action.skill and data.find("skills", action.skill)
    local value = settings.order_key and settings.order_key(battle, action) or entry.battler:stat("speed")
    return (skill and skill.priority or 0) * Battle.settings.priority_scale + value * (factors[entry.battler] or 1)
  end

  local function sorted(battle, entries, factors, preview)
    for _, e in ipairs(entries) do e.order = order_value(battle, e, factors) end
    tablex.sort_by(entries, function(e) return e.order end, true)
    battle:notify("order", entries, preview == true)
    return entries
  end

  -- Rule layers reorder the turns from `first` on (in place).
  local function reorder(battle, entries, first, preview)
    local rest = { table.unpack(entries, first) }
    battle:notify("order", rest, preview)
    for k, e in ipairs(rest) do entries[first + k - 1] = e end
  end

  local function rolls(battle)
    battle.data.rolls = battle.data.rolls or {}
    return battle.data.rolls
  end

  -- The predicted order of a round from order_key/speed, the round's
  -- rolled variation and the order hooks (actions not chosen yet).
  local function predict(battle, factors)
    local predicted = tablex.map(round_order.order(battle.battlers), function(b) return { battler = b } end)
    return tablex.map(sorted(battle, predicted, factors or {}, true), function(e) return e.battler end)
  end

  --- This round's turns still to come (the one in progress first) and the
  --- predicted order of the next round (from order_key/speed, the next
  --- round's rolled variation and the order hooks; next round's actions
  --- are not chosen yet). While a `choose = "round"` round is being chosen
  --- the first list is this round's predicted order (before the battle
  --- starts, both lists are the order without random variation).
  function scheduler.preview(battle)
    local factors = rolls(battle)
    local entries = battle.data.round_entries
    if not entries then
      return predict(battle, factors.current), predict(battle, factors.next)
    end
    local current = {}
    local index = math.max(1, battle.data.round_index or 1)
    local upcoming = {}
    for i = index + 1, #entries do upcoming[#upcoming + 1] = entries[i] end
    battle:notify("order", upcoming, true)
    for _, e in ipairs({ entries[index], table.unpack(upcoming) }) do
      if e.battler:alive() and e.battler.battle == battle then current[#current + 1] = e.battler end
    end
    return current, predict(battle, factors.next)
  end

  --- A battler swapped in (Battle:swap) takes over the leaver's turns and
  --- chooses its own command when they come.
  function scheduler.replace(battle, old, new)
    for _, e in ipairs(battle.data.round_entries or {}) do
      if e.battler == old then e.battler, e.action = new, nil end
    end
    local factors = rolls(battle)
    for _, key in ipairs({ "current", "next" }) do
      if factors[key] then factors[key][new] = factors[key][old] end
    end
  end

  function scheduler.run(battle)
    local factors = rolls(battle)
    while not battle:outcome() do
      battle.round = battle.round + 1
      factors.current = factors.next or roll(battle)
      factors.next = roll(battle)
      battle:notify("on_round", battle.round)
      battle:present("round", battle.round)
      local entries = {}
      for _, b in ipairs(round_order.order(battle.battlers)) do
        if battle:outcome() then break end
        local action = false
        if settings.choose == "turn" then
          action = nil
        elseif b:can_act() and not battle:skips_turn(b) then
          action = battle:decide(b) or false
        end
        -- A battler swapped out while choosing hands its turn to the newcomer.
        entries[#entries + 1] = { battler = action and action.user or b, action = action }
      end
      sorted(battle, entries, factors.current)
      battle.data.round_entries = entries
      battle.data.round_index = 1
      battle.data.round_order = tablex.map(entries, function(e) return e.battler end)
      battle:present("order", battle.data.round_order)
      local i = 1
      while i <= #entries and not battle:outcome() do
        if i > 1 then reorder(battle, entries, i, false) end
        battle.data.round_index = i
        local entry = entries[i]
        local b = entry.battler
        if b:alive() and b.battle == battle then
          battle:turn(b, entry.action)
        end
        i = i + 1
      end
      battle.data.round_entries = nil
    end
  end

  return scheduler
end

return round_order

-- fe.battle.intents: enemies plan their next action as soon as the previous
-- one resolves, show it, and carry it out; charges span extra turns; a
-- fallen single target means a visible re-plan.
local Battle = require("fe.battle.core")
local atb = require("fe.battle.schedulers.atb")
local data = require("fe.data")
local intents = require("fe.battle.intents")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local plans = require("tests.support.intent_plans")
local round_order = require("fe.battle.schedulers.round_order")
local state = require("fe.state")
local task = require("fe.task")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  Battle.settings.inventory, Battle.settings.party = inventory, party
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
  party.join("sage")
  plans.calls, plans.choose = 0, nil
end

-- Hero and sage (no clock of their own unless `party_rate`) against a seer
-- filling 2 gauges a second. Records "intent", "action" and turn events.
local function seer_battle(options)
  options = options or {}
  local events = {}
  local rules = options.rules or {}
  rules.outcome = rules.outcome or function(battle) if battle.data.done then return "draw" end end
  local b = Battle({ scheduler = options.scheduler or atb.new({ mode = "active",
      rate = function(x) return x:is_party() and (options.party_rate or 0) or 2 end,
      initial = function() return 0 end }),
    random = fe.random.new(8), layers = { intents.rules() }, rules = rules })
  local hero = b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
  local sage = b:add({ actor = party.actor("sage"), side = "party", position = { slot = 2 } })
  local seer = b:add({ enemy = "seer", side = "enemy", position = { slot = 1 } })
  b.events:connect(function(name, payload)
    if name == "intent" or name == "action" then
      events[#events + 1] = { name = name, payload = payload, time = task.time(),
        charge_left = name == "intent" and payload.intent and payload.intent.charge_left or nil }
    end
  end)
  b:add_rules({ on_turn_start = function(_, who)
    if who == seer then events[#events + 1] = { name = "turn", time = task.time() } end
  end })
  return b, events, hero, sage, seer
end

local function of(events, name, user)
  local out = {}
  for _, e in ipairs(events) do
    if e.name == name and (not user or e.payload.user == user or e.payload.battler == user) then out[#out + 1] = e end
  end
  return out
end

local function run(t, b, until_fn, ticks)
  local job = task.spawn(function() b:run() end)
  t.wait_until(until_fn, ticks or 60 * 20)
  b.data.done = true
  t.wait_until(function() return job.status ~= "running" end, 600)
  t.eq(job.status, "done", tostring(job.error))
end

return {
  ["intents: planned at the start and right after each action, then carried out as planned"] = function(t)
    setup()
    local b, events, hero, sage, seer = seer_battle()
    local n = 0
    plans.choose = function(_, me, previous)
      n = n + 1
      return { skill = "attack", targets = { n % 2 == 1 and hero or sage }, label = "Strike " .. n, marker = "line",
        data = { n = n, had_previous = previous ~= nil }, me = me }
    end
    run(t, b, function() return #of(events, "action", seer) >= 3 end)
    -- intent, action, intent, action, ... with the start's intent first.
    local seq = {}
    for _, e in ipairs(events) do if e.name ~= "turn" then seq[#seq + 1] = e.name end end
    t.eq({ table.unpack(seq, 1, 6) }, { "intent", "action", "intent", "action", "intent", "action" })
    local shown = of(events, "intent")
    t.eq(shown[1].payload.reason, "planned")
    t.eq(shown[1].payload.previous, nil)
    t.eq(shown[1].time, of(events, "intent")[1].time)
    for i, e in ipairs(of(events, "action", seer)) do
      local intent = shown[i].payload.intent
      t.eq(e.payload.targets, intent.targets, "action " .. i .. " uses the telegraphed target")
      t.eq(e.payload.intent, intent)
      t.ok(shown[i + 1].time >= e.time, "the next intent follows the action")
      t.eq(shown[i + 1].payload.reason, "planned")
    end
    t.eq(shown[2].payload.intent.data.had_previous, false, "the used-up intent is not passed on")
    t.eq(seer.data.intent.label, "Strike " .. #shown, "the newest plan is stored")
    t.eq(#of(events, "intent"), plans.calls)
  end,

  ["intents: charge = n makes the next n turns upkeep-only charging turns"] = function(t)
    setup()
    local b, events, hero, _, seer = seer_battle()
    plans.choose = function() return { skill = "attack", targets = { hero }, charge = 2 } end
    run(t, b, function() return #of(events, "action", seer) >= 1 end)
    local turns_before, charging = 0, {}
    for _, e in ipairs(events) do
      if e.name == "turn" then turns_before = turns_before + 1 end
      if e.name == "intent" and e.payload.reason == "charging" then charging[#charging + 1] = e.charge_left end
      if e.name == "action" then break end
    end
    t.eq(turns_before, 3, "two charging turns, then the action on the third")
    t.eq(charging, { 1, 0 }, "each charging turn is presented with what is left")
    local planned = of(events, "intent")[1].time
    local acted = of(events, "action", seer)[1].time
    t.near(acted - planned, 1.5, 4 / 60, "three clocks of half a second (a frame each to cross full)")
    t.eq(intents.turns_left(seer), 3, "the next charge starts over")
  end,

  ["intents: a fallen single target is re-planned visibly, never silently retargeted"] = function(t)
    setup()
    local felled = false
    local b, events, hero, sage, seer = seer_battle({ rules = {
      on_turn_end = function(battle, who)
        if who.def and who.def.id == "seer" and not felled then
          felled = true
          battle.battlers[1]:set_resource("hp", 0)
          battle:settle()
        end
      end,
    } })
    plans.choose = function(battle, me)
      return { skill = "attack", targets = { battle:targets_for(me, "enemy")[1] }, charge = 1 }
    end
    run(t, b, function() return #of(events, "action", seer) >= 1 end)
    local shown = of(events, "intent")
    t.eq(shown[1].payload.intent.targets, { hero })
    local replanned = nil
    for _, e in ipairs(shown) do if e.payload.reason == "replanned" then replanned = e.payload end end
    t.ok(replanned, "a new intent is presented")
    t.eq(replanned.previous.targets, { hero })
    t.eq(replanned.intent.targets, { sage })
    t.eq(of(events, "action", seer)[1].payload.targets, { sage })
  end,

  ["intents: an action already queued follows the new plan"] = function(t)
    setup()
    local release = false
    local b, events, hero, sage, seer = seer_battle({
      scheduler = atb.new({ mode = "wait", hold_queue = true,
        rate = function(x) return x:is_party() and 0 or 2 end,
        initial = function(x) return (x.def or x.actor.id == "sage") and 1 or 0 end }),
      rules = { player_controller = function(_, me)
        task.wait_until(function() return release end)
        return { kind = "defend", user = me }
      end },
    })
    hero.auto = true
    plans.choose = function(battle, me)
      return { skill = "attack", targets = { battle:targets_for(me, "enemy")[1] } }
    end
    local job = task.spawn(function() b:run() end)
    t.tick(2)
    t.ok(seer.data.queued, "the seer's attack on the hero waits behind the sage's menu")
    t.eq(seer.data.intent.targets, { hero })
    hero:set_resource("hp", 0)
    b:settle()
    t.eq(seer.data.intent.targets, { sage }, "re-planned at once")
    release = true
    t.wait_until(function() return #of(events, "action", seer) >= 1 end, 300)
    t.eq(of(events, "action", seer)[1].payload.targets, { sage })
    b.data.done = true
    t.wait_until(function() return job.status ~= "running" end, 600)
    t.eq(job.status, "done", tostring(job.error))
  end,

  ["intents: replan, cancel, presenter events and a fallen planner"] = function(t)
    setup()
    local seen = {}
    local b, events, hero, sage, seer = seer_battle({ scheduler = round_order.new() })
    b.presenter = { intent = function(_, payload, battle)
      seen[#seen + 1] = { reason = payload.reason, battle = battle }
    end }
    b.started = true
    b:notify("on_start")
    t.eq(seen[1].reason, "planned")
    t.eq(seen[1].battle, b, "presenter:intent(payload, battle)")
    t.eq(seer.data.intent.targets, { hero })
    t.eq(intents.turns_left(seer), 1)
    plans.choose = function() return { skill = "attack", targets = { sage } } end
    local fresh = intents.replan(b, seer)
    t.eq(fresh.targets, { sage })
    t.eq(seen[2].reason, "replanned")
    intents.cancel(b, seer)
    t.eq(seer.data.intent, nil)
    t.eq(seen[3].reason, "cancelled")
    intents.cancel(b, seer) -- nothing to cancel: no event
    t.eq(#seen, 3)
    local action = intents.execute(b, seer)
    t.eq(seen[4].reason, "planned", "executing without an intent plans one first")
    t.eq(action.targets, { sage })
    t.eq(action.skill, "attack")
    t.eq(intents.replan(b, hero), nil, "a battler without a planner")
    t.eq(require("fe.battle.ai").resolve("fe.battle.intents.execute"), intents.execute, "the AI path resolves")
    seer:set_resource("hp", 0)
    b:settle()
    t.eq(seer.data.intent, nil, "a fallen planner loses its intent")
    t.eq(seen[#seen].reason, "cancelled")
    t.ok(#events >= 4)
  end,
}

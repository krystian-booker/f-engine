-- Headless tests for Wayfarers' game rules.
local Battle = require("fe.battle.core")
local data = require("fe.data")
local daynight = require("fe.daynight")
local effects = require("fe.effects")
local event = require("fe.event")
local field = require("fe.field")
local game = require("fe.game")
local party = require("fe.rpg.party")
local quest = require("fe.quest")
local scene = require("fe.scene")
local state = require("fe.state")

local function new_game(t)
  if not game.started then
    fe.harness.start()
    t.wait_until(function() return scene.find("title") ~= nil end, 120)
  end
  scene.clear()
  require("fe.starter").new_game()
  t.wait_until(function() return field.player ~= nil end, 120)
end

local function press(t, key)
  fe.input.feed(key or "key:Return", 1)
  t.tick(2)
  fe.input.feed(key or "key:Return", 0)
  t.tick(2)
end

local function finish(t, task)
  for _ = 1, 200 do
    if task.status ~= "running" then return end
    press(t)
    t.tick(3)
  end
  error("event did not finish")
end

local function battle_with(troop)
  local rules = require("battle.rules")
  local b = Battle({ rules = rules, scheduler = require("fe.battle.schedulers.round_order").new(), troop = troop,
    random = fe.random.new(2) })
  b:populate(party.members(), troop)
  rules.setup(b)
  return b
end

return {
  ["custom stats: SP and battle-only BP"] = function(t)
    new_game(t)
    local rook = party.actor("rook")
    t.ok(rook:max_resource("sp") > 0)
    t.eq(rook:max_resource("bp"), 5)
  end,

  ["weakness hits break shields; broken enemies take double damage"] = function(t)
    new_game(t)
    local b = battle_with("wolves")
    local rook, wolf = b.battlers[1], b.battlers[2]
    local shot = data.get("skills", "true_shot")
    local before = effects.resolve({ user = rook, targets = { wolf }, source = shot, battle = b, preview = true })[1].amount
    effects.resolve({ user = rook, targets = { wolf }, source = shot, battle = b, random = fe.random.new(1) })
    t.eq(wolf.data.shield, 1)
    t.ok(wolf.data.revealed.bow)
    wolf:set_resource("hp", wolf:max_resource("hp"))
    effects.resolve({ user = rook, targets = { wolf }, source = shot, battle = b, random = fe.random.new(1) })
    t.ok(wolf:has_state("broken"))
    t.ok(not wolf:can_act())
    local broken = effects.resolve({ user = rook, targets = { wolf }, source = shot, battle = b, preview = true })[1].amount
    t.ok(broken >= before * 2 - 2, "broken targets take double damage")
  end,

  ["boosting commits BP for extra hits, paid when the action happens"] = function(t)
    new_game(t)
    local b = battle_with("wolves")
    local rook = b.battlers[1]
    rook:set_resource("bp", 3)
    local action = { kind = "skill", user = rook, skill = "attack", targets = { b.battlers[2] } }
    require("fe.battle.sideview").modifier.apply(action, 2)
    t.eq(action.hits, nil, "nothing changes while choosing")
    t.eq(rook:resource("bp"), 3, "nothing is spent while choosing")
    local results = b:perform(action)
    t.eq(action.hits, 3)
    t.eq(#results, 3)
    t.eq(rook:resource("bp"), 1)
    b:notify("on_round", 2)
    t.eq(rook:resource("bp"), 1, "no BP gained in a round spent boosting")
    b:notify("on_round", 3)
    t.eq(rook:resource("bp"), 2)
  end,

  ["break wears off under round order and shields recover"] = function(t)
    new_game(t)
    local b = battle_with("wolves")
    local rook, wolf, other = b.battlers[1], b.battlers[2], b.battlers[3]
    rook.auto = true
    local orders, expired, restored = {}, false, false
    b.events:connect(function(name, payload)
      if name == "order" then orders[#orders + 1] = payload end
      if name == "states_expired" and payload.battler == wolf then expired = true end
      if name == "shield_restored" and payload == wolf then restored = true end
    end)
    b:add_rules({
      on_start = function(battle)
        wolf.data.shield = 1
        effects.resolve({ user = rook, targets = { wolf }, source = data.get("skills", "true_shot"), battle = battle,
          random = fe.random.new(1) })
      end,
      before_action = function(_, action) action.cancelled = true end,
      outcome = function(battle) if battle.round > 3 then return "victory" end end,
    })
    b:run()
    t.ok(orders[1][#orders[1]] == wolf, "broken enemies act last")
    t.ok(orders[1][1] == other or orders[1][1] == rook)
    t.ok(expired, "break wears off after two turns")
    t.ok(not wolf:has_state("broken"))
    t.ok(restored, "shields recover")
    t.eq(wolf.data.shield, wolf.data.max_shield)
  end,

  ["turns come in Octopath order: chosen on the turn, with the next round previewed"] = function(t)
    new_game(t)
    local scheduler = require("fe.battle").settings.scheduler()
    t.eq(scheduler.settings.choose, "turn")
    local rules = require("battle.rules")
    local b = Battle({ rules = rules, scheduler = scheduler, troop = "wolves", random = fe.random.new(4) })
    b:populate(party.members(), "wolves")
    local on_turn, predicted, actual = true, nil, nil
    b:add_rules({
      player_controller = function(battle, me)
        on_turn = on_turn and battle.current == me
        return { kind = "defend", user = me }
      end,
      on_turn_start = function(battle)
        if battle.round == 1 and not predicted then predicted = select(2, scheduler.preview(battle)) end
      end,
      before_action = function(_, action) if action.kind ~= "defend" then action.cancelled = true end end,
      outcome = function(battle) if battle.round > 2 then return "victory" end end,
    })
    b.events:connect(function(name, payload) if name == "order" and b.round == 2 then actual = payload end end)
    b:run()
    t.ok(on_turn, "Rook chose on his own turn")
    t.eq(#actual, #predicted)
    for i = 1, #actual do t.ok(actual[i] == predicted[i], "preview slot " .. i) end
  end,

  ["the Bandit Chief acts twice a turn, three times once badly hurt"] = function(t)
    new_game(t)
    local b = battle_with("bandits")
    local chief = b.battlers[2]
    t.eq(chief.id:match("^[%w_]+"), "bandit_chief")
    local acted = 0
    b.events:connect(function(name, payload) if name == "action" and payload.user == chief then acted = acted + 1 end end)
    b:turn(chief)
    t.eq(acted, 2)
    chief:set_resource("hp", chief:max_resource("hp") // 2)
    b:turn(chief)
    t.eq(acted, 5)
    chief:add_state("broken", 1)
    b:turn(chief)
    t.eq(acted, 5, "a broken chief loses every action")
  end,

  ["divine skills need a full LP gauge, filled by taking hits"] = function(t)
    new_game(t)
    local b = battle_with("wolves")
    local rook, wolf = b.battlers[1], b.battlers[2]
    rook:set_resource("lp", 0)
    local volley = { kind = "skill", user = rook, skill = "hunters_volley", targets = b:opponents(rook) }
    t.eq(b:perform(volley), nil, "an empty gauge pays for nothing")
    for _ = 1, 3 do b:perform({ kind = "skill", user = wolf, skill = "bite", targets = { rook } }) end
    t.ok(require("fe.battle.limit").ready(rook))
    local results = b:perform({ kind = "skill", user = rook, skill = "hunters_volley", targets = b:opponents(rook) })
    t.ok(results ~= nil and #results == 4, "two hits on both wolves")
    t.ok(b.battlers[2]:has_state("broken") and b.battlers[3]:has_state("broken"), "two bow hits break each wolf")
    t.eq(rook:resource("lp"), 2, "the gauge was spent, then each break gave a point back")
  end,

  ["Selene joins and both chapters progress"] = function(t)
    new_game(t)
    local selene = field.world:find("scholar")
    finish(t, event.run(selene, nil, { player = field.player }))
    t.ok(party.is_active("selene"))
    t.eq(quest.status("selene_1"), "active")
  end,

  ["the tavern shows a different page at night"] = function(t)
    new_game(t)
    local barkeep = field.world:find("barkeep")
    daynight.set_hour(23)
    event.refresh(barkeep)
    t.eq(event.page(barkeep).script, "events.tavern_night")
    finish(t, event.run(barkeep, nil, { player = field.player }))
    t.ok(state.flag("found_cliffhaven"))
    t.eq(quest.stage("rook_1").id, "travel")
    daynight.set_hour(9)
  end,

  ["the bridge layer carries the player over the river"] = function(t)
    new_game(t)
    field.player:place({ x = 8.6, z = 7.6 }, 0)
    fe.input.feed("key:Right", 1)
    t.wait_until(function() return field.player.position.x > 11 end, 200)
    t.eq(field.player.layer, 1)
    t.ok(field.player.position.y > 0.9)
    fe.input.feed("key:Right", 0)
    t.tick(2)
  end,
}

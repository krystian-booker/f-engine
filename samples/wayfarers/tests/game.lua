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

  ["boosting spends BP for extra hits"] = function(t)
    new_game(t)
    local b = battle_with("wolves")
    local rook = b.battlers[1]
    rook:set_resource("bp", 3)
    local action = { kind = "skill", user = rook, skill = "attack", targets = { b.battlers[2] } }
    require("fe.battle.sideview").modifier.apply(action, 2)
    t.eq(action.hits, 3)
    t.eq(rook:resource("bp"), 1)
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

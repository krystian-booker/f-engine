-- Headless gameplay tests for the Vale sample (fengine_headless --test samples/vale).
local battle = require("fe.battle")
local event = require("fe.event")
local field = require("fe.field")
local game = require("fe.game")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local quest = require("fe.quest")
local save = require("fe.save")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")

local function press(t, key)
  fe.input.feed(key or "key:Return", 1)
  t.tick(2)
  fe.input.feed(key or "key:Return", 0)
  t.tick(2)
end

-- Starts the game once and begins a new game from the title screen.
local function new_game(t)
  if not game.started then
    fe.harness.start()
    t.wait_until(function() return scene.find("title") ~= nil end, 120)
  end
  -- The title's New Game cancels every other task (including a test), so
  -- tests start over through the same routine directly.
  scene.clear()
  require("fe.starter").new_game()
  t.wait_until(function() return field.player ~= nil and field.world.map_id == "town" end, 120)
end

-- Presses Confirm every few ticks until `predicate` holds (dialogue, rewards).
local function advance_until(t, predicate, limit)
  for _ = 1, limit or 200 do
    if predicate() then return end
    press(t)
    t.tick(4)
  end
  error("did not reach the expected state")
end

return {
  ["a new game starts in the town with two heroes"] = function(t)
    new_game(t)
    t.eq(field.world.map_id, "town")
    t.eq(#party.members(), 2)
    t.eq(inventory.count("potion"), 3)
    t.eq(quest.stage("fading_crystal").id, "talk")
  end,

  ["the elder sends the party to the grotto"] = function(t)
    new_game(t)
    local elder = field.world:find("elder")
    local running = event.run(elder, nil, { player = field.player })
    advance_until(t, function() return running.status ~= "running" end)
    t.eq(quest.stage("fading_crystal").id, "grotto")
  end,

  ["defeating the guardian restores the crystal"] = function(t)
    new_game(t)
    for _, actor in ipairs(party.members()) do actor:gain_exp(actor.exp_for_level(14)) end
    battle.rules.player_controller = function(b, user) return require("fe.battle.ai").default(b, user) end
    field.start("grotto", "entry")
    local guardian = field.world:find("guardian")
    local running = event.run(guardian, nil, { player = field.player })
    advance_until(t, function() return running.status ~= "running" end, 900)
    battle.rules.player_controller = nil
    t.ok(state.flag("guardian_defeated"), "guardian defeated")
    local crystal = field.world:find("vale_crystal")
    running = event.run(crystal, nil, { player = field.player })
    advance_until(t, function() return running.status ~= "running" end)
    t.ok(state.flag("crystal_restored"))
    t.eq(inventory.count("vale_crystal"), 1)
  end,

  ["saves restore the party, money and location"] = function(t)
    new_game(t)
    inventory.add_money(77)
    local money = inventory.money()
    save.write("test")
    inventory.add_money(-money)
    t.ok(save.load("test"))
    t.eq(inventory.money(), money)
    t.ok(field.resume())
    t.eq(field.world.map_id, "town")
    save.delete("test")
    task.frame()
  end,
}

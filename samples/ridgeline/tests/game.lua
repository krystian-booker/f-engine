-- Headless tests for Ridgeline: the council vote and the tactics battle.
local event = require("fe.event")
local field = require("fe.field")
local game = require("fe.game")
local scene = require("fe.scene")
local state = require("fe.state")
local tablex = require("fe.tablex")
local tactical = require("fe.battle.tactical")
local task = require("fe.task")

local council = require("council")
local ridge_battle = require("ridge_battle")

local function new_game(t)
  if not game.started then
    fe.harness.start()
    t.wait_until(function() return scene.find("title") ~= nil end, 120)
  end
  scene.clear()
  require("fe.starter").new_game()
  t.wait_until(function() return field.player ~= nil end, 120)
end

local function press(t)
  fe.input.feed("key:Return", 1)
  t.tick(2)
  fe.input.feed("key:Return", 0)
  t.tick(2)
end

local function finish(t, job)
  for _ = 1, 200 do
    if job.status ~= "running" then return end
    press(t)
    t.tick(3)
  end
  error("event did not finish")
end

local function argument(id)
  return tablex.find(council.arguments, function(a) return a.id == id end)
end

return {
  ["talking around camp unlocks arguments"] = function(t)
    new_game(t)
    t.eq(#council.available(), 1)
    for _, id in ipairs({ "bram", "lio", "wren" }) do
      finish(t, event.run(field.world:find(id), nil, { player = field.player }))
    end
    t.ok(state.flag("info_archers") and state.flag("info_grass") and state.flag("info_stream"))
    t.eq(#council.available(), 4)
  end,

  ["the vote follows conviction; persuasion can flip it"] = function(t)
    new_game(t)
    local result, votes = council.tally()
    t.eq(result, "hold")
    t.eq(votes.lio, "flank")
    t.eq(council.persuade("bram", argument("archers")), 3)
    t.eq(council.persuade("lio", argument("village")), 0)
    result, votes = council.tally()
    t.eq(votes.bram, "flank")
    t.eq(result, "flank")
    council.persuade("lio", argument("grass"))
    t.eq(council.tally(), "hold")
  end,

  ["the council scene runs to a vote and records the route"] = function(t)
    new_game(t)
    state.set_flag("info_archers")
    -- Confirm on every prompt: talk to Bram with the first argument (archers),
    -- then Lio with the first argument, then the vote.
    finish(t, task.spawn(council.run))
    t.ok(state.flag("council_done"))
    t.eq(council.conviction("bram"), 1)
    t.eq(state.var("route"), "flank")
  end,

  ["frost freezes the stream so units can cross; fire burns the grass"] = function(t)
    new_game(t)
    local world = fe.world.load("ridge_test", "ridge")
    local b = require("fe.battle.core")({ scheduler = {} })
    b.tactical = { world = world, occupied = {}, cell_timers = {} }
    local stream = world:cells_with_tag("water")[1]
    tactical.react(b, "ice", { stream })
    local cell = world:cell(stream.x, stream.z, 0)
    t.ok(tablex.contains(cell.tags, "ice") and not tablex.contains(cell.tags, "water"))
    tactical.react(b, "fire", { { x = 3, z = 7, layer = 0 } })
    t.ok(tablex.contains(world:cell(3, 7, 0).tags, "burning"))
    fe.world.unload("ridge_test")
  end,

  ["the AI fights both routes to an outcome"] = function(t)
    for _, route in ipairs({ "flank", "hold" }) do
      new_game(t)
      local units = {}
      for i, id in ipairs({ "cora", "bram", "lio", "wren" }) do
        units[i] = { actor = id, cell = { x = 4 + i * 2, z = 12, layer = 0 }, facing = { x = 0, z = -1 } }
      end
      local outcome
      local job = task.spawn(function() outcome = ridge_battle.fight(route, { units = units, auto = true }) end)
      t.wait_until(function() return job.status ~= "running" end, 60 * 60 * 15)
      t.ok(outcome == "victory" or outcome == "defeat", route .. " ended: " .. tostring(outcome))
      t.eq(state.flag("ridge_won"), outcome == "victory")
      state.set_flag("ridge_won", false)
    end
  end,
}

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
    tactical.board(b, world)
    local stream = world:cells_with_tag("water")[1]
    tactical.react(b, "ice", { stream })
    local cell = world:cell({ x = stream.x, z = stream.z, layer = 0 })
    t.ok(tablex.contains(cell.tags, "ice") and not tablex.contains(cell.tags, "water"))
    tactical.react(b, "fire", { { x = 3, z = 7, layer = 0 } })
    t.ok(tablex.contains(world:cell({ x = 3, z = 7, layer = 0 }).tags, "burning"))
    fe.world.unload("ridge_test")
  end,

  ["the AI fights both routes to an outcome"] = function(t)
    for _, route in ipairs({ "flank", "hold" }) do
      new_game(t)
      local units = {}
      for i, id in ipairs({ "cora", "bram", "lio", "wren" }) do
        units[i] = { actor = id, cell = { x = 4 + i * 2, z = 12, layer = 0 }, facing = { x = 0, z = -1 } }
      end
      local outcome, fought
      local arrivals = 0
      local disconnect = require("fe.battle").started:connect(function(b)
        fought = b
        b.events:connect(function(name, payload) if name == "reinforcements" then arrivals = arrivals + #payload.units end end)
      end)
      local job = task.spawn(function() outcome = ridge_battle.fight(route, { units = units, auto = true }) end)
      t.wait_until(function() return job.status ~= "running" end, 60 * 60 * 15)
      disconnect()
      t.ok(outcome == "victory" or outcome == "defeat", route .. " ended: " .. tostring(outcome))
      t.eq(#fought:obstacles(true), 2, route .. ": two barricades")
      t.ok(fought:rule("reaction") ~= nil, route .. ": ripostes are on")
      if route == "hold" and fought.round >= 3 then t.eq(arrivals, 2, "the second wave arrived in round 3") end
      t.eq(arrivals + #fought.tactical.reinforcements, #(route == "hold" and { 1, 2 } or { 1 }),
        route .. ": every reinforcement either arrived or was still pending")
      t.eq(state.flag("ridge_won"), outcome == "victory")
      state.set_flag("ridge_won", false)
    end
  end,

  ["barricades block arrows and break into rubble; Cora ripostes"] = function(t)
    new_game(t)
    local world = fe.world.load("ridge_test", "ridge")
    local Battle = require("fe.battle.core")
    local party = require("fe.rpg.party")
    local reactions = require("fe.battle.reactions")
    local b = Battle({ scheduler = {}, random = fe.random.new(3),
      rules = { reaction = reactions.counter({ requires = "riposte", facing = "not_behind" }) } })
    tactical.board(b, world)
    local barricade = tactical.add_obstacle(b, { enemy = "barricade", cell = { x = 7, z = 9, layer = 0 } })
    local bram = b:add({ actor = party.actor("bram"), side = "party", facing = { x = 0, z = -1 } })
    local cora = b:add({ actor = party.actor("cora"), side = "party", facing = { x = 0, z = -1 } })
    local raider = b:add({ enemy = "raider", side = "enemy", facing = { x = 0, z = 1 } })
    tactical.place(b, bram, { x = 7, z = 11, layer = 0 })
    tactical.place(b, raider, { x = 7, z = 7, layer = 0 })
    tactical.place(b, cora, { x = 3, z = 11, layer = 0 })
    local grid = require("fe.battle.tactical.grid")
    local longshot = require("fe.data").get("skills", "longshot")
    t.ok(not grid.contains_cell(tactical.range_cells(b, bram, longshot), raider.cell), "no shot through the barricade")
    barricade:set_resource("hp", 0)
    b:settle()
    t.ok(grid.contains_cell(tactical.range_cells(b, bram, longshot), raider.cell), "the way is clear once it breaks")
    t.ok(tablex.contains(world:cell({ x = 7, z = 9, layer = 0 }).tags, "rubble"))
    local reacted = 0
    b.events:connect(function(name) if name == "reaction" then reacted = reacted + 1 end end)
    tactical.place(b, raider, { x = 3, z = 10, layer = 0 })
    b:perform({ kind = "skill", user = raider, skill = "attack", targets = { cora }, cell = cora.cell })
    t.eq(reacted, 1, "a raider in front is answered")
    tactical.face(b, cora, { x = 0, z = 1 })
    b:perform({ kind = "skill", user = raider, skill = "attack", targets = { cora }, cell = cora.cell })
    t.eq(reacted, 1, "not from behind")
    b:perform({ kind = "skill", user = cora, skill = "attack", targets = { raider }, cell = raider.cell })
    t.eq(reacted, 1, "raiders cannot riposte")
    fe.world.unload("ridge_test")
  end,

  ["arts cost TP, gained every turn; Cora's quietus works once per battle"] = function(t)
    new_game(t)
    local world = fe.world.load("ridge_test", "ridge")
    local Battle = require("fe.battle.core")
    local party = require("fe.rpg.party")
    local tp = require("fe.battle.tp")
    local b = Battle({ scheduler = {}, random = fe.random.new(3), rules = tp.rules() })
    tactical.board(b, world)
    local cora = b:add({ actor = party.actor("cora"), side = "party", facing = { x = 0, z = -1 } })
    local raider = b:add({ enemy = "raider", side = "enemy", facing = { x = 0, z = 1 } })
    tactical.place(b, cora, { x = 7, z = 11, layer = 0 })
    tactical.place(b, raider, { x = 7, z = 10, layer = 0 })
    cora:reset_battle_resources()
    t.eq(cora:resource("tp"), 1)
    b:turn_start(cora)
    t.eq(cora:resource("tp"), 2)
    local steadfast = require("fe.data").get("skills", "steadfast")
    t.ok(tablex.find(cora:skills(), function(s) return s.id == "steadfast" end) ~= nil, "Cora knows her quietus")
    t.ok(not b:usable(cora, steadfast), "3 TP needed")
    b:turn_start(cora)
    t.ok(b:usable(cora, steadfast))
    t.ok(b:perform({ kind = "skill", user = cora, skill = "steadfast", targets = { raider }, cell = raider.cell }) ~= nil)
    t.eq(cora:resource("tp"), 0)
    for _ = 1, 4 do b:turn_start(cora) end
    t.eq(cora:resource("tp"), 4)
    t.ok(not b:usable(cora, steadfast), "once per battle")
    t.ok(b:usable(cora, require("fe.data").get("skills", "shove")), "other arts still work")
    fe.world.unload("ridge_test")
  end,
}

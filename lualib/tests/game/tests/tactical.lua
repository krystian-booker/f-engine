local Battle = require("fe.battle.core")
local battle_kit = require("fe.battle")
local data = require("fe.data")
local effects = require("fe.effects")
local party = require("fe.rpg.party")
local state = require("fe.state")
local tablex = require("fe.tablex")
local tactical = require("fe.battle.tactical")
local task = require("fe.task")
local vec = require("fe.vec")

local function record(b)
  local events = {}
  b.events:connect(function(name, payload) events[#events + 1] = { name = name, payload = payload } end)
  return events
end

local function count(events, name)
  return #tablex.filter(events, function(e) return e.name == name end)
end

-- Arena (12 x 10): walls at x = 5 for z = 1..5, water at x = 2..3, z = 7..8.
local function setup(options)
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  state.reset()
  party.reset()
  party.join("hero")
  party.join("scout")
  local world = fe.world.load("tactical_test", "arena")
  local b = Battle({ scheduler = {}, random = fe.random.new(4) })
  tactical.board(b, world, options)
  local hero = b:add({ actor = party.actor("hero"), side = "party", facing = { x = 0, z = -1 } })
  local scout = b:add({ actor = party.actor("scout"), side = "party", facing = { x = 0, z = -1 } })
  local slime = b:add({ enemy = "slime", side = "enemy", facing = { x = 0, z = 1 } })
  tactical.place(b, hero, { x = 2, z = 5, layer = 0 })
  tactical.place(b, scout, { x = 4, z = 5, layer = 0 })
  tactical.place(b, slime, { x = 3, z = 5, layer = 0 })
  return b, hero, scout, slime, world
end

local function teardown()
  fe.world.unload("tactical_test")
end

local function reaches(cells, x, z)
  for _, c in ipairs(cells) do
    if c.x == x and c.z == z then return c end
  end
end

local function has_tag(world, x, z, tag)
  return tablex.contains(world:cell({ x = x, z = z, layer = 0 }).tags, tag)
end

return {
  ["ranges come from the engine's shapes"] = function(t)
    local b, hero = setup()
    local cells = tactical.range_cells(b, hero, data.get("skills", "fire"))
    t.ok(#cells > 10)
    t.ok(reaches(cells, 2, 2) and not reaches(cells, 2, 5), "diamond 1..3")
    teardown()
  end,

  ["movement respects walls, occupied cells and zones of control"] = function(t)
    local b, hero = setup()
    local reach = tactical.reachable(b, hero)
    for _, c in ipairs(reach) do
      t.ok(not (c.x == 3 and c.z == 5), "cannot stop on the enemy")
      t.ok(not (c.x == 5 and c.z < 6), "the wall blocks x = 5")
    end
    -- (4, 4) is three steps away through (3, 4), but (3, 4) touches the slime,
    -- and the only other route is too long: zone of control cuts it off.
    t.ok(not reaches(reach, 4, 4), "zone of control stops movement next to the enemy")
    t.ok(reaches(reach, 3, 4), "zone cells can be entered")
    tactical.settings.zone_of_control = false
    t.ok(reaches(tactical.reachable(b, hero), 4, 4))
    tactical.settings.zone_of_control = true
    teardown()
  end,

  ["zones of control keep every destination reachable around them"] = function(t)
    local b, hero, scout, slime = setup()
    tactical.place(b, scout, { x = 8, z = 8, layer = 0 })
    tactical.place(b, slime, { x = 3, z = 3, layer = 0 })
    -- (4, 4) can be reached through (3, 4) (next to the slime) or around
    -- through (3, 5) and (4, 5); the second path must be found.
    local reach = tactical.reachable(b, hero)
    local target = reaches(reach, 4, 4)
    t.ok(target, "(4, 4) is reachable")
    for i, c in ipairs(target.path) do
      t.ok(not (c.x == 3 and c.z == 4), "the path avoids the zone cell " .. i)
    end
    t.ok(reaches(reach, 3, 4) and reaches(reach, 2, 3), "zone cells themselves can be entered")
    teardown()
  end,

  ["height and back attacks raise damage, but not for self-inflicted damage"] = function(t)
    local b, hero, scout, slime = setup()
    local attack = data.get("skills", "attack")
    tactical.face(b, slime, { x = -1, z = 0 }) -- facing the hero
    local front = effects.resolve({ user = hero, targets = { slime }, source = attack, battle = b, preview = true })[1].amount
    tactical.face(b, slime, { x = 1, z = 0 }) -- back turned to the hero
    local back = effects.resolve({ user = hero, targets = { slime }, source = attack, battle = b, preview = true })[1].amount
    t.ok(back > front, "back attacks deal more")
    local poison = data.get("states", "poison")
    local tick = effects.resolve({ user = slime, targets = { slime }, source = poison, effects = poison.on_turn, battle = b,
      environmental = true })[1]
    t.eq(tick.amount, 4, "10% of 40, no facing bonus")
    teardown()
  end,

  ["pushes move units, stop at blocked cells and slam into units"] = function(t)
    local b, hero, scout, slime, world = setup()
    tactical.place(b, scout, { x = 7, z = 6, layer = 0 })
    effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "shove"), battle = b, random = fe.random.new(1) })
    t.eq(slime.cell.x, 4)
    world:set_cell({ x = 5, z = 5, layer = 0 }, { add_tags = { "blocked" } })
    world:set_cell({ x = 5, z = 5, layer = 0 }, { walkable = true })
    tactical.place(b, hero, { x = 3, z = 5, layer = 0 })
    effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "shove"), battle = b, random = fe.random.new(1) })
    t.eq(slime.cell.x, 4, "blocked cells stop a push")
    world:set_cell({ x = 5, z = 5, layer = 0 }, { remove_tags = { "blocked" } })
    tactical.place(b, scout, { x = 5, z = 5, layer = 0 })
    local slime_hp, scout_hp = slime:resource("hp"), scout:resource("hp")
    effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "shove"), battle = b, random = fe.random.new(1) })
    t.eq(slime.cell.x, 4)
    t.ok(scout:resource("hp") < scout_hp, "the unit hit takes collision damage")
    t.ok(slime_hp - slime:resource("hp") > 5, "so does the pushed unit")
    teardown()
  end,

  ["forecasts show hit chance; the AI weighs it"] = function(t)
    local b, hero, scout, slime = setup()
    local eel = b:add({ enemy = "eel", side = "enemy", facing = { x = 0, z = 1 } })
    tactical.place(b, eel, { x = 2, z = 4, layer = 0 })
    local forecast = effects.resolve({ user = hero, targets = { eel }, source = data.get("skills", "attack"), battle = b,
      preview = true })
    t.near(forecast[1].hit_chance, 0.8)
    local sure = require("fe.battle.tactical.ai").expected(b, hero, data.get("skills", "attack"), { slime })
    local unsure = require("fe.battle.tactical.ai").expected(b, hero, data.get("skills", "attack"), { eel })
    t.ok(sure > unsure)
    teardown()
  end,

  ["thunder hurts each unit in connected water once"] = function(t)
    local b, hero, scout, slime, world = setup()
    tactical.place(b, hero, { x = 2, z = 7, layer = 0 })
    tactical.place(b, slime, { x = 3, z = 8, layer = 0 })
    local hp = { hero = hero:resource("hp"), slime = slime:resource("hp") }
    local water = world:cells_with_tag("water")
    t.eq(#water, 4)
    tactical.react(b, "thunder", water)
    t.eq(hp.hero - hero:resource("hp"), math.floor(hero:max_resource("hp") * 0.12 + 0.5))
    t.eq(hp.slime - slime:resource("hp"), math.floor(slime:max_resource("hp") * 0.12 + 0.5))
    teardown()
  end,

  ["ice makes water walkable; melting restores it"] = function(t)
    local b, hero, scout, slime, world = setup()
    world:set_cell({ x = 2, z = 7, layer = 0 }, { walkable = false })
    tactical.react(b, "ice", { { x = 2, z = 7, layer = 0 } })
    t.ok(world:cell({ x = 2, z = 7, layer = 0 }).walkable and has_tag(world, 2, 7, "ice"))
    tactical.react(b, "fire", { { x = 2, z = 7, layer = 0 } })
    t.ok(not world:cell({ x = 2, z = 7, layer = 0 }).walkable, "deep water again")
    t.ok(has_tag(world, 2, 7, "water"))
    teardown()
  end,

  ["burning lasts rounds, spreads through grass and hurts through effects"] = function(t)
    local b, hero, scout, slime, world = setup()
    for x = 6, 8 do world:set_cell({ x = x, z = 6, layer = 0 }, { add_tags = { "grass" } }) end
    tactical.reactions[1].spread_chance = 1
    tactical.react(b, "fire", { { x = 6, z = 6, layer = 0 } })
    t.ok(has_tag(world, 6, 6, "burning") and not has_tag(world, 7, 6, "burning"))
    b:notify("on_round", 2)
    t.ok(has_tag(world, 7, 6, "burning"), "fire spreads to the next grass cell")
    b:notify("on_round", 3)
    t.ok(has_tag(world, 8, 6, "burning"))
    b:notify("on_round", 4)
    t.ok(not has_tag(world, 6, 6, "burning"), "the first cell burned for three rounds")
    t.ok(has_tag(world, 7, 6, "burning"))
    tactical.reactions[1].spread_chance = 0.25
    -- A unit starting its turn in fire takes fire damage (resistances apply).
    tactical.place(b, slime, { x = 8, z = 6, layer = 0 })
    local before = slime:resource("hp")
    local events = {}
    b.events:connect(function(name, payload) events[#events + 1] = name end)
    require("fe.battle.tactical.terrain").hazards(b, slime)
    t.eq(before - slime:resource("hp"), 8, "10% doubled by the slime's fire weakness")
    slime:set_resource("hp", 1)
    require("fe.battle.tactical.terrain").hazards(b, slime)
    t.ok(not slime:alive())
    t.eq(#tablex.filter(events, function(n) return n == "knocked_out" end), 1)
    teardown()
  end,

  ["fallen units can be revived and stand again"] = function(t)
    local b, hero, scout, slime = setup()
    scout:set_resource("hp", 0)
    b:settle()
    t.eq(tactical.unit_at(b, { x = 4, z = 5 }), nil, "the cell is free")
    t.ok(tactical.corpse_at(b, { x = 4, z = 5 }) == scout)
    tactical.place(b, hero, { x = 4, z = 7, layer = 0 })
    local raise = data.get("skills", "raise")
    local targets = tactical.affected(b, hero, raise, { x = 4, z = 5 })
    t.eq(#targets, 1)
    b:perform({ kind = "skill", user = hero, skill = "raise", targets = targets, cell = { x = 4, z = 5 } })
    t.ok(scout:alive())
    t.ok(tactical.unit_at(b, { x = 4, z = 5 }) == scout, "back on its cell")
    teardown()
  end,

  ["the AI plans an attack on an adjacent foe and respects line of sight"] = function(t)
    local b, hero, scout, slime, world = setup()
    local plan = tactical.plan(b, slime)
    t.ok(plan.action ~= nil)
    t.eq(plan.action.targets[1].side, "party")
    -- An archer behind a wall it cannot climb or see past.
    tactical.place(b, scout, { x = 1, z = 8, layer = 0 })
    tactical.place(b, hero, { x = 1, z = 6, layer = 0 })
    tactical.place(b, slime, { x = 7, z = 8, layer = 0 })
    for z = 0, 9 do world:set_cell({ x = 6, z = z, layer = 0 }, { height = 6 }) end
    slime.skills = function() return { data.get("skills", "longshot") } end
    local cells = tactical.range_cells(b, slime, data.get("skills", "longshot"))
    t.ok(not reaches(cells, 1, 8), "no line of sight past the wall")
    t.eq(tactical.plan(b, slime).action, nil, "so the AI does not shoot through it")
    world:set_cell({ x = 6, z = 8, layer = 0 }, { height = 0 })
    local shot = tactical.plan(b, slime)
    t.ok(shot.action ~= nil and shot.action.skill == "longshot", "a gap in the wall opens a shot")
    teardown()
  end,

  ["terrain reacts only to actions that happen"] = function(t)
    local b, hero, scout, slime, world = setup()
    world:set_cell({ x = 2, z = 2, layer = 0 }, { add_tags = { "grass" } })
    tactical.place(b, slime, { x = 2, z = 3, layer = 0 })
    hero:set_resource("mp", 0)
    b:add_rules({ plan = function(_, unit)
      return { action = { kind = "skill", user = unit, skill = "fire", targets = {}, cell = { x = 2, z = 2, layer = 0 } } }
    end })
    hero.auto = true
    require("fe.battle.tactical.rules").take_turn(b, hero)
    t.ok(not has_tag(world, 2, 2, "burning"), "fire costs 4 MP the hero does not have")
    hero:set_resource("mp", 10)
    require("fe.battle.tactical.rules").take_turn(b, hero)
    t.ok(has_tag(world, 2, 2, "burning"))
    teardown()
  end,

  ["initiative rounds: timers and survive objectives count rounds, not turns"] = function(t)
    local b, hero, scout, slime = setup({ win = { tactical.objectives.survive(2) } })
    b.scheduler = require("fe.battle.tactical.rules").scheduler()
    b:add_rules({ plan = function() return {} end })
    for _, u in ipairs(b.battlers) do u.auto = true end
    local turns = 0
    b.events:connect(function(name) if name == "turn" then turns = turns + 1 end end)
    local outcome = b:run()
    t.eq(outcome, "victory")
    t.eq(b.round, 3)
    t.ok(turns >= 6, "every unit took at least two turns (" .. turns .. ")")
    teardown()
  end,

  ["tactical battles run through fe.battle.start with the game's rules"] = function(t)
    if not data.has_table("skills") then require("fe.rpg.database").load() end
    state.reset()
    party.reset()
    party.join("hero")
    party.join("scout")
    local field = require("fe.field")
    local world = fe.world.load("tactical_field", "arena")
    field.world = world
    local game_rules = battle_kit.rules
    local seen = {}
    battle_kit.rules = {
      on_start = function() seen.game = true end,
      outcome = function(b) if b.round > 6 then return "draw" end end,
    }
    local started
    local disconnect = battle_kit.started:connect(function(b) started = b end)
    local outcome
    local job = task.spawn(function()
      outcome = battle_kit.start({ presentation = "fe.battle.tactical", troop = "arena_slimes", auto = true,
        allow_defeat = true })
    end)
    t.wait_until(function() return started ~= nil end, 120)
    t.ok(tactical.active == started)
    local reinforcement = started:add({ enemy = "slime", side = "enemy", cell = { x = 9, z = 8, layer = 0 },
      facing = { x = 0, z = -1 } })
    t.ok(tactical.unit_at(started, { x = 9, z = 8 }) == reinforcement, "reinforcements take their cell")
    t.ok(reinforcement.entity ~= nil, "and get a sprite")
    t.wait_until(function() return job.status ~= "running" end, 60 * 120)
    disconnect()
    battle_kit.rules = game_rules
    t.ok(seen.game, "the game's rules ran")
    t.ok(outcome == "victory" or outcome == "defeat" or outcome == "draw", tostring(outcome))
    t.eq(#world:query({ tag = "unit" }), 0, "unit sprites are removed from the field world")
    t.eq(tactical.active, nil)
    field.world = nil
    fe.world.unload("tactical_field")
  end,

  ["deployment chooses which roster members fight"] = function(t)
    local b, hero, scout = setup()
    party.join("sage")
    b:remove(hero)
    b:remove(scout)
    local deploy = tactical.deploy
    t.eq(#deploy.bench(b), 3)
    local sage = deploy.add(b, nil, party.actor("sage"), { x = 1, z = 1, layer = 0 })
    t.ok(tactical.unit_at(b, { x = 1, z = 1 }) == sage)
    t.eq(#deploy.bench(b), 2)
    deploy.remove(b, nil, sage)
    t.eq(tactical.unit_at(b, { x = 1, z = 1 }), nil)
    t.eq(#b:side("party", true), 0)
    teardown()
  end,

  ["encounter symbols wander near home and chase at their own speed"] = function(t)
    local encounter = require("fe.encounter")
    local world = fe.world.load("symbol_test", "arena")
    local symbol = encounter.symbol(world, { troop = "slimes", position = { x = 8.5, z = 7.5 }, sprite = "", speed = 3,
      wander = 1.5 })
    local player = { position = { x = 1.5, z = 1.5 } }
    local moved = false
    for _ = 1, 300 do
      encounter.update_symbols(world, player)
      t.tick(1)
      local p = symbol.position
      if math.abs(p.x - 8.5) + math.abs(p.z - 7.5) > 0.2 then moved = true end
      t.ok(math.sqrt((p.x - 8.5) ^ 2 + (p.z - 7.5) ^ 2) < 2.2, "stays near home")
    end
    t.ok(moved, "it wanders")
    player.position = { x = symbol.position.x + 3, z = symbol.position.z }
    local before = symbol.position.x
    for _ = 1, 30 do
      encounter.update_symbols(world, player)
      t.tick(1)
    end
    t.ok(symbol.position.x - before > 1.2, "chases at speed 3 (" .. (symbol.position.x - before) .. ")")
    fe.world.unload("symbol_test")
  end,

  ["obstacles block movement and sight, take no turns, and leave rubble when destroyed"] = function(t)
    local b, hero, scout, slime, world = setup()
    tactical.place(b, scout, { x = 8, z = 8, layer = 0 })
    tactical.place(b, slime, { x = 9, z = 1, layer = 0 })
    local crate = tactical.add_obstacle(b, { enemy = "crate", cell = { x = 2, z = 3, layer = 0 } })
    t.eq(crate.side, "obstacle")
    t.ok(not crate:can_act())
    local reach = tactical.reachable(b, hero)
    t.ok(not reaches(reach, 2, 3), "nobody walks through an obstacle")
    t.ok(reaches(reach, 1, 2), "and it has no zone of control")
    local longshot = data.get("skills", "longshot")
    t.ok(not reaches(tactical.range_cells(b, hero, longshot), 2, 1), "it blocks line of sight")
    t.ok(reaches(tactical.range_cells(b, hero, longshot), 2, 4) == nil, "range starts at 2")
    t.ok(tablex.contains(tactical.affected(b, hero, data.get("skills", "attack"), crate.cell), crate), "it can be hit")
    t.ok(not tablex.contains(b:opponents(hero), crate), "but it is nobody's foe")
    b.scheduler = require("fe.battle.tactical.rules").scheduler()
    t.ok(not tablex.contains(b.scheduler.preview(b, 8), crate), "no turns")
    effects.resolve({ user = hero, targets = { crate }, source = data.get("skills", "shove"), battle = b, random = fe.random.new(1) })
    t.eq(crate.cell.z, 3, "pushes do not move it")
    local events = record(b)
    tactical.place(b, hero, { x = 2, z = 4, layer = 0 })
    b:perform({ kind = "skill", user = hero, skill = "attack", targets = { crate }, cell = crate.cell })
    t.ok(not crate:alive())
    t.eq(count(events, "destroyed"), 1)
    t.eq(tactical.unit_at(b, { x = 2, z = 3 }), nil)
    t.eq(tactical.corpse_at(b, { x = 2, z = 3 }), nil, "no corpse")
    t.ok(has_tag(world, 2, 3, "rubble"), "its destroyed reaction changed the cell")
    tactical.place(b, hero, { x = 2, z = 5, layer = 0 })
    t.ok(reaches(tactical.range_cells(b, hero, longshot), 2, 1), "the line of sight is clear again")
    t.eq(b:outcome(), nil)
    slime:set_resource("hp", 0)
    t.eq(b:outcome(), "victory", "obstacles do not count as enemies")
    world:set_cell({ x = 2, z = 3, layer = 0 }, { remove_tags = { "rubble" } })
    teardown()
  end,

  ["counterattacks follow facing and melee rules on the grid"] = function(t)
    local b, hero, scout, slime = setup()
    local reactions = require("fe.battle.reactions")
    b.rules.reaction = reactions.counter({ facing = "front" })
    local brute = b:add({ enemy = "brute", side = "enemy" })
    tactical.place(b, brute, { x = 2, z = 6, layer = 0 })
    tactical.face(b, brute, { x = 0, z = -1 })
    local events = record(b)
    local function hit(user, skill)
      brute:set_resource("hp", 999)
      user:set_resource("hp", 999)
      b:perform({ kind = "skill", user = user, skill = skill, targets = { brute }, cell = brute.cell })
    end
    hit(hero, "attack")
    t.eq(count(events, "reaction"), 1, "attacked from the front")
    tactical.face(b, brute, { x = 0, z = 1 })
    hit(hero, "attack")
    t.eq(count(events, "reaction"), 1, "no counter with its back turned")
    tactical.face(b, brute, { x = 0, z = -1 })
    tactical.place(b, hero, { x = 2, z = 3, layer = 0 })
    hit(hero, "longshot")
    t.eq(count(events, "reaction"), 1, "ranged attacks are not countered")
    teardown()
  end,

  ["reinforcements arrive on time or on a condition, and required ones hold off victory"] = function(t)
    local b, hero, scout, slime = setup()
    tactical.reinforce(b, { enemy = "slime", id = "late", cell = { x = 3, z = 5, layer = 0 }, turn = 2 })
    tactical.reinforce(b, { enemy = "slime", id = "alarm", cell = { x = 10, z = 8, layer = 0 }, required = true,
      when = function(battle) return battle.data.alarm == true end })
    local events = record(b)
    b.round = 1
    b:notify("on_round", 1)
    t.eq(count(events, "reinforcements"), 0)
    b.round = 2
    b:notify("on_round", 2)
    t.eq(count(events, "reinforcements"), 1)
    local late = tablex.find(b.battlers, function(u) return u.unit_id == "late" end)
    t.ok(late ~= nil and late.cell ~= nil)
    t.ok(not (late.cell.x == 3 and late.cell.z == 5), "the occupied cell is not used")
    t.ok(tactical.unit_at(b, late.cell) == late, "the nearest free cell is")
    slime:set_resource("hp", 0)
    late:set_resource("hp", 0)
    b:settle()
    t.eq(b:outcome(), nil, "a required reinforcement is still coming")
    b.data.alarm = true
    b:notify("on_turn_start", hero)
    local alarm = tablex.find(b.battlers, function(u) return u.unit_id == "alarm" end)
    t.ok(alarm and tactical.unit_at(b, { x = 10, z = 8 }) == alarm)
    alarm:set_resource("hp", 0)
    t.eq(b:outcome(), "victory")
    tactical.reinforce(b, { enemy = "slime", cell = { x = 9, z = 8, layer = 0 }, turn = 1 })
    b:notify("on_round", 3)
    t.eq(#b.tactical.reinforcements, 1, "nothing arrives once the battle is decided")
    teardown()
  end,

  ["follow-ups: pincers, diagonal and ranged allies, and a limit"] = function(t)
    local b, hero, scout, slime = setup()
    party.join("sage")
    party.actor("sage"):learn("volley")
    local sage = b:add({ actor = party.actor("sage"), side = "party", facing = { x = 0, z = -1 } })
    tactical.place(b, hero, { x = 2, z = 6, layer = 0 })
    tactical.place(b, slime, { x = 3, z = 6, layer = 0 })
    tactical.place(b, scout, { x = 4, z = 6, layer = 0 })
    tactical.place(b, sage, { x = 4, z = 7, layer = 0 })
    slime:buff({ stat = "defense", add = 500 }, 99)
    local events = record(b)
    local function attack()
      local action = { kind = "skill", user = hero, skill = "attack", targets = { slime }, cell = slime.cell }
      tactical.follow_ups(b, action, b:perform(action))
      local users = {}
      for _, e in ipairs(events) do
        if e.name == "follow_up" then users[#users + 1] = e.payload.user:name() end
      end
      for i = #events, 1, -1 do table.remove(events, i) end
      return users
    end
    local settings = tactical.settings
    t.eq(attack(), { "Scout" }, "only the ally straight behind by default")
    settings.follow_up_diagonal = true
    t.eq(attack(), { "Scout", "Sage" }, "diagonal allies join when enabled")
    settings.follow_up_max = 1
    t.eq(attack(), { "Scout" }, "up to the limit")
    settings.follow_up_diagonal, settings.follow_up_max = false, nil
    tactical.place(b, sage, { x = 6, z = 6, layer = 0 })
    t.eq(attack(), { "Scout" })
    settings.follow_up_ranged = true
    t.eq(attack(), { "Scout", "Sage" }, "a ranged ally behind the target joins with its follow-up skill")
    t.eq(b.log[#b.log].skill, "volley")
    settings.follow_up_ranged = false
    teardown()
  end,
  ["fallen units pile up on a cell and each can be revived"] = function(t)
    local b, hero, scout, slime = setup()
    scout:set_resource("hp", 0)
    b:settle()
    tactical.place(b, slime, { x = 4, z = 5, layer = 0 })
    slime:set_resource("hp", 0)
    b:settle()
    t.ok(tactical.corpse_at(b, { x = 4, z = 5 }) == slime, "the latest to fall lies on top")
    t.eq(#tactical.corpses_at(b, { x = 4, z = 5 }), 2)
    tactical.place(b, hero, { x = 4, z = 7, layer = 0 })
    local targets = tactical.affected(b, hero, data.get("skills", "raise"), { x = 4, z = 5 })
    t.eq(#targets, 1)
    t.ok(targets[1] == scout, "the fallen ally under the slime is still a revive target")
    b:perform({ kind = "skill", user = hero, skill = "raise", targets = targets, cell = { x = 4, z = 5 } })
    t.ok(scout:alive() and tactical.unit_at(b, { x = 4, z = 5 }) == scout)
    t.ok(tactical.corpse_at(b, { x = 4, z = 5 }) == slime, "the slime still lies there")
    teardown()
  end,

  ["a battle on the field world reverts every terrain change when it ends, mock battles too"] = function(t)
    if not data.has_table("skills") then require("fe.rpg.database").load() end
    state.reset()
    party.reset()
    party.join("hero")
    party.join("scout")
    local field = require("fe.field")
    local world = fe.world.load("tactical_field", "arena")
    field.world = world
    world:set_cell({ x = 2, z = 2, layer = 0 }, { add_tags = { "grass" } })
    world:set_cell({ x = 3, z = 2, layer = 0 }, { add_tags = { "grass" } })
    for _, mock in ipairs({ false, true }) do
      local burned, outcome
      local function fight()
        return battle_kit.start({ presentation = "fe.battle.tactical", troop = "arena_slimes", auto = true,
          allow_defeat = true, rules = {
            plan = function(battle, unit)
              if unit:is_party() and not battle.data.burned then
                battle.data.burned = true
                unit:set_resource("mp", 50)
                return { action = { kind = "skill", user = unit, skill = "fire", targets = {}, cell = { x = 2, z = 2, layer = 0 } } }
              end
              return {}
            end,
            on_turn_end = function(battle) burned = burned or has_tag(battle.tactical.world, 2, 2, "burning") end,
            on_turn_start = function(battle)
              local u = battle:side("enemy")[1]
              if u and battle.data.burned and not battle.data.crushed then
                battle.data.crushed = true
                tactical.set_cell(battle, { x = 9, z = 9, layer = 0 }, { walkable = true, add_tags = { "rubble" } })
              end
            end,
            outcome = function(bb) if bb.round > 2 then return "draw" end end,
          } })
      end
      local job = task.spawn(function() outcome = mock and battle_kit.mock(fight) or fight() end)
      t.wait_until(function() return job.status ~= "running" end, 60 * 120)
      t.eq(job.status, "done", tostring(job.error))
      t.eq(outcome, "draw")
      t.ok(burned, "the grass burned during the battle")
      t.ok(has_tag(world, 2, 2, "grass") and not has_tag(world, 2, 2, "burning"), "the grass is back")
      t.ok(has_tag(world, 3, 2, "grass") and not has_tag(world, 3, 2, "burning"))
      t.ok(not has_tag(world, 9, 9, "rubble") and not world:cell({ x = 9, z = 9, layer = 0 }).walkable,
        "script changes through tactical.set_cell are reverted too")
    end
    field.world = nil
    fe.world.unload("tactical_field")
  end,

  ["an aborted battle (error or cancel) reverts its terrain and tears the board down"] = function(t)
    if not data.has_table("skills") then require("fe.rpg.database").load() end
    local scene = require("fe.scene")
    local field = require("fe.field")
    local G = { x = 2, z = 2, layer = 0 }
    require("fe.input").use_defaults()
    for _, case in ipairs({
      { how = "error", mock = true }, { how = "error" }, { how = "cancel" }, { how = "cancel", mock = true },
      { how = "error", map = true }, { how = "cancel", map = true },
    }) do
      local label = case.how .. (case.mock and " (mock)" or "") .. (case.map and " (battle map)" or "")
      state.reset()
      party.reset()
      party.join("hero")
      party.join("scout")
      local world = fe.world.load("tactical_field", "arena")
      field.world = world
      world:set_cell(G, { add_tags = { "grass" } })
      local started, burned
      local disconnect = battle_kit.started:connect(function(b) started = b end)
      local ok, err
      local function fight()
        return battle_kit.start({ presentation = "fe.battle.tactical", troop = "arena_slimes", auto = true,
          allow_defeat = true, map = case.map and "arena" or nil, rules = {
            plan = function(battle, unit)
              if battle.data.burned and case.how == "error" then error("abort the battle") end
              if unit:is_party() and not battle.data.burned then
                battle.data.burned = true
                unit:set_resource("mp", 50)
                return { action = { kind = "skill", user = unit, skill = "fire", targets = {}, cell = G } }
              end
              return {}
            end,
            on_turn_end = function(battle) burned = burned or has_tag(battle.tactical.world, 2, 2, "burning") end,
          } })
      end
      local job = task.spawn(function()
        ok, err = pcall(function() return case.mock and battle_kit.mock(fight) or fight() end)
      end)
      if case.how == "cancel" then
        -- on the field world the arena's grass burns; a battle map has none
        t.wait_until(function() return burned or (case.map and started and started.data.burned) end, 60 * 60,
          label .. ": the fire was cast")
        job:cancel()
      end
      t.wait_until(function() return job.status ~= "running" end, 60 * 60, label)
      disconnect()
      local board = started and started.tactical.world
      if case.how == "error" then
        t.eq(ok, false, label)
        t.ok(tostring(err):find("abort the battle", 1, true), label .. ": the battle's error propagates: " .. tostring(err))
      end
      t.ok(burned or case.map, label .. ": the grass burned during the battle")
      t.ok(started and started.aborted, label .. ": the battle was aborted")
      t.eq(battle_kit.active, nil, label)
      t.eq(tactical.active, nil, label)
      t.tick(1)
      t.eq(scene.find("tactical"), nil, label .. ": the board scene is gone")
      if case.map then
        t.eq(fe.world.get("tactical"), nil, label .. ": the battle map is unloaded")
      else
        t.ok(board == world, label)
        t.ok(has_tag(world, 2, 2, "grass") and not has_tag(world, 2, 2, "burning"), label .. ": the grass is back")
        t.eq(#world:query({ tag = "unit" }), 0, label .. ": unit sprites are removed")
      end
      field.world = nil
      fe.world.unload("tactical_field")
      t.tick(3) -- nothing from the battle runs against the unloaded world
    end
    t.eq(fe.harness.errors(), 0)
  end,

  ["battlers added without a cell stand next to their summoner or side"] = function(t)
    local b, hero, scout, slime = setup()
    b.started = true
    local imp = b:summon({ enemy = "slime", side = "enemy" }, { owner = slime })
    t.ok(imp.cell ~= nil and vec.manhattan(imp.cell, slime.cell) == 1, "next to its owner")
    t.ok(tactical.unit_at(b, imp.cell) == imp)
    local ok, err = pcall(require("fe.battle.tactical.rules").take_turn, b, imp)
    t.ok(ok, tostring(err))
    local helper = b:add({ enemy = "old_knight", side = "party" })
    t.ok(helper.cell ~= nil and tactical.unit_at(b, helper.cell) == helper, "next to its side")
    local cell = helper.cell
    b:remove(helper)
    t.eq(tactical.unit_at(b, cell), nil, "leaving the battle frees the cell")
    t.error(function() b:add({ enemy = "slime", side = "wild" }) end, "need a cell")
    b.started = nil
    teardown()
  end,

  ["obstacles cannot be given a side"] = function(t)
    local b = setup()
    t.error(function() tactical.add_obstacle(b, { enemy = "crate", side = "enemy", cell = { x = 8, z = 8, layer = 0 } }) end,
      "no side")
    local crate = tactical.add_obstacle(b, { enemy = "crate", cell = { x = 8, z = 8, layer = 0 } })
    t.eq(crate.side, "obstacle")
    teardown()
  end,

  ["units arriving mid-round count from the next initiative round"] = function(t)
    local per_round, golem_round = {}, nil
    for _, arrive in ipairs({ false, true }) do
      local b = setup()
      b.scheduler = require("fe.battle.tactical.rules").scheduler()
      local counts, turns = {}, 0
      b:add_rules({ plan = function() return {} end,
        on_round = function(_, r) counts[r] = 0 end,
        on_turn_start = function(bb, u)
          counts[bb.round] = counts[bb.round] + 1
          turns = turns + 1
          if u.unit_id == "golem" then golem_round = golem_round or bb.round end
        end,
        outcome = function(bb) if bb.round > 3 then return "draw" end end })
      for _, u in ipairs(b.battlers) do u.auto = true end
      if arrive then
        tactical.reinforce(b, { enemy = "golem", cell = { x = 9, z = 8, layer = 0 }, when = function() return turns == 2 end })
      end
      b:run()
      per_round[arrive] = counts
      teardown()
    end
    t.eq(per_round[true][1], per_round[false][1], "the arrival does not stretch its round")
    t.eq(golem_round, 2, "the newcomer's first turn counts in the next round")
  end,

  ["a unit standing on ice when it melts is washed ashore and hurt"] = function(t)
    local b, hero, scout, slime, world = setup()
    world:set_cell({ x = 2, z = 7, layer = 0 }, { walkable = false })
    tactical.react(b, "ice", { { x = 2, z = 7, layer = 0 } })
    tactical.place(b, hero, { x = 2, z = 7, layer = 0 })
    local events = record(b)
    local hp = hero:resource("hp")
    tactical.react(b, "fire", { { x = 2, z = 7, layer = 0 } })
    t.ok(not world:cell({ x = 2, z = 7, layer = 0 }).walkable)
    t.ok(not (hero.cell.x == 2 and hero.cell.z == 7), "moved off the water")
    t.ok(world:cell(hero.cell).walkable and tactical.unit_at(b, hero.cell) == hero)
    t.eq(count(events, "pushed"), 1)
    t.eq(hero:resource("hp"), hp - math.floor(hero:max_resource("hp") * tactical.settings.sink_damage + 0.5))
    teardown()
  end,

  ["movement keeps jump marks and the presenter jumps them; ladders and gap jumps are settings"] = function(t)
    local b, hero, scout, slime, world = setup()
    local settings = tactical.settings
    tactical.place(b, slime, { x = 10, z = 8, layer = 0 })
    for x = 1, 4 do world:set_cell({ x = x, z = 4, layer = 0 }, { walkable = false }) end
    t.ok(not reaches(tactical.reachable(b, hero), 2, 3), "no way across without gap jumps")
    t.eq(require("fe.battle.tactical.grid").step_rules(b, hero).ladders, true)
    settings.ladders = false
    t.eq(require("fe.battle.tactical.grid").step_rules(b, hero).ladders, false, "the ladder switch reaches the engine rules")
    settings.ladders = true
    settings.gap_jump = { distance = 1 }
    local cells = tactical.reachable(b, hero)
    local across = reaches(cells, 2, 3)
    t.ok(across, "a one-cell gap can be jumped")
    local path = tactical.path_to(cells, across)
    t.ok(path[1].jump, "the step over the gap is marked")
    local presenter = tactical.Presenter.new(b, {})
    b.presenter = presenter
    presenter:start(b)
    local motions = {}
    local motion = presenter.motion
    presenter.motion = function(p, unit, kind)
      if unit == hero then motions[#motions + 1] = kind end
      motion(p, unit, kind)
    end
    tactical.walk(b, hero, path)
    t.wait_until(function() return #motions >= 2 end, 120)
    t.eq(motions, { "jump_start", "landed" })
    t.eq(hero.cell.x, 2)
    t.eq(hero.cell.z, 3)
    local p = hero.entity.position
    t.near(p.x, 2.5, 0.05)
    t.near(p.z, 3.5, 0.05)
    settings.gap_jump = false
    require("fe.scene").remove(presenter.scene)
    for _, u in ipairs(b.battlers) do presenter:despawn(u) end
    teardown()
  end,
}

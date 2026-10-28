local Battle = require("fe.battle.core")
local data = require("fe.data")
local effects = require("fe.effects")
local party = require("fe.rpg.party")
local state = require("fe.state")
local tactical = require("fe.battle.tactical")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  state.reset()
  party.reset()
  party.join("hero")
  party.join("scout")
  local world = fe.world.load("tactical_test", "arena")
  local b = Battle({ scheduler = {}, random = fe.random.new(4) })
  b.tactical = { world = world, occupied = {}, cell_timers = {} }
  local hero = b:add({ actor = party.actor("hero"), side = "party" })
  local scout = b:add({ actor = party.actor("scout"), side = "party" })
  local slime = b:add({ enemy = "slime", side = "enemy" })
  for _, u in ipairs(b.battlers) do
    u.entity = world:spawn({ position = { x = 1, z = 1 }, mover = { collide = false } })
  end
  tactical.place(b, hero, { x = 2, z = 5, layer = 0 })
  tactical.place(b, scout, { x = 4, z = 5, layer = 0 })
  tactical.place(b, slime, { x = 3, z = 5, layer = 0 })
  return b, hero, scout, slime, world
end

return {
  ["shapes and ranges"] = function(t)
    t.ok(tactical.in_shape({ x = 0, z = 0 }, { x = 1, z = 1 }, { shape = "diamond", min = 1, max = 2 }))
    t.ok(not tactical.in_shape({ x = 0, z = 0 }, { x = 2, z = 1 }, { shape = "diamond", min = 1, max = 2 }))
    t.ok(tactical.in_shape({ x = 0, z = 0 }, { x = 0, z = 3 }, { shape = "line", min = 1, max = 3 }, { x = 0, z = 1 }))
    local b, hero = setup()
    local cells = tactical.range_cells(b, hero, data.get("skills", "fire"))
    t.ok(#cells > 10)
    fe.world.unload("tactical_test")
  end,

  ["movement respects walls, occupied cells and zones of control"] = function(t)
    local b, hero, scout, slime = setup()
    local reach = tactical.reachable(b, hero)
    for _, c in ipairs(reach) do
      t.ok(not (c.x == 3 and c.z == 5), "cannot stop on the enemy")
      t.ok(not (c.x == 5 and c.z < 6), "the wall blocks x = 5")
    end
    -- (4, 4) is three steps away through (3, 4), but (3, 4) touches the slime,
    -- and the only other route is too long: zone of control cuts it off.
    local function reaches(cells, x, z)
      for _, c in ipairs(cells) do
        if c.x == x and c.z == z then return true end
      end
      return false
    end
    t.ok(not reaches(reach, 4, 4), "zone of control stops movement next to the enemy")
    tactical.settings.zone_of_control = false
    t.ok(reaches(tactical.reachable(b, hero), 4, 4))
    tactical.settings.zone_of_control = true
    fe.world.unload("tactical_test")
  end,

  ["height and back attacks raise damage; pushes move units"] = function(t)
    local b, hero, scout, slime = setup()
    local attack = data.get("skills", "attack")
    slime.entity:face({ x = -1, z = 0 }) -- facing the hero
    local front = effects.resolve({ user = hero, targets = { slime }, source = attack, battle = b, preview = true })[1].amount
    slime.entity:face({ x = 1, z = 0 }) -- back turned to the hero
    local back = effects.resolve({ user = hero, targets = { slime }, source = attack, battle = b, preview = true })[1].amount
    t.ok(back > front, "back attacks deal more")
    tactical.place(b, scout, { x = 7, z = 6, layer = 0 })
    effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "shove"), battle = b, random = fe.random.new(1) })
    t.eq(slime.cell.x, 4)
    fe.world.unload("tactical_test")
  end,

  ["fire on grass starts burning cells; objectives decide outcomes"] = function(t)
    local b, hero, scout, slime, world = setup()
    world:set_cell(6, 6, 0, { add_tags = { "grass" } })
    tactical.react(b, "fire", { { x = 6, z = 6, layer = 0 } })
    local cell = world:cell(6, 6, 0)
    t.ok(require("fe.tablex").contains(cell.tags, "burning"))
    local defeat = tactical.objectives.protect("hero")
    t.eq(defeat.check(b), nil)
    hero:set_resource("hp", 0)
    t.eq(defeat.check(b), "defeat")
    fe.world.unload("tactical_test")
  end,

  ["the AI plans an attack on an adjacent foe"] = function(t)
    local b, hero, scout, slime = setup()
    local plan = tactical.plan(b, slime)
    t.ok(plan.action ~= nil)
    t.eq(plan.action.targets[1].side, "party")
    fe.world.unload("tactical_test")
  end,
}

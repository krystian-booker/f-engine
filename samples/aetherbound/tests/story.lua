--- The whole prologue, played by the test: every scene runs to its end
--- with nobody at the controls but a hand that presses confirm.
local battle = require("game.battle.scene")
local dialogue = require("game.dialogue")
local field = require("game.field")
local party = require("game.party")
local prologue = require("story.prologue")
local scenes = require("game.scenes")

local started = false

--- Tick until `done()`, pressing confirm on and off, holding the dialogue
--- skip, and steering battle menus to a command that is allowed.
local function play(t, done, limit, what)
  local ticks = 0
  fe.input.feed("key:Tab", 1)
  while not done() do
    if ticks >= limit then error((what or "the scene") .. " did not finish in " .. limit .. " ticks", 2) end
    if fe.dev.errors()[1] then error("script error: " .. fe.dev.errors()[1].message, 2) end
    local scene = battle.active
    local menu = scene and scene.menu
    if menu and not menu.sub and not menu.target then
      local only = menu.unit.only
      for index, command in ipairs(menu.commands) do
        if only and command.skill == only then menu.index = index end
      end
      if not only and menu.unit.id == "rowan" and menu.commands[2] and menu.commands[2].skill == "resonance" then
        menu.index = 2
      end
    end
    fe.input.feed("key:Return", ticks % 6 < 3 and 1 or 0)
    t.tick(1)
    ticks = ticks + 1
  end
  fe.input.feed("key:Return", 0)
  fe.input.feed("key:Tab", 0)
  return ticks
end

--- Stand Cael in the middle of a zone, as if he had walked into it.
local function enter_zone(world, id)
  for _, zone in ipairs(world:zones()) do
    if zone.id == id then
      field.leader():place({ x = (zone.min.x + zone.max.x) / 2, z = (zone.min.z + zone.max.z) / 2 })
      return
    end
  end
  error("no zone '" .. id .. "'", 2)
end

local function cell_of(world, name)
  local p = world:points()[name].position
  return { x = math.floor(p.x), z = math.floor(p.z), layer = 0 }
end

return {
  _before_each = function(t)
    if not started then
      t.ok(fe.harness.start())
      started = true
    end
    fe.dev.clear_errors()
    prologue.stop()
  end,
  _after_each = function()
    prologue.stop()
  end,

  ["the title starts the prologue on the bridge"] = function(t)
    require("game.title").open()
    t.tick(90)
    t.eq(prologue.scene, nil)
    play(t, function() return prologue.scene == "fortress" and field.world ~= nil end, 600, "the title")
    t.eq(field.world.map_id, "fortress")
    t.ok(field.locked > 0, "the opening is a scene: no control yet")
    t.ok(field.actors.cael and field.actors.rowan and field.actors.lyra and field.actors.darius)
  end,

  ["the fortress: the opening hands over control, the path is walkable, both fights are won"] = function(t)
    prologue.start()
    play(t, function() return field.world ~= nil and field.locked == 0 and not dialogue.active() end, 6000, "the opening")
    local world = field.world
    -- The way from the bridge to the tower's door exists.
    for _, pair in ipairs { { "start", "gate" }, { "gate", "enc1_centre" }, { "enc1_centre", "enc2_centre" },
      { "enc2_centre", "stairs" }, { "stairs", "door" } } do
      local path = world:path(cell_of(world, pair[1]), cell_of(world, pair[2]))
      t.ok(#path > 0 and not path.truncated, "a path from " .. pair[1] .. " to " .. pair[2])
    end
    -- The player walks: Cael moves and the others follow his trail.
    local leader = field.leader()
    local before = leader.position
    fe.input.feed("key:Up", 1)
    t.tick(90)
    fe.input.feed("key:Up", 0)
    t.ok(leader.position.z < before.z - 2, "Cael walked north")
    local rowan = field.actors.rowan.position
    local gap = math.sqrt((rowan.x - leader.position.x) ^ 2 + (rowan.z - leader.position.z) ^ 2)
    t.ok(gap < 1.6, "Rowan follows close behind: " .. gap)
    -- First fight.
    enter_zone(world, "enc1")
    play(t, function() return battle.active ~= nil end, 600, "the first fight starting")
    t.eq(#battle.active.rules.enemies, 2)
    play(t, function() return battle.active == nil and field.locked == 0 end, 12000, "the first fight")
    -- Second fight: Rowan's Resonance.
    enter_zone(world, "enc2")
    play(t, function() return battle.active ~= nil end, 600, "the second fight starting")
    t.eq(#battle.active.rules.enemies, 3)
    play(t, function() return battle.active == nil and field.locked == 0 end, 12000, "the second fight")
    t.ok(party.state.rowan.corruption > 0, "Resonance left its mark")
    -- The door.
    enter_zone(world, "to_chamber")
    play(t, function() return prologue.scene == "chamber" end, 600, "entering the tower")
  end,

  ["the chamber: the Colossus stops at half, Rowan charges twice, and LAST LIGHT ends it"] = function(t)
    prologue.start("chamber")
    play(t, function() return field.world ~= nil and field.world.map_id == "chamber" end, 900, "the chamber loading")
    t.ok(field.locked > 0, "the chamber is a scene from its first frame")
    play(t, function() return battle.active ~= nil and battle.active.boss ~= nil end, 9000, "the Colossus waking")
    local intro = field.world:points().intro2.position
    local rowan_at = field.actors.rowan.position
    t.ok(math.abs(rowan_at.x - intro.x) < 4 and math.abs(rowan_at.z - intro.z) < 4, "the party walked in")
    local first = battle.active
    t.eq(first.boss.hp, 8000)
    play(t, function() return battle.active ~= first end, 30000, "phase one")
    t.eq(first.boss.hp, 4000, "it stops at half")
    play(t, function() return battle.active ~= nil and battle.active ~= first end, 20000, "the breaking point")
    local last = battle.active
    t.ok(last.boss.invulnerable)
    local rowan
    for _, unit in ipairs(last.rules.heroes) do
      if unit.id == "rowan" then rowan = unit end
    end
    t.eq(rowan.only, "unbound")
    play(t, function() return rowan.hp <= rowan.max_hp * 0.5 end, 9000, "the first round")
    t.eq(rowan.hp, math.floor(rowan.max_hp * 0.5))
    play(t, function() return rowan.hp == 1 end, 9000, "the second round")
    play(t, function()
      local menu = last.menu
      return menu ~= nil and menu.unit == rowan and #menu.commands == 1
    end, 9000, "the third round")
    t.eq(last.menu.commands[1].label, "LAST LIGHT")
    t.eq(last.boss.hp, 4000, "nothing hurt it")
    for _, unit in ipairs(last.rules.heroes) do t.ok(not unit.ko, unit.name .. " is still standing") end
    play(t, function() return prologue.scene == "last_light" end, 600, "choosing LAST LIGHT")
    -- The sacrifice is a cinematic: it runs on its own clock to the white-out and on.
    local last_light = require("story.last_light")
    play(t, function() return last_light.beat == "cael" end, 60 * 20, "Rowan's walk")
    t.ok(field.actors.rowan.position.x < 17, "Rowan walked to the Colossus: x " .. field.actors.rowan.position.x)
    t.ok(field.actors.cael.position.x > 19, "the others stayed where they stood")
    play(t, function() return last_light.beat == "cry" end, 60 * 60, "the wall of Aether")
    t.ok(field.actors.cael.position.x > 20.5, "Cael was thrown back: x " .. field.actors.cael.position.x)
    play(t, function() return prologue.scene == "remains" end, 60 * 150, "the sacrifice")
  end,

  ["what remains: Rowan is gone, the scene ends on the card, and the title returns"] = function(t)
    prologue.start("remains")
    play(t, function() return field.world ~= nil and field.world.map_id == "ruins" end, 600, "the ruins loading")
    t.tick(30)
    t.eq(field.actors.rowan, nil)
    play(t, function() return prologue.scene == "end" end, 60 * 200, "the ending")
    play(t, function() return prologue.ended end, 60 * 30, "the end card")
    t.ok(scenes.contains(require("game.title").scene))
  end,
}

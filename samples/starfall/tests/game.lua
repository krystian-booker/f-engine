-- The game played through its own input: every test starts from the title
-- screen of a freshly started engine state.
local field = require("game.field")
local input = require("game.input")
local scenes = require("game.scenes")
local state = require("game.state")
local title = require("game.title")

local started = false

local function press(t, raw, ticks)
  fe.input.feed(raw, 1)
  t.tick(ticks or 2)
  fe.input.feed(raw, 0)
  t.tick(2)
end

--- Press confirm until `done()`; fails after `limit` presses.
local function mash(t, done, limit, raw)
  for _ = 1, limit or 400 do
    if done() then return end
    press(t, raw or "key:Return")
  end
  error("still waiting after mashing confirm", 2)
end

local function new_game(t)
  title.new_game()
  t.tick(2)
end

--- Stand the player next to a placement, facing it.
local function approach(t, id)
  local entity = field.world:find(id)
  t.ok(entity, "no entity '" .. id .. "'")
  local p = entity.position
  field.player:place({ x = p.x, z = p.z + 0.9 })
  field.player:face(entity)
  t.tick(2)
  return entity
end

local function talk(t, id)
  approach(t, id)
  press(t, "key:Return")
  mash(t, function() return field.locked == 0 and scenes.top() == field.scene end)
end

return {
  _before_each = function(t)
    if not started then
      t.ok(fe.harness.start())
      started = true
    end
    title.open()
    t.tick(1)
  end,

  ["the title starts a new game in town with a party of four"] = function(t)
    t.eq(fe.world.get("field"), nil)
    press(t, "key:Return") -- New Game
    t.eq(field.world.map_id, "town")
    t.eq(#state.data.party, 4)
    t.ok(field.player:valid())
    t.eq(scenes.top(), field.scene)
  end,

  ["walking moves the party and counts steps"] = function(t)
    new_game(t)
    local before = field.player.position
    fe.input.feed("key:Up", 1)
    t.tick(40)
    fe.input.feed("key:Up", 0)
    t.tick(2)
    t.ok(field.player.position.z < before.z - 1, "walked north")
    t.ok(state.data.steps >= 1)
  end,

  ["the elder, then the stablehand, give the party a strider"] = function(t)
    new_game(t)
    talk(t, "stablehand")
    t.ok(not state.flag("strider"), "not before the elder has spoken")
    talk(t, "elder")
    t.ok(state.flag("met_elder"))
    talk(t, "stablehand")
    t.ok(state.flag("strider"))
    t.ok(state.data.vehicles.strider)
  end,

  ["a chest gives its items once"] = function(t)
    new_game(t)
    local before = state.item_count("potion")
    talk(t, "town_chest")
    t.eq(state.item_count("potion"), before + 2)
    talk(t, "town_chest")
    t.eq(state.item_count("potion"), before + 2)
  end,

  ["the gate leads to the world map, where only a strider fords the river"] = function(t)
    new_game(t)
    local gate = field.world:find("to_coast")
    field.player:place({ x = gate.position.x, z = gate.position.z - 1 })
    fe.input.feed("key:Down", 1)
    t.wait_until(function() return field.world.map_id == "overworld" and field.locked == 0 end, 600)
    fe.input.feed("key:Down", 0)
    t.ok(field.world:wraps())
    local ford = { x = 18.5, z = 14.5 }
    t.ok(not field.player:can_occupy(ford), "the river stops walkers")
    t.eq(field.world:find("strider"), nil, "no strider until one is given")
    state.set_flag("strider")
    field.grant_vehicle("strider")
    field.start("overworld", "town")
    local strider = field.world:find("strider")
    t.ok(strider)
    field.player:place(strider.position)
    t.tick(2)
    press(t, "key:V") -- board
    t.eq(state.data.riding, "strider")
    t.ok(field.player:can_occupy(ford), "the strider fords it")
    t.ok(not field.player:can_occupy({ x = 2.5, z = 2.5 }), "but not the open sea")
    field.player:place({ x = 22.5, z = 14.5 })
    t.tick(2)
    press(t, "key:V") -- leave it on the far bank
    t.eq(state.data.riding, nil)
    t.near(state.data.vehicles.strider.x, 22.5, 0.01)
    t.ok(field.world:find("strider"), "the strider waits where it was left")
  end,

  ["the skiff flies over the sea and lands only on open ground"] = function(t)
    new_game(t)
    state.set_flag("skiff")
    field.grant_vehicle("skiff")
    field.start("overworld", "town")
    local skiff = field.world:find("skiff")
    field.player:place(skiff.position)
    t.tick(2)
    press(t, "key:V")
    t.eq(state.data.riding, "skiff")
    t.ok(field.player:can_occupy({ x = 2.5, z = 2.5 }), "over the sea")
    t.tick(60)
    t.ok(field.player.position.y > 2, "it flies")
    field.player:place({ x = 2.5, z = 2.5 })
    t.tick(2)
    press(t, "key:V")
    t.eq(state.data.riding, "skiff", "no landing on water")
    field.player:place({ x = 35.5, z = 13.5 }) -- the shrine's islet
    t.tick(2)
    press(t, "key:V")
    t.eq(state.data.riding, nil)
    t.tick(30)
    t.near(field.player.position.y, 0, 0.05)
  end,

  ["danger builds with every step until a battle starts, and the menus win it"] = function(t)
    new_game(t)
    field.start("overworld", "town")
    field.player:place({ x = 8.5, z = 12.5 }) -- the plains west of town
    t.tick(2)
    state.data.danger, state.data.danger_limit = 0, 20
    local battle = require("game.battle.scene")
    fe.input.feed("key:Left", 1)
    t.wait_until(function() return battle.active ~= nil end, 900, "no battle started")
    fe.input.feed("key:Left", 0)
    t.ok(field.danger() < 0.5, "the sensor reset")
    local gold = state.data.gold
    -- Attack, first target, over and over; confirm also leaves the results.
    mash(t, function() return battle.active == nil and field.locked == 0 end, 3000)
    t.ok(state.data.gold > gold, "the battle paid out")
    t.eq(scenes.top(), field.scene)
    t.eq(state.data.battles, 1)
  end,

  ["a strider is never attacked"] = function(t)
    new_game(t)
    state.set_flag("strider")
    field.grant_vehicle("strider")
    field.start("overworld", "town")
    field.player:place(field.world:find("strider").position)
    t.tick(2)
    press(t, "key:V")
    state.data.danger, state.data.danger_limit = 0, 1
    field.player:place({ x = 8.5, z = 12.5 })
    fe.input.feed("key:Left", 1)
    t.tick(120)
    fe.input.feed("key:Left", 0)
    t.eq(require("game.battle.scene").active, nil)
  end,

  ["a save brings the party back to where it stood"] = function(t)
    new_game(t)
    field.player:place({ x = 12.5, z = 9.5 })
    state.data.gold = 777
    talk(t, "elder")
    local p = field.player.position
    state.data.position = { x = p.x, z = p.z }
    state.save("slot1")
    title.open()
    t.eq(fe.world.get("field"), nil)
    t.ok(title.continue("slot1"))
    t.tick(2)
    t.eq(state.data.gold, 777)
    t.ok(state.flag("met_elder"))
    t.near(field.player.position.x, p.x, 0.05)
    t.near(field.player.position.z, p.z, 0.05)
    fe.save.delete("slot1")
  end,

  ["button prompts follow the device in use"] = function(t)
    press(t, "key:Down")
    t.eq(input.device(), "keyboard")
    local keyboard = input.prompt("confirm")
    t.ok(keyboard:find("glyph.keyboard.return", 1, true), keyboard)
    -- Plugging a controller in switches the prompts at once...
    fe.input.feed_pad(1, { name = "DualSense", type = "playstation" })
    t.tick(1)
    t.ok(fe.input.device_changed())
    t.ok(input.prompt("confirm"):find("glyph.playstation.a", 1, true))
    -- ...a key press switches back, a button press forth again...
    press(t, "key:Down")
    t.ok(input.prompt("confirm"):find("glyph.keyboard", 1, true))
    press(t, "pad1:a")
    t.ok(input.prompt("confirm"):find("glyph.playstation.a", 1, true))
    -- ...and unplugging it falls back to the keyboard.
    fe.input.feed_pad(1)
    t.tick(1)
    t.eq(input.device(), "keyboard")
    t.ok(fe.input.device_changed())
  end,
}

local event = require("fe.event")
local field = require("fe.field")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")

return {
  ["event pages follow flags and run scripts"] = function(t)
    input.use_defaults()
    state.reset()
    local world = fe.world.load("pages", "arena")
    local npc = world:spawn({ id = "npc", position = { x = 3.5, z = 5.5 } })
    event.attach(npc, {
      { script = "tests.support.greet", trigger = "action" },
      { when = { flag = "met" }, script = "tests.support.again", trigger = "action", hidden = true },
    })
    t.eq(event.page(npc).script, "tests.support.greet")
    local task = event.run(npc)
    t.wait_until(function() return task.status ~= "running" end)
    t.ok(state.flag("met"))
    event.refresh(npc)
    t.eq(event.page(npc).script, "tests.support.again")
    t.ok(not npc:sprite().visible)
    fe.world.unload("pages")
  end,

  ["the player walks with input and talks to entities"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    local talked = false
    field.define("talker", function(world, p)
      local e = world:spawn({ id = p.id, position = p.position, collider = { radius = 0.3 } })
      e:data().interact = function() talked = true end
      return e
    end)
    field.start("arena", "start")
    local before = field.player.position
    fe.input.feed("key:Right", 1)
    t.tick(30)
    fe.input.feed("key:Right", 0)
    t.tick(2)
    local after = field.player.position
    t.ok(after.x > before.x + 0.5, "moved right")
    local p = field.player.position
    local e = field.world:spawn({ id = "friend", position = { x = p.x + 0.9, z = p.z }, collider = { radius = 0.3 } })
    e:data().interact = function() talked = true end
    table.insert(field.entities(), e)
    field.player:face("east")
    fe.input.feed("key:Z", 1)
    t.tick(1)
    fe.input.feed("key:Z", 0)
    t.tick(3)
    t.ok(talked)
    scene.clear()
  end,

  ["warping between maps writes the autosave"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    local save = require("fe.save")
    local task = require("fe.task")
    save.delete(save.autosave_slot)
    field.start("arena", "start")
    local job = task.spawn(function() field.warp("arena", "east") end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.ok(save.exists(save.autosave_slot), "autosave written")
    local _, meta = fe.save.read(save.autosave_slot)
    t.ok(meta.autosave)
    field.settings.autosave = false
    save.delete(save.autosave_slot)
    job = task.spawn(function() field.warp("arena", "start") end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.ok(not save.exists(save.autosave_slot), "autosave can be turned off")
    field.settings.autosave = true
    scene.clear()
  end,
}

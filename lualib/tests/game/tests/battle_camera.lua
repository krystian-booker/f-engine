-- fe.battle.sideview: a stage map's `battle_camera` prop overrides the
-- side-view camera settings, including an orthographic projection.
local Battle = require("fe.battle.core")
local battle_kit = require("fe.battle")
local data = require("fe.data")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local round_order = require("fe.battle.schedulers.round_order")
local sideview = require("fe.battle.sideview")
local state = require("fe.state")
local task = require("fe.task")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  Battle.settings.inventory, Battle.settings.party = inventory, party
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
end

local function camera_during(stage)
  local seen
  local job = task.spawn(function()
    battle_kit.start({ stage = stage, scheduler = round_order.new(), allow_defeat = true,
      setup = function(b)
        b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } }).auto = true
        b:add({ enemy = "golem", side = "enemy", position = { slot = 1 } })
      end,
      rules = {
        before_action = function(_, action) action.cancelled = true end,
        on_round = function()
          local cam = fe.world.get("battle"):camera()
          seen = { orthographic = cam.orthographic, ortho_height = cam.ortho_height, pitch = cam.pitch }
        end,
        outcome = function(b) if b.round >= 2 then return "draw" end end,
      } })
  end)
  return job, function() return seen end
end

return {
  ["battle camera: the stage's battle_camera prop overrides the settings key by key"] = function()
    local world = { props = function() return { battle_camera = { pitch = 35, orthographic = true, ortho_height = 9 } } end }
    local settings = sideview.camera_settings(world)
    assert(settings.pitch == 35 and settings.orthographic == true and settings.ortho_height == 9)
    assert(settings.distance == sideview.settings.camera.distance, "unset keys keep the settings")
    local plain = sideview.camera_settings({ props = function() return {} end })
    assert(plain.pitch == sideview.settings.camera.pitch and plain.orthographic == nil)
  end,

  ["battle camera: an orthographic stage gets an orthographic battle camera"] = function(t)
    require("fe.input").use_defaults()
    setup()
    local job, seen = camera_during("ortho_stage")
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    t.eq(job.status, "done", tostring(job.error))
    t.ok(seen(), "the battle ran")
    t.eq(seen().orthographic, true)
    t.near(seen().ortho_height, 9, 1e-4)
    t.near(seen().pitch, 35, 1e-3)
    setup()
    job, seen = camera_during("arena")
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    t.eq(job.status, "done", tostring(job.error))
    t.eq(seen().orthographic, false, "other stages keep a perspective camera")
    t.near(seen().pitch, sideview.settings.camera.pitch, 1e-3)
  end,
}

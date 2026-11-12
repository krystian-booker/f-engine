--- Scene 4, The Sacrifice. A cinematic: nothing here waits for the player.
--- It is cut to the music ("last_light"), so every beat has a time in
--- seconds from the bar the track starts on.
local actors = require("game.actors")
local audio = require("game.audio")
local battle = require("game.battle.scene")
local common = require("story.common")
local director = require("game.director")
local field = require("game.field")
local fx = require("game.fx")
local task = require("game.task")
local vfx = require("game.vfx")

local line = common.line
local last_light = {}

--- When things happen, in seconds of the track. The music is almost silent
--- at first, breaks into the full theme at `swell`, and peaks at `climax`.
last_light.marks = {
  stop = 6, cael = 11, tower = 16, what = 22, world = 26, please = 34, promise = 39,
  run = 46, cry = 51, raise = 56, signature = 60, failure = 66,
  swell = 75, breaking = 82, out = 88, leaving = 94, look = 102, forgive = 108,
  column = 113, silhouette = 119.4, climax = 120, black = 133, done = 146,
}

--- Seconds of the scene per second of play. 1 always, except in a replay
--- that wants to see the scene's frames without sitting through it.
last_light.speed = 1

--- The silhouette, for the few frames it exists (main.lua draws this
--- under the dialogue).
function last_light.draw()
  local shape = last_light.shape
  if not shape then return end
  local h = 1000
  local w = h * 384 / 512
  fe.canvas.image("fx/battle/silhouette.png", shape.x - w / 2, shape.y - h, { w = w, h = h, linear = true,
    color = 0x06021400 | 235 })
end

function last_light.run(stage)
  local world, boss, glow = stage.world, stage.boss, stage.glow
  local cael, rowan, lyra, darius = common.cast()
  local points = world:points()
  local m = last_light.marks

  -- The battle's panels go; the bars come in; the music begins.
  local scene = stage.scene
  task.over(0.4, function(t) scene.hud = 1 - t end)
  battle.finish(scene)
  audio.music(nil, 0.3)
  fx.letterbox(1, 0.5)
  audio.music("last_light", 0.1)
  local mark, stop_clock = common.clock(function() return last_light.speed end)
  last_light.passed = {}
  local function beat(name, at)
    mark(at)
    last_light.beat = name
    last_light.passed[name] = true -- tests and replays wait on these
  end
  for _, id in ipairs { "cael", "lyra", "darius" } do actors.pose(field.actors[id], "ready") end

  -- Rowan walks toward the Colossus. His sword begins to dissolve into light.
  if stage.flicker then task.cancel(stage.flicker) end
  rowan:sprite({ tint = { 1.15, 1.05, 1.3, 1 }, blend = "opaque" })
  actors.pose(rowan, nil)
  local aura = vfx.aura(world, rowan, { color = { 0.8, 0.6, 1.0 }, rate = 22 })
  aura:attach(rowan, { x = 0, y = 0.4, z = 0 })
  local r = rowan.position
  common.camera_async({ target = { x = (r.x + points.rowan_walk.position.x) / 2, y = 0.5, z = 16.3 }, pitch = 8,
    distance = 12 }, 5)
  director.walk(rowan, points.rowan_walk, { speed = 0.85, wait = false, timeout = 20 })

  beat("stop", m.stop)
  director.face(cael, rowan)
  actors.pose(cael, "reach")
  line("Cael", "ROWAN! STOP!", 2.4, { shout = true })

  beat("cael", m.cael)
  rowan:stop()
  -- Rowan turns.
  director.face(rowan, cael)
  common.camera_async({ target = { x = rowan.position.x + 0.6, y = 0.45, z = 16.2 }, pitch = 6, distance = 7 }, 3)
  line("Rowan", "Cael...", 2.6)

  beat("tower", m.tower)
  line("Rowan", "Remember the old watchtower?", 3.6)
  beat("what", m.what)
  actors.pose(cael, nil)
  line("Cael", "What...?", 2.2)
  beat("world", m.world)
  line("Rowan", "You said we'd see the whole world someday.", 5)
  beat("please", m.please)
  common.camera_async({ target = { x = cael.position.x - 0.6, y = 0.45, z = 16.2 }, pitch = 6, distance = 7 }, 2)
  line("Cael", "Rowan, please...", 3)
  beat("promise", m.promise)
  common.camera_async({ target = { x = rowan.position.x + 0.6, y = 0.45, z = 16.2 }, pitch = 6, distance = 6.5 }, 2.5)
  line("Rowan", "...You'd better keep that promise.", 4.2)

  -- Cael runs toward him. An invisible wall of Aether throws him backward.
  beat("run", m.run)
  director.face(rowan, { x = -1, z = 0 })
  director.walk(rowan, points.rowan_walk, { speed = 0.85, wait = false, timeout = 20 })
  common.camera_async({ target = { x = 18.5, y = 0.5, z = 16.3 }, pitch = 9, distance = 12 }, 1.2)
  director.walk(cael, points.cael_stop, { speed = 5.2 })
  local wall = cael.position
  audio.sfx("aether_wall", { volume = 1 })
  vfx.sprite(world, "fx/battle/veil.anim.json", { x = wall.x - 0.6, y = 1.6, z = wall.z },
    { scale = 3.2, life = 0.7, fade = true, tint = { 2, 1.4, 3, 1 } })
  vfx.shockwave(world, { x = wall.x - 0.6, y = 1.4, z = wall.z }, { scale = 7, seconds = 0.5 })
  fx.flash({ 0.8, 0.6, 1.0 }, 0.4, 0.5)
  fx.shake(world, 0.3, 0.5)
  fe.input.rumble(0.8, 0.8, 0.3)
  actors.pose(cael, "thrown")
  director.slide(cael, { x = wall.x + 3.4, z = wall.z }, 0.5, { arc = 0.8, ease = fx.ease_out })
  actors.pose(cael, "ko")

  beat("cry", m.cry)
  actors.pose(cael, "critical")
  director.face(cael, rowan)
  line("Cael", "ROWAN!!", 2.6, { shout = true })
  actors.pose(cael, "reach")

  -- Rowan raises his hand. The crystal in the Colossus begins tearing apart.
  beat("raise", m.raise)
  rowan:stop()
  director.face(rowan, { x = -1, z = 0 })
  actors.pose(rowan, "raise")
  common.camera_async({ target = { x = 14.5, y = 2.0, z = 16 }, pitch = 7, distance = 19 }, 3)
  boss:play("sunder", false, 0.4)
  audio.sfx("aether_charge", { volume = 1, pitch = 0.7 })
  glow:light({ intensity = 8, radius = 20, flicker = 0.7, flicker_speed = 7 })
  local core = stage.boss_point(stage.info.core)
  vfx.burst(world, core, "shard", 40)
  fx.shake(world, 0.2, 1.5)

  beat("signature", m.signature)
  audio.sfx("system_alert", { volume = 0.8, pitch = 1.1 })
  line("SYSTEM", "UNKNOWN AETHER SIGNATURE.", 3.2)
  beat("failure", m.failure)
  audio.sfx("system_alert", { volume = 0.9, pitch = 0.8 })
  line("SYSTEM", "CONTAINMENT FAILURE.", 3.2)

  -- The music swells. Rowan's body begins to glow.
  beat("swell", m.swell)
  local rumble = fx.shake(world, 0.08, 1, true)
  local stop_collapse = common.collapse(world, points.centre.position, 11, 1.1)
  aura:emitter({ paused = true })
  local blaze = vfx.aura(world, rowan, { color = { 0.55, 0.45, 0.7 }, rate = 60, scale = 1.5 })
  common.bind_rowan(rowan, 2.5)
  -- He brightens slowly: by his last line he is still a face, not a glare.
  local brighten = task.spawn(function()
    task.over(m.column - m.swell, function(t)
      if rowan:valid() then
        local k = 1 + t * 0.7
        rowan:sprite({ tint = { k, k * 0.96, k * 1.08, 1 }, flash = { 1, 0.95, 1, t * 0.22 } })
        rowan:light({ intensity = 3 + t * 6, radius = 6 + t * 4 })
      end
    end)
  end, "rowan brightens")
  common.camera_async({ target = { x = 17, y = 1.4, z = 16.3 }, pitch = 12, distance = 21 }, 6)

  beat("breaking", m.breaking)
  director.face(lyra, rowan)
  actors.pose(lyra, "shield")
  line("Lyra", "His body's breaking apart!", 3.4)
  beat("out", m.out)
  director.walk(darius, { x = cael.position.x + 0.7, z = cael.position.z + 0.5 }, { speed = 4, wait = false })
  line("Darius", "WE HAVE TO GET OUT!", 3.2, { shout = true })
  beat("leaving", m.leaving)
  actors.pose(darius, "shield")
  line("Cael", "NO! I'M NOT LEAVING HIM!", 3.6, { shout = true })

  -- Rowan looks at Cael one last time.
  beat("look", m.look)
  actors.pose(rowan, nil)
  director.face(rowan, cael)
  common.camera_async({ target = { x = rowan.position.x + 0.4, y = 0.5, z = 16.1 }, pitch = 4, distance = 5.6 }, 5)
  beat("forgive", m.forgive)
  line("Rowan", "...Forgive me.", 3.4)

  -- A brilliant column of starlight erupts from the chamber.
  beat("column", m.column)
  director.face(rowan, { x = -1, z = 0 })
  actors.pose(rowan, "raise")
  audio.sfx("last_light_sfx", { volume = 1 })
  fx.stop_shake(rumble)
  local quake = fx.shake(world, 0.25, 1, true)
  local at = rowan.position
  local beam = world:spawn { position = { x = at.x - 1.5, y = 0, z = at.z - 0.6 }, keep_height = true,
    sprite = { animation = "fx/battle/beam.anim.json", mode = "upright", blend = "additive", unlit = true,
      scale = 0.2, tint = { 3, 2.6, 3.6, 1 }, cast_shadow = false },
    light = { color = { 0.85, 0.75, 1.0 }, radius = 40, intensity = 10, position = { x = 0, y = 6, z = 0 } } }
  common.camera_async({ target = { x = 15, y = 5, z = 16 }, pitch = 2, distance = 26 }, 6.5, fx.ease_in)
  local erupt = task.spawn(function()
    task.over(m.climax - m.column, function(t)
      if beam:valid() then
        beam:sprite({ scale = 0.2 + t * t * 3.2 })
        beam:light({ intensity = 8 + t * 22 })
      end
      fe.screen.set { radial_blur = t * 0.45, radial_centre = { x = 0.42, y = 0.55 }, chromatic = t * 0.35,
        flash = { 1, 0.96, 1, t * t * 0.3 } }
    end)
  end, "erupt")
  vfx.ring(world, { x = at.x - 1.5, y = 0.2, z = at.z }, { color = { 0.9, 0.8, 1.0 }, scale = 26, seconds = 1.6, flat = true })
  vfx.burst(world, { x = at.x - 1.5, y = 1, z = at.z }, "light", 120, { 2, 1, 2 })
  for _, id in ipairs { "cael", "lyra", "darius" } do actors.pose(field.actors[id], "shield") end

  -- For one frame, an enormous celestial silhouette appears behind Rowan.
  beat("silhouette", m.silhouette)
  -- A dark shape in the light: vast, winged, crowned; gone before it can be
  -- looked at. It is drawn over the picture (the light would wash out
  -- anything standing in it), where Rowan stands.
  local sx, sy = rowan:screen_position(0)
  last_light.shape = { x = sx - 60, y = sy + 40 }
  for _ = 1, 4 do task.frame() end
  last_light.shape = nil
  last_light.shown = true

  -- He screams. The screen turns white. Silence.
  beat("climax", m.climax)
  fx.fade(1, 0.5, { 1, 1, 1 })
  task.cancel(erupt)
  task.cancel(brighten)
  fx.stop_shake(quake)
  stop_collapse()
  fe.screen.set { radial_blur = 0, chromatic = 0, flash = { 1, 1, 1, 0 } }
  for _, entity in ipairs { beam, aura, blaze, glow, boss } do
    if entity:valid() then entity:destroy() end
  end

  -- White, a ringing tone; then black.
  beat("black", m.black)
  task.over(4, function(t) fe.screen.set { fade = { 1 - t, 1 - t, 1 - t, 1 } } end)
  fx.fade_color = { 0, 0, 0 }
  beat("done", m.done)
  stop_clock()
  audio.music(nil, 2)
  audio.ambience(nil, 1, "hum")
end

return last_light

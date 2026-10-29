-- Headless tests for Wayfarers' field features: guiding a townsperson,
-- the day/night switch and the chapter select.
local daynight = require("fe.daynight")
local event = require("fe.event")
local field = require("fe.field")
local follower = require("fe.follower")
local game = require("fe.game")
local scene = require("fe.scene")
local story = require("fe.story")
local task = require("fe.task")

local function new_game(t)
  if not game.started then
    fe.harness.start()
    t.wait_until(function() return scene.find("title") ~= nil end, 120)
  end
  scene.clear()
  require("fe.starter").new_game()
  t.wait_until(function() return field.player ~= nil end, 120)
end

local function press(t, key)
  fe.input.feed(key or "key:Return", 1)
  t.tick(2)
  fe.input.feed(key or "key:Return", 0)
  t.tick(2)
end

local function finish(t, job)
  for _ = 1, 200 do
    if job.status ~= "running" then return end
    press(t)
    t.tick(3)
  end
  error("did not finish")
end

local function verb(id)
  for _, v in ipairs(field.verbs) do
    if v.id == id then return v end
  end
end

return {
  ["guiding the child makes a follower"] = function(t)
    new_game(t)
    local child = field.world:find("child")
    finish(t, task.spawn(function() verb("guide").run(child, field.player) end))
    t.ok(follower.has("child"))
    t.eq(#field.followers(), 1)
    t.eq(follower.get("child").assist.enemy, "wolf")
    follower.leave("child")
  end,

  ["T switches between day and night"] = function(t)
    new_game(t)
    t.eq(daynight.phase(), "day")
    t.wait_until(function() return field.can_control() end, 120)
    press(t, "key:T")
    t.wait_until(function() return field.locks == 0 and daynight.phase() == "night" end, 120)
    press(t, "key:T")
    t.wait_until(function() return field.locks == 0 and daynight.phase() == "day" end, 120)
  end,

  ["Selene's chapter is offered once she joins"] = function(t)
    new_game(t)
    t.eq(story.current(), "rook")
    t.eq(#story.available(), 0, "Rook's chapter is being played")
    finish(t, event.run(field.world:find("scholar"), nil, { player = field.player }))
    local offered = story.available()
    t.eq(#offered, 1)
    t.eq(offered[1].id, "selene")
    t.ok(offered[1].ok)
  end,
}

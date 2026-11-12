--- What the F1 editor knows about this game: its one zone type, and how to
--- walk a map from a point (Open map, Play from here).
local editor = {}

function editor.describe()
  return fe.json.encode {
    entities = { { type = "prop", label = "Prop", fields = {
      { key = "model", type = "asset", asset = "model", optional = true },
      { key = "sprite", type = "asset", asset = "sprite", optional = true },
      { key = "solid", type = "bool", default = true } } } },
    zones = { { type = "event", label = "Story trigger", fields = {} } },
    defaults = { entities = "prop", zones = "event", models = "prop" },
  }
end

--- Walk `map` from `point` with the party, without the story.
function editor.play_from(map, point)
  local prologue = require("story.prologue")
  local field = require("game.field")
  local fx = require("game.fx")
  local party = require("game.party")
  local task = require("game.task")
  prologue.stop()
  party.reset()
  task.spawn(function()
    field.enter(map, point or "start")
    fx.fade(0, 0)
  end, "editor play")
end

function editor.list()
  return { "Title", "Scene 1: The Fallen Fortress", "Scene 2: The Weapon Awakens", "Scene 4: The Sacrifice",
    "Scene 5: What Remains" }
end

function editor.launch(name)
  local prologue = require("story.prologue")
  if name == "Title" then
    prologue.stop()
    require("game.title").open()
  elseif name:find("Scene 1") then
    prologue.start()
  elseif name:find("Scene 2") then
    prologue.start("chamber")
  elseif name:find("Scene 4") then
    prologue.start("last_light")
  else
    prologue.start("remains")
  end
end

return editor

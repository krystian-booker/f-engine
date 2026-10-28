--- World map travel: named destinations shown on a map image, unlocked by
--- flags, chosen from a menu and travelled to with fe.field.warp.
---
--- data/worldmap.json:
--- ```json
--- {"image": "ui/worldmap.png",
---  "places": [{"id": "vale", "name": "Amber Vale", "map": "town", "point": "entry",
---              "at": [120, 80], "unlock": "found_vale", "region": "Highlands"}]}
--- ```
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")
local ui = require("fe.ui")

local worldmap = { definition = nil }

function worldmap.load(path)
  path = path or "data/worldmap.json"
  worldmap.definition = fe.fs.exists(path) and fe.json.read(path) or { places = {} }
end

--- Places the party can travel to.
function worldmap.places()
  if not worldmap.definition then worldmap.load() end
  local out = {}
  for _, place in ipairs(worldmap.definition.places) do
    if not place.unlock or state.flag(place.unlock) then out[#out + 1] = place end
  end
  return out
end

--- Mark a place discovered.
function worldmap.discover(id)
  for _, place in ipairs(worldmap.definition and worldmap.definition.places or {}) do
    if place.id == id and place.unlock then state.set_flag(place.unlock) end
  end
end

local MapScreen = { name = "worldmap", opaque = true }
MapScreen.__index = MapScreen

function MapScreen:draw()
  local cw, ch = fe.canvas.size()
  local def = worldmap.definition
  fe.canvas.rect(0, 0, cw, ch, 0x0b1424ff)
  if def.image then fe.canvas.image(def.image, 0, 0, { w = cw, h = ch }) end
  local selected = self.list and self.list:selected()
  for _, place in ipairs(self.places) do
    local at = place.at or { cw / 2, ch / 2 }
    local active = selected and selected.value == place
    fe.canvas.rect(at[1] - 2, at[2] - 2, 5, 5, active and ui.theme.highlight or 0xffffffaa)
    if active then ui.text(place.name, at[1] + 6, at[2] - 4, { color = ui.theme.highlight }) end
  end
end

--- Open the travel screen; returns the chosen place or nil. Call in a task.
---@param options? {travel?: boolean} travel defaults to true (warp to the place)
function worldmap.open(options)
  options = options or {}
  local places = worldmap.places()
  local screen = setmetatable({ places = places }, MapScreen)
  scene.push(screen)
  local entries = {}
  for _, place in ipairs(places) do
    entries[#entries + 1] = { label = place.name, right = place.region, value = place }
  end
  local done, chosen = false, nil
  local menu = ui.Menu({ title = "Travel", items = entries, anchor = "right", width = 150, rows = 8,
    on_confirm = function(item) chosen, done = item.value, true end, on_cancel = function() done = true end })
  screen.list = menu.list
  scene.push(menu)
  task.wait_until(function() return done end)
  scene.remove(menu)
  scene.remove(screen)
  if chosen and options.travel ~= false then
    require("fe.field").warp(chosen.map, chosen.point)
  end
  return chosen
end

return worldmap

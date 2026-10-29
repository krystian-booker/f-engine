--- World map travel: named destinations shown on a map image, unlocked by
--- flags, chosen from a menu and travelled to with fe.field.warp.
---
--- data/worldmap.json (loaded on first use, or with `worldmap.load(path)`):
--- ```json
--- {"image": "ui/worldmap.png",
---  "places": [{"id": "vale", "name": "Amber Vale", "map": "town", "point": "entry",
---              "at": [120, 80], "unlock": "found_vale", "region": "Highlands"}]}
--- ```
local i18n = require("fe.i18n")
local state = require("fe.state")
local ui = require("fe.ui")

local worldmap = { definition = nil }

i18n.defaults({ fe = { worldmap = { title = "Travel" } } })

function worldmap.load(path)
  path = path or "data/worldmap.json"
  worldmap.definition = fe.fs.exists(path) and fe.json.read(path) or { places = {} }
end

local function definition()
  if not worldmap.definition then worldmap.load() end
  return worldmap.definition
end

--- Places the party can travel to.
function worldmap.places()
  local out = {}
  for _, place in ipairs(definition().places) do
    if not place.unlock or state.flag(place.unlock) then out[#out + 1] = place end
  end
  return out
end

--- Mark a place discovered (sets its unlock flag).
function worldmap.discover(id)
  for _, place in ipairs(definition().places) do
    if place.id == id and place.unlock then state.set_flag(place.unlock) end
  end
end

local MapScreen = { name = "worldmap", opaque = true }
MapScreen.__index = MapScreen

function MapScreen:draw()
  local cw, ch = fe.canvas.size()
  local def = definition()
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

--- Open the travel screen; returns the chosen place, or nil when cancelled.
--- Call in a task.
---@param options? {travel?: boolean} travel defaults to true (warp to the place)
function worldmap.open(options)
  options = options or {}
  local places = worldmap.places()
  local screen = setmetatable({ places = places }, MapScreen)
  local entries = {}
  for _, place in ipairs(places) do
    entries[#entries + 1] = { label = place.name, right = place.region, value = place }
  end
  local menu
  menu = ui.Menu({ title = ui.t("fe.worldmap.title"), items = entries, anchor = "right", width = 150, rows = 8,
    on_confirm = function(item) ui.close(menu, item.value) end, on_cancel = function() ui.close(menu) end })
  screen.list = menu.list
  local chosen = ui.with_view(screen, ui.run_screen, menu)
  if chosen and options.travel ~= false then
    require("fe.field").warp(chosen.map, chosen.point)
  end
  return chosen
end

return worldmap

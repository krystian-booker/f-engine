--- World map travel: named destinations shown on a map image, unlocked by
--- flags, chosen from a menu and travelled to with fe.field.warp.
---
--- data/worldmap.json (loaded on first use, or with `worldmap.load(path)`):
--- ```json
--- {"image": "ui/worldmap.png",
---  "places": [{"id": "vale", "name": "Amber Vale", "map": "town", "point": "entry",
---              "at": [120, 80], "unlock": "found_vale", "region": "Highlands", "level": 12}]}
--- ```
--- A place's `level` (the area's danger, Octopath's recommended level) is
--- shown next to it, and the place whose `map` is the current field map is
--- marked as the party's location (`worldmap.here()`). Colours and marker
--- sizes are in `worldmap.style`.
---
--- From a menu page, give the page `closes = true` (fe.ui.screens.menu)
--- so the warp does not run under the menu's pause.
local i18n = require("fe.i18n")
local state = require("fe.state")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local worldmap = {
  definition = nil,
  style = {
    background = 0x0b1424ff,  -- behind the map image
    marker = 0xffffffaa,      -- a place
    marker_size = 5,
    here_size = 9,            -- the ring around the party's location
  },
}

--- The file's style, for `__reload`.
worldmap.file_defaults = { style = tablex.deep_copy(worldmap.style) }

--- Adopt edited style values on hot reload (values the game changed at
--- runtime are kept).
function worldmap.__reload(live, fresh)
  live.file_defaults.style = tablex.adopt(live.style, fresh.style, live.file_defaults.style)
end

i18n.defaults({ fe = { worldmap = { title = "Travel", level = "Lv {level}", here = "Here" } } })

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

--- The place the party is at: the first whose `map` is the field's current
--- map, or nil.
---@return table?
function worldmap.here()
  local world = require("fe.field").world
  local map = world and world.map_id
  if not map then return nil end
  for _, place in ipairs(definition().places) do
    if place.map == map then return place end
  end
  return nil
end

local MapScreen = { name = "worldmap", opaque = true }
MapScreen.__index = MapScreen

function MapScreen:draw()
  local cw, ch = fe.canvas.size()
  local def = definition()
  local style = worldmap.style
  fe.canvas.rect(0, 0, cw, ch, style.background)
  if def.image then fe.canvas.image(def.image, 0, 0, { w = cw, h = ch }) end
  local selected = self.list and self.list:selected()
  local size, half = style.marker_size, math.floor(style.marker_size / 2)
  for _, place in ipairs(self.places) do
    local at = place.at or { cw / 2, ch / 2 }
    local active = selected and selected.value == place
    if place == self.here then
      local ring, offset = style.here_size, math.floor(style.here_size / 2)
      fe.canvas.rect(at[1] - offset, at[2] - offset, ring, ring, ui.theme.good)
      fe.canvas.rect(at[1] - offset + 1, at[2] - offset + 1, ring - 2, ring - 2, style.background)
    end
    fe.canvas.rect(at[1] - half, at[2] - half, size, size, active and ui.theme.highlight or style.marker)
    if active then
      local label = place.name
      if place.level then label = label .. "  " .. ui.t("fe.worldmap.level", { level = place.level }) end
      ui.text(label, at[1] + 6, at[2] - 4, { color = ui.theme.highlight })
    end
  end
end

--- Open the travel screen; returns the chosen place, or nil when cancelled.
--- Call in a task.
---@param options? {travel?: boolean} travel defaults to true (warp to the place)
function worldmap.open(options)
  options = options or {}
  local places = worldmap.places()
  local here = worldmap.here()
  local screen = setmetatable({ places = places, here = here }, MapScreen)
  local entries = {}
  for _, place in ipairs(places) do
    local right = place.level and ui.t("fe.worldmap.level", { level = place.level }) or place.region
    if place == here then right = ui.t("fe.worldmap.here") end
    entries[#entries + 1] = { label = place.name, right = right, value = place }
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

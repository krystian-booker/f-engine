--- Random encounters.
---
--- Encounter tables (data/encounters.json) list weighted formations:
--- ```json
--- [{"id": "forest", "steps": 24, "formations": [{"troop": "slimes", "weight": 3}, {"troop": "wolves"}]}]
--- ```
--- An area uses a table through a zone of type "encounter" (props
--- {"table": "forest", "rate": 1.5}; the editor shows them as a form once
--- this module is loaded) or a grid tag "encounter:forest".
--- `steps` is the average distance (tiles) between battles. Modifiers (a
--- repel item, a "danger level" from party strength) scale the rate.
local schema = require("fe.schema")
local signal = require("fe.signal")
local tablex = require("fe.tablex")
local task = require("fe.task")

local encounter = {
  enabled = true,
  --- Functions(rate, table, context) -> rate; multiply to change frequency.
  modifiers = {},
  --- Emitted as (troop_id, table) when an encounter triggers.
  triggered = signal.new(),
  grace = 4, -- tiles without encounters after a battle or map change
}

schema.define("encounter_table", {
  id = "string",
  steps = { type = "number", min = 1, default = 24 },
  formations = { type = { list = { record = {
    troop = { type = { ref = "troops" } },
    weight = { type = "number", min = 0, default = 1 },
  } } } },
  stage = { type = "string", optional = true, doc = "Battle stage map for this area" },
})

schema.zone("encounter", {
  table = { type = { ref = "encounters" }, doc = "Encounter table id (data/encounters.json)" },
  rate = { type = "number", min = 0, default = 1, doc = "Multiplies how often battles start here" },
}, "Random battles from an encounter table while the player walks inside.")

local random = fe.random.new()
local wander_random = fe.random.new()
local distance = 0
local threshold

local function next_threshold(steps)
  -- Uniform 0.5x..1.5x the average spacing.
  return steps * (0.5 + random:float())
end

--- Which table applies at a position (zone first, then grid tag).
function encounter.table_at(world, position, layer)
  for _, zone in ipairs(world:zones_at(position, layer or 0)) do
    if zone.type == "encounter" and zone.props.table then
      return zone.props.table, zone.props.rate or 1, zone.props
    end
  end
  local cell = world:cell({ x = math.floor(position.x), z = math.floor(position.z), layer = layer })
  if cell then
    for _, tag in ipairs(cell.tags) do
      local id = tag:match("^encounter:(.+)$")
      if id then return id, 1, {} end
    end
  end
  return nil
end

--- Reset the step counter (after battles, warps and loads).
function encounter.reset()
  distance = -encounter.grace
  threshold = nil
end

local function pick_formation(entry)
  local total = 0
  for _, f in ipairs(entry.formations) do total = total + f.weight end
  local roll = random:float() * total
  for _, f in ipairs(entry.formations) do
    roll = roll - f.weight
    if roll <= 0 then return f.troop end
  end
  return entry.formations[#entry.formations].troop
end

--- Account for movement; returns a troop id when a battle should start.
---@param world World
---@param position table
---@param moved number distance travelled this tick
---@param layer? integer
function encounter.step(world, position, moved, layer)
  if not encounter.enabled or moved <= 0 then return nil end
  local id, rate, props = encounter.table_at(world, position, layer)
  if not id then return nil end
  local entry = require("fe.data").get("encounters", id)
  for _, modify in ipairs(encounter.modifiers) do
    rate = modify(rate, entry, props) or rate
  end
  if rate <= 0 then return nil end
  threshold = threshold or next_threshold(entry.steps)
  distance = distance + moved * rate
  if distance < threshold then return nil end
  distance = 0
  threshold = next_threshold(entry.steps)
  local troop = pick_formation(entry)
  encounter.triggered:emit(troop, entry)
  return troop, entry
end

--- Danger estimate 0..1 for UI (how close the next encounter is).
function encounter.danger()
  if not threshold then return 0 end
  return tablex.clamp(distance / threshold, 0, 1)
end

--- Spawn a visible enemy symbol. When idle it walks to a random point
--- within `wander` tiles of where it spawned every 1.5-3.5 s, and it chases the
--- player at `speed` (tiles per second) while within `sight` tiles.
--- Touching it starts a battle with its troop (fe.field handles contact).
---@param world World
---@param spec {troop: string, position: table, sprite: string, sight?: number, speed?: number, wander?: number}
function encounter.symbol(world, spec)
  local speed = spec.speed or 1.6
  local entity = world:spawn({
    position = spec.position, tags = { "encounter_symbol" },
    sprite = { animation = spec.sprite, move_clip = "walk", idle_clip = "idle" },
    mover = { speed = speed }, collider = { radius = 0.35, solid = false },
    props = { troop = spec.troop, sight = spec.sight or 4 },
  })
  local state = entity:data()
  state.home = { x = spec.position.x, z = spec.position.z }
  state.speed = speed
  state.wander = spec.wander or 2
  state.rest_until = task.time() + 1.5 + wander_random:float() * 2
  return entity
end

--- Chase the player when close, otherwise wander (fe.field calls this).
function encounter.update_symbols(world, player)
  local now = task.time()
  for _, symbol in ipairs(world:query({ tag = "encounter_symbol" })) do
    local props, state = symbol:props(), symbol:data()
    local towards = world:wrap_delta(symbol.position, player.position)
    local dx, dz = towards.x, towards.z
    local distance_to = math.sqrt(dx * dx + dz * dz)
    local speed = state.speed or 1.6
    if distance_to < props.sight and distance_to > 0.1 then
      state.chasing = true
      symbol:drive({ x = dx / distance_to * speed, z = dz / distance_to * speed })
    elseif state.home then
      if state.chasing then
        state.chasing = nil
        state.rest_until = now
      end
      if not symbol:moving() and now >= state.rest_until then
        local angle = wander_random:float() * math.pi * 2
        local radius = wander_random:float() * state.wander
        symbol:move_to({ x = state.home.x + math.cos(angle) * radius, z = state.home.z + math.sin(angle) * radius })
        state.rest_until = now + 1.5 + wander_random:float() * 2
      end
    end
  end
end

encounter.reset()
return encounter

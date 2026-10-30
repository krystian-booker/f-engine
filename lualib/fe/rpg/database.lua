--- Default schemas and loading for RPG data tables.
---
--- `database.load()` reads the standard files when they exist:
---   data/classes.json, data/actors.json, data/skills.json, data/items.json,
---   data/states.json, data/enemies.json, data/troops.json, data/shops.json,
---   data/encounters.json
--- (`fe.quest.load()` reads data/quests.json.)
--- Schemas are open (extra fields are kept), so games can add their own
--- fields (shield points, weapon types, conviction values) and read them in
--- Lua. `schema.extend(name, fields)` adds validation for them.
---
--- Effect records (skills' `effects`, items' `use.effects`, states'
--- `on_turn`) are checked by `fe.effects.validate` once every table has
--- loaded: each type needs a handler, and the built-in handlers' schemas
--- check their fields (`effects.schema(type, fields)` registers one for a
--- game's own handler). A reloaded table is checked again and its problems
--- logged.
local data = require("fe.data")
local effects = require("fe.effects")
local schema = require("fe.schema")
require("fe.encounter") -- defines the encounter_table schema

local database = {}

local modifier = { record = {
  stat = "string",
  add = { type = "number", optional = true },
  mul = { type = "number", optional = true },
  set = { type = "number", optional = true },
  priority = { type = "number", optional = true, doc = "Which `set` wins when several apply (highest)" },
} }
local effect = { type = "table", doc = "An effect record: {type, ...}; see fe.effects for each type's fields" }
local area = { record = {
  shape = { type = { enum = { "diamond", "square", "cross", "line", "cone", "ring" } }, default = "diamond" },
  min = { type = "integer", default = 0 },
  max = { type = "integer", default = 0 },
  height = { type = "number", optional = true, doc = "Max height difference reached" },
} }
local resist = { type = { map = "number" }, optional = true,
  doc = "Damage multipliers by element, damage type or state:<id> (state chance)" }
local direction = { record = { x = "integer", z = "integer" } }

schema.define("class", {
  id = "string",
  name = "string",
  growth = { type = { map = "number" }, default = {}, doc = "Stat gain per level" },
  curves = { type = { map = { list = "number" } }, optional = true, doc = "Explicit stat value per level" },
  skills = { type = { list = { record = {
    skill = { type = { ref = "skills" } },
    level = { type = "integer", optional = true },
    jp = { type = "integer", optional = true, doc = "Cost to learn with job points" },
  } } }, default = {} },
  equip = { type = { list = "string" }, default = {}, doc = "Equipment types this class can use" },
  commands = { type = { list = "string" }, optional = true, doc = "Battle commands" },
  passives = { type = { list = { ref = "skills" } }, default = {}, doc = "Always-active passive skills" },
  support_slots = { type = "integer", min = 0, optional = true,
    doc = "Slots for equipped support abilities (skills with support = true)" },
  modifiers = { type = { list = modifier }, default = {} },
}, { doc = "A class or job." })

schema.define("actor", {
  id = "string",
  name = "string",
  class = { type = { ref = "classes" } },
  level = { type = "integer", min = 1, default = 1 },
  stats = { type = { map = "number" }, doc = "Base stats at level 1" },
  sprite = { type = "string", optional = true },
  battle_sprite = { type = "string", optional = true },
  portrait = { type = "string", optional = true },
  skills = { type = { list = { ref = "skills" } }, default = {} },
  equipment = { type = { map = "string" }, default = {} },
  slots = { type = { list = "string" }, optional = true, doc = "Equipment slots (default: database.slots)" },
  support_slots = { type = "integer", min = 0, optional = true, doc = "Support ability slots (overrides the class)" },
}, { doc = "A party member." })

schema.define("skill", {
  id = "string",
  name = "string",
  description = { type = "string", default = "" },
  type = { type = "string", default = "skill", doc = "Groups skills into battle commands (magic, art...)" },
  cost = { type = { map = "number" }, default = {}, doc = "Resource costs, e.g. {mp = 4}" },
  target = { type = { enum = { "enemy", "all_enemies", "ally", "all_allies", "self", "dead_ally", "any",
    "random_enemy", "everyone", "cell" } }, default = "enemy" },
  hits = { type = "integer", min = 1, default = 1 },
  effects = { type = { list = effect }, default = {} },
  element = { type = "string", optional = true },
  damage_type = { type = "string", optional = true, doc = "Weapon/damage category (sword, fire...)" },
  uses_weapon = { type = "boolean", default = false,
    doc = "Take the element and damage type from the user's weapon when the skill has none" },
  always_hit = { type = "boolean", default = false },
  accuracy = { type = "number", optional = true, doc = "Base hit chance in percent (100 by default)" },
  power = { type = "number", optional = true, doc = "Default power for effects without their own" },
  timeline = { type = "string", optional = true },
  range = { type = area, optional = true, doc = "Tactical reach" },
  area = { type = area, optional = true, doc = "Tactical area of effect" },
  los = { type = "boolean", default = false, doc = "Tactics: needs a clear line of sight to the target" },
  scope = { type = { enum = { "battle", "field", "both" } }, default = "battle" },
  passive = { type = "boolean", default = false },
  support = { type = "boolean", default = false,
    doc = "A passive that must be equipped in a support slot when the actor has slots" },
  modifiers = { type = { list = modifier }, default = {}, doc = "Passive stat modifiers" },
  resist = resist,
  priority = { type = "number", default = 0 },
  tags = { type = { list = "string" }, default = {} },
}, { doc = "A skill, spell, art or passive ability." })

schema.define("item", {
  id = "string",
  name = "string",
  description = { type = "string", default = "" },
  kind = { type = { enum = { "consumable", "equipment", "key", "material" } }, default = "consumable" },
  price = { type = "integer", min = 0, default = 0 },
  icon = { type = "string", optional = true },
  stack = { type = "integer", min = 1, default = 99 },
  use = { type = { record = {
    target = { type = "string", default = "ally" },
    effects = { type = { list = effect }, default = {} },
    scope = { type = { enum = { "battle", "field", "both" } }, default = "both" },
    timeline = { type = "string", optional = true },
    consume = { type = "boolean", default = true },
  } }, optional = true },
  equip = { type = { record = {
    slot = "string",
    type = { type = "string", optional = true },
    modifiers = { type = { list = modifier }, default = {} },
    skills = { type = { list = { ref = "skills" } }, default = {} },
    element = { type = "string", optional = true },
    damage_type = { type = "string", optional = true },
    resist = resist,
  } }, optional = true },
}, { doc = "An item, piece of equipment or key item." })

schema.define("state", {
  id = "string",
  name = "string",
  icon = { type = "string", optional = true },
  turns = { type = "integer", min = 0, default = 3, doc = "0 lasts until removed" },
  modifiers = { type = { list = modifier }, default = {} },
  prevents_action = { type = "boolean", default = false },
  ends_on_damage = { type = "boolean", default = false },
  on_turn = { type = { list = effect }, default = {}, doc = "Effects at the start of the bearer's turn" },
  removed_by = { type = { list = { enum = { "battle_end", "rest", "death" } } }, default = { "battle_end" } },
  stacking = { type = { enum = { "refresh", "extend", "ignore" } }, default = "refresh" },
  tags = { type = { list = "string" }, default = {} },
}, { doc = "A status effect, buff or debuff." })

schema.define("enemy", {
  id = "string",
  name = "string",
  sprite = { type = "string", optional = true },
  level = { type = "integer", min = 1, default = 1 },
  stats = { type = { map = "number" } },
  skills = { type = { list = { ref = "skills" } }, default = {} },
  ai = { type = "string", optional = true, doc = "AI function: module.function" },
  actions = { type = "any", optional = true,
    doc = "Actions per turn: an integer, or a function path module.function(battle, battler) -> integer" },
  rewards = { type = { record = {
    exp = { type = "number", default = 0 },
    money = { type = "number", default = 0 },
    jp = { type = "number", default = 0 },
    drops = { type = { list = { record = { item = { type = { ref = "items" } }, chance = { type = "number", default = 1 } } } }, default = {} },
  } }, default = {} },
  resist = { type = { map = "number" }, default = {}, doc = "Damage multipliers by element, damage type or state:<id>" },
  scale = { type = "number", default = 1 },
  shield = { type = "integer", min = 0, optional = true, doc = "Shield points (fe.battle.break)" },
  weak = { type = { list = "string" }, default = {}, doc = "Weaknesses: elements or damage types (fe.battle.break)" },
  obstacle = { type = "boolean", default = false,
    doc = "A destructible that never acts (tactics: blocks movement and line of sight)" },
  destroyed = { type = "table", optional = true,
    doc = "Terrain reaction on its cell when an obstacle is destroyed: {add, remove, walkable}" },
}, { doc = "An enemy, obstacle or battle companion." })

schema.define("troop", {
  id = "string",
  name = { type = "string", optional = true },
  members = { type = { list = { record = {
    enemy = { type = { ref = "enemies" } },
    id = { type = "string", optional = true, doc = "Unit id for objectives and scripts (tactics)" },
    name = { type = "string", optional = true, doc = "Display name overriding the enemy's" },
    position = { type = { record = {
      slot = { type = "integer", min = 1, optional = true, doc = "Formation slot (side view)" },
      x = { type = "integer", optional = true, doc = "Cell (tactics)" },
      z = { type = "integer", optional = true },
      layer = { type = "integer", optional = true, doc = "Grid layer of the cell (0 by default)" },
    } }, optional = true, doc = "Formation slot or {x, z, layer?} cell" },
    facing = { type = direction, optional = true, doc = "Starting facing (tactics)" },
  } } } },
  stage = { type = "string", optional = true },
  music = { type = "string", optional = true },
  escapable = { type = "boolean", default = true },
  script = { type = "string", optional = true, doc = "Module with battle hooks for this fight" },
}, { doc = "An enemy group." })

schema.define("shop", {
  id = "string",
  name = { type = "string", optional = true },
  goods = { type = { list = { ref = "items" } } },
  buy_rate = { type = "number", default = 1 },
  sell_rate = { type = "number", default = 0.5 },
}, { doc = "A shop's stock." })

--- Equipment slots used when an actor does not list its own.
database.slots = { "weapon", "shield", "head", "body", "accessory" }

local files = {
  { "classes", "data/classes.json", "class" },
  { "skills", "data/skills.json", "skill" },
  { "items", "data/items.json", "item" },
  { "states", "data/states.json", "state" },
  { "actors", "data/actors.json", "actor" },
  { "enemies", "data/enemies.json", "enemy" },
  { "troops", "data/troops.json", "troop" },
  { "shops", "data/shops.json", "shop" },
  { "encounters", "data/encounters.json", "encounter_table" },
}

-- Where effect lists live in each table's records.
local effect_lists = {
  skills = function(record) return record.effects end,
  items = function(record) return record.use and record.use.effects end,
  states = function(record) return record.on_turn end,
}

--- Check every effect record of a loaded table (skills, items or states)
--- with fe.effects.validate; returns the list of problems.
---@param name string
---@return string[]
function database.validate_effects(name)
  local problems = {}
  local list_of = effect_lists[name]
  if not list_of or not data.has_table(name) then return problems end
  for _, record in ipairs(data.list(name)) do
    for i, effect in ipairs(list_of(record) or {}) do
      for _, problem in ipairs(effects.validate(effect, name .. "." .. record.id .. ".effects[" .. i .. "]")) do
        problems[#problems + 1] = problem
      end
    end
  end
  return problems
end

local loading = false

data.changed:connect(function(name)
  if loading or not effect_lists[name] then return end
  local problems = database.validate_effects(name)
  if #problems > 0 then fe.log.error("Effect records of " .. name .. " are broken:\n" .. table.concat(problems, "\n")) end
end, 0, "fe.rpg.database.effects")

--- Load every standard table that exists, then check references and
--- effect records.
---@param extra? {[1]: string, [2]: string, [3]: string?}[] more tables {name, file, schema}
function database.load(extra)
  local all = {}
  for _, f in ipairs(files) do all[#all + 1] = f end
  for _, f in ipairs(extra or {}) do all[#all + 1] = f end
  loading = true
  local ok, err = pcall(function()
    for _, f in ipairs(all) do
      if fe.fs.exists(f[2]) then
        data.load(f[1], f[2], f[3])
      end
    end
  end)
  loading = false
  if not ok then error(err, 0) end
  local problems
  ok, problems = data.check_references()
  if not ok then
    error("Data references are broken:\n" .. table.concat(problems, "\n"), 0)
  end
  problems = {}
  for name in pairs(effect_lists) do
    for _, problem in ipairs(database.validate_effects(name)) do problems[#problems + 1] = problem end
  end
  if #problems > 0 then
    table.sort(problems)
    error("Effect records are broken:\n" .. table.concat(problems, "\n"), 0)
  end
end

return database

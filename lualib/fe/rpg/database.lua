--- Default schemas and loading for RPG data tables.
---
--- `database.load()` reads the standard files when they exist:
---   data/classes.json, data/actors.json, data/skills.json, data/items.json,
---   data/states.json, data/enemies.json, data/troops.json, data/shops.json,
---   data/encounters.json, data/quests.json
--- Schemas are open (extra fields are kept), so games can add their own
--- fields (shield points, weapon types, conviction values) and read them in
--- Lua. `schema.extend(name, fields)` adds validation for them.
local data = require("fe.data")
local schema = require("fe.schema")
require("fe.encounter") -- defines the encounter_table schema

local database = {}

local modifier = { record = {
  stat = "string",
  add = { type = "number", optional = true },
  mul = { type = "number", optional = true },
  set = { type = "number", optional = true },
} }
local effect = "table" -- validated by fe.effects handlers at use time
local area = { record = {
  shape = { type = { enum = { "diamond", "square", "cross", "line", "cone", "ring" } }, default = "diamond" },
  min = { type = "integer", default = 0 },
  max = { type = "integer", default = 0 },
  height = { type = "number", optional = true, doc = "Max height difference reached" },
} }

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
  passives = { type = { list = { ref = "skills" } }, default = {} },
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
  timeline = { type = "string", optional = true },
  range = { type = area, optional = true, doc = "Tactical reach" },
  area = { type = area, optional = true, doc = "Tactical area of effect" },
  scope = { type = { enum = { "battle", "field", "both" } }, default = "battle" },
  passive = { type = "boolean", default = false },
  modifiers = { type = { list = modifier }, default = {}, doc = "Passive stat modifiers" },
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
  rewards = { type = { record = {
    exp = { type = "number", default = 0 },
    money = { type = "number", default = 0 },
    jp = { type = "number", default = 0 },
    drops = { type = { list = { record = { item = { type = { ref = "items" } }, chance = { type = "number", default = 1 } } } }, default = {} },
  } }, default = {} },
  resist = { type = { map = "number" }, default = {}, doc = "Damage multipliers by element or damage type" },
  scale = { type = "number", default = 1 },
}, { doc = "An enemy." })

schema.define("troop", {
  id = "string",
  name = { type = "string", optional = true },
  members = { type = { list = { record = {
    enemy = { type = { ref = "enemies" } },
    position = { type = "table", optional = true, doc = "Formation slot or {x, z}" },
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

--- Load every standard table that exists, then check references.
---@param extra? {[1]: string, [2]: string, [3]: string?}[] more tables {name, file, schema}
function database.load(extra)
  local all = {}
  for _, f in ipairs(files) do all[#all + 1] = f end
  for _, f in ipairs(extra or {}) do all[#all + 1] = f end
  for _, f in ipairs(all) do
    if fe.fs.exists(f[2]) then
      data.load(f[1], f[2], f[3])
    end
  end
  local ok, problems = data.check_references()
  if not ok then
    error("Data references are broken:\n" .. table.concat(problems, "\n"), 0)
  end
end

return database

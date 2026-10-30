--- Party members. Records live in the "actors" state section so they are
--- saved; the Actor object wraps a record with its data definition.
local Combatant = require("fe.rpg.combatant")
local class = require("fe.class")
local data = require("fe.data")
local database = require("fe.rpg.database")
local signal = require("fe.signal")
local state = require("fe.state")
local tablex = require("fe.tablex")

local records = state.section("actors")

---@class Actor
local Actor = class("Actor")
Combatant.mixin(Actor)

Actor.leveled_up = signal.new() -- (actor, new_level, learned_skill_ids)
Actor.max_level = 99
--- The default experience curve: base * (level - 1) ^ exponent + linear * (level - 1).
Actor.exp_curve = { base = 18, exponent = 2.35, linear = 12 }

--- Total experience needed to reach `level`. Replace for a custom curve.
function Actor.exp_for_level(level)
  if level <= 1 then return 0 end
  local curve = Actor.exp_curve
  return math.floor(curve.base * (level - 1) ^ curve.exponent + curve.linear * (level - 1))
end

local function new_record(def)
  return {
    level = def.level, exp = Actor.exp_for_level(def.level), class = def.class,
    jp = {}, resources = {}, equipment = tablex.copy(def.equipment), learned = {}, states = {}, custom = {},
    supports = {},
  }
end

--- Wrap (creating the record on first use) the actor with this id.
function Actor:init(id)
  self.id = id
  self.def = data.get("actors", id)
  local fresh = records[id] == nil
  if fresh then records[id] = new_record(self.def) end
  self.record = records[id]
  self.record.supports = self.record.supports or {}
  self.resources = self.record.resources
  self.state_turns = self.record.states
  if fresh then self:init_resources(true) end
end

--- Whether this object still wraps the actor's saved record (false after
--- a load or reset replaced it).
function Actor:current()
  return records[self.id] == self.record
end

function Actor:name()
  return self.record.custom.name or self.def.name
end

function Actor:rename(name)
  self.record.custom.name = name
end

function Actor:level() return self.record.level end
function Actor:exp() return self.record.exp end
function Actor:class() return data.get("classes", self.record.class) end
function Actor:secondary() return self.record.secondary and data.get("classes", self.record.secondary) end
function Actor:sprite() return self.def.sprite end
function Actor:battle_sprite() return self.def.battle_sprite or self.def.sprite end
function Actor:portrait() return self.def.portrait end

function Actor:base_stats()
  local cls = self:class()
  local level = self.record.level
  local out = {}
  for stat, value in pairs(self.def.stats) do
    local curve = cls.curves and cls.curves[stat]
    if curve then
      out[stat] = curve[math.min(level, #curve)]
    else
      out[stat] = value + (cls.growth[stat] or 0) * (level - 1)
    end
  end
  return out
end

--- Equipment slots for this actor.
function Actor:slots()
  return self.def.slots or database.slots
end

function Actor:equipped(slot)
  return self.record.equipment[slot]
end

local function each_equipment(self, fn)
  for _, slot in ipairs(self:slots()) do
    local id = self.record.equipment[slot]
    if id then fn(data.get("items", id), slot) end
  end
end

--- Every skill id the actor can use or benefits from (sorted by data order).
local function known(self)
  local ids = {}
  local function add(id) ids[id] = true end
  for _, id in ipairs(self.def.skills) do add(id) end
  for _, id in ipairs(self.record.learned) do add(id) end
  for _, cls in ipairs({ self:class(), self:secondary() }) do
    if cls then
      for _, entry in ipairs(cls.skills) do
        if entry.level and entry.level <= self.record.level then add(entry.skill) end
      end
      for _, id in ipairs(cls.passives) do add(id) end
    end
  end
  each_equipment(self, function(item) for _, id in ipairs(item.equip.skills) do add(id) end end)
  local out = {}
  for _, skill in ipairs(data.list("skills")) do
    if ids[skill.id] then out[#out + 1] = skill end
  end
  return out
end

--- Active (usable) skills, optionally of one type.
function Actor:skills(skill_type)
  return tablex.filter(known(self), function(s)
    return not s.passive and (not skill_type or s.type == skill_type)
  end)
end

--- Number of support ability slots: the actor's own setting, else its
--- class's; nil when the game does not use slots (every passive applies).
function Actor:support_slots()
  return self.def.support_slots or self:class().support_slots
end

--- Equipped support abilities by slot (false = empty).
function Actor:supports()
  return self.record.supports
end

--- Put a known support ability in a slot (nil empties it). Returns true on
--- success. An ability occupies at most one slot.
function Actor:equip_support(slot, skill_id)
  local slots = self:support_slots() or 0
  if slot < 1 or slot > slots then return false end
  if skill_id then
    local skill = data.find("skills", skill_id)
    if not skill or not skill.support or not self:knows(skill_id) then return false end
    for i = 1, slots do
      if self.record.supports[i] == skill_id then self.record.supports[i] = false end
    end
  end
  self.record.supports[slot] = skill_id or false
  self:invalidate()
  return true
end

--- Active passive skills. Class passives always apply. Support abilities
--- (skills with `support = true`) apply only while equipped when the actor
--- has support slots; without slots every known passive applies.
function Actor:passives()
  local slots = self:support_slots()
  local innate = {}
  for _, cls in ipairs({ self:class(), self:secondary() }) do
    if cls then for _, id in ipairs(cls.passives) do innate[id] = true end end
  end
  local equipped = {}
  for i = 1, slots or 0 do
    local id = self.record.supports[i]
    if id then equipped[id] = true end
  end
  return tablex.filter(known(self), function(s)
    if not s.passive then return false end
    if slots == nil or not s.support or innate[s.id] then return true end
    return equipped[s.id] == true
  end)
end

function Actor:knows(skill_id)
  return tablex.find(known(self), function(s) return s.id == skill_id end) ~= nil
end

--- The actor's own modifiers: the `modifiers` list in `actor:custom()`
--- (saved with the game), for weapon upgrades, stat choices and the like.
--- Set it with Actor:set_custom_modifiers so cached stats refresh.
function Actor:custom_modifiers()
  return self.record.custom.modifiers or {}
end

--- Replace the actor's own modifiers ({stat, add | mul | set, priority?} list).
function Actor:set_custom_modifiers(list)
  self.record.custom.modifiers = list
  self:invalidate()
end

--- Stat modifiers from the class, the secondary class, equipment, the
--- actor's own (Actor:custom_modifiers) and active passives (in that order).
function Actor:modifiers()
  local out = {}
  local function add_all(list) for _, m in ipairs(list or {}) do out[#out + 1] = m end end
  add_all(self:class().modifiers)
  local secondary = self:secondary()
  if secondary then add_all(secondary.modifiers) end
  each_equipment(self, function(item) add_all(item.equip.modifiers) end)
  add_all(self:custom_modifiers())
  for _, skill in ipairs(self:passives()) do add_all(skill.modifiers) end
  return out
end

function Actor:resistances()
  local out = {}
  for _, skill in ipairs(self:passives()) do
    if skill.resist then out[#out + 1] = skill.resist end
  end
  each_equipment(self, function(item)
    if item.equip.resist then out[#out + 1] = item.equip.resist end
  end)
  return out
end

--- Element and damage type of the equipped weapon (if any).
function Actor:weapon()
  local id = self.record.equipment.weapon
  return id and data.get("items", id)
end

--- Whether an item can go in a slot.
function Actor:can_equip(item_id, slot)
  local item = data.find("items", item_id)
  if not item or not item.equip or item.equip.slot ~= slot then return false end
  if not tablex.contains(self:slots(), slot) then return false end
  local allowed = {}
  for _, cls in ipairs({ self:class(), self:secondary() }) do
    if cls then for _, t in ipairs(cls.equip) do allowed[t] = true end end
  end
  return item.equip.type == nil or allowed[item.equip.type] == true
end

--- Equip from the inventory (nil unequips). Returns true on success, else
--- false and the refusal: "cannot_equip" (wrong slot, type or class),
--- "not_held" (none in the inventory) or "inventory_full" (the item being
--- taken off would not fit back; nothing changes, it stays equipped).
--- Equipping what is already in the slot changes nothing and returns true.
---@return boolean, string?
function Actor:equip(slot, item_id)
  local inventory = require("fe.rpg.inventory")
  local old = self.record.equipment[slot]
  if old and old == item_id then return true end
  if item_id and not self:can_equip(item_id, slot) then return false, "cannot_equip" end
  if item_id and inventory.count(item_id) < 1 then return false, "not_held" end
  if old and inventory.count(old) >= data.get("items", old).stack then
    return false, "inventory_full"
  end
  if old then inventory.add(old, 1) end
  if item_id then inventory.remove(item_id, 1) end
  self.record.equipment[slot] = item_id
  self:invalidate()
  for _, def in ipairs(require("fe.rpg.stats").resource_list) do
    self:set_resource(def.id, self:resource(def.id))
  end
  return true
end

--- Stats if `item_id` were equipped in `slot` (equipment comparison UI).
function Actor:preview_equip(slot, item_id)
  local old = self.record.equipment[slot]
  self.record.equipment[slot] = item_id
  local result = self:compute_stats()
  self.record.equipment[slot] = old
  return result
end

function Actor:learn(skill_id)
  data.get("skills", skill_id)
  if not tablex.contains(self.record.learned, skill_id) then
    self.record.learned[#self.record.learned + 1] = skill_id
    self:invalidate()
    return true
  end
  return false
end

function Actor:set_class(class_id)
  data.get("classes", class_id)
  self.record.class = class_id
  self:invalidate()
  self:init_resources(false)
end

--- Secondary class: its skills, passives, stat modifiers and equipment
--- types are added.
function Actor:set_secondary(class_id)
  if class_id then data.get("classes", class_id) end
  self.record.secondary = class_id
  self:invalidate()
end

--- Job points per class.
function Actor:jp(class_id)
  return self.record.jp[class_id or self.record.class] or 0
end

function Actor:gain_jp(amount, class_id)
  class_id = class_id or self.record.class
  self.record.jp[class_id] = (self.record.jp[class_id] or 0) + amount
end

--- Learn a class skill that costs job points.
function Actor:learn_with_jp(skill_id, class_id)
  class_id = class_id or self.record.class
  local entry = tablex.find(data.get("classes", class_id).skills, function(e) return e.skill == skill_id end)
  if not entry or not entry.jp or self:knows(skill_id) or self:jp(class_id) < entry.jp then return false end
  self.record.jp[class_id] = self:jp(class_id) - entry.jp
  return self:learn(skill_id)
end

--- Add experience; returns a list of {level = n, skills = {...}} gained.
function Actor:gain_exp(amount)
  local gains = {}
  self.record.exp = self.record.exp + amount
  while self.record.level < Actor.max_level and self.record.exp >= Actor.exp_for_level(self.record.level + 1) do
    local before = self:skills()
    local max_before = {}
    for _, def in ipairs(require("fe.rpg.stats").resource_list) do max_before[def.id] = self:max_resource(def.id) end
    self.record.level = self.record.level + 1
    self:invalidate()
    -- Growth raises current values too, but never revives a knocked-out actor.
    if self:alive() then
      for _, def in ipairs(require("fe.rpg.stats").resource_list) do
        self:set_resource(def.id, self:resource(def.id) + (self:max_resource(def.id) - max_before[def.id]))
      end
    end
    local learned = {}
    for _, skill in ipairs(self:skills()) do
      if not tablex.find(before, function(s) return s.id == skill.id end) then learned[#learned + 1] = skill.id end
    end
    gains[#gains + 1] = { level = self.record.level, skills = learned }
    Actor.leveled_up:emit(self, self.record.level, learned)
  end
  return gains
end

--- Experience still needed for the next level.
function Actor:exp_to_next()
  if self.record.level >= Actor.max_level then return 0 end
  return Actor.exp_for_level(self.record.level + 1) - self.record.exp
end

--- Full recovery (inns, save points).
function Actor:recover()
  self:clear_states("rest")
  self:init_resources(true)
end

--- Battle commands: class commands or the defaults. A secondary class's
--- extra commands go, in order, before the primary list's first "defend"
--- or "flee" (at the end when it has neither).
function Actor:commands()
  local commands = tablex.copy(self:class().commands or { "attack", "skill", "item", "defend", "flee" })
  local secondary = self:secondary()
  if secondary and secondary.commands then
    local at = #commands + 1
    for i, c in ipairs(commands) do
      if c == "defend" or c == "flee" then at = i break end
    end
    for _, c in ipairs(secondary.commands) do
      if not tablex.contains(commands, c) then
        table.insert(commands, at, c)
        at = at + 1
      end
    end
  end
  return commands
end

--- Free-form per-actor data saved with the game (affinity, conviction...).
function Actor:custom()
  return self.record.custom
end

--- Forget an actor's record (new game).
function Actor.reset(id)
  records[id] = nil
end

return Actor

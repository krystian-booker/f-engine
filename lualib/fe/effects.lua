--- The effect pipeline used by skills and items in battle and on the field.
---
--- A skill lists effects such as:
--- ```json
--- [{"type": "damage", "formula": "physical", "power": 120},
---  {"type": "add_state", "state": "poison", "chance": 0.4}]
--- ```
--- `effects.resolve` runs every effect on every target through phases.
--- Games and battle kits change the rules with hooks instead of editing
--- this file:
--- ```lua
--- effects.hook("modify", function(ctx)        -- a "stagger" mechanic
---   if ctx.target.data.staggered then ctx.multiplier = ctx.multiplier * 2 end
--- end)
--- effects.formula("physical", function(ctx) ... end)   -- replace a formula
--- effects.handler("steal", function(ctx) ... end)        -- a new effect type
--- ```
--- Phases, in order, per effect and target:
---   before    may set ctx.cancelled
---   hit       accuracy: sets ctx.hit_chance, and ctx.missed when rolled
---   calculate formula -> ctx.amount (damage and heal effects)
---   modify    multipliers (affinity, critical, guard, terrain...)
---   apply     last hooks before the change; then the effect's handler
---             changes the target (neither runs for previews, so apply
---             hooks may have side effects)
---   after     sees ctx.result (not run for previews)
--- Any hook may set ctx.cancelled; later phases and the handler are then
--- skipped and the result is {cancelled = true} (presenters show it as
--- nullified; it has no amount). Within a phase hooks run by priority
--- (higher first), then in registration order.
---
--- Hooks are global (`effects.hook`, idempotent when given an id, so module
--- reloads replace rather than duplicate them) or belong to one battle
--- (`battle:effect_hook`), merged by priority while that battle resolves.
--- Reloading this module re-registers its own hooks by id and keeps every
--- other module's.
--- Effects resolved with `environmental = true` (state ticks, terrain
--- hazards) always hit, never crit and skip positional bonuses.
---
--- Each handler may register a schema for its records
--- (`effects.schema(type, fields)`, a record spec as fe.schema takes it);
--- `effects.validate(effect, path)` checks a record against it, which
--- fe.rpg.database does for every skill, item and state when the tables
--- load. Records of a type with a handler but no schema pass; a type
--- without a handler is an error.
local data = require("fe.data")
local schema = require("fe.schema")
local signal = require("fe.signal")
local tablex = require("fe.tablex")

local effects = {
  handlers = {},
  formulas = {},
  schemas = {},
  providers = {},
  hooks = { before = {}, hit = {}, calculate = {}, modify = {}, apply = {}, after = {} },
  --- Emitted with each result table after it is applied.
  applied = signal.new(),
  settings = {
    variance = 0.1,
    critical_multiplier = 1.5,
    minimum_damage = 1,
    --- Caps on one hit's damage and healing, applied after variance and
    --- every multiplier (Octopath's 9999). nil: no cap.
    max_damage = nil,
    max_heal = nil,
  },
}

--- Insert a hook entry into a phase list (higher priority first, then
--- registration order); an entry with the same id is replaced. Returns a
--- remover.
function effects.insert_hook(list, fn, priority, id)
  if id then
    for i, other in ipairs(list) do
      if other.id == id then table.remove(list, i) break end
    end
  end
  local entry = { fn = fn, priority = priority or 0, id = id }
  local index = #list + 1
  for i, other in ipairs(list) do
    if entry.priority > other.priority then index = i break end
  end
  table.insert(list, index, entry)
  return function()
    for i, other in ipairs(list) do
      if other == entry then table.remove(list, i) return end
    end
  end
end

--- Add a global phase hook; higher priority runs first. With `id`,
--- registering again replaces the earlier hook (safe across hot reloads).
--- Returns a remover.
function effects.hook(phase, fn, priority, id)
  local list = effects.hooks[phase]
  assert(list, "unknown effect phase '" .. tostring(phase) .. "'")
  return effects.insert_hook(list, fn, priority, id)
end

function effects.handler(type_name, fn) effects.handlers[type_name] = fn end
function effects.formula(name, fn) effects.formulas[name] = fn end

--- Fields an effect record of `type_name` may carry, as an fe.schema
--- record (open: extra fields are kept). `{ref = "states"}` fields are
--- checked against the loaded tables by effects.validate.
function effects.schema(type_name, fields) effects.schemas[type_name] = fields end

--- The module that registers the handler for `type_name` when it is not a
--- built-in one: effects.validate requires it on demand, so data tables
--- can be checked before the kit providing the type has been loaded:
--- `effects.provider("steal", "mygame.thief")`.
function effects.provider(type_name, module_name) effects.providers[type_name] = module_name end

--- Check an effect record: its type must have a handler (loading its
--- provider module if one is registered), and its fields must fit the
--- handler's schema when it registered one. Returns the list of problems
--- (empty when fine); defaults are filled in place.
---@param effect table
---@param path? string
---@return string[]
function effects.validate(effect, path)
  path = path or "effect"
  if type(effect) ~= "table" then return { path .. ": expected a table" } end
  if type(effect.type) ~= "string" then return { path .. ".type: missing" } end
  if not effects.handlers[effect.type] and effects.providers[effect.type] then require(effects.providers[effect.type]) end
  if not effects.handlers[effect.type] then return { path .. ".type: no effect handler for '" .. effect.type .. "'" } end
  local fields = effects.schemas[effect.type]
  if not fields then return {} end
  local _, errors, refs = schema.validate({ record = fields }, effect, path)
  for _, ref in ipairs(refs) do
    if data.has_table(ref.table) and not data.find(ref.table, ref.id) then
      errors[#errors + 1] = ref.path .. ": unknown " .. ref.table .. " '" .. ref.id .. "'"
    end
  end
  return errors
end

-- Fields shared by the amount effects (damage and heal).
local amount_fields = {
  formula = { type = "any", optional = true, doc = "Formula name, module.function or a number" },
  power = { type = "number", optional = true },
  resource = { type = "string", optional = true, doc = "Resource affected (hp by default)" },
  variance = { type = "number", min = 0, optional = true },
  element = { type = "string", optional = true },
  damage_type = { type = "string", optional = true },
  accuracy = { type = "number", optional = true, doc = "Base hit chance in percent" },
  always_hit = { type = "boolean", optional = true },
}

-- Global hooks merged with the battle's own (by priority; globals first on ties).
local function phase_hooks(phase, battle)
  local global = effects.hooks[phase]
  local own = battle and battle.effect_hooks and battle.effect_hooks[phase]
  if not own or #own == 0 then return global end
  local merged, i, j = {}, 1, 1
  while i <= #global or j <= #own do
    local a, b = global[i], own[j]
    if b == nil or (a ~= nil and a.priority >= b.priority) then
      merged[#merged + 1] = a
      i = i + 1
    else
      merged[#merged + 1] = b
      j = j + 1
    end
  end
  return merged
end

local function run_hooks(phase, ctx)
  if ctx.cancelled then return end
  for _, entry in ipairs(phase_hooks(phase, ctx.battle)) do
    entry.fn(ctx)
    if ctx.cancelled then return end
  end
end

local function power(ctx)
  return ctx.effect.power or ctx.source.power or 100
end

-- Default formulas. ctx.user/ctx.target are combatants (fe.rpg.combatant).
effects.formula("physical", function(ctx)
  return (ctx.user:stat("attack") * 2 - ctx.target:stat("defense")) * power(ctx) / 100
end)
effects.formula("magical", function(ctx)
  return (ctx.user:stat("magic") * 2 - ctx.target:stat("spirit")) * power(ctx) / 100
end)
effects.formula("heal", function(ctx)
  return (ctx.user:stat("magic") * 1.5 + 10) * power(ctx) / 100
end)
effects.formula("fixed", function(ctx)
  return power(ctx)
end)
--- power% of the target's maximum of the affected resource.
effects.formula("percent", function(ctx)
  return ctx.target:max_resource(ctx.effect.resource or "hp") * power(ctx) / 100
end)

local function formula(ctx, default)
  local name = ctx.effect.formula or default
  if type(name) == "number" then return name end
  local module_name, fn_name = name:match("^(.*)%.([%w_]+)$")
  local fn = module_name and require(module_name)[fn_name] or effects.formulas[name]
  if not fn then error("unknown formula '" .. tostring(name) .. "'") end
  return fn(ctx)
end

local function vary(ctx, amount)
  local variance = ctx.effect.variance or effects.settings.variance
  if variance > 0 and ctx.random then
    amount = amount * (1 + (ctx.random:float() * 2 - 1) * variance)
  end
  return amount
end

--- Chance (0..1) that an effect lands, from the user's accuracy against the
--- target's evasion (percent points). Effects that cannot miss return 1.
function effects.hit_chance(ctx)
  if ctx.effect.always_hit or ctx.source.always_hit or ctx.user == ctx.target or ctx.environmental then return 1 end
  if ctx.effect.type ~= "damage" and ctx.effect.type ~= "add_state" then return 1 end
  local chance = (ctx.effect.accuracy or ctx.source.accuracy or 100) / 100
  chance = chance * (1 + ctx.user:stat("accuracy") / 100) - ctx.target:stat("evasion") / 100
  return tablex.clamp(chance, 0, 1)
end

-- Hit check. Previews and resolves without a random stream report the
-- chance without rolling (and without consuming randomness).
effects.hook("hit", function(ctx)
  ctx.hit_chance = effects.hit_chance(ctx)
  if ctx.preview or not ctx.random or ctx.hit_chance >= 1 then return end
  if ctx.random:float() >= ctx.hit_chance then ctx.missed = true end
end, -100, "fe.effects.hit")

-- Critical hits from the user's critical stat (percent chance).
effects.hook("modify", function(ctx)
  if ctx.effect.type ~= "damage" or ctx.effect.no_critical or ctx.environmental then return end
  ctx.critical_chance = ctx.user:stat("critical") / 100
  if ctx.preview or not ctx.random then return end
  if ctx.random:float() * 100 < ctx.user:stat("critical") then
    ctx.critical = true
    ctx.multiplier = ctx.multiplier * effects.settings.critical_multiplier
  end
end, 100, "fe.effects.critical")

-- Elemental and damage-type affinities.
effects.hook("modify", function(ctx)
  if ctx.effect.type ~= "damage" then return end
  local affinity = ctx.target:resist(ctx.element) * ctx.target:resist(ctx.damage_type)
  ctx.affinity = affinity
  ctx.multiplier = ctx.multiplier * affinity
end, 90, "fe.effects.affinity")

effects.handler("damage", function(ctx)
  local amount = math.floor(math.max(0, (ctx.preview and ctx.amount or vary(ctx, ctx.amount)) * ctx.multiplier) + 0.5)
  if amount < effects.settings.minimum_damage and ctx.multiplier > 0 then amount = effects.settings.minimum_damage end
  if effects.settings.max_damage then amount = math.min(amount, effects.settings.max_damage) end
  local resource = ctx.effect.resource or "hp"
  if ctx.preview then return { amount = amount, resource = resource } end
  local actual = -ctx.target:change_resource(resource, -amount)
  if ctx.effect.drain then
    ctx.user:change_resource(resource, math.floor(actual * ctx.effect.drain))
  end
  local def_states = {}
  for _, id in ipairs(ctx.target:states()) do
    local s = data.find("states", id)
    if s and s.ends_on_damage and actual > 0 then def_states[#def_states + 1] = id end
  end
  for _, id in ipairs(def_states) do ctx.target:remove_state(id) end
  return { amount = actual, resource = resource }
end)

effects.handler("heal", function(ctx)
  local amount = math.floor(math.max(0, (ctx.preview and ctx.amount or vary(ctx, ctx.amount)) * ctx.multiplier) + 0.5)
  if effects.settings.max_heal then amount = math.min(amount, effects.settings.max_heal) end
  local resource = ctx.effect.resource or "hp"
  if ctx.preview then return { amount = amount, resource = resource } end
  if resource == "hp" and not ctx.target:alive() then return { amount = 0, resource = resource } end
  return { amount = ctx.target:change_resource(resource, amount), resource = resource }
end)

--- Restore or drain a resource by a fixed amount or fraction of its max.
effects.handler("resource", function(ctx)
  local resource = ctx.effect.resource or "mp"
  local amount = ctx.effect.amount or 0
  if ctx.effect.fraction then amount = ctx.target:max_resource(resource) * ctx.effect.fraction end
  return { amount = ctx.target:change_resource(resource, math.floor(amount + 0.5)), resource = resource }
end)

effects.handler("add_state", function(ctx)
  if not ctx.target:alive() then return { applied = false } end
  local chance = ctx.effect.chance or 1
  if ctx.random and ctx.random:float() >= chance * ctx.target:resist("state:" .. ctx.effect.state) then
    return { applied = false, state = ctx.effect.state }
  end
  return { applied = ctx.target:add_state(ctx.effect.state, ctx.effect.turns), state = ctx.effect.state }
end)

effects.handler("remove_state", function(ctx)
  if ctx.effect.tag then
    return { removed = ctx.target:remove_states_tagged(ctx.effect.tag) }
  end
  return { removed = ctx.target:remove_state(ctx.effect.state) and { ctx.effect.state } or {} }
end)

--- {"type": "buff", "stat": "attack", "mul": 1.5, "turns": 3, "id"?, "cap"?}:
--- a temporary modifier on a battler (see Battler:buff for stacking).
effects.handler("buff", function(ctx)
  if not ctx.target.buff or not ctx.target:alive() then return { applied = false } end
  local e = ctx.effect
  local entry = ctx.target:buff({ stat = e.stat, add = e.add, mul = e.mul, id = e.id, cap = e.cap }, e.turns)
  return { applied = entry ~= nil, stat = e.stat, stacks = entry and entry.stacks, capped = entry and entry.capped }
end)

effects.handler("revive", function(ctx)
  if ctx.target:alive() then return { amount = 0 } end
  local life = require("fe.rpg.combatant").life
  local amount = math.max(1, math.floor(ctx.target:max_resource(life) * (ctx.effect.fraction or 0.25)))
  ctx.target:set_resource(life, amount)
  return { amount = amount, resource = life, revived = true }
end)

--- {"type": "script", "fn": "module.function"}: the function receives ctx
--- and returns a result table.
effects.handler("script", function(ctx)
  local module_name, fn_name = ctx.effect.fn:match("^(.*)%.([%w_]+)$")
  return require(module_name)[fn_name](ctx) or {}
end)

effects.schema("damage", tablex.merge({
  no_critical = { type = "boolean", optional = true },
  drain = { type = "number", optional = true, doc = "Share of the damage dealt restored to the user" },
}, amount_fields))
effects.schema("heal", amount_fields)
effects.schema("resource", {
  resource = { type = "string", optional = true, doc = "mp by default" },
  amount = { type = "number", optional = true },
  fraction = { type = "number", optional = true, doc = "Share of the resource's maximum instead of amount" },
})
effects.schema("add_state", {
  state = { type = { ref = "states" } },
  chance = { type = "number", min = 0, max = 1, optional = true },
  turns = { type = "integer", min = 0, optional = true },
  accuracy = { type = "number", optional = true },
  always_hit = { type = "boolean", optional = true },
})
effects.schema("remove_state", {
  state = { type = { ref = "states" }, optional = true },
  tag = { type = "string", optional = true, doc = "Remove every state carrying this tag instead" },
})
effects.schema("buff", {
  stat = "string",
  add = { type = "number", optional = true },
  mul = { type = "number", optional = true },
  turns = { type = "integer", min = 1, optional = true },
  id = { type = "string", optional = true },
  cap = { type = "integer", min = 1, optional = true },
})
effects.schema("revive", { fraction = { type = "number", min = 0, max = 1, optional = true } })
effects.schema("script", { fn = { type = "string", doc = "module.function receiving ctx" } })

local calculated = { damage = "physical", heal = "heal" }

--- Resolve a skill or item use.
--- With `preview = true` nothing changes: only the before, hit, calculate
--- and modify phases run; damage and healing report the expected amount
--- and `hit_chance` (forecast panels, AI), other effects their `chance`.
--- Without `random`, effects always land and never crit (deterministic).
--- Every result carries its `effect` record.
---@param spec {user: table, targets: table[], source: table, effects?: table[], battle?: table, random?: RandomStream, hits?: integer, multiplier?: number, preview?: boolean, environmental?: boolean}
---@return table[] results {target, type, effect, missed?, cancelled?, critical?, amount?, ...}
function effects.resolve(spec)
  local results = {}
  local list = spec.effects or spec.source.effects or {}
  local hits = spec.hits or spec.source.hits or 1
  local weapon = spec.source.uses_weapon and spec.user.weapon and spec.user:weapon()
  for _, target in ipairs(spec.targets) do
    for hit = 1, hits do
      for _, effect in ipairs(list) do
        local ctx = {
          user = spec.user, target = target, source = spec.source, effect = effect, battle = spec.battle,
          random = spec.random, hit = hit, multiplier = spec.multiplier or 1, amount = 0, preview = spec.preview,
          environmental = spec.environmental,
          element = effect.element or spec.source.element or (weapon and weapon.equip.element) or nil,
          damage_type = effect.damage_type or spec.source.damage_type
            or (weapon and (weapon.equip.damage_type or weapon.equip.type)) or nil,
        }
        local alive_before = target:alive()
        local handler = effects.handlers[effect.type]
        if not handler then error("unknown effect type '" .. tostring(effect.type) .. "'") end
        run_hooks("before", ctx)
        run_hooks("hit", ctx)
        if not ctx.cancelled and not ctx.missed then
          if calculated[effect.type] then
            ctx.amount = formula(ctx, calculated[effect.type])
            run_hooks("calculate", ctx)
          end
          run_hooks("modify", ctx)
          if not ctx.preview then run_hooks("apply", ctx) end
        end
        local result
        if ctx.cancelled then
          result = { cancelled = true }
        elseif ctx.missed then
          result = { missed = true, hit_chance = ctx.hit_chance }
        else
          if ctx.preview and effect.type ~= "damage" and effect.type ~= "heal" then
            result = { chance = effect.chance or 1 } -- other effects are not simulated
          else
            result = handler(ctx) or {}
          end
          result.hit_chance = ctx.hit_chance
          result.critical = ctx.critical
          result.affinity = ctx.affinity
          result.element = ctx.element
          result.damage_type = ctx.damage_type
        end
        result.target = target
        result.user = spec.user
        result.type = effect.type
        result.effect = effect
        result.hit = hit
        result.knocked_out = alive_before and not target:alive()
        ctx.result = result
        if not spec.preview then
          run_hooks("after", ctx)
          effects.applied:emit(result, ctx)
        end
        results[#results + 1] = result
      end
    end
  end
  return results
end

--- Hot reload: hook lists merge by position, which can mix up entries, so
--- this module's hooks (all registered with ids) are put back by id.
function effects.__reload(live, fresh)
  for phase, list in pairs(fresh.hooks) do
    local own = {}
    for _, entry in ipairs(list) do own[entry.id] = entry end
    local current = live.hooks[phase]
    for i = #current, 1, -1 do
      if current[i].id and own[current[i].id] then table.remove(current, i) end
    end
    for _, entry in ipairs(list) do effects.insert_hook(current, entry.fn, entry.priority, entry.id) end
  end
end

--- Whether `source` (skill or item) does anything useful to `target` out of
--- battle (menus grey out pointless choices).
function effects.useful(source, target)
  for _, effect in ipairs(source.effects or (source.use and source.use.effects) or {}) do
    local resource = effect.resource or "hp"
    if effect.type == "heal" and target:alive() and target:resource(resource) < target:max_resource(resource) then return true end
    if effect.type == "resource" and target:resource(resource) < target:max_resource(resource) then return true end
    if effect.type == "revive" and not target:alive() then return true end
    if effect.type == "remove_state" and (effect.state and target:has_state(effect.state) or effect.tag) then return true end
    if effect.type == "add_state" or effect.type == "script" then return true end
  end
  return false
end

return effects

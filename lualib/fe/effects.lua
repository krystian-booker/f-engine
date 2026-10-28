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
--- effects.hook("modify", function(ctx)        -- a "break" mechanic
---   if ctx.target.broken then ctx.multiplier = ctx.multiplier * 2 end
--- end)
--- effects.formula("physical", function(ctx) ... end)   -- replace a formula
--- effects.handler("steal", function(ctx) ... end)        -- a new effect type
--- ```
--- Phases, in order, per effect and target: before (may set ctx.cancelled),
--- hit (may set ctx.missed), calculate (formula -> ctx.amount), modify
--- (multipliers, critical), apply (handler changes the target), after.
local data = require("fe.data")
local signal = require("fe.signal")

local effects = {
  handlers = {},
  formulas = {},
  hooks = { before = {}, hit = {}, calculate = {}, modify = {}, apply = {}, after = {} },
  --- Emitted with each result table after it is applied.
  applied = signal.new(),
  settings = { variance = 0.1, critical_multiplier = 1.5, minimum_damage = 1 },
}

--- Add a phase hook; higher priority runs first. Returns a remover.
function effects.hook(phase, fn, priority)
  local list = effects.hooks[phase]
  assert(list, "unknown effect phase '" .. tostring(phase) .. "'")
  local entry = { fn = fn, priority = priority or 0 }
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

function effects.handler(type_name, fn) effects.handlers[type_name] = fn end
function effects.formula(name, fn) effects.formulas[name] = fn end

local function run_hooks(phase, ctx)
  for _, entry in ipairs(effects.hooks[phase]) do
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

-- Hit check: user accuracy against target evasion (percent points).
effects.hook("hit", function(ctx)
  if ctx.effect.always_hit or ctx.source.always_hit or ctx.user == ctx.target or not ctx.random then return end
  if ctx.effect.type ~= "damage" and ctx.effect.type ~= "add_state" then return end
  local chance = (ctx.effect.accuracy or ctx.source.accuracy or 100) / 100
  chance = chance * (1 + ctx.user:stat("accuracy") / 100) - ctx.target:stat("evasion") / 100
  ctx.hit_chance = math.max(0, math.min(1, chance))
  if ctx.preview then return end
  if ctx.random:float() >= chance then ctx.missed = true end
end, -100)

-- Critical hits from the user's critical stat (percent chance).
effects.hook("modify", function(ctx)
  if ctx.effect.type ~= "damage" or ctx.effect.no_critical or not ctx.random then return end
  ctx.critical_chance = ctx.user:stat("critical") / 100
  if ctx.preview then return end
  if ctx.random:float() * 100 < ctx.user:stat("critical") then
    ctx.critical = true
    ctx.multiplier = ctx.multiplier * effects.settings.critical_multiplier
  end
end, 100)

-- Elemental and damage-type affinities.
effects.hook("modify", function(ctx)
  if ctx.effect.type ~= "damage" then return end
  local affinity = ctx.target:resist(ctx.element) * ctx.target:resist(ctx.damage_type)
  ctx.affinity = affinity
  ctx.multiplier = ctx.multiplier * affinity
end, 90)

effects.handler("damage", function(ctx)
  local amount = math.floor(math.max(0, (ctx.preview and ctx.amount or vary(ctx, ctx.amount)) * ctx.multiplier) + 0.5)
  if amount < effects.settings.minimum_damage and ctx.multiplier > 0 then amount = effects.settings.minimum_damage end
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

local calculated = { damage = "physical", heal = "heal" }

--- Resolve a skill or item use.
--- With `preview = true` nothing changes: damage and healing report the
--- expected amount and `hit_chance` (forecast panels).
---@param spec {user: table, targets: table[], source: table, effects?: table[], battle?: table, random?: RandomStream, hits?: integer, multiplier?: number, preview?: boolean}
---@return table[] results {target, type, missed?, critical?, amount?, ...}
function effects.resolve(spec)
  local results = {}
  local list = spec.effects or spec.source.effects or {}
  local hits = spec.hits or spec.source.hits or 1
  for _, target in ipairs(spec.targets) do
    for hit = 1, hits do
      for _, effect in ipairs(list) do
        local weapon = spec.user.weapon and spec.user:weapon()
        local ctx = {
          user = spec.user, target = target, source = spec.source, effect = effect, battle = spec.battle,
          random = spec.random, hit = hit, multiplier = spec.multiplier or 1, amount = 0, preview = spec.preview,
          element = effect.element or spec.source.element or (spec.source.uses_weapon and weapon and weapon.equip.element),
          damage_type = effect.damage_type or spec.source.damage_type
            or (spec.source.uses_weapon and weapon and (weapon.equip.damage_type or weapon.equip.type)),
        }
        run_hooks("before", ctx)
        if not ctx.cancelled then
          run_hooks("hit", ctx)
        end
        local result
        if ctx.cancelled then
          result = { cancelled = true }
        elseif ctx.missed then
          result = { missed = true }
        else
          if calculated[effect.type] then
            ctx.amount = formula(ctx, calculated[effect.type])
            run_hooks("calculate", ctx)
          end
          run_hooks("modify", ctx)
          local handler = effects.handlers[effect.type]
          if not handler then error("unknown effect type '" .. tostring(effect.type) .. "'") end
          run_hooks("apply", ctx)
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
        result.hit = hit
        result.knocked_out = not target:alive()
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

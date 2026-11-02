-- fe.battle.core rule hooks: forced_action, targetable, and battlers
-- without turns of their own (enemy data "turns": false).
local Battle = require("fe.battle.core")
local ai = require("fe.battle.ai")
local atb = require("fe.battle.schedulers.atb")
local data = require("fe.data")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local round_order = require("fe.battle.schedulers.round_order")
local state = require("fe.state")
local tablex = require("fe.tablex")
local task = require("fe.task")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  Battle.settings.inventory, Battle.settings.party = inventory, party
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
  party.join("sage")
end

local function battle_with(scheduler, troop, rules)
  local b = Battle({ scheduler = scheduler or round_order.new(), troop = troop, random = fe.random.new(3), rules = rules })
  b:populate(party.members(), troop)
  return b
end

local function hit_targets(results)
  local hit = {}
  for _, r in ipairs(results or {}) do hit[r.target] = true end
  return hit
end

return {
  ["forced_action comes before the player controller and the AI, specific layers first"] = function(t)
    setup()
    local asked = 0
    local b = battle_with(nil, "slimes", {
      player_controller = function(_, me)
        asked = asked + 1
        return { kind = "skill", user = me, skill = "attack", targets = { me.battle.battlers[3] } }
      end,
    })
    local hero, sage, slime = b.battlers[1], b.battlers[2], b.battlers[3]
    local general = b:add_rules({ forced_action = function(_, me)
      if me == hero or me == slime then return { kind = "defend", reason = "general" } end
    end })
    t.ok(general)
    b:add_rules({ forced_action = function(_, me)
      if me == hero then return { kind = "defend", reason = "specific" } end
    end })
    local action = b:decide(hero)
    t.eq(action.reason, "specific", "the most specific answer wins")
    t.eq(action.user, hero, "the user is filled in")
    t.eq(asked, 0, "the controller is not asked")
    t.eq(b:decide(slime).reason, "general", "a layer that answers nil passes the question on")
    t.eq(b:decide(sage).kind, "skill", "no forced action: the controller decides")
    t.eq(asked, 1)
    t.eq(b:forced_action(sage), nil)
  end,

  ["forced_action drives whole battles in every scheduler"] = function(t)
    for _, make in ipairs({ function() return round_order.new() end,
      function() return atb.new({ mode = "active", speed = 8 }) end }) do
      setup()
      local defended = {}
      local b = battle_with(make(), "slimes", {
        player_controller = function() error("the controller must not be asked") end,
        forced_action = function(_, me) if me:is_party() then return { kind = "defend" } end end,
        outcome = function(battle) if #defended >= 4 then return "draw" end end,
      })
      b.events:connect(function(name, action) if name == "defend" then defended[#defended + 1] = action.user end end)
      local job = task.spawn(function() b:run() end)
      t.wait_until(function() return job.status ~= "running" end, 60 * 60)
      t.eq(job.status, "done", tostring(job.error))
      t.ok(#defended >= 4)
    end
  end,

  ["targetable vetoes menu targets, AI options and the fallback target"] = function(t)
    setup()
    local b = battle_with(nil, "slimes")
    local hero, slime_a, slime_b = b.battlers[1], b.battlers[3], b.battlers[4]
    local asked = {}
    b.rules.targetable = function(_, user, target, source)
      asked[#asked + 1] = { user = user, target = target, source = source and source.id }
      -- Slime A can only be hit with fire.
      if target == slime_a then return source ~= nil and source.id == "fire" end
    end
    t.eq(b:targets_for(hero, "enemy", data.get("skills", "attack")), { slime_b })
    t.eq(b:targets_for(hero, "enemy", data.get("skills", "fire")), { slime_a, slime_b })
    t.eq(b:targets_for(hero, "all_enemies"), { slime_b }, "no source: vetoed too")
    t.eq(b:targets_for(hero, "self"), { hero }, "self skills are not asked")
    t.ok(b:targetable(hero, slime_b, nil))
    t.ok(not b:targetable(hero, slime_a, nil))
    -- AI options never offer it.
    local options = ai.options(b, hero)
    for _, option in ipairs(options) do
      if option.skill.id ~= "fire" then
        t.ok(not tablex.contains(option.targets, slime_a), option.skill.id .. " never targets slime A")
      end
    end
    t.ok(asked[1].source == "attack", "the hook sees the source")
    -- A queued attack at a vetoed target falls back to an allowed one.
    for seed = 1, 6 do
      b.random = fe.random.new(seed)
      local results = b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime_a } })
      local hit = hit_targets(results)
      t.ok(hit[slime_b] and not hit[slime_a], "retargeted away from the vetoed slime")
      slime_b:set_resource("hp", slime_b:max_resource("hp"))
    end
    local hit = hit_targets(b:perform({ kind = "skill", user = hero, skill = "cleave", targets = { slime_a, slime_b } }))
    t.ok(hit[slime_b] and not hit[slime_a], "an all-enemies skill drops the vetoed target")
    slime_b:set_resource("hp", slime_b:max_resource("hp"))
    hit = hit_targets(b:perform({ kind = "skill", user = hero, skill = "flurry", targets = {} }))
    t.ok(not hit[slime_a], "random hits never land on it")
    slime_b:set_resource("hp", slime_b:max_resource("hp"))
    t.eq(b:retarget(hero, "enemy", data.get("skills", "attack")), { slime_b })
    b.rules.targetable = function(_, _, target) return target:is_party() end
    t.eq(b:retarget(hero, "enemy", data.get("skills", "attack")), {}, "nothing left to target")
    local results = b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime_a } })
    t.eq(#results, 0, "the action resolves against nobody")
  end,

  ["battlers with turns = false never act but can be targeted and knocked out"] = function(t)
    for name, make in pairs({ round_order = function() return round_order.new() end,
      atb = function() return atb.new({ mode = "active", speed = 8 }) end }) do
      setup()
      local turns, knocked, done = {}, false, false
      local b = Battle({ scheduler = make(), random = fe.random.new(5), opening = "preemptive", rules = {
        player_controller = function(battle, me)
          local clamp = tablex.find(battle:side("enemy"), function(x) return x.def.id == "clamp" end)
          return { kind = "skill", user = me, skill = "attack", targets = { clamp or battle:side("enemy")[1] } }
        end,
        on_turn_start = function(_, who) turns[who] = (turns[who] or 0) + 1 end,
        -- The golem's attacks miss on purpose, so the hero lasts.
        before_action = function(_, action)
          if action.user.def and action.user.def.id == "golem" then action.cancelled = true end
        end,
        on_knocked_out = function(_, who) if who.def and who.def.id == "clamp" then knocked = true end end,
        outcome = function() if done then return "draw" end end,
      } })
      b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
      local clamp = b:add({ enemy = "clamp", side = "enemy", position = { slot = 1 } })
      b:add({ enemy = "golem", side = "enemy", position = { slot = 2 } })
      t.ok(not clamp:takes_turns(), name)
      t.ok(clamp:can_act(), name .. ": it is not disabled, it just has no clock")
      t.ok(tablex.contains(b:targets_for(b.battlers[1], "enemy"), clamp), name)
      local job = task.spawn(function() b:run() end)
      t.wait_until(function() return knocked end, 60 * 60, name .. ": the clamp is knocked out")
      t.tick(30)
      done = true
      t.wait_until(function() return job.status ~= "running" end, 60 * 60)
      t.eq(job.status, "done", tostring(job.error))
      t.eq(turns[clamp], nil, name .. ": the clamp never had a turn")
      t.ok(not clamp.data.opening_skip, name .. ": openings skip nothing for it")
      t.ok((turns[b.battlers[3]] or 0) >= 1, name .. ": the golem has turns")
    end
  end,
}

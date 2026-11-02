-- fe.battle.retry: a lost battle can be fought again from exactly the state
-- it started in; other menu choices end the loop.
local Battle = require("fe.battle.core")
local battle_kit = require("fe.battle")
local data = require("fe.data")
local i18n = require("fe.i18n")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local retry = require("fe.battle.retry")
local round_order = require("fe.battle.schedulers.round_order")
local state = require("fe.state")
local task = require("fe.task")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  Battle.settings.inventory, Battle.settings.party = inventory, party
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
  inventory.add("potion", 2)
end

-- A battle without a presentation whose outcome is `outcomes[attempt]`.
-- Each attempt changes the game state: HP, items, a flag and a variable.
local function options(attempts, outcomes, starts)
  return {
    presentation = { new = function() return {} end },
    stage = "arena",
    scheduler = round_order.new(),
    setup = function(b)
      b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } }).auto = true
      b:add({ enemy = "golem", side = "enemy", position = { slot = 1 } })
    end,
    rules = {
      on_start = function()
        local hero = party.actor("hero")
        starts[#starts + 1] = { hp = hero:resource("hp"), potions = inventory.count("potion"),
          flag = state.flag("scarred"), tries = state.var("tries", 0) }
        hero:set_resource("hp", 7)
        inventory.remove("potion", 1)
        state.set_flag("scarred")
        state.add("tries", 1)
      end,
      outcome = function(b)
        if b.round >= 1 then return outcomes[attempts.n] end
      end,
    },
  }
end

return {
  ["retry restores the snapshot exactly before every new attempt"] = function(t)
    setup()
    local attempts, starts, menus = { n = 0 }, {}, {}
    local outcome, choice, rewards
    local job = task.spawn(function()
      outcome, choice, rewards = retry.start(options(attempts, { "defeat", "defeat", "victory" }, starts), {
        before = function(attempt) attempts.n = attempt end,
        labels = { retry = "Try again" },
        menu = function(result, context)
          menus[#menus + 1] = { outcome = result.outcome, battle = result.battle, attempt = context.attempt,
            label = retry.label("retry", context.labels), choices = context.choices,
            hp = party.actor("hero"):resource("hp") }
          return "retry"
        end,
      })
    end)
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    t.eq(job.status, "done", tostring(job.error))
    t.eq(outcome, "victory")
    t.eq(choice, nil, "a won battle asks nothing")
    t.ok(rewards ~= nil)
    t.eq(#menus, 2)
    t.eq(menus[1].attempt, 1)
    t.eq(menus[2].attempt, 2)
    t.eq(menus[1].outcome, "defeat")
    t.ok(menus[1].battle ~= nil and menus[1].battle ~= menus[2].battle, "each attempt is a new battle")
    t.eq(menus[1].label, "Try again")
    t.eq(menus[1].choices, { "retry", "checkpoint", "title" })
    t.eq(menus[1].hp, 7, "the menu sees the state the defeat left")
    t.eq(#starts, 3)
    for i = 2, 3 do
      t.eq(starts[i], starts[1], "attempt " .. i .. " starts where the first did")
    end
    t.eq(starts[1].tries, 0)
    t.ok(not starts[1].flag)
    t.eq(state.var("tries"), 1, "only the last attempt's changes remain")
    t.ok(state.flag("scarred"))
    t.eq(inventory.count("potion"), 1, "one potion used once, not once per attempt")
  end,

  ["retry ends on any other choice and leaves the state as the defeat left it"] = function(t)
    setup()
    local attempts, starts = { n = 0 }, {}
    local outcome, choice
    local defeats = 0
    local stop = battle_kit.finished:connect(function(_, o) if o == "defeat" then defeats = defeats + 1 end end)
    local job = task.spawn(function()
      outcome, choice = retry.start(options(attempts, { "defeat" }, starts), {
        before = function(attempt) attempts.n = attempt end,
        menu = function() return "checkpoint" end,
      })
    end)
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    stop()
    t.eq(job.status, "done", tostring(job.error))
    t.eq(outcome, "defeat")
    t.eq(choice, "checkpoint")
    t.eq(defeats, 1)
    t.eq(#starts, 1)
    t.eq(state.var("tries"), 1)
    t.eq(inventory.count("potion"), 1)
  end,

  ["retry keep lists sections a retry leaves alone; the default menu has i18n labels"] = function(t)
    setup()
    t.eq(retry.label("retry"), "Retry Battle")
    t.eq(retry.label("checkpoint"), "Load Checkpoint")
    t.eq(retry.label("title"), "Return to Title")
    t.eq(i18n.t("fe.battle.retry.prompt"), "Defeated")
    t.eq(retry.label("retry", { retry = "fe.battle.retry.checkpoint" }), "Load Checkpoint", "labels may be i18n keys")
    local attempts, starts = { n = 0 }, {}
    local job = task.spawn(function()
      retry.start(options(attempts, { "defeat", "victory" }, starts), {
        keep = { "flags" },
        before = function(attempt) attempts.n = attempt end,
        menu = function() return "retry" end,
      })
    end)
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    t.eq(job.status, "done", tostring(job.error))
    t.ok(starts[2].flag, "kept: the flag survived the retry")
    t.eq(starts[2].tries, 0, "the rest was restored")
  end,
}

-- Verbs: press Info near certain townsfolk to Inquire (learn something,
-- with a chance based on the leader's level), Purchase (buy what they
-- carry) or Guide (ask them to travel with you as a follower who can help
-- in battle). Failing an inquiry lowers the town's opinion of you.
-- Entirely game code: fe.field verbs on the "info" input, fe.follower, and a
-- HUD prompt. Placements opt in with props.verbs = ["inquire", "purchase"];
-- who can be guided is `verbs.guides`, by placement id.
local data = require("fe.data")
local dialogue = require("fe.dialogue")
local field = require("fe.field")
local follower = require("fe.follower")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local state = require("fe.state")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local verbs = {
  random = fe.random.new(),
  --- Townsfolk who agree to be guided, with their battle assist.
  guides = {
    child = { assist = { enemy = "wolf", skill = "attack", chance = 0.5 } },
  },
}

--- Chance that the leader's inquiry succeeds (0..1).
function verbs.chance(target)
  local leader = party.leader()
  local difference = leader:level() - (target:props().level or 1)
  return tablex.clamp(0.6 + difference * 0.1, 0.1, 0.95)
end

local function inquire(target)
  local props = target:props()
  local flag = "inquired:" .. target.id
  if state.flag(flag) then
    dialogue.say(props.name, props.info)
    return
  end
  if verbs.random:float() < verbs.chance(target) then
    state.set_flag(flag)
    dialogue.say(props.name, props.info)
    if props.reveals then
      state.set_flag(props.reveals)
      dialogue.say(nil, "{color:good}You learned something new.{color:reset}")
    end
  else
    state.add("reputation", -1)
    dialogue.say(props.name, "Why are you asking me that? Leave me be.")
  end
end

local function purchase(target)
  local props = target:props()
  local options = {}
  for _, id in ipairs(props.wares or {}) do
    local item = data.get("items", id)
    options[#options + 1] = { label = item.name, right = tostring(item.price), value = item,
      enabled = inventory.money() >= item.price }
  end
  local _, chosen = ui.choose(options, { title = props.name .. "'s wares", cancel = true })
  if chosen and inventory.spend(chosen.value.price) then
    inventory.add(chosen.value.id, 1)
    dialogue.say(props.name, "Pleasure doing business.")
  end
end

local function guide(target)
  local props = target:props()
  dialogue.say(props.name, "You'll take me along? Hooray!")
  follower.join_entity(target, verbs.guides[target.id])
  dialogue.say(nil, "{color:good}" .. props.name .. " is following you.{color:reset}")
end

local function offers(verb)
  return function(target)
    if verb == "guide" then return verbs.guides[target.id] ~= nil end
    local list = target:props().verbs
    return list ~= nil and tablex.contains(list, verb)
  end
end

local function verb(id, spec)
  local run = spec.run
  spec.input, spec.range, spec.facing, spec.ask, spec.when = "info", 1.8, false, true, offers(id)
  spec.run = function(target, player)
    target:face(player)
    run(target)
  end
  field.verb(id, spec)
end

function verbs.install()
  verb("inquire", {
    label = function(target) return string.format("Inquire (%d%%)", math.floor(verbs.chance(target) * 100)) end,
    run = inquire,
  })
  verb("purchase", { label = "Purchase", run = purchase })
  verb("guide", { label = "Guide", run = guide })
  field.hud:connect(function()
    if not field.can_control() then return end
    local _, h = fe.canvas.size()
    local target = field.target("info")
    if target then
      ui.text(ui.prompt("info") .. ": talk to " .. target:props().name, 8, h - 16, { color = ui.theme.heading })
    end
    ui.text(ui.prompt("toggle_time") .. ": day/night", 8, h - 28, { color = ui.theme.muted })
  end, 0, "wayfarers.hud")
end

return verbs

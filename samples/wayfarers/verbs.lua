-- Verbs: press Info near certain townsfolk to Inquire (learn something,
-- with a chance based on the leader's level) or Purchase (buy what they
-- carry). Failing an inquiry lowers the town's opinion of you.
-- Entirely game code built on fe.field, fe.ui and fe.state.
local data = require("fe.data")
local dialogue = require("fe.dialogue")
local field = require("fe.field")
local game = require("fe.game")
local input = require("fe.input")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local state = require("fe.state")
local task = require("fe.task")
local ui = require("fe.ui")

local verbs = { random = fe.random.new() }

local function nearest()
  local player = field.player
  if not player or not player:valid() then return nil end
  local best, best_distance
  for _, e in ipairs(field.entities()) do
    if e:valid() and e:props().verbs then
      local p, q = player.position, e.position
      local d = math.sqrt((p.x - q.x) ^ 2 + (p.z - q.z) ^ 2)
      if d < 1.8 and (not best or d < best_distance) then best, best_distance = e, d end
    end
  end
  return best
end

--- Chance that the leader's inquiry succeeds (0..1).
function verbs.chance(target)
  local leader = party.leader()
  local difference = leader:level() - (target:props().level or 1)
  return math.max(0.1, math.min(0.95, 0.6 + difference * 0.1))
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

local actions = { inquire = { label = "Inquire", run = inquire }, purchase = { label = "Purchase", run = purchase } }

function verbs.install()
  game.updated:connect(function()
    if not field.can_control() or not fe.input.pressed("info") then return end
    local target = nearest()
    if not target then return end
    task.named("verbs", function()
      field.lock()
      target:face(field.player)
      local entries = {}
      for _, verb in ipairs(target:props().verbs) do
        local label = actions[verb].label
        if verb == "inquire" then label = label .. string.format(" (%d%%)", math.floor(verbs.chance(target) * 100)) end
        entries[#entries + 1] = { label = label, value = verb }
      end
      local _, chosen = ui.choose(entries, { title = target:props().name, cancel = true, width = 140 })
      if chosen then actions[chosen.value].run(target) end
      dialogue.close()
      field.unlock()
    end)
  end)
  field.hud:connect(function()
    local target = field.can_control() and nearest()
    if target then
      local _, h = fe.canvas.size()
      ui.text(input.prompt("info") .. ": talk to " .. target:props().name, 8, h - 16, { color = ui.theme.heading })
    end
  end)
end

return verbs

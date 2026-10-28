--- RPG kit: stats, actors, party, inventory and the data schemas.
local rpg = {
  stats = require("fe.rpg.stats"),
  database = require("fe.rpg.database"),
  Actor = require("fe.rpg.actor"),
  party = require("fe.rpg.party"),
  inventory = require("fe.rpg.inventory"),
  Combatant = require("fe.rpg.combatant"),
}

--- Connect the RPG kit to the field and events: chests give items, page
--- conditions can test items and party members, the field leader sprite
--- follows the party. fe.game calls nothing here; games call rpg.install().
function rpg.install()
  local dialogue = require("fe.dialogue")
  local event = require("fe.event")
  local field = require("fe.field")
  local data = require("fe.data")
  field.give = function(item, count, money)
    if item then
      local def = data.get("items", item)
      rpg.inventory.add(item, count)
      dialogue.say(nil, "Obtained {color:highlight}" .. def.name .. "{color:reset}" .. (count > 1 and " x" .. count or "") .. ".")
    end
    if money then
      rpg.inventory.add_money(money)
      dialogue.say(nil, "Obtained " .. money .. " " .. rpg.inventory.settings.currency .. ".")
    end
  end
  event.condition("item", function(value)
    return rpg.inventory.has(value)
  end)
  event.condition("member", function(value)
    return rpg.party.is_active(value)
  end)
  field.settings.player_sprite = function()
    local leader = rpg.party.leader()
    return leader and leader:sprite()
  end
  field.follower_sprites = function()
    local out = {}
    for i, actor in ipairs(rpg.party.members()) do
      if i > 1 then out[#out + 1] = actor:sprite() end
    end
    return out
  end
  require("fe.save").add_meta(function(meta)
    local members = {}
    for _, actor in ipairs(rpg.party.members()) do
      members[#members + 1] = { id = actor.id, name = actor:name(), level = actor:level() }
    end
    meta.party = members
  end)
  require("fe.i18n").add_resolver(function(tag)
    if tag == "leader" or tag == "player" then
      local leader = rpg.party.leader()
      return leader and leader:name()
    end
    local id = tag:match("^actor:(.+)$")
    if id then return rpg.party.actor(id):name() end
  end)
end

return rpg

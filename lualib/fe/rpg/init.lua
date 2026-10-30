--- RPG kit: stats, actors, party, inventory and the data schemas.
local i18n = require("fe.i18n")

local rpg = {
  stats = require("fe.rpg.stats"),
  database = require("fe.rpg.database"),
  Actor = require("fe.rpg.actor"),
  party = require("fe.rpg.party"),
  inventory = require("fe.rpg.inventory"),
  Combatant = require("fe.rpg.combatant"),
  installed = false,
}

i18n.defaults({ fe = { rpg = {
  obtained_item = "Obtained {color:highlight}{item}{color:reset}.",
  obtained_items = "Obtained {color:highlight}{item}{color:reset} x{count}.",
  obtained_money = "Obtained {money} {currency}.",
} } })

--- Show what a treasure gave with `field.announce`: a toast with the item's
--- icon, or a message when `field.settings.pickup_toasts` is false. Replace
--- it for another presentation.
function rpg.announce(item, count, money)
  local field = require("fe.field")
  if item then
    local record = require("fe.data").get("items", item)
    local key = count > 1 and "fe.rpg.obtained_items" or "fe.rpg.obtained_item"
    field.announce(i18n.t(key, { item = record.name, count = count }), { icon = record.icon })
  end
  if money then
    field.announce(i18n.t("fe.rpg.obtained_money", { money = money, currency = rpg.inventory.settings.currency }))
  end
end

--- Connect the RPG kit to the field and events. fe.starter calls it for
--- games with RPG data; calling it again does nothing. Its listeners are
--- keyed, so even a reloaded copy of the kit that installs again replaces
--- them instead of adding more. It adds:
---   * a `field.treasure` listener that puts items and money in the
---     inventory and announces them (`rpg.announce`: toasts by default)
---   * page conditions `item` and `member`
---   * the party leader's sprite for the player, unless the game set
---     `field.settings.player_sprite`, and party members as followers
---   * party details in save metadata, and `{leader}` / `{actor:id}` markup
function rpg.install()
  if rpg.installed then return end
  rpg.installed = true
  local event = require("fe.event")
  local field = require("fe.field")
  field.treasure:connect(function(item, count, money)
    if item then rpg.inventory.add(item, count) end
    if money then rpg.inventory.add_money(money) end
    rpg.announce(item, count, money)
    return true
  end, 0, "fe.rpg.treasure")
  event.condition("item", function(value)
    return rpg.inventory.has(value)
  end)
  event.condition("member", function(value)
    return rpg.party.is_active(value)
  end)
  if field.settings.player_sprite == nil then
    field.settings.player_sprite = function()
      local leader = rpg.party.leader()
      return leader and leader:sprite()
    end
  end
  require("fe.field.followers").watch_party(rpg.party)
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
  end, "fe.rpg.party")
  require("fe.i18n").add_resolver(function(tag)
    if tag == "leader" or tag == "player" then
      local leader = rpg.party.leader()
      return leader and leader:name()
    end
    local id = tag:match("^actor:(.+)$")
    if id then return rpg.party.actor(id):name() end
  end, "fe.rpg.actors")
end

return rpg

--- Helpers shared by the side-view and tactical presenters (and useful to
--- a game's own): damage popups, banners with a timeout, hit flashes, KO
--- fades, the once-only close guard, result popups that handle missed and
--- nullified (cancelled) results, reward and level-up texts, and waits
--- scaled by the battle speed (Battle.speed).
---
--- A presenter using these keeps `popups` (a list) and a banner field;
--- `common.result_popups` reads `fe.battle.*` texts unless given another
--- prefix (the tactical presenter uses `fe.tactics.*`).
local Battle = require("fe.battle.core")
local data = require("fe.data")
local i18n = require("fe.i18n")
local task = require("fe.task")
local tablex = require("fe.tablex")
local tween = require("fe.tween")
local ui = require("fe.ui")

local common = {
  settings = {
    popup_life = 1.2,   -- seconds a popup stays
    popup_rise = 10,    -- pixels a popup rises
    flash = 0.25,       -- seconds a hit flash fades
    fade = 0.4,         -- seconds a knocked-out enemy fades
    colors = { damage = 0xffffffff, critical = 0xffd060ff, weakness = 0xff9070ff, state_tick = 0xc890ffff },
  },
}

-- Texts both presenters show, defined once: the side-view reads them as
-- fe.battle.*, the tactical presenter as fe.tactics.* (its prefix).
local shared = { miss = "Miss", nullified = "Nullified", counters = "{name} counters!" }
i18n.defaults({ fe = {
  battle = tablex.merge({ exp_gain = "+{exp} EXP", ambush = "Ambush!",
    preemptive = "Pre-emptive strike!", level_up = "{name} reached level {level}!", learned = "  Learned {skill}",
    victory = "Victory!", rewards = "{exp} EXP   {money} {currency}", jp = "{jp} JP", found = "Found {item}" },
    shared),
  tactics = shared,
} })

--- Wait `seconds` of battle time: divided by the battle speed.
function common.wait(seconds)
  task.wait(seconds / Battle.speed())
end

--- A duration scaled by the battle speed (for tweens and banners).
function common.scaled(seconds)
  return seconds / Battle.speed()
end

--- The battler's sprite while it still exists (a dismissed summon has none).
function common.sprite_of(b)
  local entity = b.entity
  if entity and entity:valid() then return entity end
end

--- Whether an entity's sprite animation has a clip (false without a sprite).
function common.has_clip(entity, clip)
  local sprite = entity and entity:valid() and entity:sprite()
  return sprite ~= nil and sprite.animation ~= nil and fe.app.sprite_clips(sprite.animation)[clip] ~= nil
end

--- Play a clip on an entity when its sprite has it.
function common.play_clip(entity, clip)
  if common.has_clip(entity, clip) then entity:clip(clip) end
end

--- Queue a popup above a battler's sprite.
function common.popup(p, b, text, color)
  local entity = common.sprite_of(b)
  if not entity then return end
  local x, y = entity:screen_position(1.4)
  p.popups[#p.popups + 1] = { x = x, y = y, text = text, color = color or ui.theme.text, age = 0 }
end

--- Age the popups by `dt` and drop the ones that ran out.
function common.age_popups(p, dt)
  local life = common.settings.popup_life
  for _, popup in ipairs(p.popups) do popup.age = popup.age + dt end
  p.popups = tablex.filter(p.popups, function(popup) return popup.age < life end)
end

--- Draw the popups: they rise, then fade out over the last quarter.
function common.draw_popups(p)
  local life = common.settings.popup_life
  for _, popup in ipairs(p.popups) do
    local rise = math.min(1, popup.age * 4) * common.settings.popup_rise
    local fade_from = life * 0.75
    local alpha = popup.age > fade_from and 1 - (popup.age - fade_from) / (life - fade_from) or 1
    fe.canvas.push({ alpha = alpha })
    fe.canvas.text(popup.text, popup.x - 40, popup.y - rise, { color = popup.color, outline = 0x000000ff, width = 80,
      align = "center" })
    fe.canvas.pop()
  end
end

--- Show `text` in the presenter's banner field (`field`, "banner" by
--- default) for `duration` seconds of battle time, unless another banner
--- replaced it meanwhile.
function common.banner(p, text, duration, field)
  field = field or "banner"
  p[field] = text
  task.spawn(function()
    common.wait(duration or 1)
    if p[field] == text then p[field] = nil end
  end)
end

--- Draw the banner centred near the top.
function common.draw_banner(text, y)
  local cw = fe.canvas.size()
  local bw = ui.measure(text) + 24
  local bx = math.floor((cw - bw) / 2)
  ui.window(bx, y, bw, 18)
  ui.text(text, bx + 12, y + 5, { color = ui.theme.heading })
end

--- Flash a sprite white and fade the flash out.
function common.flash(entity, color)
  color = color or { 1, 1, 1, 0.8 }
  entity:sprite({ flash = color })
  task.spawn(function()
    tween.run(common.scaled(common.settings.flash), function(t)
      if entity:valid() then entity:sprite({ flash = { color[1], color[2], color[3], color[4] * (1 - t) } }) end
    end)
  end)
end

--- Fade a knocked-out sprite to `tint` (its colour at alpha 0) and hide it
--- while the battler is still down afterwards.
function common.fade_out(entity, b, tint)
  tint = tint or { 1, 1, 1 }
  task.spawn(function()
    tween.run(common.scaled(common.settings.fade), function(t)
      if entity:valid() then entity:sprite({ tint = { tint[1], tint[2], tint[3], 1 - t } }) end
    end)
    if entity:valid() and not b:alive() then entity:sprite({ visible = false }) end
  end)
end

--- Bring a revived sprite back.
function common.restore(entity)
  entity:sprite({ visible = true, tint = { 1, 1, 1, 1 } })
end

--- True the first time it is called with `key` on `p` (close/abort guards).
function common.once(p, key)
  if p[key] then return false end
  p[key] = true
  return true
end

--- Popups for a list of effect results: missed and nullified (cancelled)
--- results show their text, damage its amount (critical and weakness
--- colours, and a flash on the sprite), healing, resources and revivals
--- their amount, added states their name. `options.prefix` picks the i18n
--- group ("fe.battle" by default); `options.on_damage(r, entity)` runs for
--- each damage result that hurt.
---@param options? {prefix?: string, on_damage?: fun(r: table, entity: any), states?: boolean}
function common.result_popups(p, results, options)
  options = options or {}
  local prefix = options.prefix or "fe.battle"
  local colors = common.settings.colors
  for _, r in ipairs(results) do
    local b = r.target
    if r.cancelled then
      common.popup(p, b, i18n.t(prefix .. ".nullified"), ui.theme.muted)
    elseif r.missed then
      common.popup(p, b, i18n.t(prefix .. ".miss"), ui.theme.muted)
    elseif r.type == "damage" then
      local amount = r.amount or 0
      local color = r.critical and colors.critical or colors.damage
      if r.affinity and r.affinity > 1 then color = colors.weakness end
      common.popup(p, b, tostring(amount) .. (r.critical and "!" or ""), color)
      local entity = common.sprite_of(b)
      if amount > 0 and entity then
        common.flash(entity)
        if options.on_damage then options.on_damage(r, entity) end
      end
    elseif r.type == "heal" or r.type == "resource" or r.type == "revive" then
      if (r.amount or 0) ~= 0 then common.popup(p, b, tostring(r.amount), ui.theme.good) end
    elseif r.type == "add_state" and r.applied and options.states ~= false then
      common.popup(p, b, data.get("states", r.state).name, ui.theme.heading)
    end
  end
end

--- Lines describing level-ups ({actor, gains = {{level, skills}}} entries
--- from party.gain_exp or an "exp" event's battler and gains).
function common.level_up_lines(entries)
  local lines = {}
  for _, entry in ipairs(entries or {}) do
    local name = entry.actor and entry.actor:name() or entry.battler and entry.battler:name()
    for _, gain in ipairs(entry.gains or {}) do
      lines[#lines + 1] = "{color:good}" .. i18n.t("fe.battle.level_up", { name = name, level = gain.level }) .. "{color:reset}"
      for _, skill in ipairs(gain.skills or {}) do
        lines[#lines + 1] = i18n.t("fe.battle.learned", { skill = data.get("skills", skill).name })
      end
    end
  end
  return lines
end

--- The victory window's lines: experience, money, job points, items and
--- level-ups.
function common.reward_lines(battle, rewards)
  local inventory = battle:inventory()
  local currency = inventory and inventory.settings and inventory.settings.currency or ""
  local lines = { "{color:heading}" .. i18n.t("fe.battle.victory") .. "{color:reset}" }
  lines[#lines + 1] = i18n.t("fe.battle.rewards", { exp = rewards.exp, money = rewards.money, currency = currency })
  if rewards.jp and rewards.jp > 0 then lines[#lines + 1] = i18n.t("fe.battle.jp", { jp = rewards.jp }) end
  for _, id in ipairs(rewards.items or {}) do
    lines[#lines + 1] = i18n.t("fe.battle.found", { item = data.get("items", id).name })
  end
  for _, line in ipairs(common.level_up_lines(rewards.levels)) do lines[#lines + 1] = line end
  return lines
end

--- Draw a centred window of text (rewards, level-ups).
function common.draw_text_window(text, width)
  width = width or 220
  local rh = select(2, ui.measure(text, { width = width - 20 })) + 18
  local rx, ry = ui.anchor(width, rh, "center")
  ui.window(rx, ry, width, rh)
  ui.text(text, rx + 10, ry + 8, { width = width - 20 })
end

return common

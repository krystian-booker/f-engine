--- The side-view battle HUD: party status with gauges and battle-only
--- resources, enemy names, the modifier (boost) gauge, the scheduler's
--- turn preview (cached per battle event), targeting cursors, the banner,
--- popups and the victory window. `fe.battle.sideview` wraps `hud.draw` as
--- `sideview.draw_hud(p)` and `hud.show_rewards` as `sideview.show_rewards`;
--- every function takes the sideview module so games' settings, modifier
--- and overlays are read live.
local Combatant = require("fe.rpg.combatant")
local common = require("fe.battle.presenter_common")
local i18n = require("fe.i18n")
local stats = require("fe.rpg.stats")
local task = require("fe.task")
local ui = require("fe.ui")

local hud = {}

local function modifier_label(sideview)
  local label = sideview.modifier.label
  return type(label) == "function" and label() or label
end

--- The scheduler's preview for the HUD, recomputed only after a battle
--- event or a change of the acting battler.
function hud.preview(p)
  local battle = p.battle
  local cache = p.preview_cache
  if cache and cache.stamp == p.stamp and cache.current == battle.current and cache.count == #battle.battlers then
    return cache.order, cache.next_round
  end
  local order, next_round = battle.scheduler.preview(battle, 8)
  p.preview_cache = { stamp = p.stamp, current = battle.current, count = #battle.battlers, order = order,
    next_round = next_round }
  return order, next_round
end

--- Draw the battle HUD.
function hud.draw(sideview, p)
  local battle = p.battle
  local cw, ch = fe.canvas.size()
  local life = Combatant.life
  -- Party status (bottom right)
  local members = battle:side("party", true)
  local h = #members * 16 + 12
  local x, y, w = math.floor(cw * 0.42), ch - h - 6, math.floor(cw * 0.58) - 8
  ui.window(x, y, w, h)
  for i, b in ipairs(members) do
    local row = y + 6 + (i - 1) * 16
    local shown = p.shown[b] or {}
    local color = b == p.active and ui.theme.highlight or (b:alive() and ui.theme.text or ui.theme.bad)
    ui.text(b:name(), x + 8, row, { color = color })
    local hp = math.floor((shown[life] or b:resource(life)) + 0.5)
    ui.text(string.format("%d/%d", hp, b:max_resource(life)), x + 66, row, { width = 70, align = "right" })
    ui.gauge(x + 66, row + 9, 70, 3, hp, b:max_resource(life), ui.theme.gauges.hp)
    -- The first resource after life (MP, SP...) is shown as a number.
    for _, def in ipairs(stats.resource_list) do
      if def.id ~= life and not def.battle_only and b:max_resource(def.id) > 0 then
        ui.text(tostring(b:resource(def.id)), x + 140, row, { width = 30, align = "right", color = ui.theme.gauges[def.id] or ui.theme.gauges.mp })
        break
      end
    end
    local pips = ""
    for _, def in ipairs(stats.resource_list) do
      if def.battle_only then
        local n = b:resource(def.id)
        pips = pips .. def.name .. " " .. string.rep("◆", n) .. string.rep("·", math.max(0, b:max_resource(def.id) - n)) .. " "
      end
    end
    if pips ~= "" then ui.text(pips, x + 176, row, { color = ui.theme.heading }) end
    if battle.scheduler.gauge then
      local fill = battle.scheduler.gauge(b)
      ui.gauge(x + w - 52, row + 3, 44, 4, fill, 1, fill >= 1 and ui.theme.highlight or ui.theme.gauges.mp)
    end
  end
  -- Enemy names (bottom left) when no menu is open
  if not p.active then
    local enemies = battle:side("enemy")
    local eh = math.max(1, #enemies) * 11 + 12
    ui.window(8, ch - eh - 6, math.floor(cw * 0.42) - 12, eh)
    for i, b in ipairs(enemies) do
      ui.text(b:name(), 16, ch - eh + (i - 1) * 11)
    end
  end
  -- Modifier gauge (boost)
  if sideview.modifier and p.active then
    local max = sideview.modifier.max(p.active)
    ui.text(modifier_label(sideview) .. " " .. string.rep("◆", p.modifier_value or 0) .. string.rep("·", max - (p.modifier_value or 0)),
      104, ch - 112, { color = ui.theme.heading })
  end
  -- Turn order (if the scheduler can preview): this round, then the next.
  if battle.scheduler.preview then
    local order, next_round = hud.preview(p)
    local bx = 8
    local function chip(b, alpha)
      ui.window(bx, 6, 26, 14, { alpha = alpha })
      local color = b == battle.current and ui.theme.highlight or (b:is_party() and ui.theme.good or ui.theme.bad)
      ui.text(b:name():sub(1, 3), bx + 4, 9, { color = color })
      bx = bx + 28
    end
    for i, b in ipairs(order) do
      if i > 8 then break end
      chip(b, b:is_party() and 1 or 0.8)
    end
    if next_round and #next_round > 0 then
      local label = i18n.t("fe.battle.next")
      ui.text(label, bx + 2, 9, { color = ui.theme.muted })
      bx = bx + ui.measure(label) + 6
      for i, b in ipairs(next_round) do
        if i > 8 then break end
        chip(b, 0.55)
      end
    end
  end
  -- Targeting cursor
  for _, b in ipairs(p.targeting or {}) do
    local entity = common.sprite_of(b)
    if entity then
      local tx, ty = entity:screen_position(1.5)
      ui.text("▼", tx - 3, ty - 10 + math.floor(task.time() * 4) % 2, { color = ui.theme.highlight })
    end
  end
  if p.banner then common.draw_banner(p.banner, 24) end
  common.draw_popups(p)
  if p.rewards_text then common.draw_text_window(p.rewards_text) end
end

--- Victory window: experience, money, items, level ups. Waits for Confirm.
function hud.show_rewards(_, presenter, rewards)
  presenter.rewards_text = table.concat(common.reward_lines(presenter.battle, rewards), "\n")
  task.frame()
  task.wait_until(function() return fe.input.pressed("confirm") end)
  presenter.rewards_text = nil
end

return hud

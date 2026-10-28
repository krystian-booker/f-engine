-- Wayfarers' battle rules, all in game Lua on top of fe.battle.core:
--  * Shields: enemies carry shield points and hidden weaknesses (damage
--    types and elements). Each weakness hit removes a point and reveals the
--    weakness; at zero the enemy is Broken: it loses a turn and takes double
--    damage, then its shield recovers.
--  * Boost: party members gain 1 BP per round (max 5) and spend up to 3 when
--    choosing a command (Q/E or shoulders) for extra hits or power.
local effects = require("fe.effects")
local sideview = require("fe.battle.sideview")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local rules = {}

local function weaknesses(b)
  return b.def and b.def.weak or {}
end

function rules.setup(battle)
  for _, b in ipairs(battle.battlers) do
    if b.def then
      b.data.max_shield = b.def.shield or 2
      b.data.shield = b.data.max_shield
      b.data.revealed = {}
    else
      b:set_resource("bp", 1)
    end
  end
end

function rules.on_round(battle, round)
  if round == 1 then return end
  for _, b in ipairs(battle:side("party")) do
    if not b.data.boosted then b:change_resource("bp", 1) end
    b.data.boosted = nil
  end
end

-- After a damaging hit: weakness checks chip the shield.
effects.hook("after", function(ctx)
  local battle, target = ctx.battle, ctx.target
  if not battle or not target.def or ctx.effect.type ~= "damage" or ctx.result.missed or not target:alive() then return end
  local weak = weaknesses(target)
  for _, key in ipairs({ ctx.damage_type, ctx.element }) do
    if key and tablex.contains(weak, key) then
      target.data.revealed[key] = true
      if not target:has_state("broken") and target.data.shield > 0 then
        target.data.shield = target.data.shield - 1
        if target.data.shield == 0 then
          target:add_state("broken")
          battle:present("break", target)
        end
      end
    end
  end
end)

effects.hook("modify", function(ctx)
  if ctx.battle and ctx.effect.type == "damage" and ctx.target:has_state("broken") then
    ctx.multiplier = ctx.multiplier * 2
  end
end, 80)

function rules.on_turn_end(battle, b)
  if b.def and not b:has_state("broken") and b.data.shield == 0 then
    b.data.shield = b.data.max_shield
  end
end

sideview.modifier = {
  label = "Boost",
  max = function(b) return math.min(3, b:resource("bp")) end,
  apply = function(action, amount)
    action.user:change_resource("bp", -amount)
    action.user.data.boosted = true
    if action.skill == "attack" then
      action.hits = 1 + amount
    else
      action.multiplier = 1 + 0.6 * amount
    end
  end,
}

-- Show shields and discovered weaknesses above enemies.
local base_hud = sideview.draw_hud
sideview.draw_hud = function(p)
  base_hud(p)
  for _, b in ipairs(p.battle:side("enemy")) do
    local x, y = b.entity:screen_position(1.9)
    local broken = b:has_state("broken")
    ui.text(broken and "BREAK" or ("◆" .. b.data.shield), x - 12, y - 10, { color = broken and ui.theme.bad or ui.theme.heading })
    local known = {}
    for _, key in ipairs(weaknesses(b)) do known[#known + 1] = b.data.revealed[key] and key or "?" end
    ui.text(table.concat(known, " "), x - 30, y, { width = 60, align = "center", color = ui.theme.muted })
  end
end

-- Breaking an enemy gets a banner and a shake.
sideview.Presenter["break"] = function(presenter, target)
  presenter:show_banner(target:name() .. " is broken!", 0.9)
  presenter.rig:shake(0.15, 0.4)
  require("fe.task").wait(0.4)
end

return rules

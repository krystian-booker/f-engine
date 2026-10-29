--- Screen transitions using fe.screen. All functions wait (call in a task).
---
--- ```lua
--- transition.out("swirl", 0.6)   -- cover the screen
--- -- change maps / start a battle
--- transition.into("fade", 0.4)   -- reveal
--- ```
--- Kinds: "fade" (uses fade colour), "swirl", "mosaic", "wipe", "iris".
local tween = require("fe.tween")

local transition = {}

local function animate(kind, from, to, duration, color)
  color = color or { 0, 0, 0 }
  if kind == "fade" then
    tween.run(duration, function(t)
      fe.screen.set({ fade = { color[1], color[2], color[3], from + (to - from) * t } })
    end, "in_out_quad")
  else
    tween.run(duration, function(t)
      fe.screen.set({ transition = { kind = kind, progress = from + (to - from) * t,
        color = { color[1], color[2], color[3], 1 } } })
    end, "in_out_quad")
    if to == 0 then
      fe.screen.set({ transition = { kind = "none", progress = 0 } })
    end
  end
end

--- Cover the screen.
function transition.out(kind, duration, color)
  animate(kind or "fade", 0, 1, duration or 0.4, color)
end

--- Reveal the screen.
function transition.into(kind, duration, color)
  animate(kind or "fade", 1, 0, duration or 0.4, color)
end

--- Remove any fade or transition at once (cleanup after a cancelled
--- transition).
function transition.clear()
  fe.screen.set({ fade = { 0, 0, 0, 0 }, transition = { kind = "none", progress = 0 } })
end

--- Flash the screen (additive), e.g. on a critical hit.
function transition.flash(color, duration)
  color = color or { 1, 1, 1 }
  tween.run(duration or 0.25, function(t)
    fe.screen.set({ flash = { color[1], color[2], color[3], (1 - t) * (color[4] or 0.8) } })
  end)
end

--- Cinematic bars.
function transition.letterbox(on, duration)
  local start = fe.screen.get().letterbox
  tween.run(duration or 0.4, function(t)
    fe.screen.set({ letterbox = start + ((on and 1 or 0) - start) * t })
  end, "in_out_cubic")
end

--- Tint the 3D scene (night, poison, flashbacks). amount 0 removes it.
function transition.tint(color, amount, duration)
  local current = fe.screen.get().tint
  tween.run(duration or 0.5, function(t)
    fe.screen.set({ tint = {
      current[1] + (color[1] - current[1]) * t,
      current[2] + (color[2] - current[2]) * t,
      current[3] + (color[3] - current[3]) * t,
      current[4] + (amount - current[4]) * t,
    } })
  end)
end

return transition

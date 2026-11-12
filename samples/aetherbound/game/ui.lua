--- Drawing helpers for the game's screens, on a 1920 x 1080 canvas: panels,
--- styled text, gauges, the prompt row. Immediate mode: scenes call these
--- from their draw every frame.
local input = require("game.input")

local ui = {
  colors = {
    text = 0xf2ede2ff, dim = 0x9aa0b8ff, faint = 0x6b7190ff, gold = 0xe7c36bff, shade = 0x000000b0,
    hp = 0x7fe0a0ff, hp_low = 0xff8a5cff, mp = 0x79b8ffff, atb = 0xf4d27aff, atb_full = 0xfff6c8ff,
    aether = 0xb07cffff, aether_hot = 0xe6d0ffff, bad = 0xff6b6bff, good = 0x8fe3a2ff,
    gauge = 0x0b0d18e0, system = 0xc9a8ffff,
    cael = 0x8fb8ffff, rowan = 0xff8f8fff, lyra = 0x8fe6d4ff, darius = 0xe6c28fff,
  },
  time = 0,
}

--- Fonts of the game; called once from main.init.
function ui.setup()
  local body = { "ui/fonts/CrimsonPro_500Medium.ttf" }
  local bold = { files = { "ui/fonts/CrimsonPro_700Bold.ttf" } }
  fe.text.font("default", { files = body, size = 40, bold = bold })
  fe.text.font("body", { files = body, size = 46, bold = bold })
  fe.text.font("small", { files = body, size = 30, bold = bold })
  fe.text.font("italic", { files = { "ui/fonts/CrimsonPro_500Medium_Italic.ttf" }, size = 46 })
  fe.text.font("label", { files = { "ui/fonts/Cinzel_600SemiBold.ttf" }, size = 30 })
  fe.text.font("name", { files = { "ui/fonts/Cinzel_600SemiBold.ttf" }, size = 38 })
  fe.text.font("title", { files = { "ui/fonts/Cinzel_400Regular.ttf" }, size = 84 })
  fe.text.font("banner", { files = { "ui/fonts/Cinzel_600SemiBold.ttf" }, size = 64 })
  fe.text.font("mono", { files = { "ui/fonts/ShareTechMono_400Regular.ttf" }, size = 40 })
  fe.text.font("digits", { files = { "ui/fonts/Cinzel_600SemiBold.ttf" }, size = 52 })
  for name, color in pairs(ui.colors) do fe.text.color(name, color) end
end

function ui.size()
  return fe.canvas.size()
end

function ui.update(dt)
  ui.time = ui.time + dt
  local c = ui.shown_caption
  if c then
    c.age = c.age + dt
    if c.age >= c.life then ui.shown_caption = nil end
  end
end

--- 0xRRGGBBAA with its alpha scaled.
function ui.fade(color, alpha)
  local a = math.floor((color & 0xff) * math.max(0, math.min(1, alpha)) + 0.5)
  return (color & 0xffffff00) | a
end

--- A panel: "panel" (dark glass with a thin gold line), "system" (the
--- machine's voice) or "plate" (a name plate).
function ui.panel(x, y, w, h, kind, alpha)
  local image = "ui/" .. (kind or "panel") .. ".png"
  fe.canvas.nine_slice(image, x, y, w, h, 24, { linear = true, color = ui.fade(0xffffffff, alpha or 1) })
end

function ui.text(markup, x, y, options)
  options = options or {}
  options.color = options.color or ui.colors.text
  if options.shadow == nil then options.shadow = 0x000000c0 end
  return fe.canvas.text(markup, x, y, options)
end

--- A gauge: a dark slot with a coloured fill and a bright leading edge.
function ui.bar(x, y, w, h, fraction, color, back)
  fraction = math.max(0, math.min(1, fraction))
  fe.canvas.rect(x - 2, y - 2, w + 4, h + 4, 0x000000a0)
  fe.canvas.rect(x, y, w, h, back or ui.colors.gauge)
  local filled = math.floor(w * fraction + 0.5)
  if filled > 0 then
    fe.canvas.rect(x, y, filled, h, color)
    fe.canvas.rect(x, y, filled, math.max(1, h // 3), 0xffffff30)
    if filled < w then fe.canvas.rect(x + filled - 2, y, 2, h, 0xffffffc0) end
  end
end

--- The menu cursor, bobbing, pointing at (x, y) from the left.
function ui.cursor(x, y)
  local bob = math.sin(ui.time * 7) * 4
  fe.canvas.image("ui/cursor.png", x - 44 + bob, y - 20, { linear = true })
end

--- The row of button prompts at the bottom right.
function ui.hints(list, alpha)
  local w, h = ui.size()
  local text = input.hints(list)
  local tw = fe.text.measure(text, { font = "small" })
  fe.canvas.push({ alpha = alpha or 1 })
  ui.text(text, w - tw - 48, h - 52, { font = "small", color = ui.colors.dim })
  fe.canvas.pop()
end

--- A place name that fades in at the top left and out again.
function ui.caption(text, seconds)
  ui.shown_caption = { text = text, age = 0, life = seconds or 5 }
end

function ui.draw_caption()
  local c = ui.shown_caption
  if not c then return end
  local alpha = math.min(1, c.age / 1.2, (c.life - c.age) / 1.2)
  fe.canvas.push({ alpha = math.max(0, alpha) })
  local tw = fe.text.measure(c.text, { font = "banner" })
  ui.text(c.text, 110, 120, { font = "banner", color = ui.colors.text })
  fe.canvas.rect(110, 204, tw * math.min(1, c.age / 1.6), 2, ui.colors.gold)
  fe.canvas.pop()
end

--- Text centred on x.
function ui.centred(markup, x, y, options)
  options = options or {}
  local tw = fe.text.measure(markup, options)
  return ui.text(markup, x - tw / 2, y, options)
end

return ui

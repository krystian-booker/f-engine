--- The UI theme: window skin, colours, font and sounds used by fe.ui and
--- the default screens. Games change it with `theme.set{...}`.
local theme = {
  skin = "@fe/assets/ui/window.png",
  skin_border = 4,
  cursor = "@fe/assets/ui/cursor.png",
  font = "default",
  padding = 6,
  row_height = 12,
  text = 0xe8ecf4ff,
  muted = 0x9aa6bdff,
  highlight = 0xffe08aff,
  disabled = 0x6d7488ff,
  selection = 0x5577aa66,
  shadow = 0x00000099,
  heading = 0xffd27aff,
  good = 0x8ff0a4ff,
  bad = 0xff8a7aff,
  gauge_back = 0x0a1024ff,
  gauges = { hp = 0x6fd49fff, mp = 0x7fa6f6ff, sp = 0xf0c46cff, tp = 0xd99cf2ff, exp = 0xf2e36cff },
  sounds = { move = "cursor", confirm = "confirm", cancel = "cancel", buzzer = "buzzer" },
  open_time = 0.12,
}

--- Override theme values.
function theme.set(values)
  for key, value in pairs(values) do
    theme[key] = value
  end
end

return theme

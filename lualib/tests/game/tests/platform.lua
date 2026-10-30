-- Platform services the engine fakes headless: the clipboard, gamepad
-- connection events and the kit's signals for them, content packs and the
-- display scale.
local game = require("fe.game")
local input = require("fe.input")

return {
  ["the clipboard round-trips text within the session"] = function(t)
    t.eq(fe.app.clipboard(), nil)
    fe.app.set_clipboard("Aria")
    t.eq(fe.app.clipboard(), "Aria")
    fe.app.set_clipboard("")
    t.eq(fe.app.clipboard(), nil)
    t.error(function() fe.app.set_clipboard(string.rep("x", 64 * 1024 + 1)) end, "64 KiB")
  end,

  ["gamepad events arrive on the next tick and raise the input signals"] = function(t)
    local log = {}
    local off_connected = input.pad_connected:connect(function(pad, name) log[#log + 1] = { "on", pad, name } end)
    local off_disconnected = input.pad_disconnected:connect(function(pad, name) log[#log + 1] = { "off", pad, name } end)
    fe.input.feed_pad(1, { type = "playstation", rumble = true })
    -- pads() changes at once; the event waits for the tick.
    t.eq(#fe.input.pads(), 1)
    t.eq(fe.input.pads()[1].name, "Virtual")
    t.eq(fe.input.device_type(1), "playstation")
    t.eq(fe.input.pad_events(), {})
    t.eq(log, {})
    t.tick(1)
    t.eq(fe.input.pad_events(), { { type = "connected", pad = 1, name = "Virtual" } })
    t.eq(log, { { "on", 1, "Virtual" } })
    t.tick(1)
    t.eq(fe.input.pad_events(), {})
    fe.input.feed_pad(2, { name = "Second" })
    fe.input.feed_pad(1)
    t.eq(fe.input.device_type(1), nil)
    t.tick(1)
    t.eq(log, { { "on", 1, "Virtual" }, { "on", 2, "Second" }, { "off", 1, "Virtual" } })
    -- The signals come from game.update, so a game pausing on disconnect can act in the same tick.
    local paused
    local off_pause = input.pad_disconnected:connect(function() paused = game.paused end)
    fe.input.feed_pad(2)
    t.tick(1)
    t.eq(paused, false)
    off_connected()
    off_disconnected()
    off_pause()
    t.eq(#fe.input.pads(), 0)
    t.error(function() fe.input.feed_pad(0, {}) end, "start at 1")
  end,

  ["the test game mounts no packs and the display reports its scale"] = function(t)
    t.eq(fe.app.packs(), {})
    t.eq(fe.app.display().scale, 1)
  end,
}

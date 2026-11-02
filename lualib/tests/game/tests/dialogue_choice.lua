-- fe.dialogue: wrapped choices that grow with the text scale, per-option
-- previews and reading-time auto-advance; fe.cutscene's hold-to-skip.
local config = require("fe.config")
local cutscene = require("fe.cutscene")
local dialogue = require("fe.dialogue")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")
local ui = require("fe.ui")

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

local long = {
  "Open the clinic stores. I will sign for every crate myself.",
  "Nessa, show me what they need before anything else happens.",
}

return {
  ["long choices wrap and grow instead of being cut off, also at text scale 1.5"] = function(t)
    input.use_defaults()
    scene.clear()
    config.set("text_scale", 1)
    local normal = dialogue.choice_layout(long)
    t.ok(normal.width <= dialogue.style.choice_max_width, "at text scale 1 the panel keeps its maximum width")
    config.set("text_scale", 1.5)
    t.eq(ui.text_scale, 1.5)
    local layout = dialogue.choice_layout(long)
    local cw = fe.canvas.size()
    t.ok(layout.width <= cw - dialogue.style.margin * 2, "the panel stays on the canvas")
    for _, option in ipairs(long) do
      local w, h = ui.measure(option, { width = layout.wrap })
      t.ok(w <= layout.wrap + 0.5, "each option wraps inside the panel")
      t.ok(h <= layout.row_height, "and its row is tall enough for every line")
    end
    local _, single = ui.measure("Ag")
    t.ok(layout.row_height > single * 1.5, "rows grew to hold several lines")
    t.eq(layout.rows, #long, "every option is shown")
    local answer
    local job = task.spawn(function() answer = dialogue.choice(long) end)
    t.tick(1)
    local menu = scene.top()
    t.eq(menu.name, "menu")
    t.eq(menu.width, layout.width)
    t.eq(menu.list:row(), layout.row_height)
    menu:draw()
    press(t, "key:Down")
    press(t, "key:Z")
    t.eq(job.status, "done")
    t.eq(answer, 2)
    config.set("text_scale", 1)
    config.dirty = false
    scene.clear()
  end,

  ["a choice previews the focused option's details and bars"] = function(t)
    input.use_defaults()
    scene.clear()
    local answer
    local job = task.spawn(function()
      answer = dialogue.choice({ "Turn the wheel", "Use the raw line" }, {
        details = { "Slow and safe.", "Fast. Costs exposure." },
        bars = { nil, { { label = "Exposure", value = 60, max = 100 }, { label = "Time", value = 2, max = 8 } } },
      })
    end)
    t.tick(1)
    local preview = scene.find("choice_details")
    t.ok(preview ~= nil, "the preview panel is shown with the choice")
    t.eq(preview.focused, 1)
    preview.draw()
    press(t, "key:Down")
    t.eq(preview.focused, 2, "it follows the focused option")
    preview.draw()
    press(t, "key:X")
    t.eq(job.status, "done")
    t.eq(answer, 2, "cancel picks the last option")
    t.eq(scene.find("choice_details"), nil, "the panel closes with the choice")
    scene.clear()
  end,

  ["auto-advance may wait a reading time computed per line"] = function(t)
    input.use_defaults()
    scene.clear()
    local saved = dialogue.style.auto_delay
    local asked = {}
    dialogue.style.auto_delay = function(line)
      asked[#asked + 1] = line.text
      return dialogue.reading_time(line.text, { cps = 20, min = 0.2, extra = 0 })
    end
    config.set("auto_advance", true)
    local job = task.spawn(function()
      dialogue.say("Guide", "Hi.", { speed = 0 })
      dialogue.say("Guide", "A much longer line that takes a while to read through.", { speed = 0 })
    end)
    t.tick(20)
    t.eq(dialogue.current().line.text, "A much longer line that takes a while to read through.",
      "the short line advanced after its short reading time")
    t.tick(60)
    t.eq(job.status, "running", "the long line waits longer")
    t.wait_until(function() return job.status ~= "running" end, 300)
    t.eq(asked, { "Hi.", "A much longer line that takes a while to read through." })
    t.near(dialogue.reading_time("{color:good}abc{color:reset}", { cps = 3, min = 0, extra = 0 }), 1, 1e-6,
      "markup is not counted")
    dialogue.style.auto_delay = saved
    config.set("auto_advance", false)
    config.dirty = false
    scene.clear()
  end,

  ["with skip_hold the skip input must be held before skipping is offered"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    local field = require("fe.field")
    field.reset()
    field.start("arena", "start")
    cutscene.settings.skip_hold = 1
    local after = false
    local job = task.spawn(function()
      cutscene.play(function()
        task.wait(30)
        after = true
      end, { skippable = true, letterbox = false })
    end)
    t.tick(2)
    press(t, "key:Tab")
    t.tick(2)
    t.ok(scene.top() == field.scene, "a press does not ask")
    fe.input.feed("key:Tab", 1)
    t.tick(30)
    t.ok(cutscene.skip_progress() > 0.3 and cutscene.skip_progress() < 0.7, "the hold shows its progress")
    cutscene.draw_skip_hold(cutscene.skip_progress())
    fe.input.feed("key:Tab", 0)
    t.tick(2)
    t.eq(cutscene.skip_progress(), 0, "releasing starts over")
    fe.input.feed("key:Tab", 1)
    t.wait_until(function() return scene.top() ~= field.scene end, 90, "holding long enough asks")
    fe.input.feed("key:Tab", 0)
    t.tick(1)
    t.eq(scene.top().name, "menu")
    press(t, "key:Z")
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.ok(after, "the skipped scene still ran to its end")
    t.eq(field.locks, 0)
    cutscene.settings.skip_hold = nil
    scene.clear()
  end,

  ["the backlog keeps backlog_size lines and takes logged lines"] = function(t)
    local saved_size, saved_log = dialogue.backlog_size, dialogue.backlog
    dialogue.backlog = {}
    dialogue.backlog_size = 3
    for i = 1, 5 do dialogue.log_line("A", "line " .. i, { kind = "bark" }) end
    t.eq(#dialogue.backlog, 3)
    t.eq(dialogue.backlog[1].text, "line 3")
    t.eq(dialogue.backlog[3].kind, "bark")
    dialogue.backlog_size, dialogue.backlog = saved_size, saved_log
  end,
}

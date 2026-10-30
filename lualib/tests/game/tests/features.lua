-- Kit features: toasts, button glyphs, accessibility and language settings,
-- voice-over, devtools hooks, followers, day/night toggling, stories and the
-- platform-backed screens (with and without the engine services).
local config = require("fe.config")
local data = require("fe.data")
local daynight = require("fe.daynight")
local devtools = require("fe.devtools")
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local field = require("fe.field")
local follower = require("fe.follower")
local game = require("fe.game")
local i18n = require("fe.i18n")
local input = require("fe.input")
local quest = require("fe.quest")
local save = require("fe.save")
local scene = require("fe.scene")
local signal = require("fe.signal")
local sound = require("fe.sound")
local state = require("fe.state")
local story = require("fe.story")
local task = require("fe.task")
local ui = require("fe.ui")

local function fresh()
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  game.reset_time()
  ui.clear_toasts()
end

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

-- Replace fields of an engine table for the duration of `fn`.
local function with_stubs(target, stubs, fn)
  local saved = {}
  for key, value in pairs(stubs) do
    saved[key] = { value = target[key] }
    target[key] = value
  end
  local ok, err = pcall(fn)
  for key, entry in pairs(saved) do target[key] = entry.value end
  if not ok then error(err, 0) end
end

return {
  ["keyed listeners replace each other"] = function(t)
    local sig = signal.new()
    local seen = {}
    sig:connect(function() seen[#seen + 1] = "a" end, 0, "key")
    sig:connect(function() seen[#seen + 1] = "b" end, 0, "key")
    sig:connect(function() seen[#seen + 1] = "c" end)
    sig:emit()
    t.eq(sig:count(), 2)
    t.eq(seen, { "b", "c" })
  end,

  ["toasts queue without waiting and show a few at a time"] = function(t)
    fresh()
    for i = 1, 4 do ui.toast("Toast " .. i, { icon = "glyph.keyboard.z" }) end
    t.tick(2)
    local toasts = ui.toasts()
    t.eq(#toasts, 4)
    t.ok(toasts[3].shown_at ~= nil)
    t.eq(toasts[4].shown_at, nil, "the fourth waits for a free slot")
    scene.draw()
    ui.draw_toasts()
    t.tick(math.ceil(ui.toast_settings.duration * 60) + 2)
    t.eq(#ui.toasts(), 1)
    t.eq(ui.toasts()[1].text, "Toast 4")
    t.ok(ui.toasts()[1].shown_at ~= nil)
    ui.clear_toasts()
  end,

  ["pickups are toasts by default, messages when toasts are off"] = function(t)
    fresh()
    field.give("potion", 2, 30)
    local toasts = ui.toasts()
    t.eq(#toasts, 2)
    t.eq(toasts[1].text, "Found potion x2.")
    t.eq(toasts[2].text, "Found 30 money.")
    ui.clear_toasts()
    field.settings.pickup_toasts = false
    local job = task.spawn(function() field.give("potion", 1) end)
    t.tick(1)
    t.eq(dialogue.current().line.text, "Found potion.")
    job:cancel()
    field.settings.pickup_toasts = true
    t.eq(#ui.toasts(), 0)
    scene.clear()
  end,

  ["prompts show the button glyph for the device in use"] = function(t)
    fresh()
    with_stubs(fe.input, { device_type = function() return "keyboard" end }, function()
      t.eq(ui.prompt("confirm"), "{icon:glyph.keyboard.return}")
      t.eq(ui.prompt("run"), "{icon:glyph.keyboard.left_shift}")
    end)
    with_stubs(fe.input, { device_type = function() return "playstation" end }, function()
      t.eq(ui.prompt("confirm"), "{icon:glyph.playstation.a}")
      t.eq(ui.prompt("page_left"), "{icon:glyph.playstation.leftshoulder}")
    end)
    with_stubs(fe.input, { device_type = function() return "steamdeck" end }, function()
      t.eq(ui.prompt("cancel"), "{icon:glyph.xbox.b}")
    end)
    fe.input.define("screenshot", { "key:F5" })
    with_stubs(fe.input, { device_type = function() return "keyboard" end }, function()
      t.eq(ui.prompt("screenshot"), "F5", "a key without a glyph shows its name")
    end)
    t.ok(ui.prompts({ { "confirm", "OK" } }):find("{icon:glyph", 1, true) ~= nil)
    local w = fe.text.measure("{icon:glyph.xbox.a}")
    t.ok(w > 0, "the glyph is a registered icon")
  end,

  ["text scale and high contrast follow the settings"] = function(t)
    fresh()
    local skin = ui.theme.skin
    config.set("text_scale", 1.5)
    t.eq(ui.text_scale, 1.5)
    t.eq(ui.row_height(), 18)
    t.eq(ui.text_size(), 15)
    local _, small = fe.text.measure("Hello", { size = 10 })
    local _, big = ui.measure("Hello")
    t.ok(big > small, "measured text grows")
    config.set("high_contrast", true)
    t.eq(ui.theme.skin, false)
    t.eq(ui.theme.text, 0xffffffff)
    ui.window(0, 0, 40, 20)
    config.set("high_contrast", false)
    t.eq(ui.theme.skin, skin)
    config.set("text_scale", 1)
    t.eq(ui.row_height(), ui.theme.row_height)
    config.dirty = false
  end,

  ["languages: shipped files, system default and switching"] = function(t)
    t.eq(config.languages(), { "en", "fr" })
    with_stubs(fe.app, { locale = function() return "fr-CA" end }, function()
      t.eq(config.system_language(), "fr")
    end)
    with_stubs(fe.app, { locale = function() return "ja-JP" end }, function()
      t.eq(config.system_language(), "en")
    end)
    with_stubs(fe.app, { locale = function() return "" end }, function()
      t.eq(config.system_language(), "en")
    end)
    local before = config.get("language")
    config.set("language", "fr")
    t.eq(i18n.t("fe.settings.title"), "Options")
    t.eq(i18n.t("fe.field.found_item", { item = "x" }), "Trouvé : x.")
    config.set("language", before or "en")
    t.eq(i18n.t("fe.settings.title"), "Settings")
    config.dirty = false
  end,

  ["the settings screen toggles auto-advance and changes display settings"] = function(t)
    fresh()
    local screen = require("fe.ui.screens.config")
    local job = task.spawn(screen.open)
    t.tick(1)
    for _ = 1, 5 do press(t, "key:Down") end
    press(t, "key:Z")
    t.eq(config.get("auto_advance"), true)
    press(t, "key:Z")
    t.eq(config.get("auto_advance"), false)
    press(t, "key:Escape")
    t.tick(1)
    t.eq(job.status, "done")
    local applied
    local current = { fullscreen = false, width = 640, height = 360, vsync = true, monitor = 1, monitors = { "A", "B" },
      modes = { { width = 640, height = 360 }, { width = 1280, height = 720 } } }
    with_stubs(fe.app, {
      display = function() return current end,
      set_display = function(settings) applied = settings end,
    }, function()
      job = task.spawn(screen.open)
      t.tick(1)
      for _ = 1, 11 do press(t, "key:Down") end
      press(t, "key:Right")
      t.eq(applied.width, 1280)
      t.eq(applied.height, 720)
      t.eq(applied.fullscreen, false)
      press(t, "key:Down")
      press(t, "key:Z")
      t.eq(applied.vsync, false)
      press(t, "key:Escape")
      t.tick(1)
    end)
    t.eq(job.status, "done")
    config.values.display = {}
    config.apply()
  end,

  ["auto-advance waits for voice-over; advancing stops it; speaking is announced"] = function(t)
    fresh()
    local playing, stopped, events = true, {}, {}
    local disconnect = dialogue.speaking:connect(function(speaker, talking) events[#events + 1] = { speaker, talking } end)
    local voiced = {}
    with_stubs(sound, { voice = function(id) voiced[#voiced + 1] = id return 7 end }, function()
      with_stubs(fe.audio, {
        playing = function(voice) return voice == 7 and playing end,
        stop = function(voice) stopped[#stopped + 1] = voice end,
      }, function()
        config.set("auto_advance", true)
        local job = task.spawn(function()
          dialogue.say("Mira", "Listen.", { voice = "vo/mira_01.ogg", speed = 0, mouth = { "a.png", "b.png" } })
        end)
        t.tick(150)
        t.eq(job.status, "running", "auto-advance waits for the voice")
        t.eq(voiced, { "vo/mira_01.ogg" }, "the line plays through sound.voice (catalog ids or paths)")
        t.ok(dialogue.current():talking())
        playing = false
        t.wait_until(function() return job.status ~= "running" end, 120)
        t.eq(events[1], { "Mira", true })
        t.eq(events[#events], { "Mira", false })
        config.set("auto_advance", false)
        playing = true
        stopped = {}
        job = task.spawn(function() dialogue.say("Mira", "Again.", { voice = "vo/mira_02.ogg", speed = 0 }) end)
        t.tick(2)
        press(t, "key:Z")
        t.eq(job.status, "done")
        t.eq(stopped[1], 7, "advancing stops the voice")
      end)
    end)
    disconnect()
    config.dirty = false
    scene.clear()
  end,

  ["holding skip fast-forwards dialogue"] = function(t)
    fresh()
    local job = task.spawn(function()
      dialogue.say(nil, "A long line that would take a while to type out.", { speed = 5 })
      dialogue.say(nil, "And another one.", { speed = 5 })
    end)
    fe.input.feed("key:Left Ctrl", 1)
    t.wait_until(function() return job.status ~= "running" end, 40)
    fe.input.feed("key:Left Ctrl", 0)
    t.tick(1)
    t.eq(job.status, "done")
    scene.clear()
  end,

  ["devtools describe zones and defaults, validate tables and play from a point"] = function(t)
    fresh()
    local described = fe.json.decode(devtools.describe())
    t.ok(described.zones.camera.fields.pitch ~= nil)
    t.eq(described.zones.camera.fields.min.type, "list")
    t.eq(described.defaults.entities, "npc")
    t.eq(described.defaults.zones, "camera")
    t.ok(described.entities.npc ~= nil)
    quest.load("data/quests.json")
    local ok = devtools.validate_table("quests", { { id = "a", name = "A" } })
    t.ok(ok)
    local message
    ok, message = devtools.validate_table("quests", { { id = "a" }, { id = "a", name = "B" } })
    t.ok(not ok)
    t.ok(message:find("name", 1, true) ~= nil, message)
    t.ok(message:find("duplicate", 1, true) ~= nil, message)
    t.ok(data.get("quests", data.list("quests")[1].id) ~= nil, "the loaded table is untouched")
    task.spawn(function() ui.message("A menu in the way") end)
    t.tick(1)
    local job = devtools.play_from("arena", "east")
    t.wait_until(function() return job.status ~= "running" end, 30)
    t.eq(job.status, "done")
    t.eq(scene.find("message"), nil)
    t.eq(scene.top(), field.scene)
    t.near(field.player.position.x, 9.5, 0.01)
    t.ok(field.can_control())
    scene.clear()
  end,

  ["NPC followers join the trail, are saved and become battle companions"] = function(t)
    fresh()
    field.start("arena", "start")
    local probe = require("tests.support.assist_probe")
    probe.calls = {}
    follower.assist_module = "tests.support.assist_probe"
    local joined = {}
    local disconnect = follower.joined:connect(function(id) joined[#joined + 1] = id end)
    local npc = field.world:spawn({ id = "old_man", position = { x = 4.5, z = 5.5 },
      sprite = { animation = "@fe/assets/ui/spark.anim.json" }, props = { name = "Old Man" } })
    follower.join_entity(npc, { assist = { enemy = "sage", skill = "kick", chance = 0.5 } })
    disconnect()
    t.eq(joined, { "old_man" })
    t.ok(not npc:valid(), "the NPC leaves its spot")
    t.eq(#field.followers(), 1)
    t.eq(follower.get("old_man").name, "Old Man")
    t.eq(follower.get("old_man").sprite, "@fe/assets/ui/spark.anim.json")
    t.eq(state.snapshot().followers.list[1].assist.skill, "kick")
    t.eq(probe.calls, { { "join", "old_man", "sage", "kick", "Old Man" } })
    follower.join("girl", { name = "Girl", sprite = "@fe/assets/ui/spark.anim.json" })
    t.ok(not follower.has("old_man"), "one follower at a time by default")
    t.eq(probe.calls[2], { "leave", "old_man" })
    t.eq(#probe.calls, 2, "a follower without assist data stays out of battles")
    t.eq(#field.followers(), 1)
    fe.input.feed("key:Right", 1)
    t.tick(20)
    fe.input.feed("key:Right", 0)
    t.tick(20)
    t.ok(field.followers()[1].position.x > 2.6, "the follower walks the trail")
    follower.leave("girl")
    t.eq(#field.followers(), 0)
    follower.assist_module = "tests.support.no_battle_kit"
    follower.join("pal", { name = "Pal", sprite = "@fe/assets/ui/spark.anim.json", assist = { enemy = "x", skill = "y" } })
    t.ok(follower.has("pal"), "without a battle kit the follower only walks")
    follower.leave("pal")
    follower.assist_module = "tests.support.broken_assist"
    t.error(function()
      follower.join("pal", { name = "Pal", sprite = "@fe/assets/ui/spark.anim.json", assist = { enemy = "x", skill = "y" } })
    end, "the assist module failed to load")
    t.ok(not follower.has("pal"), "a failed join changes nothing")
    follower.assist_module = "fe.battle.assist"
    scene.clear()
  end,

  ["the day/night command switches phase with a fade; the clock stops while paused"] = function(t)
    fresh()
    field.start("arena", "start")
    daynight.set_hour(12)
    t.eq(daynight.phase(), "day")
    fe.input.define("toggle_time", { "key:T" })
    field.command("daynight", { input = "toggle_time", run = function() daynight.toggle() end })
    press(t, "key:T")
    t.ok(field.locks > 0, "control is locked during the switch")
    t.wait_until(function() return field.locks == 0 end, 120)
    t.eq(daynight.hour(), daynight.phases.night)
    t.eq(daynight.phase(), "night")
    t.eq(fe.screen.get().fade[4], 0)
    t.eq(state.snapshot().daynight.minutes, daynight.phases.night * 60, "saved")
    field.commands.daynight = nil
    daynight.start({ minutes_per_second = 60 })
    local release = game.pause(field.world)
    local minutes = state.snapshot().daynight.minutes
    t.tick(10)
    t.eq(state.snapshot().daynight.minutes, minutes)
    release()
    t.tick(10)
    t.ok(state.snapshot().daynight.minutes > minutes)
    daynight.minutes_per_second = 0
    scene.clear()
  end,

  ["holding fast-forward advances a voiced line and cuts the voice"] = function(t)
    fresh()
    local playing = true
    with_stubs(sound, { voice = function() return 9 end }, function()
      with_stubs(fe.audio, {
        playing = function(voice) return voice == 9 and playing end,
        stop = function() playing = false end,
      }, function()
        local job = task.spawn(function() dialogue.say("Mira", "Voiced.", { voice = "vo/x.ogg", speed = 0 }) end)
        fe.input.feed("key:Left Ctrl", 1)
        t.tick(30)
        fe.input.feed("key:Left Ctrl", 0)
        t.tick(1)
        t.eq(job.status, "done")
        t.ok(not playing, "the voice-over stopped")
      end)
    end)
    scene.clear()
  end,

  ["a cancelled day/night switch uncovers the screen"] = function(t)
    fresh()
    field.start("arena", "start")
    daynight.set_hour(12)
    local job = task.spawn(function() daynight.toggle() end)
    t.tick(5)
    t.ok(fe.screen.get().fade[4] > 0)
    job:cancel()
    t.eq(fe.screen.get().fade[4], 0)
    scene.clear()
  end,

  ["chapters belong to fe.story; another protagonist's chapter can end; the leader is a hook"] = function(t)
    fresh()
    story.define("cy", { name = "Cy", actor = "cy", chapters = { { title = "One" }, { title = "Two" } } })
    story.define("di", { name = "Di", chapters = { { title = "Only" } } })
    local original_member, original_lead = story.has_member, story.lead_with
    local led
    story.has_member = function() return true end
    story.lead_with = function(actor) led = actor end
    local seen = {}
    local disconnect = state.changed:connect(function(kind, id, what)
      if kind == "story" then seen[#seen + 1] = id .. ":" .. what end
    end)
    story.recruit("cy")
    story.recruit("di")
    story.begin("cy")
    t.eq(led, "cy", "story.lead_with puts the protagonist first")
    t.eq({ story.finish("di") }, { "di", 1 })
    t.eq(story.current(), "cy", "Cy's chapter is still being played")
    t.eq(story.chapter("di"), 2)
    t.eq(story.finish("di"), nil, "Di has no chapter left")
    t.eq({ story.finish() }, { "cy", 1 })
    t.eq(story.current(), nil)
    disconnect()
    t.eq(seen, { "cy:recruited", "di:recruited", "cy:started", "di:finished", "cy:finished" })
    t.eq(quest.chapter, nil, "fe.quest keeps no chapters of its own")
    story.has_member, story.lead_with = original_member, original_lead
    story.protagonists.cy, story.protagonists.di = nil, nil
    story.order = {}
  end,

  ["stories track chapters per protagonist with requirements"] = function(t)
    fresh()
    story.define("ada", { name = "Ada", chapters = { { title = "Dawn" }, { title = "Dusk", requires = { flags = { "bridge" } } } } })
    story.define("bo", { name = "Bo", actor = "bo", chapters = { { title = "Sea", requires = { chapters = { ada = 1 } } } } })
    local members = {}
    local original = story.has_member
    story.has_member = function(actor) return members[actor] == true end
    t.eq(#story.available(), 0, "nobody recruited yet")
    story.recruit("ada")
    story.recruit("bo")
    local available = story.available()
    t.eq(#available, 2)
    t.ok(available[1].ok)
    t.ok(not available[2].ok)
    t.eq(#available[2].reasons, 2, "Bo must be in the party and needs Ada's chapter 1")
    story.begin("ada")
    t.eq(story.current(), "ada")
    t.ok(event.matches({ when = { chapter = { ada = 1 }, playing = "ada" } }, {}))
    t.eq(story.finish(), "ada")
    t.eq(story.current(), nil)
    t.eq(story.chapter("ada"), 2)
    t.ok(not story.can_start("ada"), "Dusk needs the bridge")
    state.set_flag("bridge")
    members.bo = true
    t.ok(story.can_start("ada"))
    t.ok(story.can_start("bo"))
    local snapshot = state.snapshot()
    state.reset()
    t.eq(story.chapter("ada"), 1)
    state.restore(snapshot)
    t.eq(story.chapter("ada"), 2)
    local chosen
    local job = task.spawn(function() chosen = story.open() end)
    t.tick(1)
    press(t, "key:Down")
    press(t, "key:Z")
    t.tick(1)
    t.eq(job.status, "done")
    t.eq(chosen, "bo")
    story.has_member = original
    story.protagonists.ada, story.protagonists.bo = nil, nil
    story.order = {}
    scene.clear()
  end,

  ["save screens show thumbnails and local dates"] = function(t)
    fresh()
    local screen = require("fe.ui.screens.save")
    t.eq(screen.date(nil), "", "no time saved")
    t.eq(screen.thumbnail("slot1"), nil)
    save.write("slot1")
    with_stubs(fe.time, { local_date = function()
      return { year = 2026, month = 3, day = 4, hour = 5, min = 6, sec = 7, weekday = 3, utc_offset = 0 }
    end }, function()
      with_stubs(fe.save, { thumbnail_path = function(slot) return slot == "slot1" and "@fe/assets/ui/window.png" or nil end },
        function()
          t.eq(screen.date(1000), "2026-03-04 05:06")
          local job = task.spawn(function() screen.open("load") end)
          t.tick(1)
          local before = fe.canvas.quad_count()
          scene.draw()
          t.ok(fe.canvas.quad_count() > before)
          press(t, "key:Escape")
          t.eq(job.status, "done")
        end)
    end)
    save.delete("slot1")
  end,

  ["name entry takes typed text and IME input"] = function(t)
    fresh()
    local typed, composition, pressed = {}, "", {}
    local began, ended = false, false
    with_stubs(fe.input, {
      text_begin = function(rect) began = rect.w ~= nil end,
      text_end = function() ended = true end,
      text = function() return table.remove(typed, 1) or "" end,
      composition = function() return composition, 0 end,
      pressed_inputs = function() return pressed end,
    }, function()
      local name
      local job = task.spawn(function() name = require("fe.ui.screens.name_entry").open("", 5) end)
      t.tick(1)
      t.ok(began)
      typed = { "Jo" }
      t.tick(2)
      composition = "ア"
      t.tick(2)
      composition = ""
      typed = { "アキ" }
      t.tick(2)
      typed = { "zzzz" }
      t.tick(2)
      pressed = { "key:Backspace" }
      t.tick(1)
      pressed = { "key:Return" }
      t.tick(2)
      pressed = {}
      t.eq(job.status, "done")
      t.eq(name, "Joアキ", "five characters at most, then one deleted")
      t.ok(ended, "text input ends with the screen")
    end)
  end,

  ["a party change refreshes the followers and the leader's sprite"] = function(t)
    fresh()
    local party = require("fe.rpg.party")
    local followers = require("fe.field.followers")
    local sprites = { "@fe/assets/ui/spark.anim.json" }
    local leader = "@fe/assets/ui/emotes.anim.json"
    local saved = { field.follower_sprites, field.settings.followers, field.settings.player_sprite }
    field.follower_sprites = function() return sprites end
    field.settings.followers = 2
    field.settings.player_sprite = function() return leader end
    field.start("arena", "start")
    t.ok(followers.watch_party(party), "the party module handed over is watched")
    t.ok(followers.watch_party(), "and stays watched without an argument")
    t.eq(#field.followers(), 1)
    t.eq(field.player:sprite().animation, leader)
    sprites = { "@fe/assets/ui/spark.anim.json", "@fe/assets/ui/spark.anim.json" }
    leader = "@fe/assets/ui/spark.anim.json"
    party.changed:emit()
    t.eq(#field.followers(), 2, "the trail follows the new line-up")
    t.eq(field.player:sprite().animation, leader, "the leader's sprite is re-read")
    field.follower_sprites, field.settings.followers, field.settings.player_sprite = saved[1], saved[2], saved[3]
    scene.clear()
  end,

  ["finished chapters can be replayed without touching the record"] = function(t)
    fresh()
    local runs = {}
    story.define("ed", { name = "Ed", chapters = { { title = "One", script = "tests.support.chapter" }, { title = "Two" } } })
    local original = story.has_member
    story.has_member = function() return true end
    local chapter_script = require("tests.support.chapter")
    chapter_script.calls = runs
    story.recruit("ed")
    t.error(function() story.replay("ed", 1) end, "has not finished")
    story.begin("ed")
    t.eq(runs[1], { "ed", "One", nil })
    story.finish()
    t.eq(story.chapter("ed"), 2)
    local listed = story.available({ include_done = true })
    t.eq(#listed, 2)
    t.eq(listed[1].number, 1)
    t.ok(listed[1].done)
    t.eq(listed[2].number, 2)
    t.eq(#story.available(), 1, "finished chapters are hidden by default")
    story.replay("ed", 1)
    t.eq(runs[2], { "ed", "One", true }, "the script runs again, told it is a replay")
    t.eq(story.current(), nil, "nothing is being played")
    t.eq(story.chapter("ed"), 2, "progress is untouched")
    t.eq(story.finish(), nil)
    local chosen, number
    local job = task.spawn(function() chosen, number = story.open({ include_done = true }) end)
    t.tick(1)
    press(t, "key:Z")
    t.tick(1)
    t.eq(job.status, "done")
    t.eq({ chosen, number }, { "ed", 1 }, "the select screen offers the finished chapter first")
    story.has_member = original
    story.protagonists.ed = nil
    story.order = {}
    scene.clear()
  end,

  ["the world map knows where the party is"] = function(t)
    fresh()
    local worldmap = require("fe.worldmap")
    worldmap.definition = nil
    local saved = field.world
    field.world = nil
    t.eq(worldmap.here(), nil, "no field yet")
    field.world = saved
    field.start("arena", "start")
    t.eq(worldmap.here().id, "arena")
    t.eq({ input.prompt("confirm") }, { "Return" }, "prompts return one value")
    scene.clear()
  end,

  ["warps load the map in the background with a loading indicator"] = function(t)
    fresh()
    field.start("arena", "start")
    local progress, requested = 0, nil
    local saw_indicator = false
    with_stubs(fe.world, {
      load_async = function(name, map)
        requested = { name, map }
        return 1
      end,
      load_progress = function()
        progress = progress + 0.02
        saw_indicator = saw_indicator or scene.find("loading") ~= nil
        return progress
      end,
      finish_load = function() return fe.world.load(requested[1], requested[2]) end,
    }, function()
      local job = task.spawn(function() field.warp("arena", "east", { autosave = false }) end)
      t.wait_until(function() return job.status ~= "running" end, 300)
      t.eq(job.status, "done")
    end)
    t.eq(requested, { "field", "arena" })
    t.ok(saw_indicator, "the indicator shows when loading takes a while")
    t.eq(scene.find("loading"), nil)
    t.near(field.player.position.x, 9.5, 0.01)
    scene.clear()
  end,

  ["a cancelled warp cancels its background load"] = function(t)
    fresh()
    field.start("arena", "start")
    local requested, cancelled, finished = false, nil, false
    with_stubs(fe.world, {
      load_async = function() requested = true; return 7 end,
      load_progress = function() return 0 end,
      finish_load = function() finished = true end,
      cancel_load = function(ticket) cancelled = ticket end,
    }, function()
      local job = task.spawn(function() field.warp("arena", "east", { autosave = false }) end)
      t.wait_until(function() return requested end, 300)
      t.eq(cancelled, nil)
      job:cancel()
      t.eq(job.status, "cancelled")
    end)
    t.eq(cancelled, 7, "the ticket is dropped with the task")
    t.ok(not finished)
    scene.clear()
    -- Against the engine: a cancelled ticket is unknown from then on.
    local ticket = fe.world.load_async("scratch", "arena")
    fe.world.cancel_load(ticket)
    t.error(function() fe.world.load_progress(ticket) end, "Unknown world load")
    t.error(function() fe.world.finish_load(ticket) end, "Unknown world load")
    t.error(function() fe.world.cancel_load(ticket) end, "Unknown world load")
    t.eq(fe.world.get("scratch"), nil)
  end,
}

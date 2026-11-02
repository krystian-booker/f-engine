-- fe.bark: priority classes, the global gap, cooldowns, scope caps,
-- availability, suspension and interruption by dialogue lines.
local bark = require("fe.bark")
local dialogue = require("fe.dialogue")
local scene = require("fe.scene")
local task = require("fe.task")

-- A controllable bark clock.
local clock = { now = 0 }
local real_now = bark.now

local function setup()
  require("fe.input").use_defaults()
  scene.clear()
  bark.reset()
  bark.resume_all()
  clock.now = 0
  bark.now = function() return clock.now end
  bark.available = function() return true end
  bark.define({
    { id = "warn", speaker = "rowan", class = "warning", text = "Behind you!" },
    { id = "ko", speaker = "sera", class = "ko", text = "Down!" },
    { id = "fray", speaker = "tovan", class = "fraying", text = "Steady..." },
    { id = "flavor_a", speaker = "caelan", class = "flavor", text = "Nice weather." },
    { id = "flavor_b", speaker = "sera", class = "flavor", text = "Quiet today." },
    { id = "flavor_c", speaker = "tovan", class = "flavor", text = "Long road." },
    { id = "idle", speaker = "caelan", class = "incidental", text = "Hm." },
    { id = "again", speaker = "lio", class = "incidental", text = "Again!", cooldown = 10 },
  })
end

local function teardown()
  bark.reset()
  bark.resume_all()
  bark.now = real_now
  bark.available = function() return true end
end

return {
  ["barks keep a global gap and per-speaker cooldowns"] = function(t)
    setup()
    t.ok(bark.play("flavor_a"))
    t.eq(bark.current().record.id, "flavor_a")
    local ok, why = bark.play("flavor_b")
    t.eq(ok, false)
    t.eq(why, "gap", "another bark within the gap is refused")
    clock.now = 6
    t.ok(bark.play("flavor_b"), "the gap is 6 seconds from the previous start")
    clock.now = 12
    ok, why = bark.play("flavor_a")
    t.eq(why, "speaker", "caelan barked 12 s ago: inside his 15 s cooldown")
    clock.now = 15.5
    t.ok(bark.play("flavor_a"), "after the cooldown he may bark again")
    t.eq(bark.play("missing"), false)
    teardown()
  end,

  ["a higher class cuts a lower bark on screen, which never resumes"] = function(t)
    setup()
    t.ok(bark.play("flavor_a"))
    clock.now = 1
    t.ok(bark.play("warn"), "a warning cuts flavor inside the gap")
    t.eq(bark.current().record.id, "warn")
    clock.now = 2
    local ok, why = bark.play("ko")
    t.eq(why, "gap", "a lower class does not cut a warning")
    clock.now = 30
    bark.update()
    t.eq(bark.current(), nil, "the warning ended")
    t.ok(bark.play("ko"))
    t.eq(bark.current().record.id, "ko", "the cut flavor line is not resumed")
    teardown()
  end,

  ["scope caps limit classes per battle; force and reset_scope"] = function(t)
    setup()
    local played = 0
    for i, id in ipairs({ "flavor_a", "flavor_b", "flavor_c" }) do
      clock.now = i * 20
      if bark.play(id, { scope = "battle" }) then played = played + 1 end
    end
    t.eq(played, 2, "flavor plays at most twice per scope")
    clock.now = 100
    t.ok(bark.play("warn", { scope = "battle" }))
    clock.now = 120
    local ok, why = bark.play("warn", { scope = "battle" })
    t.eq(why, "cap", "a warning once per scope")
    t.ok(bark.play("warn", { scope = "other" }), "another scope has its own counts")
    clock.now = 140
    bark.reset_scope("battle")
    t.ok(bark.play("flavor_c", { scope = "battle" }), "a reset scope counts from zero")
    clock.now = 141
    t.ok(bark.play("flavor_a", { force = true }), "force skips gap, cooldowns and caps")
    teardown()
  end,

  ["repeat cooldowns, availability and queued barks"] = function(t)
    setup()
    t.ok(bark.play("again"))
    clock.now = 6
    local ok, why = bark.play("again")
    t.eq(why, "repeat", "the record's 10 s repeat cooldown")
    bark.cooldown("idle", 30)
    clock.now = 20
    t.ok(bark.play("idle"))
    clock.now = 40
    ok, why = bark.play("idle")
    t.eq(why, "repeat", "bark.cooldown set 30 s")
    bark.available = function(speaker) return speaker ~= "sera" end
    clock.now = 60
    ok, why = bark.play("ko")
    t.eq(why, "unavailable")
    bark.available = function() return true end
    t.ok(bark.play("warn"))
    clock.now = 61
    ok, why = bark.play("flavor_c", { queue = 10 })
    t.eq(why, "queued", "a lower class waits for the gap when asked to queue")
    ok, why = bark.play("fray", { queue = 10 })
    t.eq(why, "queued")
    clock.now = 65
    bark.update()
    t.eq(bark.current(), nil, "nothing starts inside the gap, even after the warning ended")
    clock.now = 66.5
    bark.update()
    t.eq(bark.current().record.id, "fray", "the highest queued class plays once the gap opens")
    t.eq(#bark.queued(), 1)
    clock.now = 80
    bark.update()
    t.eq(#bark.queued(), 0, "a queued bark expires after its queue time")
    teardown()
  end,

  ["suspension drops barks; a dialogue line interrupts incidental barks"] = function(t)
    setup()
    t.ok(bark.play("idle"))
    local release = bark.suspend()
    t.eq(bark.current(), nil, "suspending ends the bark on screen")
    clock.now = 50
    local ok, why = bark.play("warn")
    t.eq(why, "suspended")
    bark.play("flavor_a", { queue = 5 })
    t.eq(#bark.queued(), 0, "nothing queues while suspended")
    release()
    t.ok(bark.play("idle"))
    local job = task.spawn(function() dialogue.say("Guide", "Listen.", { speed = 0 }) end)
    t.tick(2)
    t.eq(bark.current(), nil, "a shown line cut the incidental bark")
    clock.now = 100
    t.ok(bark.play("flavor_a"))
    dialogue.finish_line(job)
    t.tick(2)
    local job2 = task.spawn(function() dialogue.say("Guide", "Again.", { speed = 0 }) end)
    t.tick(2)
    t.eq(bark.current().record.id, "flavor_a", "flavor barks are not cut by dialogue by default")
    dialogue.finish_line(job2)
    t.tick(2)
    local last = dialogue.backlog[#dialogue.backlog - 1]
    t.eq(last.kind, "bark", "barks reach the backlog")
    bark.draw()
    teardown()
    scene.clear()
  end,

  ["a suspension ends with its task; bark records are validated"] = function(t)
    setup()
    local job = task.spawn(function()
      bark.suspend()
      task.wait(10)
    end)
    t.ok(bark.suspended())
    job:cancel()
    t.ok(not bark.suspended())
    t.error(function() bark.define({ { id = "bad" } }) end, "speaker")
    t.ok(bark.duration("A fairly long line of bark text to read aloud.") > bark.settings.min_duration)
    teardown()
  end,
}

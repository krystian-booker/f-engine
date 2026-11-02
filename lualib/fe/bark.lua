--- Barks: short lines characters say while the player keeps control
--- (battle callouts, exploration remarks), from a finite bank with rules
--- that keep them sparse.
---
--- ```lua
--- bark.define({
---   {id = "b_warn", speaker = "rowan", class = "warning", text_key = "barks.warn", voice = "b_warn"},
---   {id = "b_idle", speaker = "caelan", class = "flavor", text_key = "barks.idle"},
--- })                                         -- or bark.load() for data/barks.json
--- bark.play("b_warn", {scope = "battle:e01"}) -- true when it played
--- local release = bark.suspend()             -- cinematics: nothing plays
--- ```
--- Records: `{id, speaker, class, text_key, voice?, text?, cooldown?}`
--- (`text` is literal text used when there is no `text_key`; `cooldown` is
--- the record's repeat cooldown in seconds).
---
--- Rules, checked by `bark.play(id, {scope?, entity?, force?, queue?})`,
--- which returns true when the bark plays, else false and the reason:
---   * priority classes: `settings.priority` (warning > ko > fraying >
---     flavor > incidental by default); a bark of a higher class cuts a
---     lower one on screen, which never resumes;
---   * a global gap: `settings.gap` seconds (6) from the start of one bark
---     to the start of the next;
---   * a per-speaker cooldown, `settings.speaker_cooldown` (15);
---   * repeat cooldowns per id: `bark.cooldown(id, seconds)` (seconds
---     default `settings.repeat_cooldown`, 10) or the record's `cooldown`;
---   * per-scope caps, `settings.caps` (`{flavor = 2, warning = 1, ko = 1,
---     fraying = 1}`): how often each class plays per scope id (a battle);
---     barks without a scope are not capped; `bark.reset_scope(scope)`;
---   * `bark.available(speaker, record)`, replaceable (alive, present);
---   * `bark.suspend()` returns a release: while any is held nothing plays,
---     the bark on screen ends and queued barks are dropped;
---   * `queue = seconds` keeps a bark the gap refused for up to that long
---     and plays it when the gap opens (highest class first);
---   * `force = true` skips the gap, cooldowns and caps (not suspension).
--- `bark.interrupt(class?)` ends the bark on screen and drops the queue
--- (only barks of that class or lower, when given). A dialogue line shown
--- with fe.dialogue interrupts barks of `settings.dialogue_interrupts`
--- ("incidental") or lower.
---
--- Presentation: `bark.show(record, text, options)` (replaceable) starts
--- it and returns how long it stays: the default plays the record's voice
--- (`fe.sound.voice`), shows a subtitle strip (speaker name and text) for
--- the reading time (`bark.duration(text)`, `settings.min_duration`,
--- `settings.reading_speed`) and adds the line to `dialogue.backlog`;
--- `bark.hide(current)` ends it early. `bark.speaker_name(speaker)` names
--- the speaker (an i18n key or the id). `bark.played` fires (record, text,
--- options) for every bark that plays. `bark.now()` is the clock (game
--- time; tests may replace it).
local data = require("fe.data")
local dialogue = require("fe.dialogue")
local game = require("fe.game")
local i18n = require("fe.i18n")
local schema = require("fe.schema")
local signal = require("fe.signal")
local tablex = require("fe.tablex")
local task = require("fe.task")

local bark = {
  settings = {
    priority = { warning = 5, ko = 4, fraying = 3, flavor = 2, incidental = 1 },
    gap = 6,                  -- seconds between the starts of two barks
    speaker_cooldown = 15,    -- seconds before a speaker barks again
    repeat_cooldown = 10,     -- default of bark.cooldown
    caps = { flavor = 2, warning = 1, ko = 1, fraying = 1 }, -- per scope
    dialogue_interrupts = "incidental", -- a dialogue line cuts barks of this class or lower (false: none)
    min_duration = 2,         -- shortest time a bark stays on screen
    reading_speed = 15,       -- characters per second of reading time
    hold = 0.35,              -- extra seconds after the reading time
    width = 260,              -- subtitle strip width
    margin = 72,              -- distance of the strip from the bottom of the canvas
  },
  --- Emitted (record, text, options) when a bark plays.
  played = signal.new(),
}

--- The file's settings, for `__reload`.
bark.file_defaults = { settings = tablex.deep_copy(bark.settings) }

--- Adopt edited settings on hot reload (values the game changed at runtime
--- are kept).
function bark.__reload(live, fresh)
  live.file_defaults.settings = tablex.adopt(live.settings, fresh.settings, live.file_defaults.settings)
end

schema.define("bark", {
  id = "string",
  speaker = "string",
  class = { type = "string", default = "flavor", doc = "warning, ko, fraying, flavor, incidental or a class of settings.priority" },
  text_key = { type = "string", optional = true, doc = "i18n key of the line" },
  text = { type = "string", optional = true, doc = "Literal text when there is no text_key" },
  voice = { type = "string", optional = true, doc = "Voice id or file (fe.sound.voice)" },
  cooldown = { type = "number", optional = true, min = 0, doc = "Seconds before this bark repeats" },
})

-- Kept across hot reload: records by id, timers, the bark on screen, the
-- queue and the suspensions.
local live = {
  records = {}, last_start = nil, speakers = {}, repeats = {}, cooldowns = {}, scopes = {},
  current = nil, queue = {}, holds = {}, suspended = 0,
}

--- Seconds on the bark clock (game time).
---@return number
function bark.now()
  return task.time()
end

--- Add records (replacing ones with the same id). Each is checked against
--- the "bark" schema; raises on the first problem.
---@param list table[]
function bark.define(list)
  for _, record in ipairs(list) do
    local ok, errors = schema.validate("bark", record, "bark." .. tostring(record.id))
    if not ok then error(table.concat(errors, "\n"), 2) end
    live.records[record.id] = record
  end
end

--- Load records from a JSON file (default data/barks.json, when it exists)
--- into the "barks" data table and define them.
---@param path? string
function bark.load(path)
  path = path or "data/barks.json"
  if not fe.fs.exists(path) then return end
  data.load("barks", path, "bark")
  bark.define(data.list("barks"))
end

--- A record by id, or nil.
---@param id string
---@return table?
function bark.get(id)
  return live.records[id]
end

--- Forget timers, caps, the queue and the bark on screen (new game, tests).
function bark.reset()
  if live.current then bark.hide(live.current) end
  live.current = nil
  live.last_start = nil
  live.speakers, live.repeats, live.scopes, live.queue = {}, {}, {}, {}
end

--- Whether a speaker may bark now (alive, in the party, on stage).
--- Replace it; the default allows everyone.
---@param speaker string
---@param record table
---@return boolean
function bark.available(speaker, record)
  return true
end

--- The name shown for a speaker: its i18n string (`speaker` as a key), or
--- the id itself.
---@param speaker string
---@return string
function bark.speaker_name(speaker)
  return i18n.t(speaker)
end

--- How long a bark's text stays on screen.
---@param text string
---@return number
function bark.duration(text)
  local s = bark.settings
  return dialogue.reading_time(text, { cps = s.reading_speed, min = s.min_duration, extra = s.hold })
end

--- Set the repeat cooldown of a bark id: after it plays it cannot play
--- again for `seconds` (default `settings.repeat_cooldown`; 0 removes it).
---@param id string
---@param seconds? number
function bark.cooldown(id, seconds)
  seconds = seconds or bark.settings.repeat_cooldown
  live.cooldowns[id] = seconds > 0 and seconds or nil
end

--- Forget how many barks played in a scope (a battle that starts again).
---@param scope string
function bark.reset_scope(scope)
  live.scopes[scope] = nil
end

local function priority(class)
  return bark.settings.priority[class] or 0
end

local function text_of(record)
  if record.text_key then return i18n.t(record.text_key) end
  return record.text or record.id
end

--- The bark on screen: {record, text, options, started, ends}, or nil.
---@return table?
function bark.current()
  local current = live.current
  if current and bark.now() >= current.ends then
    live.current = nil
    return nil
  end
  return current
end

--- Whether barks are suspended.
---@return boolean
function bark.suspended()
  return live.suspended > 0
end

--- Barks waiting for the gap ({id, options, until_time}), oldest first.
---@return table[]
function bark.queued()
  return live.queue
end

--- End the bark on screen and drop queued barks; with `class`, only when
--- the bark's class is that one or lower (queued barks likewise).
---@param class? string
function bark.interrupt(class)
  local limit = class and priority(class) or math.huge
  local current = bark.current()
  if current and priority(current.record.class) <= limit then
    bark.hide(current)
    live.current = nil
  end
  for i = #live.queue, 1, -1 do
    local record = live.records[live.queue[i].id]
    if not record or priority(record.class) <= limit then table.remove(live.queue, i) end
  end
end

--- Stop barks until the returned function is called (or the calling task
--- ends): the bark on screen ends and queued barks are dropped.
---@return fun()
function bark.suspend()
  local token = {}
  live.holds[token] = true
  live.suspended = live.suspended + 1
  bark.interrupt()
  return task.cleanup(function()
    if live.holds[token] then
      live.holds[token] = nil
      live.suspended = live.suspended - 1
    end
  end)
end

--- Drop every suspension (new game, loading a save).
function bark.resume_all()
  live.holds = {}
  live.suspended = 0
end

-- Why `record` may not play now, the gap aside (nil when it may).
local function refusal(record, options, now)
  if live.suspended > 0 then return "suspended" end
  if not bark.available(record.speaker, record) then return "unavailable" end
  if options.force then return nil end
  local repeat_until = live.repeats[record.id]
  if repeat_until and now < repeat_until then return "repeat" end
  local speaker_until = live.speakers[record.speaker]
  if speaker_until and now < speaker_until then return "speaker" end
  if options.scope then
    local cap = bark.settings.caps[record.class]
    local counts = live.scopes[options.scope]
    if cap and counts and (counts[record.class] or 0) >= cap then return "cap" end
  end
  return nil
end

local function gap_open(record, now)
  if not live.last_start or now >= live.last_start + bark.settings.gap then return true end
  local current = bark.current()
  return current ~= nil and priority(record.class) > priority(current.record.class)
end

local function start(record, options, now)
  local current = bark.current()
  if current then bark.hide(current) end
  local text = text_of(record)
  live.last_start = now
  live.speakers[record.speaker] = now + bark.settings.speaker_cooldown
  local repeat_for = record.cooldown or live.cooldowns[record.id]
  if repeat_for then live.repeats[record.id] = now + repeat_for end
  if options.scope then
    local counts = live.scopes[options.scope] or {}
    live.scopes[options.scope] = counts
    counts[record.class] = (counts[record.class] or 0) + 1
  end
  local entry = { record = record, text = text, options = options, started = now }
  live.current = entry
  local duration = bark.show(record, text, options) or bark.duration(text)
  entry.ends = now + duration
  bark.played:emit(record, text, options)
  return true
end

--- Play a bark if the rules allow it (see the header). Returns true, or
--- false and why: "unknown", "suspended", "unavailable", "repeat",
--- "speaker", "cap", "gap" (or "queued" when `queue` kept it).
---@param id string
---@param options? {scope?: string, entity?: Entity, force?: boolean, queue?: number}
---@return boolean, string?
function bark.play(id, options)
  options = options or {}
  local record = live.records[id]
  if not record then return false, "unknown" end
  local now = bark.now()
  local why = refusal(record, options, now)
  if why then return false, why end
  if not options.force and not gap_open(record, now) then
    if options.queue and options.queue > 0 then
      live.queue[#live.queue + 1] = { id = id, options = options, until_time = now + options.queue }
      return false, "queued"
    end
    return false, "gap"
  end
  return start(record, options, now)
end

--- Expire the bark on screen and play queued barks once the gap opens,
--- highest class first (fe.game runs this every tick).
function bark.update()
  bark.current()
  if #live.queue == 0 then return end
  local now = bark.now()
  for i = #live.queue, 1, -1 do
    if now > live.queue[i].until_time then table.remove(live.queue, i) end
  end
  if live.suspended > 0 then
    live.queue = {}
    return
  end
  local best, best_index
  for i, entry in ipairs(live.queue) do
    local record = live.records[entry.id]
    if record and (not best or priority(record.class) > priority(best.class)) then best, best_index = record, i end
  end
  if not best or not gap_open(best, now) then return end
  local entry = table.remove(live.queue, best_index)
  local options = tablex.copy(entry.options)
  options.queue = nil
  if not refusal(best, options, now) then start(best, options, now) end
end

--- Present a bark (replace to restyle): plays the record's voice, shows
--- the subtitle strip and writes the line to the backlog. Returns how long
--- it stays on screen.
---@param record table
---@param text string
---@param options table
---@return number
function bark.show(record, text, options)
  if record.voice then
    local ok, voice = pcall(require("fe.sound").voice, record.voice)
    if ok and live.current then live.current.voice = voice end
  end
  dialogue.log_line(bark.speaker_name(record.speaker), text, { kind = "bark", id = record.id })
  return bark.duration(text)
end

--- End a bark early (interrupted or replaced): stops its voice.
---@param current table the entry `bark.current()` returned
function bark.hide(current)
  if current.voice then pcall(fe.audio.stop, current.voice) end
end

--- Draw the subtitle strip of the bark on screen (fe.game's `drawn`);
--- replace to restyle.
function bark.draw()
  local current = bark.current()
  if not current then return end
  local ui = require("fe.ui")
  local s = bark.settings
  local cw, ch = fe.canvas.size()
  local w = math.min(s.width, cw - 16)
  local name = bark.speaker_name(current.record.speaker)
  local markup = "{color:heading}" .. name .. "{color:reset}  " .. current.text
  local _, th = ui.measure(markup, { width = w - ui.theme.padding * 2 })
  local h = th + ui.theme.padding * 2
  local x = math.floor((cw - w) / 2)
  local y = ch - s.margin - h
  ui.window(x, y, w, h, { alpha = 0.85 })
  ui.text(markup, x + ui.theme.padding, y + ui.theme.padding, { width = w - ui.theme.padding * 2 })
end

game.updated:connect(function() bark.update() end, 0, "fe.bark.update")
game.drawn:connect(function() bark.draw() end, 0, "fe.bark.draw")
dialogue.line_shown:connect(function()
  local class = bark.settings.dialogue_interrupts
  if class then bark.interrupt(class) end
end, 0, "fe.bark.dialogue")

return bark

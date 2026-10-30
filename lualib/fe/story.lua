--- Multi-protagonist stories (Octopath style): protagonists with their own
--- chapters, requirements for each chapter, the chapter being played, and
--- a chapter select screen.
---
--- ```lua
--- story.define("rook", {
---   name = "Rook", actor = "rook",
---   chapters = {
---     {title = "The Hunter's Oath", map = "riverford", point = "start", script = "events.rook_1"},
---     {title = "Cliffs of Ash", level = 8, requires = {party = {"selene"}, flags = {"bridge_fixed"}}},
---   },
--- })
--- story.recruit("rook")
--- story.begin("rook")        -- in a task: warps and runs the chapter's script
--- story.finish()             -- the chapter being played is done
--- story.finish("selene")     -- selene's next chapter is done (not being played)
--- local id = story.open()    -- chapter select; nil when cancelled
--- story.replay("rook", 1)    -- play a finished chapter again (a flashback, NG+)
--- ```
--- `story.load()` reads the same definitions from data/story.json
--- (`[{"id": "rook", "name": ..., "chapters": [...]}]`).
---
--- Chapters: `title`, optional `map` and `point` (where it starts),
--- `script` (a module returning `function(chapter, id)`, run by `begin`),
--- `level` (recommended, shown on the select screen) and `requires`:
--- `party` (actor ids in the active party), `flags` (state flags) and
--- `chapters` ({protagonist = chapters finished}). A protagonist with an
--- `actor` needs that actor in the party (`story.has_member`) and leads it
--- while playing (`story.lead_with`); both use fe.rpg's party by default,
--- and games without it replace them.
---
--- Finished chapters can be played again: `story.replay(id, number)` warps
--- to the chapter and runs its script without touching the record (nothing
--- is "current", `story.finish` has nothing to finish), and
--- `story.available{include_done = true}` lists finished chapters too
--- (marked `done`), which `story.open`/`story.select` offer with the same
--- option and replay when chosen.
---
--- Progress is saved in the "story" state section: who is recruited, how
--- many chapters each has finished, and the chapter being played. Event
--- pages can test `{"chapter": {"rook": 2}}` (the chapter rook plays next,
--- or an fe.event comparison such as ">= 2") and `{"playing": "rook"}`.
--- This module is the only record of chapters. Signals: `chapter_started`
--- and `chapter_finished` (id, number); both also emit
--- `fe.state.changed("story", id, "started" | "finished")`, and recruiting
--- emits `("story", id, "recruited")`.
local data = require("fe.data")
local i18n = require("fe.i18n")
local schema = require("fe.schema")
local signal = require("fe.signal")
local state = require("fe.state")

local story = {
  protagonists = {},
  order = {},
  chapter_started = signal.new(),
  chapter_finished = signal.new(),
}

local chapter_spec = { record = {
  title = "string",
  map = { type = "string", optional = true },
  point = { type = "string", optional = true },
  script = { type = "string", optional = true },
  level = { type = "integer", optional = true },
  requires = { type = { record = {
    party = { type = { list = "string" }, optional = true },
    flags = { type = { list = "string" }, optional = true },
    chapters = { type = { map = "integer" }, optional = true },
  } }, optional = true },
} }

schema.define("story", {
  id = "string",
  name = "string",
  actor = { type = "string", optional = true, doc = "Party member who is this protagonist" },
  chapters = { type = { list = chapter_spec } },
}, { doc = "A protagonist and their chapters." })

i18n.defaults({ fe = { story = {
  title = "Chapters", chapter = "Chapter {number}", level = "Lv {level}", done = "Completed", replay = "Replay",
  needs_member = "{name} must be in the party", needs_flag = "Not yet", needs_chapter = "Finish {name}'s chapter {number}",
  none = "(No chapters available)",
} } })

local record = state.section("story", function() return { protagonists = {}, current = {} } end)

local function progress(id)
  local p = record.protagonists[id]
  if not p then
    p = { recruited = false, finished = 0 }
    record.protagonists[id] = p
  end
  return p
end

--- Define (or replace) a protagonist.
---@param id string
---@param spec {name: string, actor?: string, chapters: table[]}
function story.define(id, spec)
  spec.id = id
  if not story.protagonists[id] then story.order[#story.order + 1] = id end
  story.protagonists[id] = spec
  return spec
end

--- Load protagonists from a data file (default data/story.json).
---@param path? string
function story.load(path)
  path = path or "data/story.json"
  if not fe.fs.exists(path) then return end
  data.load("story", path, "story")
  for _, entry in ipairs(data.list("story")) do story.define(entry.id, entry) end
end

---@param id string
---@return table
function story.get(id)
  local spec = story.protagonists[id]
  if not spec then error("no protagonist '" .. tostring(id) .. "'", 2) end
  return spec
end

--- Make a protagonist available on the chapter select screen.
function story.recruit(id)
  story.get(id)
  progress(id).recruited = true
  state.changed:emit("story", id, "recruited")
end

---@return boolean
function story.recruited(id)
  return progress(id).recruited == true
end

--- Chapters `id` has finished.
---@return integer
function story.finished(id)
  return progress(id).finished
end

--- The chapter number `id` plays next (finished + 1).
---@return integer
function story.chapter(id)
  return progress(id).finished + 1
end

--- The chapter being played: id, number and definition; nil when none.
---@return string?, integer?, table?
function story.current()
  local current = record.current
  if not current.id then return nil end
  return current.id, current.chapter, story.get(current.id).chapters[current.chapter]
end

--- Whether an actor is in the active party (the RPG kit's, by default).
--- Replace for games without fe.rpg.
---@param actor string
---@return boolean
function story.has_member(actor)
  return require("fe.rpg.party").is_active(actor)
end

local function member_name(actor)
  local ok, name = pcall(function() return require("fe.rpg.party").actor(actor):name() end)
  return ok and name or actor
end

--- Whether `id` can start its next chapter; false and the reasons (texts)
--- when not, or when every chapter is finished.
---@param id string
---@return boolean, string[]
function story.can_start(id)
  local spec = story.get(id)
  local chapter = spec.chapters[story.chapter(id)]
  local reasons = {}
  if not chapter then return false, { i18n.t("fe.story.done") } end
  local requires = chapter.requires or {}
  local members = {}
  if spec.actor then members[#members + 1] = spec.actor end
  for _, actor in ipairs(requires.party or {}) do members[#members + 1] = actor end
  for _, actor in ipairs(members) do
    if not story.has_member(actor) then
      reasons[#reasons + 1] = i18n.t("fe.story.needs_member", { name = member_name(actor) })
    end
  end
  for _, flag in ipairs(requires.flags or {}) do
    if not state.flag(flag) then
      reasons[#reasons + 1] = i18n.t("fe.story.needs_flag")
      break
    end
  end
  local other_ids = {}
  for other in pairs(requires.chapters or {}) do other_ids[#other_ids + 1] = other end
  table.sort(other_ids)
  for _, other in ipairs(other_ids) do
    local count = requires.chapters[other]
    if story.finished(other) < count then
      reasons[#reasons + 1] = i18n.t("fe.story.needs_chapter", { name = story.get(other).name, number = count })
    end
  end
  return #reasons == 0, reasons
end

--- Next chapters of recruited protagonists, in definition order, except
--- the one being played: {id, number, chapter, ok, reasons}. With
--- `options.include_done`, each protagonist's finished chapters come
--- first, as {id, number, chapter, ok = true, done = true, reasons = {}}.
---@param options? {include_done?: boolean}
---@return table[]
function story.available(options)
  local out = {}
  local playing = story.current()
  for _, id in ipairs(story.order) do
    if story.recruited(id) then
      local chapters = story.protagonists[id].chapters
      if options and options.include_done then
        for number = 1, story.finished(id) do
          if chapters[number] then
            out[#out + 1] = { id = id, number = number, chapter = chapters[number], ok = true, done = true, reasons = {} }
          end
        end
      end
      local chapter = chapters[story.chapter(id)]
      if chapter and id ~= playing then
        local ok, reasons = story.can_start(id)
        out[#out + 1] = { id = id, number = story.chapter(id), chapter = chapter, ok = ok, reasons = reasons }
      end
    end
  end
  return out
end

--- Put `actor` at the head of the active party for their chapter (the RPG
--- kit's party, by default). Replace for games without fe.rpg.
---@param actor string
function story.lead_with(actor)
  local party = require("fe.rpg.party")
  local ids = { actor }
  for _, member in ipairs(party.members()) do
    if member.id ~= actor then ids[#ids + 1] = member.id end
  end
  party.set_active(ids)
end

--- Start `id`'s next chapter (call from a task): it becomes the current
--- chapter, the protagonist leads the party, the field warps to the
--- chapter's map and its script runs. Raises when `can_start` refuses.
---@param id string
function story.begin(id)
  local ok, reasons = story.can_start(id)
  if not ok then error("cannot start " .. id .. "'s chapter: " .. table.concat(reasons, "; "), 2) end
  local spec = story.get(id)
  local number = story.chapter(id)
  local chapter = spec.chapters[number]
  record.current = { id = id, chapter = number }
  if spec.actor then story.lead_with(spec.actor) end
  story.chapter_started:emit(id, number)
  state.changed:emit("story", id, "started")
  if chapter.map then require("fe.field").warp(chapter.map, chapter.point or "start") end
  if chapter.script then require(chapter.script)(chapter, id) end
end

--- Play a finished chapter again (call from a task): the field warps to
--- the chapter's map and its script runs with (chapter, id, true), while
--- the record stays as it is: no chapter becomes current and
--- `story.finish` does not count it again. Raises for a chapter that is
--- not finished.
---@param id string
---@param number integer
function story.replay(id, number)
  local spec = story.get(id)
  local chapter = spec.chapters[number]
  if not chapter or number > story.finished(id) then
    error(id .. " has not finished chapter " .. tostring(number), 2)
  end
  if chapter.map then require("fe.field").warp(chapter.map, chapter.point or "start") end
  if chapter.script then require(chapter.script)(chapter, id, true) end
end

--- Finish the chapter being played, or with `id` that protagonist's next
--- chapter (one that ends while someone else's is played): it counts as
--- finished, and when it was being played nothing is current. Returns the
--- protagonist and chapter number; nil when there is nothing to finish.
---@param id? string
---@return string?, integer?
function story.finish(id)
  local playing, number = story.current()
  if id and id ~= playing then
    number = story.chapter(id)
    if not story.get(id).chapters[number] then return nil end
  else
    id = playing
    if not id then return nil end
    record.current = {}
  end
  progress(id).finished = number
  story.chapter_finished:emit(id, number)
  state.changed:emit("story", id, "finished")
  return id, number
end

--- The chapter select screen (call from a task): lists `story.available()`
--- (with `options.include_done`, finished chapters too, marked for replay)
--- and returns the chosen protagonist id and chapter number, or nil when
--- cancelled.
---@param options? {include_done?: boolean}
---@return string?, integer?
function story.open(options)
  local ui = require("fe.ui")
  local entries = {}
  for _, entry in ipairs(story.available(options)) do
    local spec = story.protagonists[entry.id]
    local label = spec.name .. "  " .. i18n.t("fe.story.chapter", { number = entry.number }) .. "  " .. entry.chapter.title
    local right = entry.chapter.level and i18n.t("fe.story.level", { level = entry.chapter.level }) or nil
    if entry.done then right = i18n.t("fe.story.replay") end
    entries[#entries + 1] = { label = label, value = { id = entry.id, number = entry.number, done = entry.done },
      enabled = entry.ok, right = right, reasons = entry.reasons }
  end
  if #entries == 0 then entries = { { label = i18n.t("fe.story.none"), enabled = false } } end
  local hovered = entries[1]
  local panel = { name = "story_reasons", blocks_update = false }
  function panel.draw()
    if not hovered or not hovered.reasons or #hovered.reasons == 0 then return end
    local cw, ch = fe.canvas.size()
    local text = "{color:bad}" .. table.concat(hovered.reasons, "\n") .. "{color:reset}"
    local w = math.min(cw - 16, 260)
    local _, h = ui.measure(text, { width = w - 12 })
    ui.window(8, ch - h - 20, w, h + 12)
    ui.text(text, 14, ch - h - 14, { width = w - 12 })
  end
  local _, chosen = ui.with_view(panel, ui.choose, entries, { title = i18n.t("fe.story.title"), width = 260,
    rows = math.min(8, #entries), cancel = true, on_change = function(item) hovered = item end })
  if not (chosen and chosen.value) then return nil end
  return chosen.value.id, chosen.value.number
end

--- Chapter select, then `begin` the chosen chapter, or `replay` it when it
--- is a finished one (`options.include_done`). Returns the id or nil.
--- From a menu page, give the page `closes = true` so the warp does not
--- run under the menu's pause.
---@param options? {include_done?: boolean}
---@return string?
function story.select(options)
  local id, number = story.open(options)
  if id and number <= story.finished(id) then
    story.replay(id, number)
  elseif id then
    story.begin(id)
  end
  return id
end

--- Event page conditions (fe.event loads them through its providers).
story.conditions = {
  chapter = function(value)
    local event = require("fe.event")
    for id, expression in pairs(value) do
      if not event.compare(story.chapter(id), expression) then return false end
    end
    return true
  end,
  playing = function(value)
    return story.current() == value
  end,
}

return story

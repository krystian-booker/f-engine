--- The codex: journal entries the player unlocks (evidence, people, notes)
--- shown on the menu's Journal page (`menu.journal_page`, see
--- fe.ui.screens.menu) next to an Objectives tab built from fe.quest.
---
--- data/journal.json (loaded on first use when it exists, or with
--- `codex.load(path)`; `codex.define(list)` adds entries from Lua):
--- ```json
--- [{"id": "notice", "tab": "evidence", "title_key": "journal.notice.title",
---   "text_key": "journal.notice.text", "image": "ui/notice.png"},
---  {"id": "rowan", "tab": "people", "title_key": "people.rowan", "text_key": "people.rowan.text"}]
--- ```
--- `optional` marks an entry the player may never find (for completion
--- counts); `image` is shown in the entry's detail panel.
---
--- ```lua
--- codex.unlock("notice")          -- true the first time; idempotent
--- codex.unlocked("evidence")      -- unlocked entries of a tab, in data order
--- codex.is_unlocked("notice")
--- ```
--- Unlocks are saved (state section "codex") and announced with
--- `codex.changed(id)` and `state.changed("codex", id)`. An unknown id
--- raises. Event pages can test `{"codex": "notice"}` (or a list of ids).
---
--- Tabs: `codex.tabs()` is `settings.tabs` ("objectives", "evidence",
--- "people") followed by every other tab name the entries use, in the
--- order they first appear; a tab is named by the `fe.journal.tab.<name>`
--- string (built in for objectives, evidence, people and notes) or its
--- name capitalised.
local data = require("fe.data")
local event = require("fe.event")
local i18n = require("fe.i18n")
local schema = require("fe.schema")
local signal = require("fe.signal")
local state = require("fe.state")
local tablex = require("fe.tablex")

local codex = {
  settings = {
    --- Tabs shown first, in order ("objectives" lists active quests).
    tabs = { "objectives", "evidence", "people" },
  },
  --- Emitted with the entry id when an entry unlocks.
  changed = signal.new(),
}

--- The file's settings, for `__reload`.
codex.file_defaults = { settings = tablex.deep_copy(codex.settings) }

function codex.__reload(live, fresh)
  live.file_defaults.settings = tablex.adopt(live.settings, fresh.settings, live.file_defaults.settings)
end

i18n.defaults({ fe = { journal = {
  title = "Journal", empty = "(Nothing yet)", no_objectives = "(No current objectives)",
  tab = { objectives = "Objectives", evidence = "Evidence", people = "People", notes = "Notes" },
} } })

schema.define("journal_entry", {
  id = "string",
  tab = { type = "string", doc = "Journal tab: evidence, people, notes or any name" },
  title_key = { type = "string", doc = "i18n key (or text) of the title" },
  text_key = { type = "string", doc = "i18n key (or text) of the body" },
  optional = { type = "boolean", optional = true, doc = "The player may never find it" },
  image = { type = { asset = "image" }, optional = true, doc = "Shown in the entry's detail panel" },
}, { doc = "A codex entry (data/journal.json)." })

local record = state.section("codex", function() return { unlocked = {} } end)

-- Kept across hot reload: entries added with codex.define (no file).
local live = { defined = nil }

--- Load entries from a JSON file (default data/journal.json).
---@param path? string
function codex.load(path)
  data.load("journal", path or "data/journal.json", "journal_entry")
  live.defined = nil
end

--- Use these entries instead of a file (tests, generated journals).
---@param list table[]
function codex.define(list)
  for _, entry in ipairs(list) do
    local ok, errors = schema.validate("journal_entry", entry, "journal." .. tostring(entry.id))
    if not ok then error(table.concat(errors, "\n"), 2) end
  end
  live.defined = { list = list, by_id = {} }
  for _, entry in ipairs(list) do live.defined.by_id[entry.id] = entry end
end

--- Every entry, in data order (loads data/journal.json on first use).
---@return table[]
function codex.entries()
  if live.defined then return live.defined.list end
  if not data.has_table("journal") then
    if not fe.fs.exists("data/journal.json") then return {} end
    codex.load()
  end
  return data.list("journal")
end

--- An entry by id; raises for an unknown one.
---@param id string
---@return table
function codex.get(id)
  codex.entries()
  local entry = live.defined and live.defined.by_id[id] or (not live.defined and data.find("journal", id))
  if not entry then error("no journal entry '" .. tostring(id) .. "'", 2) end
  return entry
end

--- Unlock an entry; true when it was locked. Idempotent.
---@param id string
---@return boolean
function codex.unlock(id)
  codex.get(id)
  if record.unlocked[id] then return false end
  record.unlocked[id] = true
  codex.changed:emit(id)
  state.changed:emit("codex", id)
  return true
end

---@param id string
---@return boolean
function codex.is_unlocked(id)
  return record.unlocked[id] == true
end

--- Unlocked entries (of one tab, or all), in data order.
---@param tab? string
---@return table[]
function codex.unlocked(tab)
  local out = {}
  for _, entry in ipairs(codex.entries()) do
    if record.unlocked[entry.id] and (not tab or entry.tab == tab) then out[#out + 1] = entry end
  end
  return out
end

--- Tab names: `settings.tabs`, then the other tabs entries use.
---@return string[]
function codex.tabs()
  local out, seen = {}, {}
  for _, tab in ipairs(codex.settings.tabs) do
    out[#out + 1] = tab
    seen[tab] = true
  end
  for _, entry in ipairs(codex.entries()) do
    if not seen[entry.tab] then
      seen[entry.tab] = true
      out[#out + 1] = entry.tab
    end
  end
  return out
end

--- A tab's display name.
---@param tab string
---@return string
function codex.tab_name(tab)
  local key = "fe.journal.tab." .. tab
  local name = i18n.t(key)
  return name == key and (tab:gsub("^%l", string.upper)) or name
end

--- Unlocked and total counts, overall or in one tab; optional entries
--- count only once unlocked.
---@param tab? string
---@return integer unlocked, integer total
function codex.count(tab)
  local unlocked, total = 0, 0
  for _, entry in ipairs(codex.entries()) do
    if not tab or entry.tab == tab then
      local open = record.unlocked[entry.id] == true
      if open then unlocked = unlocked + 1 end
      if open or not entry.optional then total = total + 1 end
    end
  end
  return unlocked, total
end

event.condition("codex", function(value)
  for _, id in ipairs(type(value) == "table" and value or { value }) do
    if not record.unlocked[id] then return false end
  end
  return true
end)

return codex

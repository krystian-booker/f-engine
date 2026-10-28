--- Quest journal: quests with stages and objectives, plus chapters.
---
--- data/quests.json:
--- ```json
--- [{"id": "crystal", "name": "The Fading Crystal", "chapter": 1, "kind": "main",
---   "stages": [{"id": "talk", "text": "Speak with the elder."},
---              {"id": "grotto", "text": "Search the northern grotto."}]}]
--- ```
--- ```lua
--- quest.start("crystal")          -- stage 1 active
--- quest.advance("crystal")        -- next stage
--- quest.complete("crystal")
--- ```
local data = require("fe.data")
local schema = require("fe.schema")
local signal = require("fe.signal")
local state = require("fe.state")

local quest = {
  changed = signal.new(), -- (quest_id, status)
}

schema.define("quest", {
  id = "string",
  name = "string",
  kind = { type = "string", default = "side", doc = "main, side, character..." },
  chapter = { type = "integer", optional = true },
  owner = { type = "string", optional = true, doc = "Character whose story this is" },
  description = { type = "string", default = "" },
  stages = { type = { list = { record = { id = "string", text = "string" } } }, default = {} },
  rewards = { type = "table", optional = true },
})

local record = state.section("quests", function() return { quests = {}, chapter = {} } end)

function quest.load(path)
  path = path or "data/quests.json"
  if fe.fs.exists(path) then data.load("quests", path, "quest") end
end

function quest.status(id)
  local q = record.quests[id]
  return q and q.status or "unknown"
end

function quest.stage(id)
  local q = record.quests[id]
  if not q then return nil end
  return data.get("quests", id).stages[q.stage]
end

function quest.start(id)
  data.get("quests", id)
  if record.quests[id] then return false end
  record.quests[id] = { status = "active", stage = 1, started = state.var("playtime", 0) }
  quest.changed:emit(id, "active")
  return true
end

--- Move to the next stage (or a named stage).
function quest.advance(id, stage_id)
  local q = record.quests[id]
  if not q or q.status ~= "active" then return false end
  local def = data.get("quests", id)
  if stage_id then
    for i, s in ipairs(def.stages) do
      if s.id == stage_id then q.stage = i end
    end
  else
    q.stage = math.min(#def.stages, q.stage + 1)
  end
  quest.changed:emit(id, "active")
  return true
end

function quest.complete(id)
  local q = record.quests[id] or {}
  record.quests[id] = q
  q.status = "completed"
  quest.changed:emit(id, "completed")
end

function quest.fail(id)
  local q = record.quests[id] or {}
  record.quests[id] = q
  q.status = "failed"
  quest.changed:emit(id, "failed")
end

--- Quests with a status, in data order.
---@param status? "active"|"completed"|"failed"
function quest.list(status)
  local out = {}
  if not data.has_table("quests") then return out end
  for _, def in ipairs(data.list("quests")) do
    local q = record.quests[def.id]
    if q and (not status or q.status == status) then out[#out + 1] = { def = def, state = q } end
  end
  return out
end

--- Chapters per story owner (multi-protagonist games); `owner` defaults to "main".
function quest.chapter(owner)
  return record.chapter[owner or "main"] or 1
end

function quest.set_chapter(owner, chapter)
  record.chapter[owner or "main"] = chapter
  quest.changed:emit(nil, "chapter")
end

require("fe.event").condition("quest", function(value)
  for id, wanted in pairs(value) do
    if quest.status(id) ~= wanted then return false end
  end
  return true
end)

return quest

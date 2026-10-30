--- Quest journal: quests with stages and per-stage objectives. A quest's
--- `chapter` and `owner` only label it for the journal; protagonists'
--- chapters are fe.story's.
---
--- data/quests.json:
--- ```json
--- [{"id": "crystal", "name": "The Fading Crystal", "chapter": 1, "kind": "main",
---   "stages": [{"id": "talk", "text": "Speak with the elder."},
---              {"id": "grotto", "text": "Search the northern grotto.",
---               "objectives": [{"id": "shards", "text": "Collect crystal shards", "count": 3},
---                              {"id": "guardian", "text": "Defeat the guardian"}]}]}]
--- ```
--- ```lua
--- quest.start("crystal")                     -- stage 1 active
--- quest.advance("crystal")                   -- next stage (or a named one)
--- quest.progress("crystal", "shards")        -- +1 towards an objective of the current stage
--- if quest.stage_done("crystal") then quest.advance("crystal") end
--- quest.complete("crystal")
--- ```
--- Every change emits `quest.changed(quest_id, what, detail)` and
--- `fe.state.changed("quest", quest_id, what)`, so event pages refresh.
--- Quest, stage and objective ids are checked: an unknown one raises.
--- Page conditions (provided to fe.event on first use):
---   {"quest": {"crystal": "active"}}            status: active, completed, failed, unknown
---   {"quest_stage": {"crystal": "grotto"}}      current stage id
---   {"objective": {"crystal": "guardian"}}      objective of the current stage completed
--- `quest.load()` reads the data (fe.starter calls it when the file exists).
local data = require("fe.data")
local game = require("fe.game")
local schema = require("fe.schema")
local signal = require("fe.signal")
local state = require("fe.state")

local quest = {
  --- Emitted as (quest_id, what, detail): what is "active", "completed",
  --- "failed", "stage" (detail = stage id) or "objective" (detail =
  --- objective id).
  changed = signal.new(),
}

schema.define("quest", {
  id = "string",
  name = "string",
  kind = { type = "string", default = "side", doc = "main, side, character..." },
  chapter = { type = "integer", optional = true },
  owner = { type = "string", optional = true, doc = "Character whose story this is" },
  description = { type = "string", default = "" },
  stages = { type = { list = { record = {
    id = "string",
    text = "string",
    objectives = { type = { list = { record = {
      id = "string",
      text = "string",
      count = { type = "integer", min = 1, default = 1 },
    } } }, default = {} },
  } } }, default = {} },
  rewards = { type = "table", optional = true },
})

local record = state.section("quests", function() return { quests = {} } end)

local function progress_of(q, objective_id)
  return q and q.objectives and q.objectives[objective_id] or 0
end

local function emit(id, what, detail)
  quest.changed:emit(id, what, detail)
  state.changed:emit("quest", id, what)
end

function quest.load(path)
  path = path or "data/quests.json"
  if fe.fs.exists(path) then data.load("quests", path, "quest") end
end

function quest.status(id)
  local q = record.quests[id]
  return q and q.status or "unknown"
end

--- The current stage definition of a started quest, or nil.
function quest.stage(id)
  local q = record.quests[id]
  if not q or not q.stage then return nil end
  return data.get("quests", id).stages[q.stage]
end

function quest.start(id)
  data.get("quests", id)
  if record.quests[id] then return false end
  record.quests[id] = { status = "active", stage = 1, started = game.playtime(), objectives = {} }
  emit(id, "active")
  return true
end

--- Move to the next stage (or a named stage). Objective progress starts
--- over for the new stage. Returns false, changing nothing, when the quest
--- is not active or is already on its last stage; raises for an unknown
--- quest or stage.
function quest.advance(id, stage_id)
  local def = data.get("quests", id)
  local index
  if stage_id then
    for i, s in ipairs(def.stages) do
      if s.id == stage_id then index = i end
    end
    if not index then error("quest '" .. id .. "' has no stage '" .. tostring(stage_id) .. "'", 2) end
  end
  local q = record.quests[id]
  if not q or q.status ~= "active" then return false end
  if not stage_id then
    index = q.stage + 1
    if index > #def.stages then return false end
  end
  q.stage = index
  q.objectives = {}
  emit(id, "stage", def.stages[index] and def.stages[index].id)
  return true
end

local function objective_def(id, objective_id)
  local stage = quest.stage(id)
  for _, objective in ipairs(stage and stage.objectives or {}) do
    if objective.id == objective_id then return objective end
  end
  return nil
end

--- Objectives of the current stage: {id, text, count, progress, done}.
function quest.objectives(id)
  local q = record.quests[id]
  local stage = quest.stage(id)
  local out = {}
  for _, objective in ipairs(stage and stage.objectives or {}) do
    local progress = progress_of(q, objective.id)
    out[#out + 1] = { id = objective.id, text = objective.text, count = objective.count, progress = progress,
      done = progress >= objective.count }
  end
  return out
end

--- Add progress to an objective of the current stage (default 1, capped at
--- its count). Returns true when the objective is complete.
function quest.progress(id, objective_id, amount)
  local q = record.quests[id]
  if not q or q.status ~= "active" then return false end
  local objective = objective_def(id, objective_id)
  if not objective then
    error("quest '" .. id .. "' has no objective '" .. tostring(objective_id) .. "' in its current stage", 2)
  end
  local before = progress_of(q, objective_id)
  local after = math.min(objective.count, before + (amount or 1))
  if after ~= before then
    q.objectives = q.objectives or {}
    q.objectives[objective_id] = after
    emit(id, "objective", objective_id)
  end
  return after >= objective.count
end

--- Complete an objective of the current stage outright.
function quest.complete_objective(id, objective_id)
  local objective = objective_def(id, objective_id)
  return quest.progress(id, objective_id, objective and objective.count or 1)
end

--- Whether an objective of the current stage is complete.
function quest.objective_done(id, objective_id)
  local objective = objective_def(id, objective_id)
  return objective ~= nil and progress_of(record.quests[id], objective_id) >= objective.count
end

--- Whether every objective of the current stage is complete (true for a
--- stage without objectives).
function quest.stage_done(id)
  for _, objective in ipairs(quest.objectives(id)) do
    if not objective.done then return false end
  end
  return record.quests[id] ~= nil
end

local function set_status(id, status)
  data.get("quests", id)
  local q = record.quests[id] or { objectives = {} }
  record.quests[id] = q
  q.status = status
  emit(id, status)
end

function quest.complete(id)
  set_status(id, "completed")
end

function quest.fail(id)
  set_status(id, "failed")
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

--- Event page conditions (fe.event loads them through its providers).
quest.conditions = {
  quest = function(value)
    for id, wanted in pairs(value) do
      if quest.status(id) ~= wanted then return false end
    end
    return true
  end,
  quest_stage = function(value)
    for id, wanted in pairs(value) do
      local stage = quest.stage(id)
      if not stage or stage.id ~= wanted then return false end
    end
    return true
  end,
  objective = function(value)
    for id, objective_id in pairs(value) do
      if quest.status(id) ~= "active" or not quest.objective_done(id, objective_id) then return false end
    end
    return true
  end,
}

return quest

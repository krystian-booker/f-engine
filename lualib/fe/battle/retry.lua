--- Retry after defeat: run a battle that can be fought again from the
--- moment it started.
---
--- ```lua
--- local outcome, choice = retry.start({ troop = "warden" }, {
---   labels = { retry = "Retry the boss battle" },          -- text or i18n keys
---   before = function(attempt) ... end,                    -- before every attempt
--- })
--- if choice == "checkpoint" then ... elseif choice == "title" then ... end
--- ```
--- `retry.start(options, retry_options)` snapshots the game state
--- (fe.state) and runs `fe.battle.start(options)` with `allow_defeat`, so
--- the game's on_defeat is not called. On a defeat it asks
--- `retry_options.menu(result, context)` (default `retry.menu`) for a
--- choice; "retry" restores the snapshot exactly (HP, items, flags, the
--- game's own sections; nothing carries over from the lost attempt) and
--- starts the battle again; any other choice ends the loop with the state as
--- the defeat left it, for the caller to act on (load a checkpoint, go to
--- the title). `result` is {outcome, rewards, battle}; `context` is
--- {attempt, options, choices, labels, title}. `retry_options.before(attempt)`
--- runs before every attempt, after the restore (re-apply a toggle the
--- menu offered). `retry_options.keep` names fe.state sections a retry
--- keeps as they are (e.g. {"clock"} keeps playtime running); by default
--- none. Returns the last outcome, the menu choice (nil when the battle
--- was not lost) and the rewards. Call it inside a task.
---
--- The default menu (fe.ui) offers `choices` (default retry.settings.choices:
--- retry, checkpoint, title) labelled by `labels[id]` or the
--- `fe.battle.retry.<id>` text, under the `fe.battle.retry.prompt` title.
local i18n = require("fe.i18n")
local state = require("fe.state")
local tablex = require("fe.tablex")
local task = require("fe.task")

i18n.defaults({ fe = { battle = { retry = {
  prompt = "Defeated", retry = "Retry Battle", checkpoint = "Load Checkpoint", title = "Return to Title",
} } } })

local retry = {
  settings = {
    choices = { "retry", "checkpoint", "title" },
    width = 180,
  },
}

--- Label of a menu choice: labels[id] (text or an i18n key), else the
--- fe.battle.retry.<id> text.
function retry.label(id, labels)
  if labels and labels[id] then return i18n.t(labels[id]) end
  return i18n.t("fe.battle.retry." .. id)
end

--- The default defeat menu: a fe.ui choice; returns the chosen id.
function retry.menu(_, context)
  local ui = require("fe.ui")
  local entries = tablex.map(context.choices, function(id)
    return { label = retry.label(id, context.labels), value = id }
  end)
  local _, item = ui.choose(entries, { title = context.title or i18n.t("fe.battle.retry.prompt"),
    width = retry.settings.width })
  return item and item.value
end

---@param options table fe.battle.start options
---@param retry_options? {menu?: fun(result: table, context: table): string?, before?: fun(attempt: integer), labels?: table<string, string>, choices?: string[], title?: string, keep?: string[]}
---@return string outcome, string? choice, table? rewards
function retry.start(options, retry_options)
  retry_options = retry_options or {}
  local battle = require("fe.battle")
  local saved = state.snapshot()
  local attempt = 0
  while true do
    attempt = attempt + 1
    if retry_options.before then retry_options.before(attempt) end
    local run = tablex.copy(options)
    run.allow_defeat = true
    local fought
    local stop = task.cleanup(battle.finished:connect(function(b) fought = fought or b end))
    local outcome, rewards = battle.start(run)
    stop()
    if outcome ~= "defeat" then return outcome, nil, rewards end
    local context = { attempt = attempt, options = options, labels = retry_options.labels,
      choices = retry_options.choices or retry.settings.choices, title = retry_options.title }
    local choice = (retry_options.menu or retry.menu)({ outcome = outcome, rewards = rewards, battle = fought }, context)
    if choice ~= "retry" then return outcome, choice, rewards end
    local snapshot = saved
    if retry_options.keep then
      snapshot = tablex.copy(saved)
      local now = state.snapshot()
      for _, name in ipairs(retry_options.keep) do snapshot[name] = now[name] end
    end
    state.restore(snapshot)
  end
end

return retry

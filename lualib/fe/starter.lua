--- A standard bootstrap for RPG-style games built on the library kits.
--- `main.lua` can be as short as:
---
--- ```lua
--- return require("fe.starter").main{
---   title = "Crystal of the Vale",
---   new_game = function() ... field.start("town", "entry") end,
--- }
--- ```
--- It loads settings and input, localisation, sounds, quests (when
--- data/quests.json exists) and the RPG kit when the game uses it, wires
--- the field's menu button, shows the title screen and connects Continue to
--- the save system. New Game and Continue cancel running tasks and reset
--- field locks, event holds, pauses, time scales and toasts before
--- starting. New Game is also `fe.devtools.new_game` (the editor's "Play
--- from here").
---
--- Options: `title`, `subtitle`, `title_background`, `language` (used until
--- the player picks one; otherwise the system language when the game ships
--- it),
--- `new_game` (required), `continue(slot)` (default `field.resume()`),
--- `setup()` (after loading, before the title; define game actions here),
--- `skip_title`, `validate()`, `rpg` (default: true when data/actors.json
--- exists; installs fe.rpg and resets party and inventory on new games) and
--- `menu` (false, or a function run in a task when the menu button is
--- pressed; default: the fe.ui.screens.menu screen when the RPG kit is on).
--- Every step is optional or replaceable; skip this module entirely for a
--- different structure.
local config = require("fe.config")
local devtools = require("fe.devtools")
local field = require("fe.field")
local game = require("fe.game")
local i18n = require("fe.i18n")
local scene = require("fe.scene")
local sound = require("fe.sound")
local state = require("fe.state")
local task = require("fe.task")

local starter = {}

---@param options {title?: string, subtitle?: string, new_game: fun(), continue?: fun(slot: string), setup?: fun(), menu?: boolean|fun(), rpg?: boolean, skip_title?: boolean, language?: string, validate?: fun(), title_background?: fun()}
function starter.main(options)
  local rpg = options.rpg
  if rpg == nil then rpg = fe.fs.exists("data/actors.json") end
  local menu = options.menu
  if menu == nil or menu == true then
    menu = rpg and function() require("fe.ui.screens.menu").open() end
  end

  local function start_over()
    task.cancel_all()
    field.reset()
    game.reset_time()
    require("fe.ui").clear_toasts()
  end

  local function begin_new_game()
    start_over()
    state.reset()
    if rpg then
      require("fe.rpg.party").reset()
      require("fe.rpg.inventory").reset()
    end
    options.new_game()
  end

  local function continue(slot)
    start_over()
    if options.continue then
      options.continue(slot)
    elseif not field.resume() then
      fe.log.warn("The save has no field location; starting a new game")
      begin_new_game()
    end
  end

  --- Exposed for tests, tools and the game's own screens (a game over):
  --- `starter.new_game()` starts over from scratch, `starter.continue(slot)`
  --- resumes a save that was just loaded (cancelling tasks and resetting
  --- locks first, as the title's Continue does) and `starter.start_over()`
  --- is that reset alone.
  starter.new_game = begin_new_game
  starter.continue = continue
  starter.start_over = start_over
  devtools.new_game = begin_new_game

  local function load_everything()
    config.load({ language = options.language })
    i18n.install()
    sound.load()
    if rpg then
      require("fe.rpg.database").load()
      require("fe.rpg").install()
    end
    if fe.fs.exists("data/quests.json") then require("fe.quest").load() end
  end

  return game.main({
    start = function()
      load_everything()
      if menu then
        field.menu_requested:connect(function()
          task.named("menu", menu)
        end, 0, "fe.starter.menu")
      end
      if options.setup then options.setup() end
      config.apply_bindings(true)
      if options.skip_title then
        begin_new_game()
        return
      end
      if sound.has_music("title") then sound.music("title") end
      scene.push(require("fe.ui.screens.title").new({
        title = options.title, subtitle = options.subtitle, draw_background = options.title_background,
        new_game = begin_new_game, continue = continue,
      }))
    end,
    validate = function()
      load_everything()
      if options.validate then options.validate() end
    end,
  })
end

return starter

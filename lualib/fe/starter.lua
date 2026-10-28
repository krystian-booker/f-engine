--- A standard bootstrap for RPG-style games built on the library kits.
--- `main.lua` can be as short as:
---
--- ```lua
--- return require("fe.starter").main{
---   title = "Crystal of the Vale",
---   new_game = function() ... field.start("town", "entry") end,
--- }
--- ```
--- It loads settings, input, sounds, the RPG database and quests, connects
--- the field to the RPG kit and main menu, shows the title screen and wires
--- Continue to the save system. Every step is optional or replaceable;
--- skip this module entirely for a different structure.
local config = require("fe.config")
local field = require("fe.field")
local game = require("fe.game")
local i18n = require("fe.i18n")
local scene = require("fe.scene")
local sound = require("fe.sound")
local state = require("fe.state")
local task = require("fe.task")

local starter = {}

---@param options {title?: string, subtitle?: string, new_game: fun(), continue?: fun(slot: string), setup?: fun(), menu?: boolean, skip_title?: boolean, language?: string, validate?: fun(), title_background?: fun()}
function starter.main(options)
  local function begin_new_game()
    task.cancel_all()
    state.reset()
    require("fe.rpg.party").reset()
    require("fe.rpg.inventory").reset()
    options.new_game()
  end

  --- Exposed for tests and tools: start over from scratch.
  starter.new_game = begin_new_game

  local function continue(slot)
    if options.continue then
      options.continue(slot)
    elseif not field.resume() then
      fe.log.warn("The save has no field location; starting a new game")
      begin_new_game()
    end
  end

  local function load_everything()
    config.load()
    i18n.set_language(options.language or config.get("language"))
    i18n.install()
    sound.load()
    require("fe.rpg.database").load()
    require("fe.quest").load()
    require("fe.rpg").install()
  end

  return game.main({
    start = function()
      load_everything()
      if options.menu ~= false then
        field.menu_requested:connect(function()
          task.named("menu", require("fe.ui.screens.menu").open)
        end)
      end
      if options.setup then options.setup() end
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

--- The title screen, and the way back to it.
local audio = require("game.audio")
local input = require("game.input")
local scenes = require("game.scenes")
local state = require("game.state")
local task = require("game.task")
local ui = require("game.ui")

local title = {}

local Title = { opaque = true }
Title.__index = Title

local function reset()
  local field = require("game.field")
  task.clear()
  scenes.clear()
  for _, name in ipairs(fe.world.list()) do fe.world.unload(name) end
  field.world, field.player, field.locked, field.zone = nil, nil, 0, nil
  field.epoch = field.epoch + 1
  fe.screen.set { fade = { 0, 0, 0, 0 }, flash = { 1, 1, 1, 0 }, transition = { kind = "none", progress = 0 } }
  if fe.screen.frozen() then fe.screen.unfreeze() end
end

--- Start a new game at the town gate.
function title.new_game()
  reset()
  state.new_game()
  require("game.field").start(state.data.map, state.data.point)
end

function title.continue(slot)
  if not state.load(slot) then
    ui.toast("That file cannot be read.")
    return false
  end
  reset()
  require("game.field").start(state.data.map, state.data.point, state.data.position)
  return true
end

function Title:update()
  self:update_backdrop()
  ui.list_input(self.cursor, #self.rows)
  if fe.input.pressed("confirm") then
    local id = self.rows[self.cursor.index].id
    audio.sfx("confirm", { volume = 0.6 })
    if id == "new" then
      title.new_game()
    elseif id == "continue" then
      require("game.menu").load_screen(title.continue)
    elseif id == "quit" then
      fe.app.quit()
    end
  end
end

function Title:update_backdrop()
  -- The town at dusk, slowly turning, behind the menu.
  local camera = self.world:camera()
  camera.target = { x = 10, y = 0.5, z = 8 }
  camera.pitch, camera.distance, camera.fov = 30, 22, 30
  camera.yaw = 20 + fe.time.now() * 2
end

function Title:draw()
  local w, h = ui.size()
  self.world:draw()
  fe.canvas.rect(0, 0, w, h, 0x0b1020a0)
  local t = fe.time.now() % 4 / 4
  fe.canvas.line(w * (0.8 - t * 0.5), h * t * 0.5, w * (0.8 - t * 0.5) + 14, h * t * 0.5 - 7, 0xffd27aff, 1)
  ui.text("{size:3}STARFALL{/size}", 0, 60, { width = w, align = "center", color = ui.colors.accent })
  ui.text("an f-engine sample", 0, 100, { width = w, align = "center", color = ui.colors.dim })
  ui.list(w / 2 - 60, 140, 120, self.rows, self.cursor)
  if self.message then ui.text(self.message, 0, 122, { width = w, align = "center", color = ui.colors.bad }) end
  -- The prompts below follow the device in use: press a key, then a gamepad button.
  ui.text(input.hints({ { "confirm", "Choose" } }) .. "     Playing with: " .. fe.input.device_type(), 0, h - 14,
    { width = w, align = "center", color = ui.colors.dim })
end

function title.open(message)
  reset()
  local rows = { { "New Game", id = "new" } }
  if require("game.menu").has_saves() then rows[#rows + 1] = { "Continue", id = "continue" } end
  rows[#rows + 1] = { "Quit", id = "quit" }
  audio.music("title")
  local world = fe.world.load("title", "town")
  world:set_environment { ambient = { 0.3, 0.3, 0.42 }, sun_color = { 1, 0.6, 0.4 } }
  local screen = scenes.push(setmetatable({ rows = rows, cursor = ui.cursor(), message = message, world = world }, Title))
  screen:update_backdrop()
  world:camera():snap()
  return screen
end

--- The party fell: back to the title. Called from the battle's task, which
--- ends here (reset cancels every task).
function title.game_over()
  title.open("The party has fallen...")
end

return title

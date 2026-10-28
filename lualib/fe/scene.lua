--- A stack of scenes. The top scene gets input; scenes underneath keep
--- drawing (a menu over the field) unless a scene hides them.
---
--- A scene is any table with optional methods:
---   enter(self, ...), leave(self), pause(self), resume(self),
---   update(self, dt), draw(self)
--- and optional fields:
---   name, opaque (hides scenes below when drawing),
---   blocks_update (default true: scenes below stop updating).
--- Changes requested during update/draw apply at the end of the tick.
local scene = {}

local stack = {}
local pending = {}
local walking = false

local function apply(op)
  if op.kind == "push" then
    local top = stack[#stack]
    if top and top.pause then top:pause() end
    stack[#stack + 1] = op.scene
    if op.scene.enter then op.scene:enter(table.unpack(op.args, 1, op.args.n)) end
  elseif op.kind == "pop" then
    local top = table.remove(stack)
    if top and top.leave then top:leave() end
    local below = stack[#stack]
    if below and below.resume then below:resume() end
  elseif op.kind == "remove" then
    for i = #stack, 1, -1 do
      if stack[i] == op.scene then
        table.remove(stack, i)
        if op.scene.leave then op.scene:leave() end
        if i > #stack and stack[#stack] and stack[#stack].resume then stack[#stack]:resume() end
        break
      end
    end
  elseif op.kind == "clear" then
    while #stack > 0 do
      local top = table.remove(stack)
      if top.leave then top:leave() end
    end
  end
end

local function request(op)
  if walking then
    pending[#pending + 1] = op
  else
    apply(op)
  end
end

local function flush()
  while #pending > 0 do
    local ops = pending
    pending = {}
    for _, op in ipairs(ops) do apply(op) end
  end
end

--- Push a scene; extra arguments go to its enter().
function scene.push(s, ...)
  request({ kind = "push", scene = s, args = table.pack(...) })
  return s
end

function scene.pop()
  request({ kind = "pop" })
end

--- Remove a specific scene wherever it is in the stack.
function scene.remove(s)
  request({ kind = "remove", scene = s })
end

--- Replace the whole stack with one scene.
function scene.switch(s, ...)
  request({ kind = "clear" })
  return scene.push(s, ...)
end

function scene.clear()
  request({ kind = "clear" })
end

function scene.top()
  return stack[#stack]
end

function scene.size()
  return #stack
end

function scene.contains(s)
  for _, other in ipairs(stack) do
    if other == s then return true end
  end
  return false
end

--- Find a scene by name.
function scene.find(name)
  for i = #stack, 1, -1 do
    if stack[i].name == name then return stack[i] end
  end
end

function scene.update(dt)
  flush()
  walking = true
  local first = 1
  for i = #stack, 1, -1 do
    first = i
    if stack[i].blocks_update ~= false then break end
  end
  local snapshot = { table.unpack(stack) }
  for i = first, #snapshot do
    local s = snapshot[i]
    if s.update then s:update(dt) end
  end
  walking = false
  flush()
end

function scene.draw()
  walking = true
  local first = 1
  for i = #stack, 1, -1 do
    first = i
    if stack[i].opaque then break end
  end
  for i = first, #stack do
    local s = stack[i]
    if s.draw then s:draw() end
  end
  walking = false
  flush()
end

return scene

--- The scene stack. A scene is a table with optional `enter`, `leave`,
--- `update(dt)`, `post_update(dt)` and `draw(alpha)`. Only the top scene
--- updates; scenes draw bottom first, so a menu or a message box draws over
--- the field it was opened from (a scene with `opaque = true` hides the ones
--- below it).
local scenes = { stack = {} }

function scenes.top()
  return scenes.stack[#scenes.stack]
end

function scenes.push(scene)
  scenes.stack[#scenes.stack + 1] = scene
  if scene.enter then scene:enter() end
  return scene
end

function scenes.pop(scene)
  for i = #scenes.stack, 1, -1 do
    if scene == nil or scenes.stack[i] == scene then
      local removed = table.remove(scenes.stack, i)
      if removed.leave then removed:leave() end
      return removed
    end
  end
end

function scenes.clear()
  while #scenes.stack > 0 do scenes.pop() end
end

function scenes.contains(scene)
  for _, entry in ipairs(scenes.stack) do
    if entry == scene then return true end
  end
  return false
end

function scenes.update(dt)
  local top = scenes.top()
  if top and top.update then top:update(dt) end
end

function scenes.post_update(dt)
  local top = scenes.top()
  if top and top.post_update then top:post_update(dt) end
end

function scenes.draw(alpha)
  -- Start at the topmost scene that covers the screen (`opaque = true`):
  -- a battle does not draw the field under it.
  local first = 1
  for i = #scenes.stack, 1, -1 do
    if scenes.stack[i].opaque then
      first = i
      break
    end
  end
  for i = first, #scenes.stack do
    local scene = scenes.stack[i]
    if scene.draw then scene:draw(alpha) end
  end
end

return scenes

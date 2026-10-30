-- A chapter script that records how it was run (story tests read `calls`).
local chapter = { calls = {} }

setmetatable(chapter, {
  __call = function(_, definition, id, replay)
    chapter.calls[#chapter.calls + 1] = { id, definition.title, replay }
  end,
})

return chapter

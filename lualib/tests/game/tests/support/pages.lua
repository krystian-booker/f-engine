-- Counters the page scripts below write and field tests read.
local pages = {}

function pages.reset()
  pages.runs, pages.count, pages.late, pages.arrived = 0, 0, nil, nil
end

pages.reset()
return pages

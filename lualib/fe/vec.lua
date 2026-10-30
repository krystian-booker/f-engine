--- Small helpers for {x, y, z} tables (the engine's vector convention).
local vec = {}

function vec.new(x, y, z) return { x = x or 0, y = y or 0, z = z or 0 } end
function vec.copy(v) return { x = v.x or 0, y = v.y or 0, z = v.z or 0 } end
function vec.add(a, b) return { x = a.x + b.x, y = (a.y or 0) + (b.y or 0), z = a.z + b.z } end
function vec.sub(a, b) return { x = a.x - b.x, y = (a.y or 0) - (b.y or 0), z = a.z - b.z } end
function vec.scale(v, s) return { x = v.x * s, y = (v.y or 0) * s, z = v.z * s } end
function vec.dot(a, b) return a.x * b.x + (a.y or 0) * (b.y or 0) + a.z * b.z end
function vec.length(v) return math.sqrt(vec.dot(v, v)) end
--- Distance on the ground plane (ignores height).
function vec.flat_distance(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2) end
--- Ground-plane distance inside `world`: the short way round on a wrapping map.
function vec.flat_distance_in(world, a, b)
  local d = world:wrap_delta(a, b)
  return math.sqrt(d.x * d.x + d.z * d.z)
end
function vec.distance(a, b) return vec.length(vec.sub(a, b)) end
function vec.normalize(v)
  local length = vec.length(v)
  if length < 1e-6 then return vec.new() end
  return vec.scale(v, 1 / length)
end
function vec.lerp(a, b, t)
  return { x = a.x + (b.x - a.x) * t, y = (a.y or 0) + ((b.y or 0) - (a.y or 0)) * t, z = a.z + (b.z - a.z) * t }
end
--- Centre of a grid cell.
function vec.cell_centre(cell, height)
  return { x = cell.x + 0.5, y = height or 0, z = cell.z + 0.5 }
end
--- Cell containing a position.
function vec.cell_of(position, layer)
  return { x = math.floor(position.x), z = math.floor(position.z), layer = layer or 0 }
end
function vec.same_cell(a, b)
  return a.x == b.x and a.z == b.z and (a.layer or 0) == (b.layer or 0)
end
function vec.cell_key(cell)
  return ((cell.layer or 0) * 4096 + cell.z) * 4096 + cell.x
end
--- Rotate a ground vector by `degrees` around the vertical axis.
function vec.rotate_flat(v, degrees)
  local r = math.rad(degrees)
  local c, s = math.cos(r), math.sin(r)
  return { x = v.x * c + v.z * s, y = v.y or 0, z = -v.x * s + v.z * c }
end
--- Manhattan distance between cells.
function vec.manhattan(a, b) return math.abs(a.x - b.x) + math.abs(a.z - b.z) end

return vec

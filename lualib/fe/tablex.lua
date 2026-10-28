--- Table helpers used across the library.
local tablex = {}

function tablex.copy(t)
  local out = {}
  for k, v in pairs(t) do out[k] = v end
  return out
end

function tablex.deep_copy(t, seen)
  if type(t) ~= "table" then return t end
  seen = seen or {}
  if seen[t] then return seen[t] end
  local out = {}
  seen[t] = out
  for k, v in pairs(t) do out[tablex.deep_copy(k, seen)] = tablex.deep_copy(v, seen) end
  return setmetatable(out, getmetatable(t))
end

--- Recursively copy `source` into `target` (tables merge, values replace).
function tablex.merge(target, source)
  for k, v in pairs(source) do
    if type(v) == "table" and type(target[k]) == "table" and #v == 0 then
      tablex.merge(target[k], v)
    else
      target[k] = v
    end
  end
  return target
end

function tablex.keys(t)
  local out = {}
  for k in pairs(t) do out[#out + 1] = k end
  table.sort(out, function(a, b) return tostring(a) < tostring(b) end)
  return out
end

function tablex.map(list, fn)
  local out = {}
  for i, v in ipairs(list) do out[i] = fn(v, i) end
  return out
end

function tablex.filter(list, fn)
  local out = {}
  for i, v in ipairs(list) do
    if fn(v, i) then out[#out + 1] = v end
  end
  return out
end

function tablex.find(list, fn)
  for i, v in ipairs(list) do
    if fn(v, i) then return v, i end
  end
end

function tablex.contains(list, value)
  for _, v in ipairs(list) do
    if v == value then return true end
  end
  return false
end

function tablex.index_of(list, value)
  for i, v in ipairs(list) do
    if v == value then return i end
  end
end

function tablex.remove_value(list, value)
  local i = tablex.index_of(list, value)
  if i then table.remove(list, i) end
  return i ~= nil
end

function tablex.sum(list, fn)
  local total = 0
  for _, v in ipairs(list) do total = total + (fn and fn(v) or v) end
  return total
end

--- Stable sort by a key function (ascending; `descending` flips it).
function tablex.sort_by(list, key, descending)
  local decorated = {}
  for i, v in ipairs(list) do decorated[i] = { v = v, k = key(v), i = i } end
  table.sort(decorated, function(a, b)
    if a.k == b.k then return a.i < b.i end
    if descending then return a.k > b.k end
    return a.k < b.k
  end)
  for i, d in ipairs(decorated) do list[i] = d.v end
  return list
end

function tablex.count(t)
  local n = 0
  for _ in pairs(t) do n = n + 1 end
  return n
end

function tablex.clamp(value, low, high)
  return math.max(low, math.min(high, value))
end

return tablex

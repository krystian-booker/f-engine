-- Standard library calls that loop in C are bounded (probes pat, tmove,
-- srep): the patched Lua library (cmake/LuaPatches.cmake) lets the watchdog
-- stop pattern matching and caps table.move and the strings it builds.
local function stopped_by_watchdog(t, fn)
  local start = fe.dev.clock()
  local ok, err = pcall(fn)
  local elapsed = fe.dev.clock() - start
  fe.harness.begin_test() -- the rest of the test gets a fresh watchdog
  t.ok(not ok, "expected the watchdog to stop the call")
  t.ok(tostring(err):find("script watchdog", 1, true), tostring(err))
  t.ok(elapsed < 60, "stopped after " .. elapsed .. " s")
end

return {
  ["the watchdog stops a backtracking pattern"] = function(t)
    -- 36 seconds before (and 10^9 times longer for 300000 characters).
    stopped_by_watchdog(t, function() return string.find(("a"):rep(800), ".-.-.-b") end)
  end,

  ["the watchdog stops a long backtracking gsub and gmatch"] = function(t)
    stopped_by_watchdog(t, function() return ("a"):rep(300000):gsub(".-.-.-.-b", "") end)
    stopped_by_watchdog(t, function()
      for _ in ("a"):rep(3000):gmatch(".-.-.-b") do end
    end)
  end,

  ["the watchdog stops a slow plain find"] = function(t)
    local haystack = ("a"):rep(1 << 22)
    local needle = ("a"):rep(1 << 21) .. "b"
    stopped_by_watchdog(t, function() return haystack:find(needle, 1, true) end)
  end,

  ["patterns that finish are unchanged"] = function(t)
    t.eq(string.find(("a"):rep(200), ".-.-b"), nil)
    t.eq(select(2, ("a"):rep(100000):gsub("a", "b")), 100000)
    t.eq(string.match("key = value", "(%w+) = (%w+)"), "key")
    t.eq(("x"):rep(1 << 20):find("y", 1, true), nil)
  end,

  ["table.move moves at most 2^26 elements"] = function(t)
    t.error(function() table.move({}, 1, math.maxinteger - 1, 1, {}) end, "too many elements to move")
    t.error(function() table.move({}, 1, (1 << 26) + 1, 1, {}) end, "the limit is 67108864")
    t.eq(table.move({ 1, 2, 3 }, 1, 3, 2), { 1, 1, 2, 3 })
    t.eq(table.move({ 1, 2, 3 }, 1, 3, 1, {}), { 1, 2, 3 })
  end,

  ["strings the libraries build stop at 256 MiB"] = function(t)
    t.error(function() return string.rep("x", (1 << 28) + 1) end, "string too large")
    t.error(function() return string.rep("abcdefgh", 1 << 30) end, "too large")
    local piece = ("x"):rep(1 << 20)
    local parts = {}
    for i = 1, 257 do parts[i] = piece end
    t.error(function() return table.concat(parts) end, "string too large %(the limit is 256 MiB%)")
    t.error(function() return piece:gsub("x", ("y"):rep(300)) end, "string too large")
    parts[257] = nil
    t.eq(#table.concat(parts, "", 1, 16), 16 << 20)
  end,
}

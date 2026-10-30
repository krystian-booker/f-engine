-- Resolved markup is bounded (probe pag_25): a resolver returning 25 tags per
-- tag expanded "{a}{a}" into 25^4 resolver calls and 73243 pages.
return {
  ["tags that expand too much raise one clear error"] = function(t)
    fe.text.resolver(function() return ("{x}"):rep(25) end)
    local start = fe.dev.clock()
    local ok, err = pcall(fe.text.paginate, "{a}{a}", { width = 100 }, 2)
    fe.text.resolver(nil)
    t.ok(not ok)
    t.ok(tostring(err):find("expand too much", 1, true), tostring(err))
    t.ok(fe.dev.clock() - start < 30, "stopped after " .. (fe.dev.clock() - start) .. " s")
  end,

  ["nested tags still resolve"] = function(t)
    local values = { one = "<{two}>", two = "[{three}]", three = "ok" }
    fe.text.resolver(function(tag) return values[tag] end)
    t.eq(fe.text.plain("{one}!"), "<[ok]>!")
    t.eq(fe.text.plain(("{three}"):rep(1000)), ("ok"):rep(1000))
    fe.text.resolver(nil)
  end,
}

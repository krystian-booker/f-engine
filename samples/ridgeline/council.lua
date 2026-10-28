-- The council: three companions vote on how to meet the raiders. Each holds
-- a conviction (negative favours holding the ridge, positive favours the
-- flanking strike). Arguments learned by talking around camp sway them.
local dialogue = require("fe.dialogue")
local state = require("fe.state")

local council = {}

council.members = {
  { id = "bram", name = "Bram", portrait = "portraits/bram.png", lean = -2,
    hold = "We dig in on the ridge. Let them climb to us.", flank = "Fine. We go around - but quickly." },
  { id = "lio", name = "Lio", portrait = "portraits/lio.png", lean = 1,
    hold = "I'll keep you standing on the ridge, then.", flank = "Strike the captain and the rest scatter." },
  { id = "wren", name = "Wren", portrait = "portraits/wren.png", lean = -1,
    hold = "Holding feels safer. I'll watch the flanks.", flank = "I know the way across. Let's move." },
}

council.routes = {
  hold = { name = "Hold the ridge", text = "Defend the southern line until the raiders break." },
  flank = { name = "Strike the captain", text = "Cross the stream and take down Captain Vey." },
}

--- Arguments sway each member by `sway[id]` toward `route`. `flag` must be
--- set (by talking to someone) before the argument can be made.
council.arguments = {
  { id = "archers", flag = "info_archers", route = "flank", sway = { bram = 3, wren = 1 },
    text = "Their archers own the terraces. Holding means being shot at all day." },
  { id = "stream", flag = "info_stream", route = "flank", sway = { wren = 3, lio = 1 },
    text = "Lio's frost can freeze the stream. We cross where they won't look." },
  { id = "grass", flag = "info_grass", route = "hold", sway = { lio = 3, bram = 1 },
    text = "The grass below the ridge is dry. Fire arrows will stop any charge." },
  { id = "village", route = "hold", sway = { bram = 1, wren = 1 },
    text = "The village needs time, not heroics." },
}

local function member(id)
  for _, m in ipairs(council.members) do
    if m.id == id then return m end
  end
end

--- A member's current conviction.
function council.conviction(id)
  return state.var("conviction." .. id, member(id).lean)
end

--- Arguments the player knows.
function council.available()
  local out = {}
  for _, a in ipairs(council.arguments) do
    if not a.flag or state.flag(a.flag) then out[#out + 1] = a end
  end
  return out
end

--- Make an argument to a member; returns how far they moved.
function council.persuade(id, argument)
  local amount = argument.sway[id] or 0
  if amount == 0 then return 0 end
  local sign = argument.route == "flank" and 1 or -1
  state.set_var("conviction." .. id, council.conviction(id) + sign * amount)
  return amount
end

--- Each member's vote and the result. Ties go to holding the ridge.
function council.tally()
  local votes, count = {}, { hold = 0, flank = 0 }
  for _, m in ipairs(council.members) do
    local vote = council.conviction(m.id) > 0 and "flank" or "hold"
    votes[m.id] = vote
    count[vote] = count[vote] + 1
  end
  return count.flank > count.hold and "flank" or "hold", votes, count
end

local function speak(m, text) dialogue.say(m.name, text, { portrait = m.portrait }) end

--- Play the council scene: persuade up to two members, then vote.
function council.run()
  dialogue.say("Cora", "Raiders are camped on Hollow Ridge. We decide together how to face them.",
    { portrait = "portraits/cora.png" })
  for _, m in ipairs(council.members) do
    speak(m, council.conviction(m.id) > 0 and m.flank or m.hold)
  end
  local persuaded = {}
  for _ = 1, 2 do
    local names, ids = {}, {}
    for _, m in ipairs(council.members) do
      if not persuaded[m.id] then
        names[#names + 1] = "Talk to " .. m.name
        ids[#ids + 1] = m.id
      end
    end
    names[#names + 1] = "Call the vote"
    local pick = dialogue.choice(names, { title = "Persuade", cancel = false })
    local id = ids[pick]
    if not id then break end
    persuaded[id] = true
    local options = {}
    local known = council.available()
    for _, a in ipairs(known) do options[#options + 1] = a.text end
    local which = known[dialogue.choice(options, { cancel = false })]
    local m = member(id)
    if council.persuade(id, which) >= 2 then
      speak(m, "...That changes things.")
    elseif (which.sway[id] or 0) > 0 then
      speak(m, "A fair point, but I'm not sure.")
    else
      speak(m, "That doesn't move me.")
    end
  end
  local result, votes, count = council.tally()
  for _, m in ipairs(council.members) do
    speak(m, "I vote to " .. (votes[m.id] == "flank" and "strike the captain." or "hold the ridge."))
  end
  dialogue.say("Cora", string.format("%d to %d. We %s.", math.max(count.hold, count.flank), math.min(count.hold, count.flank),
    result == "flank" and "strike the captain" or "hold the ridge"), { portrait = "portraits/cora.png" })
  state.set_var("route", result)
  state.set_flag("council_done")
  return result
end

return council

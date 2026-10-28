return function(ev)
  if get_flag("crystal_restored") then
    say("", "The crystal shines with a quiet warmth.")
    return
  end
  say("Mira", "The guardian... it's drawing power from the crystal!")
  if choice({"Face the guardian", "Retreat"}) == 1 then
    local result = battle("guardian")
    if result == "victory" then
      set_flag("crystal_restored")
      give_item("crystal")
      say("Aster", "Its light is returning. Let's tell the elder.")
    end
  end
end

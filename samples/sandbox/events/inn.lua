return function(ev)
  ev:face_player()
  say("Innkeeper", "A warm bed and a meal for 20 gold. Stay the night?")
  if choice({"Rest (20 gold)", "No thanks"}) == 1 then
    if gold() >= 20 then
      add_gold(-20)
      fade_out(0.4)
      rest_party()
      wait(0.4)
      fade_in(0.4)
      say("Innkeeper", "May the road be kind to you.")
    else
      say("Innkeeper", "You don't have enough gold.")
    end
  end
end

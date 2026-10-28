return function(ev)
  ev:face_player()
  if get_flag("crystal_restored") then
    say("Elder", "The light has returned! {color:gold}Amber Vale{color:reset} owes you a debt, {player}.")
    if not get_flag("reward_claimed") then
      add_gold(200)
      set_flag("reward_claimed")
      say("", "Received {color:gold}200 gold{color:reset}.")
    end
  elseif get_flag("quest_started") then
    say("Elder", "Follow the northern path to the grotto. The guardian protects our crystal.")
  else
    say("Elder", "The Vale Crystal is fading, {player}.{wait:20} Will you seek its light in the northern grotto?")
    local answer = choice({"We'll help.", "We need to prepare."})
    if answer == 1 then
      set_flag("quest_started")
      say("Elder", "Take care. You can buy supplies from the merchant and rest at the inn.")
    else
      say("Elder", "Of course. I will wait here.")
    end
  end
end

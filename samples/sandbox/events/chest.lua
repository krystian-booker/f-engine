return function()
  if not get_flag("town_chest_opened") then
    give_item("potion", 2)
    set_flag("town_chest_opened")
    say("", "Found two {color:gold}Potions{color:reset}!")
  else
    say("", "The chest is empty.")
  end
end

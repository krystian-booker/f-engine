return function()
  play_bgm("audio/vale.wav", {loop_start = 0.5, loop_end = 1.5, fade = 0.1})
  wait(1)
  play_sfx("audio/chime.wav")
  play_bgm("audio/grotto.wav", {loop_start = 1, loop_end = 3, fade = 0.4})
  wait(1)
  set_flag("audio_smoke_complete")
end

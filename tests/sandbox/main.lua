-- Sandbox robustness tests: each module under tests/ turns probes of the Lua
-- sandbox (hostile or careless scripts) into checks that the engine raises a
-- clear error and stays responsive instead of crashing, corrupting state or
-- hanging. CTest runs this folder as `lua_sandbox`, with the arena map of
-- tests/game copied in (tests/CMakeLists.txt). Tests install
-- SANDBOX_UPDATE to run code inside the engine's own ticks.
return {
  init = function() end,
  update = function() if _G.SANDBOX_UPDATE then _G.SANDBOX_UPDATE() end end,
  draw = function() end,
}

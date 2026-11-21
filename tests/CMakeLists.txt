add_executable(fengine_tests
  main.cpp
  GridTests.cpp
  TextTests.cpp
  WorldTests.cpp
  ServiceTests.cpp
  ScriptTests.cpp
  RenderTests.cpp
  PostTests.cpp
  EngineTests.cpp
  MiscTests.cpp
  EditorTests.cpp
  ReflectionTests.cpp SessionTests.cpp
  ReplayTests.cpp
  PlatformTests.cpp
  PackTests.cpp
  SkinTests.cpp
  # The renderer's CPU-only parts; the rest of engine/render needs bgfx.
  ${PROJECT_SOURCE_DIR}/engine/render/LightGrid.cpp
  ${PROJECT_SOURCE_DIR}/engine/render/ShadowCascades.cpp
  ${PROJECT_SOURCE_DIR}/engine/render/OverlayGeometry.cpp
  ${PROJECT_SOURCE_DIR}/engine/render/SpriteGeometry.cpp)
target_link_libraries(fengine_tests PRIVATE fengine_core doctest::doctest)
target_compile_definitions(fengine_tests PRIVATE
  FENGINE_TEST_GAME="${PROJECT_SOURCE_DIR}/tests/game"
  FENGINE_SAMPLES="${PROJECT_SOURCE_DIR}/samples"
  FENGINE_BUILTIN="${PROJECT_SOURCE_DIR}/builtin")
add_test(NAME fengine_tests COMMAND fengine_tests)

if(FENGINE_BUILD_RUNTIME)
  # Runtime services that need SDL or miniaudio but no window or GPU.
  add_executable(fengine_runtime_tests main.cpp AudioTests.cpp InputTests.cpp ShaderTests.cpp)
  target_link_libraries(fengine_runtime_tests PRIVATE fengine_runtime doctest::doctest)
  target_compile_definitions(fengine_runtime_tests PRIVATE
    FENGINE_ENGINE_SHADERS="${PROJECT_BINARY_DIR}/shaders"
    FENGINE_TEST_GAME="${PROJECT_SOURCE_DIR}/tests/game"
    FENGINE_BUILTIN="${PROJECT_SOURCE_DIR}/builtin")
  add_test(NAME fengine_runtime_tests COMMAND fengine_runtime_tests)
endif()

# The copy mode of cmake/MirrorFolder.cmake, which stages builtin and shaders
# on Windows, run again after the source changes.
add_test(NAME mirror_folder COMMAND "${CMAKE_COMMAND}" "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/mirror-test"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/MirrorFolderTest.cmake")
# The engine's Lua-facing behaviour, tested from Lua with the built-in runner.
add_test(NAME lua_engine COMMAND fengine_headless --test "${PROJECT_SOURCE_DIR}/tests/game")
# Hostile and careless scripts (sandbox/tests) against the arena map of the
# fixture game, assembled into one game folder.
set(sandbox_game "${CMAKE_CURRENT_BINARY_DIR}/sandbox-game")
file(REMOVE_RECURSE "${sandbox_game}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sandbox/" DESTINATION "${sandbox_game}")
file(COPY "${PROJECT_SOURCE_DIR}/tests/game/maps/arena" DESTINATION "${sandbox_game}/maps")
file(GLOB_RECURSE sandbox_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sandbox/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${sandbox_sources})
add_test(NAME lua_sandbox COMMAND fengine_headless --test "${sandbox_game}")
# A game in a folder named outside any legacy code page: on Windows the
# arguments must arrive as UTF-8 (the executable's manifest).
set(unicode_game "${CMAKE_CURRENT_BINARY_DIR}/unicode-path/Józef-łódź-ゲーム")
add_test(NAME unicode_path_copy COMMAND "${CMAKE_COMMAND}" -E copy_directory
  "${PROJECT_SOURCE_DIR}/samples/starfall" "${unicode_game}")
add_test(NAME check_unicode_path COMMAND fengine_headless --check "${unicode_game}")
set_tests_properties(unicode_path_copy PROPERTIES FIXTURES_SETUP unicode_game)
set_tests_properties(check_unicode_path PROPERTIES FIXTURES_REQUIRED unicode_game)
foreach(sample IN ITEMS starfall aetherbound)
  if(EXISTS "${PROJECT_SOURCE_DIR}/samples/${sample}/game.json")
    add_test(NAME check_${sample} COMMAND fengine_headless --check "${PROJECT_SOURCE_DIR}/samples/${sample}")
    if(EXISTS "${PROJECT_SOURCE_DIR}/samples/${sample}/tests")
      add_test(NAME lua_${sample} COMMAND fengine_headless --test "${PROJECT_SOURCE_DIR}/samples/${sample}")
    endif()
  endif()
endforeach()

# The committed walkability of the sample's maps is the bake of their terrain and placed models.
if(EXISTS "${PROJECT_SOURCE_DIR}/samples/starfall/game.json")
  add_test(NAME bake_check_starfall COMMAND fengine_headless --bake "${PROJECT_SOURCE_DIR}/samples/starfall" --check-only)
endif()

# The starfall sample packed into a .pak archive passes the same check and tests.
if(FENGINE_BUILD_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/samples/starfall/game.json")
  set(pak "${CMAKE_CURRENT_BINARY_DIR}/pak-test/starfall.pak")
  add_test(NAME pak_starfall COMMAND fengine_assetc pack "${PROJECT_SOURCE_DIR}/samples/starfall" "${pak}")
  add_test(NAME pak_check_starfall COMMAND fengine_headless --check "${pak}")
  add_test(NAME pak_lua_starfall COMMAND fengine_headless --test "${pak}")
  set_tests_properties(pak_starfall PROPERTIES FIXTURES_SETUP starfall_pak)
  set_tests_properties(pak_check_starfall pak_lua_starfall PROPERTIES FIXTURES_REQUIRED starfall_pak)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND FENGINE_BUILD_RUNTIME AND FENGINE_BUILD_TOOLS)
  # tools/package_game.py end to end: a pak-based distribution of starfall whose
  # packaged runtime passes --check.
  set(package "${CMAKE_CURRENT_BINARY_DIR}/package-test")
  add_test(NAME package_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${package}")
  # Text the packager reads or writes without naming an encoding (the
  # locale's, not UTF-8, on Windows) fails the test on every host.
  add_test(NAME package_starfall COMMAND "${Python3_EXECUTABLE}" -X warn_default_encoding -W error::EncodingWarning
    "${PROJECT_SOURCE_DIR}/tools/package_game.py"
    --runtime "$<TARGET_FILE:fengine>" --assetc "$<TARGET_FILE:fengine_assetc>"
    --shaders "${PROJECT_BINARY_DIR}/shaders" --licenses "${PROJECT_BINARY_DIR}/licenses"
    --shaderc "${shader_compiler}" --shader-include "${PROJECT_BINARY_DIR}/shader-include"
    --game "${PROJECT_SOURCE_DIR}/samples/starfall" --output "${package}/starfall" --zip --self-check)
  set_tests_properties(package_clean PROPERTIES FIXTURES_SETUP package_clean)
  set_tests_properties(package_starfall PROPERTIES FIXTURES_REQUIRED package_clean TIMEOUT 300)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME package_launchers COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/package_launchers.py")
  add_test(NAME engine_boundary COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ci/check_boundary.py")
  add_test(NAME api_stubs_current COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ci/check_api.py"
    "$<TARGET_FILE:fengine_headless>")
endif()

# The asset kits (tools/modelkit, animkit, texturekit, spritekit): each
# validator proves itself on flawed fixtures, and every model, animation,
# texture and sprite a sample ships passes its manifest: the measured checks
# for its profile, the engine's own loader, and a visual review of exactly
# these files. Needs numpy and pillow; without them the tests are not registered.
if(Python3_Interpreter_FOUND)
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy, PIL"
    RESULT_VARIABLE kits_missing OUTPUT_QUIET ERROR_QUIET)
  if(kits_missing EQUAL 0)
    foreach(kit IN ITEMS model:models anim:animations texture:textures sprite:sprites)
      string(REPLACE ":" ";" kit "${kit}")
      list(GET kit 0 name)
      list(GET kit 1 section)
      if(EXISTS "${PROJECT_SOURCE_DIR}/tools/${name}kit/selftest.py")
        add_test(NAME ${name}kit_selftest COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/${name}kit/selftest.py")
        set_tests_properties(${name}kit_selftest PROPERTIES TIMEOUT 300)
        if(EXISTS "${PROJECT_SOURCE_DIR}/tools/aetherbound/${section}.json")
          add_test(NAME ${section}_aetherbound COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/${name}kit/validate.py"
            --manifest "${PROJECT_SOURCE_DIR}/tools/aetherbound/${section}.json" --engine "$<TARGET_FILE:fengine_headless>")
          set_tests_properties(${section}_aetherbound PROPERTIES TIMEOUT 300)
        endif()
      endif()
    endforeach()
  else()
    message(STATUS "asset kit tests skipped: python3 lacks numpy or pillow (pip install numpy pillow)")
  endif()
endif()

# Every replay runs headless (no GPU; captures skipped). With
# FENGINE_RUN_REPLAYS it also runs in the windowed runtime (needs a display).
file(GLOB replays CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/samples/*/replays/*.json")
foreach(replay IN LISTS replays)
  get_filename_component(name "${replay}" NAME_WE)
  get_filename_component(folder "${replay}" DIRECTORY)
  get_filename_component(game "${folder}" DIRECTORY)
  get_filename_component(sample "${game}" NAME)
  add_test(NAME replay_headless_${sample}_${name} COMMAND fengine_headless "${game}" --replay "${replay}")
  set_tests_properties(replay_headless_${sample}_${name} PROPERTIES TIMEOUT 300)
  if(FENGINE_BUILD_RUNTIME AND FENGINE_RUN_REPLAYS)
    add_test(NAME replay_${sample}_${name}
      COMMAND fengine "${game}" --mute --replay "${replay}" --replay-skip 30
              --user-data "${CMAKE_BINARY_DIR}/replay-user/${sample}-${name}")
    # --replay-skip: between the frames a replay captures the game ticks
    # without rendering, so a ten-minute playthrough fits a CI runner whose
    # renderer is software.
    set_tests_properties(replay_${sample}_${name} PROPERTIES LABELS windowed TIMEOUT 900)
  endif()
endforeach()

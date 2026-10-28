add_executable(fengine_tests
  main.cpp
  GridTests.cpp
  TextTests.cpp
  WorldTests.cpp
  ServiceTests.cpp
  ScriptTests.cpp)
target_link_libraries(fengine_tests PRIVATE fengine_core doctest::doctest)
target_compile_definitions(fengine_tests PRIVATE
  FENGINE_TEST_GAME="${PROJECT_SOURCE_DIR}/lualib/tests/game"
  FENGINE_LUALIB="${PROJECT_SOURCE_DIR}/lualib")
add_test(NAME cpp_services COMMAND fengine_tests)

add_test(NAME lua_library COMMAND fengine_headless --test "${PROJECT_SOURCE_DIR}/lualib/tests/game")
foreach(sample IN ITEMS vale wayfarers ridgeline)
  if(EXISTS "${PROJECT_SOURCE_DIR}/samples/${sample}/game.json")
    add_test(NAME check_${sample} COMMAND fengine_headless --check "${PROJECT_SOURCE_DIR}/samples/${sample}")
    if(EXISTS "${PROJECT_SOURCE_DIR}/samples/${sample}/tests")
      add_test(NAME lua_${sample} COMMAND fengine_headless --test "${PROJECT_SOURCE_DIR}/samples/${sample}")
    endif()
  endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME engine_boundary COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ci/check_boundary.py")
  add_test(NAME api_stubs_current COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ci/check_api.py"
    "$<TARGET_FILE:fengine_headless>")
endif()

if(FENGINE_BUILD_RUNTIME AND FENGINE_RUN_REPLAYS)
  file(GLOB replays CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/samples/*/replays/*.json")
  foreach(replay IN LISTS replays)
    get_filename_component(name "${replay}" NAME_WE)
    get_filename_component(folder "${replay}" DIRECTORY)
    get_filename_component(game "${folder}" DIRECTORY)
    get_filename_component(sample "${game}" NAME)
    add_test(NAME replay_${sample}_${name}
      COMMAND fengine "${game}" --mute --replay "${replay}" --user-data "${CMAKE_BINARY_DIR}/replay-user/${sample}-${name}")
    set_tests_properties(replay_${sample}_${name} PROPERTIES LABELS runtime TIMEOUT 300)
  endforeach()
endif()

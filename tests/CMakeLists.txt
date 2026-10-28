add_executable(fengine_tests CoreTests.cpp WorldRpgTests.cpp ScriptTests.cpp FieldTests.cpp AssetUiTests.cpp)
target_link_libraries(fengine_tests PRIVATE fengine_logic doctest::doctest)
target_compile_definitions(fengine_tests PRIVATE FENGINE_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/samples/sandbox")
add_test(NAME fengine_headless COMMAND fengine_tests)

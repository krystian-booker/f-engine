add_executable(fengine_assetc main.cpp)
target_link_libraries(fengine_assetc PRIVATE fengine_core)
# Texture cooking needs bimg's encoders, which come with the runtime's dependencies.
if(TARGET bimg_encode)
  target_link_libraries(fengine_assetc PRIVATE bimg_encode bimg bx)
  target_compile_definitions(fengine_assetc PRIVATE FENGINE_ASSETC_COOK)
endif()
fengine_warnings(fengine_assetc)
fengine_windows_manifest(fengine_assetc)
install(TARGETS fengine_assetc RUNTIME DESTINATION bin)

if(FENGINE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    # The output folder doesn't exist beforehand: assetc creates it.
    set(output "${CMAKE_CURRENT_BINARY_DIR}/atlas-test/out")
    add_test(NAME assetc_atlas_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/atlas-test")
    add_test(NAME assetc_atlas COMMAND fengine_assetc atlas "${CMAKE_CURRENT_SOURCE_DIR}/tests/atlas.json" "${output}")
    add_test(NAME assetc_atlas_output
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_atlas.py" "${output}")
    set_tests_properties(assetc_atlas_clean PROPERTIES FIXTURES_SETUP assetc_clean)
    set_tests_properties(assetc_atlas PROPERTIES FIXTURES_REQUIRED assetc_clean FIXTURES_SETUP assetc_packed)
    set_tests_properties(assetc_atlas_output PROPERTIES FIXTURES_REQUIRED assetc_packed)
    if(TARGET bimg_encode)
      # Cooks a copy of the vale rendering assets and checks the KTX files.
      set(cooked "${CMAKE_CURRENT_BINARY_DIR}/cook-test")
      add_test(NAME assetc_cook_copy COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/samples/vale/fx" "${cooked}/fx")
      add_test(NAME assetc_cook COMMAND fengine_assetc cook "${cooked}" --force)
      add_test(NAME assetc_cook_output
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cook.py" "${cooked}/fx")
      set_tests_properties(assetc_cook_copy PROPERTIES FIXTURES_SETUP assetc_cook_source)
      set_tests_properties(assetc_cook PROPERTIES FIXTURES_REQUIRED assetc_cook_source FIXTURES_SETUP assetc_cooked)
      set_tests_properties(assetc_cook_output PROPERTIES FIXTURES_REQUIRED assetc_cooked)
    endif()
  endif()
endif()

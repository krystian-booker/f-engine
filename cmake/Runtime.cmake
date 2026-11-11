# The windowed runtime: SDL3 window/input, bgfx rendering, miniaudio, the
# developer editor and the main loop.
add_library(fengine_audio STATIC engine/audio/MiniAudio.cpp)
target_link_libraries(fengine_audio PUBLIC fengine_core PRIVATE fengine_audio_dependency)
fengine_warnings(fengine_audio)

add_library(fengine_runtime STATIC
  engine/platform/Window.cpp
  engine/input/SdlInput.cpp
  engine/render/GpuAssets.cpp
  engine/render/Renderer.cpp
  engine/render/ImGuiRenderer.cpp
  engine/render/RenderCommon.cpp
  engine/render/LightGrid.cpp
  engine/render/ShadowCascades.cpp
  engine/render/SceneLighting.cpp
  engine/render/SpriteBatch.cpp
  engine/render/SpriteGeometry.cpp
  engine/render/OverlayGeometry.cpp
  engine/render/WorldRenderer.cpp
  engine/render/CustomShaders.cpp
  engine/render/PostPass.cpp
  engine/render/CanvasRenderer.cpp
  engine/render/FrameRenderer.cpp
  engine/runtime/MainLoop.cpp
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/Editor.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorWidgets.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorWorldTab.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorMapTab.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorMapObjects.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorDataTab.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorToolsTabs.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorShell.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorViewport.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorHierarchy.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorInspector.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorAssetsPanel.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorScripts.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorPickers.cpp>
  $<${FENGINE_DEV_ENABLED}:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorScript.cpp>
)
# Dismissing the OS file dialog from a scripted walkthrough needs AppKit on macOS.
if(FENGINE_DEV_ENABLED)
  if(APPLE)
    target_sources(fengine_runtime PRIVATE engine/editor/EditorNativeMac.mm)
    set_source_files_properties(engine/editor/EditorNativeMac.mm PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_link_libraries(fengine_runtime PUBLIC "-framework AppKit")
  else()
    target_sources(fengine_runtime PRIVATE engine/editor/EditorNativeStub.cpp)
  endif()
endif()
target_link_libraries(fengine_runtime PUBLIC fengine_core fengine_audio SDL3::SDL3 bgfx bx fengine_imgui)
# Development builds fall back to the build and source trees (shaders, shader
# hot rebuild); ship builds only look next to the executable.
target_compile_definitions(fengine_runtime PRIVATE
  $<${FENGINE_DEV_ENABLED}:FENGINE_SHADER_DIR="${PROJECT_BINARY_DIR}/shaders">
  $<${FENGINE_DEV_ENABLED}:FENGINE_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/shaders">
  $<${FENGINE_DEV_ENABLED}:FENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}">
  $<${FENGINE_DEV_ENABLED}:FENGINE_EDITOR_FONT="${FENGINE_EDITOR_FONT}">
  $<${FENGINE_DEV_ENABLED}:FENGINE_CMAKE="${CMAKE_COMMAND}">)
fengine_warnings(fengine_runtime)
add_dependencies(fengine_runtime fengine_shaders)

option(FENGINE_MACOS_BUNDLE "macOS: build fengine as an .app bundle with shaders and builtin in Resources" OFF)
add_executable(fengine apps/runtime/main.cpp)
target_link_libraries(fengine PRIVATE fengine_runtime)
target_compile_definitions(fengine PRIVATE
  $<${FENGINE_DEV_ENABLED}:FENGINE_SHADER_DIR="${PROJECT_BINARY_DIR}/shaders">)
fengine_warnings(fengine)
# builtin is staged next to the executables (CMakeLists.txt); building only
# this target must stage it too.
if(TARGET fengine_builtin)
  add_dependencies(fengine fengine_builtin)
endif()
# Compiled shaders are built into <build>/shaders. With a multi-configuration
# generator the executable sits in a configuration folder, so they are staged
# beside it the same way as builtin (nothing to do when the folders coincide;
# an app bundle carries its own copy in Resources).
if(NOT (APPLE AND FENGINE_MACOS_BUNDLE))
  set(shader_stage_mode "-DLINK=ON")
  if(WIN32)
    set(shader_stage_mode "-DENABLED=ON")
  endif()
  add_custom_target(fengine_shader_stage ALL
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${PROJECT_BINARY_DIR}/shaders"
      "-DDESTINATION=$<TARGET_FILE_DIR:fengine>/shaders" "${shader_stage_mode}"
      -P "${PROJECT_SOURCE_DIR}/cmake/MirrorFolder.cmake"
    COMMENT "Staging compiled shaders" VERBATIM)
  add_dependencies(fengine_shader_stage fengine_shaders)
  add_dependencies(fengine fengine_shader_stage)
endif()
# A GUI program on Windows (no console window); it attaches to the console
# it was started from so command-line output still appears.
set_target_properties(fengine PROPERTIES WIN32_EXECUTABLE ON)
fengine_windows_manifest(fengine)
if(APPLE AND FENGINE_MACOS_BUNDLE)
  set_target_properties(fengine PROPERTIES
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "f-engine"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.fengine.runtime"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
  add_custom_command(TARGET fengine POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_BINARY_DIR}/shaders"
      "$<TARGET_BUNDLE_CONTENT_DIR:fengine>/Resources/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/builtin/assets"
      "$<TARGET_BUNDLE_CONTENT_DIR:fengine>/Resources/builtin/assets"
    VERBATIM)
  install(TARGETS fengine BUNDLE DESTINATION .)
else()
  install(TARGETS fengine RUNTIME DESTINATION bin)
  install(DIRECTORY "${PROJECT_BINARY_DIR}/shaders/" DESTINATION bin/shaders)
endif()

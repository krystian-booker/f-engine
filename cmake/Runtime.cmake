# The windowed runtime: SDL3 window/input, bgfx rendering, miniaudio, the
# developer editor and the main loop.
add_library(fengine_runtime STATIC
  engine/platform/Window.cpp
  engine/input/SdlInput.cpp
  engine/audio/MiniAudio.cpp
  engine/render/GpuAssets.cpp
  engine/render/Renderer.cpp
  engine/render/ImGuiRenderer.cpp
  engine/render/WorldRenderer.cpp
  engine/render/CanvasRenderer.cpp
  engine/runtime/MainLoop.cpp
  engine/runtime/ReplayHarness.cpp
  $<$<BOOL:${FENGINE_DEV}>:${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/Editor.cpp>
)
target_link_libraries(fengine_runtime PUBLIC fengine_core SDL3::SDL3 bgfx bx fengine_imgui fengine_audio_dependency)
target_compile_definitions(fengine_runtime PRIVATE
  FENGINE_SHADER_DIR="${PROJECT_BINARY_DIR}/shaders"
  FENGINE_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/shaders"
  FENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}"
  FENGINE_CMAKE="${CMAKE_COMMAND}")
fengine_warnings(fengine_runtime)
add_dependencies(fengine_runtime fengine_shaders)

add_executable(fengine apps/runtime/main.cpp)
target_link_libraries(fengine PRIVATE fengine_runtime)
target_compile_definitions(fengine PRIVATE FENGINE_SHADER_DIR="${PROJECT_BINARY_DIR}/shaders")
fengine_windows_manifest(fengine)
install(TARGETS fengine RUNTIME DESTINATION bin)
install(DIRECTORY "${PROJECT_BINARY_DIR}/shaders/" DESTINATION bin/shaders)

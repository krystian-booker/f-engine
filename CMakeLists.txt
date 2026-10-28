cmake_minimum_required(VERSION 3.25)
project(fengine VERSION 0.1.0 LANGUAGES C CXX)
include(cmake/Windows.cmake)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "Target architectures" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build configuration" FORCE)
endif()
option(FENGINE_BUILD_RUNTIME "Build the SDL/bgfx runtime and editor" ON)
option(FENGINE_BUILD_TESTS "Build headless tests" ON)
option(FENGINE_DEV "Enable editor and development file watching" ON)
option(FENGINE_BUILD_TOOLS "Build asset authoring tools" ON)
set(FENGINE_GAME_MODULE "" CACHE PATH "Optional native game-module directory")
option(FENGINE_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(FENGINE_ENABLE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
include(cmake/Dependencies.cmake)

add_library(fengine_logic STATIC
  engine/core/Log.cpp
  engine/core/GameConfig.cpp
  engine/platform/FileWatcher.cpp
  engine/scene/SceneStack.cpp
  engine/world/NavGrid.cpp
  engine/world/Map.cpp
  engine/world/World.cpp
  engine/world/Particles.cpp
  engine/rpg/Database.cpp
  engine/rpg/GameState.cpp
  engine/battle/Battle.cpp
  engine/battle/ActionSequence.cpp
  engine/script/ScriptEngine.cpp
  engine/ui/Text.cpp
  engine/ui/BitmapFont.cpp
  engine/assets/Image.cpp
  engine/assets/Mesh.cpp
  engine/assets/SpriteAnimation.cpp
  engine/render/Camera.cpp
  engine/editor/GridPicking.cpp
)
target_include_directories(fengine_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fengine_logic PUBLIC nlohmann_json::nlohmann_json glm::glm EnTT::EnTT fengine_sol2 fengine_asset_formats)
target_compile_definitions(fengine_logic PUBLIC FENGINE_DEV=$<BOOL:${FENGINE_DEV}>)
if(MSVC)
  target_compile_options(fengine_logic PRIVATE /W4 /permissive-)
else()
  target_compile_options(fengine_logic PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FENGINE_BUILD_RUNTIME)
  include(cmake/Shaders.cmake)
  add_library(fengine_core STATIC
    engine/core/App.cpp
    engine/platform/Window.cpp
    engine/render/Renderer.cpp
    engine/render/ImGuiRenderer.cpp
    engine/render/WorldRenderer.cpp
    engine/audio/Audio.cpp
    engine/input/Input.cpp
    engine/ui/Canvas.cpp
    engine/scene/GameSession.cpp
    engine/editor/Editor.cpp
  )
  target_link_libraries(fengine_core PUBLIC fengine_logic SDL3::SDL3 bgfx bx fengine_imgui fengine_audio_dependency)
  target_compile_definitions(fengine_core PRIVATE
    FENGINE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
    FENGINE_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/shaders"
    FENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}"
    FENGINE_CMAKE="${CMAKE_COMMAND}")
  add_dependencies(fengine_core fengine_shaders)
  if(FENGINE_GAME_MODULE)
    get_filename_component(FENGINE_GAME_MODULE "${FENGINE_GAME_MODULE}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    add_subdirectory("${FENGINE_GAME_MODULE}" "${PROJECT_BINARY_DIR}/game-module")
    if(NOT TARGET fengine_game_module)
      message(FATAL_ERROR "Game module must define a target named fengine_game_module")
    endif()
  endif()
  add_subdirectory(apps/runtime)
endif()

if(FENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(FENGINE_BUILD_TOOLS)
  add_subdirectory(tools/assetc)
endif()
if(FENGINE_BUILD_RUNTIME)
  include(cmake/Packaging.cmake)
endif()

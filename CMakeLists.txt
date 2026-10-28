cmake_minimum_required(VERSION 3.25)
project(fengine VERSION 0.2.0 LANGUAGES C CXX)
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
option(FENGINE_BUILD_TESTS "Build tests" ON)
option(FENGINE_DEV "Enable the editor and hot reload in the runtime" ON)
option(FENGINE_BUILD_TOOLS "Build asset authoring tools" ON)
option(FENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings in engine code as errors" OFF)
option(FENGINE_RUN_REPLAYS "Register windowed replay tests with CTest (needs a GPU/display)" OFF)
option(FENGINE_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(FENGINE_ENABLE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
include(cmake/Dependencies.cmake)

function(fengine_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- $<$<BOOL:${FENGINE_WARNINGS_AS_ERRORS}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${FENGINE_WARNINGS_AS_ERRORS}>:-Werror>)
  endif()
endfunction()

# Headless engine services: everything except the window, GPU and audio device.
add_library(fengine_core STATIC
  engine/core/AssetCache.cpp
  engine/core/GameConfig.cpp
  engine/core/Json.cpp
  engine/core/Log.cpp
  engine/core/Paths.cpp
  engine/assets/Image.cpp
  engine/assets/Model.cpp
  engine/assets/SpriteAnimation.cpp
  engine/text/Font.cpp
  engine/text/Markup.cpp
  engine/text/TextLayout.cpp
  engine/text/TextSystem.cpp
  engine/nav/Grid.cpp
  engine/nav/Query.cpp
  engine/world/Camera.cpp
  engine/world/MapData.cpp
  engine/world/World.cpp
  engine/services/Canvas.cpp
  engine/services/InputState.cpp
  engine/services/SaveStore.cpp
  engine/platform/FileWatcher.cpp
  engine/script/Binder.cpp
  engine/script/LuaHost.cpp
  engine/script/bindings/CoreBindings.cpp
  engine/script/bindings/EntityBindings.cpp
  engine/script/bindings/ServiceBindings.cpp
  engine/script/bindings/UiBindings.cpp
  engine/script/bindings/WorldBindings.cpp
  engine/runtime/Engine.cpp
  engine/runtime/Headless.cpp
)
target_include_directories(fengine_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fengine_core PUBLIC nlohmann_json::nlohmann_json glm::glm EnTT::EnTT fengine_sol2 fengine_asset_formats)
target_compile_definitions(fengine_core PUBLIC FENGINE_DEV=$<BOOL:${FENGINE_DEV}>
  PRIVATE FENGINE_SOURCE_LUALIB="${PROJECT_SOURCE_DIR}/lualib")
fengine_warnings(fengine_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # sol2 bindings are template heavy; keep debug objects reasonable.
  set_source_files_properties(engine/script/bindings/EntityBindings.cpp engine/script/bindings/WorldBindings.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O1>")
endif()

add_executable(fengine_headless apps/headless/main.cpp)
target_link_libraries(fengine_headless PRIVATE fengine_core)

if(FENGINE_BUILD_RUNTIME)
  include(cmake/Shaders.cmake)
  include(cmake/Runtime.cmake)
endif()

if(FENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(FENGINE_BUILD_TOOLS)
  add_subdirectory(tools/assetc)
endif()
include(cmake/Packaging.cmake)

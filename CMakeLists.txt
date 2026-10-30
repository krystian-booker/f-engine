cmake_minimum_required(VERSION 3.25)
project(fengine VERSION 0.2.0 LANGUAGES C CXX)
include(cmake/Windows.cmake)

option(FENGINE_UNIVERSAL "macOS: build universal binaries (arm64 and x86_64)" OFF)
if(APPLE AND FENGINE_UNIVERSAL)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures" FORCE)
elseif(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
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
# Development features (editor, hot reload, error overlay, source-tree
# fallbacks). AUTO enables them for Debug configurations only.
set(FENGINE_DEV AUTO CACHE STRING "Editor and hot reload in the runtime: AUTO (Debug only), ON or OFF")
set_property(CACHE FENGINE_DEV PROPERTY STRINGS AUTO ON OFF)
if(FENGINE_DEV STREQUAL "AUTO")
  set(FENGINE_DEV_ENABLED "$<CONFIG:Debug>")
elseif(FENGINE_DEV)
  set(FENGINE_DEV_ENABLED 1)
else()
  set(FENGINE_DEV_ENABLED 0)
endif()
option(FENGINE_BUILD_TOOLS "Build asset authoring tools" ON)
option(FENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings in engine code as errors" OFF)
option(FENGINE_RUN_REPLAYS "Register windowed replay tests with CTest (needs a GPU/display)" OFF)
option(FENGINE_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(FENGINE_ENABLE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
  # Lua allows 200 nested C calls (metamethods, pcall, callbacks through the
  # bindings). With AddressSanitizer's red zones those frames are several
  # times larger, so the main thread gets a 256 MiB stack, as an
  # unsanitized build's 8 MiB comfortably holds them: on macOS through the
  # linker, on Linux by raising RLIMIT_STACK at startup (LuaHost.cpp).
  add_compile_definitions(FENGINE_SANITIZERS=1)
  if(APPLE)
    add_link_options(-Wl,-stack_size,0x10000000)
  endif()
endif()
# Ship builds don't embed the build machine's paths (__FILE__, debug info)
# and, on MSVC, write reproducible objects and executables (/Brepro).
set(path_prefixes "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "$ENV{CPM_SOURCE_CACHE}" "${CPM_SOURCE_CACHE}")
get_cmake_property(cache_variables CACHE_VARIABLES)
foreach(variable IN LISTS cache_variables)
  if(variable MATCHES "^CPM_.+_SOURCE$")
    list(APPEND path_prefixes "${${variable}}") # dependency sources supplied from elsewhere
  endif()
endforeach()
# Deterministic floating point: no fused multiply-add contraction, so a seed
# and a replay give the same results on every compiler and CPU. Then the path
# mapping and reproducible-build flags for ship builds.
if(MSVC)
  add_compile_options(/fp:precise)
  foreach(prefix IN LISTS path_prefixes)
    if(prefix)
      file(TO_NATIVE_PATH "${prefix}" native)
      add_compile_options("$<$<NOT:${FENGINE_DEV_ENABLED}>:/pathmap:${native}=.>")
    endif()
  endforeach()
  add_compile_options("$<$<NOT:${FENGINE_DEV_ENABLED}>:/Brepro>")
  add_link_options("$<$<NOT:${FENGINE_DEV_ENABLED}>:/Brepro>")
else()
  add_compile_options(-ffp-contract=off)
  foreach(prefix IN LISTS path_prefixes)
    if(prefix)
      add_compile_options("$<$<NOT:${FENGINE_DEV_ENABLED}>:-ffile-prefix-map=${prefix}/=>")
    endif()
  endforeach()
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
  engine/core/NumberFormat.cpp
  engine/core/Pack.cpp
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
  engine/world/Animation.cpp
  engine/world/Camera.cpp
  engine/world/Collision.cpp
  engine/world/MapData.cpp
  engine/world/Movement.cpp
  engine/world/Particles.cpp
  engine/world/World.cpp
  engine/services/Canvas.cpp
  engine/services/InputState.cpp
  engine/services/PlatformServices.cpp
  engine/services/SaveStore.cpp
  engine/platform/FileWatcher.cpp
  engine/script/Binder.cpp
  engine/script/GcHold.cpp
  engine/script/LuaHost.cpp
  engine/script/LuaJson.cpp
  engine/script/ModulePatcher.cpp
  engine/script/bindings/Convert.cpp
  engine/script/bindings/CoreBindings.cpp
  engine/script/bindings/EntityBindings.cpp
  engine/script/bindings/PlatformBindings.cpp
  engine/script/bindings/ServiceBindings.cpp
  engine/script/bindings/UiBindings.cpp
  engine/script/bindings/WorldBindings.cpp
  engine/runtime/Engine.cpp
  engine/runtime/EmitterAudio.cpp
  engine/runtime/Headless.cpp
  engine/runtime/ReplayHarness.cpp
  engine/runtime/WorldLoader.cpp
  engine/editor/MapEdits.cpp
)
target_include_directories(fengine_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fengine_core PUBLIC nlohmann_json::nlohmann_json glm::glm EnTT::EnTT fengine_sol2 fengine_asset_formats)
target_compile_definitions(fengine_core PUBLIC FENGINE_DEV=${FENGINE_DEV_ENABLED}
  PRIVATE $<${FENGINE_DEV_ENABLED}:FENGINE_SOURCE_LUALIB="${PROJECT_SOURCE_DIR}/lualib">)
fengine_warnings(fengine_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # sol2 bindings are template heavy; keep debug objects reasonable.
  set_source_files_properties(engine/script/bindings/EntityBindings.cpp engine/script/bindings/WorldBindings.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O1>")
endif()

add_executable(fengine_headless apps/headless/main.cpp)
target_link_libraries(fengine_headless PRIVATE fengine_core)
fengine_warnings(fengine_headless)
fengine_windows_manifest(fengine_headless) # UTF-8 arguments and paths on Windows

# Executables look for lualib beside them (development builds also in parent
# folders), so the build tree carries it next to the executables: a link to
# the sources, or on Windows a copy that every build brings up to date. With a
# multi-configuration generator (Visual Studio, Xcode, Ninja Multi-Config)
# that is each configuration's folder (build/Release/lualib), where a shipped
# configuration, which looks nowhere else, finds it. Development
# configurations on Windows use the source tree directly (hot reload sees
# edits there).
if(WIN32)
  set(FENGINE_STAGE_MODE "-DENABLED=$<NOT:${FENGINE_DEV_ENABLED}>")
else()
  set(FENGINE_STAGE_MODE "-DLINK=ON")
endif()
if(NOT PROJECT_BINARY_DIR STREQUAL PROJECT_SOURCE_DIR)
  add_custom_target(fengine_lualib ALL
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${PROJECT_SOURCE_DIR}/lualib"
      "-DDESTINATION=$<TARGET_FILE_DIR:fengine_headless>/lualib" "${FENGINE_STAGE_MODE}"
      -P "${PROJECT_SOURCE_DIR}/cmake/MirrorFolder.cmake"
    COMMENT "Staging lualib" VERBATIM)
  add_dependencies(fengine_headless fengine_lualib)
endif()

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

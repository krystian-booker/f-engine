include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")

CPMAddPackage(NAME json VERSION 3.12.0
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF")
CPMAddPackage(NAME glm VERSION 1.0.1
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
  OPTIONS "GLM_BUILD_TESTS OFF" "GLM_BUILD_INSTALL OFF")
CPMAddPackage(NAME EnTT VERSION 3.15.0
  URL https://github.com/skypjack/entt/archive/refs/tags/v3.15.0.tar.gz
  OPTIONS "ENTT_BUILD_TESTING OFF")
CPMAddPackage(NAME lua VERSION 5.4.8
  URL https://www.lua.org/ftp/lua-5.4.8.tar.gz DOWNLOAD_ONLY YES)
file(GLOB lua_sources CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/src/lua.c" "${lua_SOURCE_DIR}/src/luac.c")
add_library(fengine_lua STATIC ${lua_sources})
target_include_directories(fengine_lua PUBLIC "${lua_SOURCE_DIR}/src")
if(UNIX)
  target_compile_definitions(fengine_lua PRIVATE LUA_USE_POSIX)
  target_link_libraries(fengine_lua PUBLIC ${CMAKE_DL_LIBS} m)
endif()
CPMAddPackage(NAME sol2 VERSION 3.5.0
  URL https://github.com/ThePhD/sol2/archive/refs/tags/v3.5.0.tar.gz DOWNLOAD_ONLY YES)
add_library(fengine_sol2 INTERFACE)
target_include_directories(fengine_sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
target_link_libraries(fengine_sol2 INTERFACE fengine_lua)
target_compile_definitions(fengine_sol2 INTERFACE SOL_ALL_SAFETIES_ON=1)
CPMAddPackage(NAME cgltf VERSION 1.15
  URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz DOWNLOAD_ONLY YES)
CPMAddPackage(NAME stb
  URL https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz DOWNLOAD_ONLY YES)
add_library(fengine_asset_formats INTERFACE)
target_include_directories(fengine_asset_formats SYSTEM INTERFACE "${cgltf_SOURCE_DIR}" "${stb_SOURCE_DIR}")
if(FENGINE_BUILD_TESTS)
  CPMAddPackage(NAME doctest VERSION 2.4.12
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON")
endif()

if(NOT FENGINE_BUILD_RUNTIME)
  return()
endif()

CPMAddPackage(NAME miniaudio VERSION 0.11.23
  URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.23.tar.gz DOWNLOAD_ONLY YES)
add_library(fengine_audio_dependency INTERFACE)
target_include_directories(fengine_audio_dependency SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")

CPMAddPackage(NAME SDL VERSION 3.2.10
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.10.tar.gz
  OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF" "SDL_HIDAPI_LIBUSB OFF")
# The wrapper and its three sources are pinned together. Archive fetches avoid
# cloning bgfx's large binary asset history and work with CPM_SOURCE_CACHE.
CPMAddPackage(NAME bx_source
  URL https://github.com/bkaradzic/bx/archive/0e5bbca6878000f6371c65c53fe06f6f0ae87ca9.tar.gz
  DOWNLOAD_ONLY YES)
CPMAddPackage(NAME bimg_source
  URL https://github.com/bkaradzic/bimg/archive/9bfbd6493b48a15238f2d1374a718ec026524515.tar.gz
  DOWNLOAD_ONLY YES)
CPMAddPackage(NAME bgfx_source
  URL https://github.com/bkaradzic/bgfx/archive/fe9bbdec66110f8626b9251a4947fc336ac3f62c.tar.gz
  DOWNLOAD_ONLY YES)
set(BX_DIR "${bx_source_SOURCE_DIR}" CACHE PATH "bx source" FORCE)
set(BIMG_DIR "${bimg_source_SOURCE_DIR}" CACHE PATH "bimg source" FORCE)
set(BGFX_DIR "${bgfx_source_SOURCE_DIR}" CACHE PATH "bgfx source" FORCE)
CPMAddPackage(NAME bgfx_cmake
  URL https://github.com/bkaradzic/bgfx.cmake/archive/5ae5fa5b121974b9de084539bb784d97fa4cd8a6.tar.gz
  OPTIONS "BGFX_BUILD_EXAMPLES OFF" "BGFX_BUILD_TESTS OFF" "BGFX_INSTALL OFF"
    "BGFX_CONFIG_MULTITHREADED OFF" "BGFX_BUILD_TOOLS_GEOMETRY OFF"
    "BGFX_BUILD_TOOLS_TEXTURE OFF" "BGFX_BUILD_TOOLS_BIN2C OFF"
    "BGFX_WITH_WAYLAND OFF")
if(MINGW)
  # The engine compiles Shader Model 5 for D3D11. MinGW's SDK provides
  # D3DCompiler, but not Microsoft's optional Shader Model 6 DXC headers.
  target_compile_definitions(shaderc PRIVATE SHADERC_CONFIG_HAS_DXC=0)
  set_source_files_properties("${BGFX_DIR}/tools/shaderc/shaderc_hlsl.cpp"
    TARGET_DIRECTORY shaderc PROPERTIES COMPILE_OPTIONS "-include;ole2.h")
endif()
CPMAddPackage(NAME imgui
  URL https://github.com/ocornut/imgui/archive/52fe0a05a7b1aa180a202bb24f0f2a049a9c1b7d.tar.gz
  DOWNLOAD_ONLY YES)
add_library(fengine_imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
target_include_directories(fengine_imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(fengine_imgui PUBLIC SDL3::SDL3)

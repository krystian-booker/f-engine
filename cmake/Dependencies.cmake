include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")

CPMAddPackage(NAME json VERSION 3.12.0
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF")
CPMAddPackage(NAME glm VERSION 1.0.1
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
  URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
  OPTIONS "GLM_BUILD_TESTS OFF" "GLM_BUILD_INSTALL OFF")
CPMAddPackage(NAME EnTT VERSION 3.15.0
  URL https://github.com/skypjack/entt/archive/refs/tags/v3.15.0.tar.gz
  URL_HASH SHA256=01466fcbf77618a79b62891510c0bbf25ac2804af5751c84982b413852234d66
  OPTIONS "ENTT_BUILD_TESTING OFF")
CPMAddPackage(NAME lua VERSION 5.4.8
  URL https://www.lua.org/ftp/lua-5.4.8.tar.gz
  URL_HASH SHA256=4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae DOWNLOAD_ONLY YES)
file(GLOB lua_sources CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/src/lua.c" "${lua_SOURCE_DIR}/src/luac.c")
# Bounded library loops (pattern matching, table.move, string sizes): see the file.
include("${CMAKE_CURRENT_LIST_DIR}/LuaPatches.cmake")
foreach(lua_patched IN ITEMS lauxlib.c lstrlib.c ltablib.c)
  fe_patch_lua_file("${lua_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/lua-patched" ${lua_patched} lua_patched_file)
  list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/src/${lua_patched}")
  list(APPEND lua_sources "${lua_patched_file}")
endforeach()
add_library(fengine_lua STATIC ${lua_sources})
target_include_directories(fengine_lua PUBLIC "${lua_SOURCE_DIR}/src")
# Constant string-hash seed (see the header) so `pairs` order is reproducible.
if(MSVC)
  target_compile_options(fengine_lua PRIVATE "/FI${PROJECT_SOURCE_DIR}/engine/script/LuaSeed.h")
else()
  target_compile_options(fengine_lua PRIVATE -include "${PROJECT_SOURCE_DIR}/engine/script/LuaSeed.h")
endif()
if(UNIX)
  target_compile_definitions(fengine_lua PRIVATE LUA_USE_POSIX)
  target_link_libraries(fengine_lua PUBLIC ${CMAKE_DL_LIBS} m)
endif()
CPMAddPackage(NAME sol2 VERSION 3.5.0
  URL https://github.com/ThePhD/sol2/archive/refs/tags/v3.5.0.tar.gz
  URL_HASH SHA256=86c0f6d2836b184a250fc2907091c076bf53c9603dd291eaebade36cc342e13c DOWNLOAD_ONLY YES)
add_library(fengine_sol2 INTERFACE)
target_include_directories(fengine_sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
target_link_libraries(fengine_sol2 INTERFACE fengine_lua)
# Errors reach scripts as Lua errors; sol2 would also print every one to stderr.
target_compile_definitions(fengine_sol2 INTERFACE SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)
CPMAddPackage(NAME cgltf VERSION 1.15
  URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
  URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a DOWNLOAD_ONLY YES)
CPMAddPackage(NAME stb
  URL https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz
  URL_HASH SHA256=a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d DOWNLOAD_ONLY YES)
add_library(fengine_asset_formats INTERFACE)
target_include_directories(fengine_asset_formats SYSTEM INTERFACE "${cgltf_SOURCE_DIR}" "${stb_SOURCE_DIR}")
if(FENGINE_BUILD_TESTS)
  CPMAddPackage(NAME doctest VERSION 2.4.12
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON")
endif()

if(NOT FENGINE_BUILD_RUNTIME)
  return()
endif()

CPMAddPackage(NAME miniaudio VERSION 0.11.23
  URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.23.tar.gz
  URL_HASH SHA256=1062155cc5882b55c48cb37f57a4dc783669e83ae0838535c62b206eeb1587a6 DOWNLOAD_ONLY YES)
add_library(fengine_audio_dependency INTERFACE)
target_include_directories(fengine_audio_dependency SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")

CPMAddPackage(NAME SDL VERSION 3.2.10
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.10.tar.gz
  URL_HASH SHA256=8a6ddc3e70de897f7921da275ac71e375de89dad802aa2932649d1fea9068478
  OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF" "SDL_HIDAPI_LIBUSB OFF")
# The wrapper and its three sources are pinned together. Archive fetches avoid
# cloning bgfx's large binary asset history and work with CPM_SOURCE_CACHE.
CPMAddPackage(NAME bx_source
  URL https://github.com/bkaradzic/bx/archive/0e5bbca6878000f6371c65c53fe06f6f0ae87ca9.tar.gz
  URL_HASH SHA256=948f777092e1dc60dcc9d77dacc570617542d652426f8fddbcf98d5cf647ce53
  DOWNLOAD_ONLY YES)
CPMAddPackage(NAME bimg_source
  URL https://github.com/bkaradzic/bimg/archive/9bfbd6493b48a15238f2d1374a718ec026524515.tar.gz
  URL_HASH SHA256=b531e97fa5db1060a11b9831dedc7c9a5190eab4fcf3b3c9553b64d0f66596b0
  DOWNLOAD_ONLY YES)
CPMAddPackage(NAME bgfx_source
  URL https://github.com/bkaradzic/bgfx/archive/fe9bbdec66110f8626b9251a4947fc336ac3f62c.tar.gz
  URL_HASH SHA256=1357a5dfeb8534f8c9a32ea9e993084871f8ceb55147c36e1d8dde7d63941b40
  DOWNLOAD_ONLY YES)
set(BX_DIR "${bx_source_SOURCE_DIR}" CACHE PATH "bx source" FORCE)
set(BIMG_DIR "${bimg_source_SOURCE_DIR}" CACHE PATH "bimg source" FORCE)
set(BGFX_DIR "${bgfx_source_SOURCE_DIR}" CACHE PATH "bgfx source" FORCE)
CPMAddPackage(NAME bgfx_cmake
  URL https://github.com/bkaradzic/bgfx.cmake/archive/5ae5fa5b121974b9de084539bb784d97fa4cd8a6.tar.gz
  URL_HASH SHA256=2b3981e0b23c086326554a1908be763688dd1058c12b06bdf81b6f6b71ff7c51
  OPTIONS "BGFX_BUILD_EXAMPLES OFF" "BGFX_BUILD_TESTS OFF" "BGFX_INSTALL OFF"
    "BGFX_CONFIG_MULTITHREADED OFF" "BGFX_BUILD_TOOLS_GEOMETRY OFF"
    "BGFX_BUILD_TOOLS_TEXTURE OFF" "BGFX_BUILD_TOOLS_BIN2C OFF"
    "BGFX_WITH_WAYLAND ON")
# Third-party headers are system headers so engine warnings (-Wpedantic,
# -Werror) apply only to engine code.
foreach(dependency bx bimg bgfx)
  if(TARGET ${dependency})
    set_target_properties(${dependency} PROPERTIES SYSTEM TRUE)
  endif()
endforeach()
if(FENGINE_ENABLE_SANITIZERS AND APPLE AND TARGET bgfx)
  # bgfx's Metal renderer (metal-cpp) calls methods through Objective-C tagged
  # pointers, values packed into the pointer itself, which the undefined
  # behavior sanitizer's alignment check reports although nothing is
  # dereferenced.
  target_compile_options(bgfx PRIVATE -fno-sanitize=alignment)
endif()
if(MINGW)
  # The engine compiles Shader Model 5 for D3D11. MinGW's SDK provides
  # D3DCompiler, but not Microsoft's optional Shader Model 6 DXC headers.
  target_compile_definitions(shaderc PRIVATE SHADERC_CONFIG_HAS_DXC=0)
endif()
if(WIN32)
  # shaderc_hlsl.cpp defines COM_NO_WINDOWS_H and WIN32_LEAN_AND_MEAN is set
  # (cmake/Windows.cmake), so nothing defines COM's `interface` before the
  # D3DCompiler headers use it: include ole2.h first.
  if(MSVC)
    set(ole2 "/FIole2.h")
  else()
    set(ole2 "-include;ole2.h")
  endif()
  set_source_files_properties("${BGFX_DIR}/tools/shaderc/shaderc_hlsl.cpp"
    TARGET_DIRECTORY shaderc PROPERTIES COMPILE_OPTIONS "${ole2}")
endif()
CPMAddPackage(NAME imgui
  URL https://github.com/ocornut/imgui/archive/52fe0a05a7b1aa180a202bb24f0f2a049a9c1b7d.tar.gz
  URL_HASH SHA256=84196b24c66cd3be22cb0b313ef70941481238ec33e595401b07976baeb7c3db
  DOWNLOAD_ONLY YES)
add_library(fengine_imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
# The demo window is a development aid only.
target_sources(fengine_imgui PRIVATE "$<${FENGINE_DEV_ENABLED}:${imgui_SOURCE_DIR}/imgui_demo.cpp>")
target_compile_definitions(fengine_imgui PUBLIC "$<$<NOT:${FENGINE_DEV_ENABLED}>:IMGUI_DISABLE_DEMO_WINDOWS>")
target_include_directories(fengine_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
  "${imgui_SOURCE_DIR}/misc/cpp")
target_link_libraries(fengine_imgui PUBLIC SDL3::SDL3)
# The developer editor's 3D gizmo (ImGuizmo, MIT) and icon glyph names
# (IconFontCppHeaders, zlib) with the Font Awesome 6 Free solid face (OFL):
# development builds only, like the editor itself.
if(FENGINE_DEV_ENABLED)
  CPMAddPackage(NAME imguizmo
    URL https://github.com/CedricGuillemet/ImGuizmo/archive/105e4fde45e4e74aae8973d98750820ef7aca7e2.tar.gz
    URL_HASH SHA256=d625a9f3207f15b7a5b7706269747676f17b7e4df00f898ced9480e5063d493a
    DOWNLOAD_ONLY YES)
  CPMAddPackage(NAME iconfontcppheaders
    URL https://github.com/juliettef/IconFontCppHeaders/archive/210b5a399a64270674560d633638952d1e8d804d.tar.gz
    URL_HASH SHA256=aae5fd355521f62a0257064f40bab5676aee93c9fa9dd7fd53375d3594933b3f
    DOWNLOAD_ONLY YES)
  target_sources(fengine_imgui PRIVATE "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
  target_include_directories(fengine_imgui SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src" "${iconfontcppheaders_SOURCE_DIR}")
  set(FENGINE_EDITOR_FONT "${PROJECT_BINARY_DIR}/fonts/fa-solid-900.ttf")
  if(NOT EXISTS "${FENGINE_EDITOR_FONT}")
    file(DOWNLOAD https://github.com/FortAwesome/Font-Awesome/raw/6.7.2/webfonts/fa-solid-900.ttf
      "${FENGINE_EDITOR_FONT}" EXPECTED_HASH SHA256=af19d135d3a935b3ebfbd80320716ffe1202052c5f68dc2c5f1abc57005ac605
      STATUS editor_font_status)
    list(GET editor_font_status 0 editor_font_code)
    if(NOT editor_font_code EQUAL 0)
      message(WARNING "Could not download the editor icon font (${editor_font_status}); the editor shows text labels")
      file(REMOVE "${FENGINE_EDITOR_FONT}")
    endif()
  endif()
endif()

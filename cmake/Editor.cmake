# The standalone editor application (Qt 6). A development tool: it is built
# when Qt is available and is never installed or packaged with a game.
set(FENGINE_BUILD_EDITOR AUTO CACHE STRING "Build the Qt editor: AUTO (when Qt 6 is found), ON or OFF")
set_property(CACHE FENGINE_BUILD_EDITOR PROPERTY STRINGS AUTO ON OFF)
if(NOT FENGINE_BUILD_EDITOR STREQUAL "AUTO" AND NOT FENGINE_BUILD_EDITOR)
  return()
endif()

# GuiPrivate carries the platform native interface, the only way to reach a
# window's wl_surface on Wayland.
find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets OPTIONAL_COMPONENTS GuiPrivate)
set(FENGINE_EDITOR_MISSING "")
if(NOT Qt6_FOUND)
  set(FENGINE_EDITOR_MISSING "Qt 6.5+ (Core, Gui, Widgets)")
elseif(UNIX AND NOT APPLE AND NOT TARGET Qt6::GuiPrivate)
  set(FENGINE_EDITOR_MISSING "Qt 6 private Gui headers (qt6-qtbase-private-devel / qt6-base-private-dev)")
endif()
if(FENGINE_EDITOR_MISSING)
  if(FENGINE_BUILD_EDITOR STREQUAL "AUTO")
    message(STATUS "fengine_editor: skipped, ${FENGINE_EDITOR_MISSING} not found")
    return()
  endif()
  message(FATAL_ERROR "FENGINE_BUILD_EDITOR=ON needs ${FENGINE_EDITOR_MISSING}")
endif()
message(STATUS "fengine_editor: Qt ${Qt6_VERSION}")

# The Scene view's transform gizmo (ImGuizmo, MIT), drawn through the
# renderer's ImGui.
CPMAddPackage(NAME imguizmo
  URL https://github.com/CedricGuillemet/ImGuizmo/archive/105e4fde45e4e74aae8973d98750820ef7aca7e2.tar.gz
  URL_HASH SHA256=d625a9f3207f15b7a5b7706269747676f17b7e4df00f898ced9480e5063d493a
  DOWNLOAD_ONLY YES)
add_library(fengine_imguizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(fengine_imguizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(fengine_imguizmo PUBLIC fengine_imgui)

add_executable(fengine_editor
  apps/editor/main.cpp
  apps/editor/CodeEditor.cpp
  apps/editor/EditorWindow.cpp
  apps/editor/HierarchyPanel.cpp
  apps/editor/InspectorPanel.cpp
  apps/editor/JsonForm.cpp
  apps/editor/NativeSurface.cpp
  apps/editor/Panels.cpp
  apps/editor/PlaySession.cpp
  apps/editor/ProjectPanel.cpp
  apps/editor/SceneView.cpp
  apps/editor/Thumbnails.cpp
  apps/editor/ViewportWindow.cpp)
target_link_libraries(fengine_editor PRIVATE fengine_runtime fengine_imguizmo Qt6::Core Qt6::Gui Qt6::Widgets)
if(TARGET Qt6::GuiPrivate)
  target_link_libraries(fengine_editor PRIVATE Qt6::GuiPrivate)
endif()
# The editor always runs from a build tree: compiled shaders come from there
# unless a copy sits beside the executable.
target_compile_definitions(fengine_editor PRIVATE FENGINE_SHADER_DIR="${PROJECT_BINARY_DIR}/shaders"
  FENGINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
fengine_warnings(fengine_editor)
fengine_windows_manifest(fengine_editor)
if(TARGET fengine_builtin)
  add_dependencies(fengine_editor fengine_builtin)
endif()
if(TARGET fengine_shader_stage)
  add_dependencies(fengine_editor fengine_shader_stage)
endif()

# The editor's self-test, on a scratch copy of the sample: edits a scene
# through the session and through real mouse events in the Scene view
# (select, drag, gizmo, paint, drop), plays the game and inspects it, saves,
# and runs the Build panel's jobs (validate, tests, bake, replay, record; and
# package when FENGINE_RELEASE_BUILD names a Release build). Needs a display
# and a GPU, so it is registered with the windowed replays.
if(FENGINE_BUILD_TESTS AND FENGINE_RUN_REPLAYS)
  add_test(NAME editor_scene_view
    COMMAND fengine_editor "${PROJECT_SOURCE_DIR}/samples/starfall" --map town --frames 230 --redock --selftest --mute
      --shot "${PROJECT_BINARY_DIR}/editor-test/scene.png" --user-data "${PROJECT_BINARY_DIR}/editor-test/user")
  set_tests_properties(editor_scene_view PROPERTIES LABELS windowed TIMEOUT 600 PASS_REGULAR_EXPRESSION "EDITOR SELF-CHECK PASS")
endif()

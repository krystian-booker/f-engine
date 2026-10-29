#!/usr/bin/env bash
set -euo pipefail
task_build=build-linux
task_args=(-S . -B "$task_build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFENGINE_RUN_REPLAYS=ON -DFENGINE_WARNINGS_AS_ERRORS=ON)
# Reuse already-fetched sources when running this container from macOS. Binary
# directories remain separate, so no host compiler artifacts enter this build.
for task_package in json glm EnTT lua sol2 cgltf stb doctest miniaudio SDL bx_source bimg_source bgfx_source bgfx_cmake imgui; do
  task_source="/workspace/build/_deps/${task_package,,}-src"
  if [[ -d "$task_source" ]]; then task_args+=("-DCPM_${task_package}_SOURCE=$task_source"); fi
done
cmake "${task_args[@]}"
cmake --build "$task_build" --parallel 4
# Headless tests (including headless replays) first, then the windowed replays
# on a virtual display (Mesa).
ctest --test-dir "$task_build" --output-on-failure -LE windowed
xvfb-run -a -s "-screen 0 1280x720x24" ctest --test-dir "$task_build" --output-on-failure -L windowed
cmake --install "$task_build" --prefix "$task_build/package"

#!/usr/bin/env bash
set -euo pipefail
task_build=build-linux
task_args=(-S . -B "$task_build" -G Ninja -DCMAKE_BUILD_TYPE=Debug)
# Reuse already-fetched sources when running this container from macOS. Binary
# directories remain separate, so no host compiler artifacts enter this build.
for task_package in json glm EnTT lua sol2 cgltf stb doctest miniaudio SDL bx_source bimg_source bgfx_source bgfx_cmake imgui; do
  task_source="/workspace/build/_deps/${task_package,,}-src"
  if [[ -d "$task_source" ]]; then task_args+=("-DCPM_${task_package}_SOURCE=$task_source"); fi
done
cmake "${task_args[@]}"
cmake --build "$task_build" --parallel 4
ctest --test-dir "$task_build" --output-on-failure
xvfb-run -a "$task_build/apps/runtime/fengine" samples/sandbox --frames 120 --mute --skip-title --user-data "$task_build/user-data" --capture "$task_build/qa/town"
cmake --install "$task_build" --prefix "$task_build/package"

#!/usr/bin/env bash
set -euo pipefail
task_args=(-S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-x64.cmake)
for task_package in json glm EnTT lua sol2 cgltf stb doctest miniaudio SDL bx_source bimg_source bgfx_source bgfx_cmake imgui; do
  task_source="/workspace/build/_deps/${task_package,,}-src"
  if [[ -d "$task_source" ]]; then task_args+=("-DCPM_${task_package}_SOURCE=$task_source"); fi
done
cmake "${task_args[@]}"
# First build the Windows shader compiler and headless programs. Compile real
# D3D shader binaries under Wine before building the runtime target.
cmake --build build-windows --target shaderc fengine_tests fengine_headless fengine_assetc --parallel 4

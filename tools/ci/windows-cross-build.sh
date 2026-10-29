#!/usr/bin/env bash
# MinGW cross build of the Windows programs, in two phases around Wine:
#   windows-cross-build.sh tools     shaderc, the doctests, fengine_headless, fengine_assetc
#   windows-wine.sh                  runs the tests and compiles D3D11 shaders with shaderc.exe
#   windows-cross-build.sh runtime   fengine.exe and the runtime tests, using those shaders
# The runtime phase copies the Wine-compiled shaders to build-windows/shaders/dx11,
# where fengine.exe (and tools/package_game.py) read them.
set -euo pipefail
phase=${1:-tools}
task_args=(-S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DFENGINE_DEV=OFF
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-x64.cmake)
for task_package in json glm EnTT lua sol2 cgltf stb doctest miniaudio SDL bx_source bimg_source bgfx_source bgfx_cmake imgui; do
  task_source="/workspace/build/_deps/${task_package,,}-src"
  if [[ -d "$task_source" ]]; then task_args+=("-DCPM_${task_package}_SOURCE=$task_source"); fi
done
case "$phase" in
  tools)
    cmake "${task_args[@]}"
    cmake --build build-windows --target shaderc fengine_tests fengine_headless fengine_assetc --parallel 4
    ;;
  runtime)
    if [[ ! -f build-windows/wine-shaders/dx11/fs_world.bin ]]; then
      echo "Run tools/ci/windows-wine.sh first: it compiles the D3D11 shaders" >&2
      exit 1
    fi
    cmake "${task_args[@]}" -DFENGINE_PRECOMPILED_SHADER_DIR="$PWD/build-windows/wine-shaders"
    cmake --build build-windows --target fengine fengine_runtime_tests fengine_shaders --parallel 4
    ;;
  *)
    echo "usage: $0 [tools|runtime]" >&2
    exit 2
    ;;
esac

#!/usr/bin/env bash
set -euo pipefail
if [[ ${FENGINE_WINE_DISPLAY_READY:-0} != 1 ]]; then
  export DISPLAY=:99
  Xvfb "$DISPLAY" -screen 0 1280x1024x24 -ac -nolisten tcp >/tmp/fengine-wine-x11.log 2>&1 &
  task_display_pid=$!
  trap 'kill "$task_display_pid" 2>/dev/null || true' EXIT
  for task_attempt in {1..100}; do
    if [[ -S /tmp/.X11-unix/X99 ]]; then break; fi
    sleep .1
  done
  if [[ ! -S /tmp/.X11-unix/X99 ]]; then cat /tmp/fengine-wine-x11.log;exit 1;fi
fi
task_wine=$(command -v wine || command -v wine64 || true)
if [[ -z "$task_wine" ]]; then task_wine=/usr/lib/wine/wine64; fi
# After `windows-cross-build.sh runtime`: the runtime's own tests, and the
# headless commands through fengine.exe (a GUI program; output still arrives).
if [[ ${1:-} == runtime ]]; then
  "$task_wine" build-windows/tests/fengine_runtime_tests.exe
  for task_sample in samples/*/; do
    "$task_wine" build-windows/fengine.exe --check "$task_sample"
  done
  exit 0
fi
"$task_wine" build-windows/tests/fengine_tests.exe
"$task_wine" build-windows/fengine_headless.exe --test tests/game
"$task_wine" build-windows/fengine_headless.exe --test build-windows/tests/sandbox-game
for task_sample in samples/*/; do
  "$task_wine" build-windows/fengine_headless.exe --check "$task_sample"
  "$task_wine" build-windows/fengine_headless.exe --test "$task_sample"
  for task_replay in "$task_sample"replays/*.json; do
    "$task_wine" build-windows/fengine_headless.exe "$task_sample" --replay "$task_replay"
  done
done
# Wine's own HLSL compiler lacks functions bgfx's shader library uses (asint),
# so shaderc loads Microsoft's d3dcompiler_47.dll, which bgfx ships, instead.
task_shaderc_dir=build-windows/_deps/bgfx_cmake-build/cmake/bgfx
cp build/_deps/bgfx_source-src/tools/bin/windows/d3dcompiler_47.dll "$task_shaderc_dir/"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-};d3dcompiler_47=n"
mkdir -p build-windows/wine-shaders/dx11
for task_source in shaders/vs_*.sc shaders/fs_*.sc; do
  task_name=$(basename "$task_source" .sc)
  if [[ $task_name == vs_* ]]; then task_stage=vertex; else task_stage=fragment; fi
  "$task_wine" "$task_shaderc_dir/shaderc.exe" \
    -f "$task_source" -o "build-windows/wine-shaders/dx11/$task_name.bin" \
    --type "$task_stage" --platform windows --profile s_5_0 \
    --varyingdef shaders/varying.def.sc -i build/_deps/bgfx_source-src/src -i shaders
done

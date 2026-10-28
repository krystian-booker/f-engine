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
"$task_wine" build-windows/tests/fengine_tests.exe
"$task_wine" build-windows/fengine_headless.exe --test lualib/tests/game
for task_sample in samples/*/; do
  "$task_wine" build-windows/fengine_headless.exe --check "$task_sample"
  "$task_wine" build-windows/fengine_headless.exe --test "$task_sample"
done
mkdir -p build-windows/wine-shaders/dx11
for task_source in shaders/vs_*.sc shaders/fs_*.sc; do
  task_name=$(basename "$task_source" .sc)
  if [[ $task_name == vs_* ]]; then task_stage=vertex; else task_stage=fragment; fi
  "$task_wine" build-windows/_deps/bgfx_cmake-build/cmake/bgfx/shaderc.exe \
    -f "$task_source" -o "build-windows/wine-shaders/dx11/$task_name.bin" \
    --type "$task_stage" --platform windows --profile s_5_0 \
    --varyingdef shaders/varying.def.sc -i build/_deps/bgfx_source-src/src -i shaders
done

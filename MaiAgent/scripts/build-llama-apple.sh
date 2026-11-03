#!/usr/bin/env bash
# 文件作用：离线从已固定源码构建 Mac/iOS 静态库及链接 smoke，不创建预编译 SDK。
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
agent_dir="$(cd "$script_dir/.." && pwd)"
platform="${1:-macos}"
build_dir="${2:-$agent_dir/build/vendor/llama-$platform}"
jobs="${MAI_LLAMA_BUILD_JOBS:-2}"
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "MAI_LLAMA_BUILD_JOBS must be a positive integer" >&2
  exit 2
fi

arguments=(-S "$agent_dir/tests/llama-source-build" -B "$build_dir"
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
  -DMAI_LLAMA_METAL=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
case "$platform" in
  macos)
    arguments+=(-DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3)
    ;;
  ios-device|ios-sim)
    sdk=iphoneos
    if [[ "$platform" == ios-sim ]]; then sdk=iphonesimulator; fi
    arguments+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk"
      -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
      -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO)
    ;;
  *)
    echo "Usage: build-llama-apple.sh {macos|ios-device|ios-sim} [isolated-build-dir]" >&2
    exit 2
    ;;
esac
cmake "${arguments[@]}"
cmake --build "$build_dir" --parallel "$jobs" --target MaiLlamaBuildSmoke
if [[ "$platform" == macos ]]; then "$build_dir/MaiLlamaBuildSmoke"; fi

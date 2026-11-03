#!/usr/bin/env bash
# 文件作用：用指定 NDK 从固定源码构建 Android 静态库与链接 smoke，不操作设备。
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
agent_dir="$(cd "$script_dir/.." && pwd)"
abi="${1:-arm64-v8a}"
build_dir="${2:-$agent_dir/build/vendor/llama-android-$abi}"
ndk_root="${3:-${ANDROID_NDK_HOME:-}}"
jobs="${MAI_LLAMA_BUILD_JOBS:-2}"
case "$abi" in
  arm64-v8a|armeabi-v7a) ;;
  *) echo "Expected arm64-v8a or armeabi-v7a" >&2; exit 2 ;;
esac
if [[ ! -f "$ndk_root/build/cmake/android.toolchain.cmake" ]]; then
  echo "Pass the Android NDK directory as argument 3 or ANDROID_NDK_HOME" >&2
  exit 2
fi
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "MAI_LLAMA_BUILD_JOBS must be a positive integer" >&2
  exit 2
fi
cmake -S "$agent_dir/tests/llama-source-build" -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$ndk_root/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_shared -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
  -DMAI_LLAMA_METAL=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build_dir" --parallel "$jobs" --target MaiLlamaBuildSmoke

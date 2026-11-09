#!/usr/bin/env bash
# 文件作用：把 macOS 桌面程序和运行时依赖封装为分发产物。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
set -euo pipefail

usage() {
  cat <<'USAGE'
Package the Qt Desktop IM app for macOS.

Usage:
  MaiChat/desktop/scripts/package-macos.sh [--build-dir build] [--out-dir dist] [--app-only] [--macdeployqt /path/to/macdeployqt] [--python /path/to/python3]

The script expects a built macOS bundle:
  <build-dir>/bin/MaiChat.app
Build/output paths may be absolute or relative to MaiChat/desktop.
DMG layout needs Python 3.10+ and scripts/requirements-dmg.txt; --app-only does not.

Outputs:
  <out-dir>/MaiChat-macos-arm64/
  <out-dir>/MaiChat-macos-arm64-v<version>-<date>-<git-short-hash>.dmg
USAGE
}

build_dir="build"
out_dir="dist"
macdeployqt=""
python="python3"
app_only=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="${2:-}"
      shift
      ;;
    --out-dir)
      out_dir="${2:-}"
      shift
      ;;
    --macdeployqt)
      macdeployqt="${2:-}"
      shift
      ;;
    --app-only)
      app_only=true
      ;;
    --python)
      python="${2:-}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(git -C "$project_root" rev-parse --show-toplevel)"
case "$build_dir" in
  /*) build_path="$build_dir" ;;
  *) build_path="$project_root/$build_dir" ;;
esac
app_name="MaiChat.app"
source_app="$build_path/bin/$app_name"
if [[ ! -d "$source_app" && -d "$build_path/$app_name" ]]; then
  source_app="$build_path/$app_name"
fi

if [[ ! -d "$source_app" ]]; then
  cat >&2 <<EOF
Missing app bundle: $source_app

Build it first, for example:
  cmake -S MaiChat/desktop -B MaiChat/desktop/$build_dir  -DCMAKE_BUILD_TYPE=Release
  cmake --build MaiChat/desktop/$build_dir --target maichat --config Release
EOF
  exit 1
fi

if [[ -z "$macdeployqt" ]]; then
  cache_path="$build_path/CMakeCache.txt"
  if [[ -f "$cache_path" ]]; then
    qt6_dir="$(sed -n 's/^Qt6_DIR:PATH=//p' "$cache_path" | head -1)"
    if [[ -n "$qt6_dir" ]]; then
      candidate="$(cd "$qt6_dir/../../.." && pwd)/bin/macdeployqt"
      if [[ -x "$candidate" ]]; then
        macdeployqt="$candidate"
      fi
    fi
  fi
fi


if [[ -z "$macdeployqt" || ! -x "$macdeployqt" ]]; then
  echo "Unable to locate macdeployqt. Pass --macdeployqt /path/to/macdeployqt." >&2
  exit 1
fi

if [[ "$app_only" != true ]] &&
    ! "$python" -c 'import sys; assert sys.version_info >= (3, 10); from ds_store import DSStore' >/dev/null 2>&1; then
  cat >&2 <<EOF
DMG layout requires Python 3.10+ with the pinned packaging dependencies.
Install them in a separate environment, then pass its interpreter:
  python3 -m venv /private/tmp/maichat-dmg-tools
  /private/tmp/maichat-dmg-tools/bin/python -m pip install -r "$project_root/scripts/requirements-dmg.txt"
  $0 --python /private/tmp/maichat-dmg-tools/bin/python [other options]

No staging files have been changed. --app-only does not need this dependency.
EOF
  exit 1
fi

case "$out_dir" in
  /*) dist_root="$out_dir" ;;
  *) dist_root="$project_root/$out_dir" ;;
esac
staging="$dist_root/MaiChat-macos-arm64"
staged_app="$staging/$app_name"

# 重打包已生成的 App 时，输入可能就在输出目录内；真实路径也要覆盖符号链接别名。
# 删除或复制前拒绝双向包含，防止清空唯一输入，或把 App 递归复制进自身。
mkdir -p "$dist_root"
source_app_real="$(cd "$source_app" && pwd -P)"
if [[ -d "$staging" ]]; then
  staging_real="$(cd "$staging" && pwd -P)"
else
  staging_real="$(cd "$dist_root" && pwd -P)/MaiChat-macos-arm64"
fi
case "$source_app_real/" in
  "$staging_real/"*)
    echo "Input App overlaps output staging; choose a separate --out-dir." >&2
    exit 1
    ;;
esac
case "$staging_real/" in
  "$source_app_real/"*)
    echo "Output staging is inside the input App; choose a separate --out-dir." >&2
    exit 1
    ;;
esac
rm -rf "$staging"
mkdir -p "$staging"

ditto "$source_app" "$staged_app"

# The font itself is in Qt resources; distribute the original OFL/copyright as
# a readable file in the application bundle, just as Android and iOS do.
code_font_license="$repo_root/MaiChat/Resources/fonts/LICENSE-SarasaFixedSC.txt"
code_font_licenses_dir="$staged_app/Contents/Resources/ThirdPartyLicenses"
if [[ ! -f "$code_font_license" ]]; then
  echo "Missing shared code font license: $code_font_license" >&2
  exit 1
fi
mkdir -p "$code_font_licenses_dir"
cp "$code_font_license" "$code_font_licenses_dir/LICENSE-SarasaFixedSC.txt"
cmp "$code_font_license" "$code_font_licenses_dir/LICENSE-SarasaFixedSC.txt"

trtc_root="$project_root/vendor/tencent-trtc/macos/TXLiteAVSDK_TRTC_Mac"
trtc_slice="macos-arm64_x86_64"
trtc_target_dir="$staged_app/Contents/Frameworks"
mkdir -p "$trtc_target_dir"
for framework in TXLiteAVSDK_TRTC_Mac TXFFmpeg TXSoundTouch; do
  trtc_source="$trtc_root/$framework.xcframework/$trtc_slice/$framework.framework"
  if [[ ! -d "$trtc_source" ]]; then
    echo "Missing native TRTC SDK framework: $trtc_source" >&2
    exit 1
  fi
  ditto "$trtc_source" "$trtc_target_dir/$framework.framework"
done

# Qt 仅承担 UI。macdeployqt 会扫描 bundle 内已有的每个 dylib，旧 generic/TUIO
# 即使不在默认插件列表，也会重新带入 QtNetwork；这里只重建 staging 的 Qt 插件目录。
# 源 SDK 不修改。显式种子仍交给 macdeployqt 重写 install_name/rpath 并补齐实际闭包。
qt_install_root="$("$python" -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve().parent.parent)' "$macdeployqt")"
qt_plugin_source="$qt_install_root/plugins"
qt_plugin_target="$staged_app/Contents/PlugIns"
for plugin in platforms/libqcocoa.dylib imageformats/libqsvg.dylib \
              imageformats/libqjpeg.dylib iconengines/libqsvgicon.dylib styles/libqmacstyle.dylib; do
  if [[ ! -f "$qt_plugin_source/$plugin" ]]; then
    echo "Missing Qt 6 UI plugin in source SDK: $plugin" >&2
    exit 1
  fi
done
# This SDK profile uses FFmpeg for user raster images. Reject a stale optional
# decoder before collecting UI seeds; the source SDK itself is never modified.
for plugin in libqwebp.dylib libqtiff.dylib libqicns.dylib libqtga.dylib libqwbmp.dylib libqmng.dylib \
              libqjp2.dylib libqmacheif.dylib libqmacjp2.dylib; do
  if [[ -f "$qt_plugin_source/imageformats/$plugin" ]]; then
    echo "Retired Qt image decoder in source SDK: $plugin" >&2
    exit 1
  fi
done
rm -rf "$qt_plugin_target"
mkdir -p "$qt_plugin_target"
qt_plugin_args=()
for plugin_source in "$qt_plugin_source"/platforms/libqcocoa.dylib \
                     "$qt_plugin_source"/imageformats/*.dylib \
                     "$qt_plugin_source"/iconengines/*.dylib \
                     "$qt_plugin_source"/styles/*.dylib; do
  [[ -f "$plugin_source" ]] || continue
  [[ "$plugin_source" != *_debug.dylib ]] || continue
  plugin="${plugin_source#"$qt_plugin_source"/}"
  mkdir -p "$qt_plugin_target/$(dirname "$plugin")"
  cp -p "$plugin_source" "$qt_plugin_target/$plugin"
  qt_plugin_args+=("-executable=$qt_plugin_target/$plugin")
done
# 配置插件只从分发 bundle 读取；版本专用 qt6.conf 的优先级也不能指回构建 SDK。
mkdir -p "$staged_app/Contents/Resources"
for qt_config in qt.conf qt6.conf; do
  cat > "$staged_app/Contents/Resources/$qt_config" <<'QTCONF'
[Paths]
Prefix=.
Plugins=PlugIns
QTCONF
done
"$macdeployqt" "$staged_app" -always-overwrite -no-plugins "${qt_plugin_args[@]}"
for framework in QtNetwork QtWebSockets QtSql QtMultimedia QtMultimediaWidgets; do
  if [[ -d "$staged_app/Contents/Frameworks/$framework.framework" ]]; then
    echo "Retired Qt runtime in staged MaiChat App: $framework" >&2
    exit 1
  fi
done
for plugin in platforms/libqcocoa.dylib imageformats/libqsvg.dylib \
              imageformats/libqjpeg.dylib iconengines/libqsvgicon.dylib styles/libqmacstyle.dylib; do
  if [[ ! -f "$staged_app/Contents/PlugIns/$plugin" ]]; then
    echo "Missing Qt 6 runtime plugin: $plugin" >&2
    exit 1
  fi
done

# 审计 WebP 的额外动态依赖；源码 SDK 使用内置 WebP，此分支通常无需补库。
# 将同一安装包的依赖放到库旁边，显式使用 loader_path，避免只在有 Homebrew 的打包机上能解码。
webp_needs_sharpyuv=false
for webp_library in "$trtc_target_dir"/libwebp*.dylib; do
  if [[ -f "$webp_library" ]] && otool -L "$webp_library" | grep -Fq '@rpath/libsharpyuv.0.dylib'; then
    webp_needs_sharpyuv=true
  fi
done
if [[ "$webp_needs_sharpyuv" == true ]]; then
  webp_runtime_dir="${MAICHAT_WEBP_RUNTIME_DIR:-}"
  webp_prefix=""
  if [[ -z "$webp_runtime_dir" ]] && command -v brew >/dev/null 2>&1; then
    webp_prefix="$(brew --prefix webp)"
    webp_runtime_dir="$webp_prefix/lib"
  fi
  sharpyuv_source="$webp_runtime_dir/libsharpyuv.0.dylib"
  if [[ ! -f "$sharpyuv_source" ]]; then
    echo "Missing WebP dependency libsharpyuv.0.dylib; set MAICHAT_WEBP_RUNTIME_DIR to its matching WebP lib directory." >&2
    exit 1
  fi
  lipo -verify_arch arm64 "$sharpyuv_source"
  cp -pL "$sharpyuv_source" "$trtc_target_dir/libsharpyuv.0.dylib"
  install_name_tool -id '@rpath/libsharpyuv.0.dylib' "$trtc_target_dir/libsharpyuv.0.dylib"
  for webp_library in "$trtc_target_dir"/libwebp*.dylib; do
    if [[ -f "$webp_library" ]] && otool -L "$webp_library" | grep -Fq '@rpath/libsharpyuv.0.dylib'; then
      install_name_tool -change '@rpath/libsharpyuv.0.dylib' '@loader_path/libsharpyuv.0.dylib' "$webp_library"
    fi
  done
  if [[ -n "$webp_prefix" ]]; then
    for notice in COPYING PATENTS; do
      for notice_source in "$webp_prefix/$notice" "$webp_prefix/share/doc/webp/$notice"; do
        if [[ -f "$notice_source" ]]; then
          cp "$notice_source" "$code_font_licenses_dir/libwebp-$notice.txt"
          break
        fi
      done
    done
  fi
fi

sdk_source="$project_root/vendor/tencent-im/macos/ImSDKForMac_Plus.framework"
if [[ ! -d "$sdk_source" ]]; then
  echo "Missing native IM SDK framework: $sdk_source" >&2
  exit 1
fi

sdk_target_dir="$staged_app/Contents/MacOS/vendor/tencent-im/macos"
mkdir -p "$sdk_target_dir"
ditto "$sdk_source" "$sdk_target_dir/ImSDKForMac_Plus.framework"

# 仅启动主程序不足以覆盖延迟加载的图片插件。签名前核对加载依赖及 App 所需架构，额外 SDK 驱动单列。
"$python" "$project_root/scripts/verify-macos-runtime.py" "$staged_app" \
  --json-output "$dist_root/macos-runtime-audit.json"

# 使用标准拖拽安装布局，用户可把 App 拖进“应用程序”。
ln -s /Applications "$staging/Applications"

# 临时签名保证内嵌 Framework 可加载，不要求 Developer ID。
if command -v codesign >/dev/null 2>&1; then
  codesign --force --deep --sign - "$staged_app"
  codesign --verify --deep --strict "$staged_app"
fi

bundle_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$staged_app/Contents/Info.plist" 2>/dev/null || true)"
short_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$staged_app/Contents/Info.plist" 2>/dev/null || true)"
if [[ -z "$bundle_version" || -z "$short_version" ]]; then
  echo "Invalid app bundle: CFBundleVersion and CFBundleShortVersionString must be set." >&2
  exit 1
fi

# 打包前核对 App 实际版本与本次 CMake 配置，避免把旧 bundle 当成新构建发布。
expected_version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build_path/CMakeCache.txt" | head -1)"
if [[ -z "$expected_version" || "$bundle_version" != "$expected_version" ||
      "$short_version" != "$expected_version" ]]; then
  echo "App bundle version does not match the configured project version: $expected_version." >&2
  exit 1
fi

git_hash="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || true)"
if [[ -z "$git_hash" ]]; then
  git_hash="unknown"
fi

if [[ "$app_only" == true ]]; then
  echo "MaiChat macOS portable app: $staged_app"
  echo "Version: $short_version ($bundle_version)"
  exit 0
fi

dmg_name="MaiChat-macos-arm64-v${short_version}-$(date +%Y%m%d)-$git_hash.dmg"
dmg_path="$dist_root/$dmg_name"
rw_dmg="$dist_root/.${dmg_name%.dmg}-rw.dmg"
mount_dir=""
mounted_device=""

cleanup() {
  if [[ -n "$mounted_device" ]]; then
    hdiutil detach "$mounted_device" -quiet || true
  fi
  rm -f "$rw_dmg"
}
trap cleanup EXIT

rm -f "$dmg_path" "$rw_dmg"
hdiutil create \
  -volname "MaiChat" \
  -srcfolder "$staging" \
  -ov \
  -format UDRW \
  -fs HFS+ \
  "$rw_dmg"

attach_output="$(hdiutil attach "$rw_dmg" -readwrite -noverify -noautoopen)"
mounted_device="$(printf '%s\n' "$attach_output" | awk '/Apple_HFS/ {print $1; exit}')"
mount_dir="$(printf '%s\n' "$attach_output" | sed -nE 's|^/dev/[^[:space:]]+[[:space:]]+Apple_HFS[[:space:]]+||p' | head -1)"
if [[ -z "$mounted_device" || -z "$mount_dir" || ! -d "$mount_dir" ]]; then
  echo "Unable to mount writable DMG." >&2
  exit 1
fi

# 直接写入 Finder 元数据，避免安装布局依赖打包机器是否允许 Finder 保存 .DS_Store。
"$python" "$project_root/scripts/create-dmg-layout.py" "$mount_dir"

sync
if [[ ! -f "$mount_dir/.DS_Store" ]]; then
  echo "Unable to create the DMG Finder layout." >&2
  exit 1
fi
hdiutil detach "$mounted_device" -quiet
mounted_device=""
hdiutil convert "$rw_dmg" -format UDZO -imagekey zlib-level=9 -ov -o "$dmg_path"
rm -f "$rw_dmg"

size="$(du -h "$dmg_path" | awk '{print $1}')"
echo
echo "Qt IM macOS DMG:"
echo "  Directory: $staging"
echo "  Image:     $dmg_path ($size)"
echo "  Version:   $short_version ($bundle_version)"

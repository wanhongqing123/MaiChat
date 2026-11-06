#!/usr/bin/env python3
"""Build the portrait-only macOS regressions without mutating product/vendor staging.

The supplied native cache provides already-built core/OpenCV static libraries; its
OpenCV configuration headers must describe those libraries. All fresh objects and
synthetic output remain in --output. No network or user/device data is accessed.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-cache", required=True, type=Path)
    parser.add_argument("--opencv-config", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--openssl", type=Path, default=Path("/opt/homebrew/opt/openssl@3"))
    parser.add_argument("--eigen-include", type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("The Vision/CoreImage host regression requires macOS and Xcode")
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    objects = []
    command = ["xcrun", "clang++", "-std=c++17", "-O1", "-fobjc-arc", "-D__OPENCV_BUILD"]
    includes = ["MaiChat/Portrait", "MaiAgent/include", "MaiAgent/third_party/nlohmann",
                "MaiAgent/third_party/sqlite", "MaiAgent/third_party/onnxruntime/include/onnxruntime/core/session",
                "MaiAgent/third_party/opencv/modules/core/include", "MaiAgent/third_party/opencv/modules/imgproc/include",
                "MaiAgent/third_party/opencv/modules/photo/include"]
    # 配置启用了 Eigen 的既有 OpenCV 缓存也需要同一依赖头；不改 vendor 或缓存。
    eigen = args.eigen_include
    if eigen is None:
        cache = args.build_cache / "CMakeCache.txt"
        if cache.is_file():
            for line in cache.read_text().splitlines():
                if line.startswith("Eigen3_DIR:PATH="):
                    candidate = Path(line.split("=", 1)[1]).parent / "eigen3-src"
                    if (candidate / "Eigen/Core").is_file():
                        eigen = candidate
    if eigen is not None:
        command += ["-I", str(eigen.resolve())]
    for include in [root / item for item in includes] + [args.opencv_config.resolve(), args.openssl / "include"]:
        command += ["-I", str(include)]
    for source in ["MaiChat/Portrait/MaiPortraitEngine.cpp", "MaiChat/Portrait/MaiPortraitPersonalization.cpp",
                   "MaiChat/Portrait/MaiPortraitBridge.cpp", "MaiChat/Portrait/MaiPortraitRegion.cpp",
                   "MaiChat/Portrait/MaiBodySkinRetouch.cpp", "MaiChat/Portrait/MaiPortraitWhiteningLut.cpp",
                   "MaiAgent/third_party/opencv/modules/photo/src/inpaint.cpp"]:
        target = output / (Path(source).stem + ".o")
        objects.append(str(target))
        subprocess.run(command + ["-c", str(root / source), "-o", str(target)], check=True)
    libraries = [str(args.build_cache.resolve() / item) for item in [
        "lib/libmaiagent.a", "lib/libmai_sqlite.a", "opencv/lib/libopencv_imgproc.a", "opencv/lib/libopencv_core.a",
        "opencv/3rdparty/lib/libtegra_hal.a", "opencv/3rdparty/lib/libkleidicv_hal.a",
        "opencv/3rdparty/lib/libkleidicv_thread.a", "opencv/3rdparty/lib/libkleidicv.a", "opencv/3rdparty/lib/libittnotify.a"]]
    link = objects + libraries + ["-L", str(args.openssl / "lib"), "-lcrypto", "-lz"]
    for framework in ["Foundation", "Vision", "CoreGraphics", "ImageIO", "Accelerate"]:
        link += ["-framework", framework]
    native = output / "MaiPortraitTests"
    subprocess.run(command + [str(root / "MaiChat/Portrait/tests/MaiPortraitTests.mm")] + link + ["-o", str(native)], check=True)
    skin = output / "MaiPortraitSkinTests"
    subprocess.run(command + [str(root / "MaiChat/Portrait/tests/MaiPortraitSkinTests.mm")] + link + ["-o", str(skin)], check=True)
    host = output / "MaiPortraitRegionTests"
    subprocess.run(["xcrun", "swiftc", "-O", "-import-objc-header", str(root / "MaiChat/Portrait/MaiPortraitBridge.h"),
                    str(root / "MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MaiPortraitPhotoProcessor.swift"),
                    str(root / "MaiChat/Portrait/tests/MaiPortraitRegionTests.swift")] + link + ["-lc++", "-o", str(host)], check=True)
    resources = [str(root / "MaiChat/Portrait/models"), str(args.runtime.resolve()),
                 str(root / "MaiAgent/third_party/opencv/samples/data/lena.jpg")]
    subprocess.run([str(native)] + resources + [str(root / "MaiChat/Portrait/tests/fixtures/pose.jpg"), str(output / "legacy-results")], check=True)
    subprocess.run([str(skin)] + resources + [str(root / "MaiChat/Portrait/tests/fixtures/pose.jpg"), str(output / "skin-results")], check=True)
    host_result = subprocess.run(
        [str(host)] + resources + [str(root / "MaiChat/Portrait/tests/fixtures/pose.jpg"), str(output / "region-results")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(host_result.stdout, end="")
    host_result.check_returncode()
    # 保存普通宿主实际source/output及元数据；v7不再运行旧Lab曝光/均匀化公式的业务guard。
    # 新LUT与净肤场的逐像素/频率/分块连续性回归由上面的共享native测试验证。
    summary = next(json.loads(line) for line in reversed(host_result.stdout.splitlines())
                   if line.startswith('{"') and '"whole_skin_results"' in line)
    (output / "ordinary-summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()

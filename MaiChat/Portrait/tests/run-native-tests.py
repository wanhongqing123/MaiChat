#!/usr/bin/env python3
"""Build the portrait-only macOS regressions without mutating product/vendor staging.

The supplied native cache provides already-built core/OpenCV static libraries; its
OpenCV configuration headers must describe those libraries. All fresh objects and
synthetic output remain in --output. No network or user/device data is accessed.
"""
import argparse
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
    for include in [root / item for item in includes] + [args.opencv_config.resolve(), args.openssl / "include"]:
        command += ["-I", str(include)]
    for source in ["MaiChat/Portrait/MaiPortraitEngine.cpp", "MaiChat/Portrait/MaiPortraitPersonalization.cpp",
                   "MaiChat/Portrait/MaiPortraitBridge.cpp", "MaiChat/Portrait/MaiPortraitRegion.cpp",
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
    host = output / "MaiPortraitRegionTests"
    subprocess.run(["xcrun", "swiftc", "-O", "-import-objc-header", str(root / "MaiChat/Portrait/MaiPortraitBridge.h"),
                    str(root / "MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MaiPortraitPhotoProcessor.swift"),
                    str(root / "MaiChat/Portrait/tests/MaiPortraitRegionTests.swift")] + link + ["-lc++", "-o", str(host)], check=True)
    resources = [str(root / "MaiChat/Portrait/models"), str(args.runtime.resolve()),
                 str(root / "MaiAgent/third_party/opencv/samples/data/lena.jpg")]
    subprocess.run([str(native)] + resources + [str(root / "MaiChat/Portrait/tests/fixtures/pose.jpg"), str(output / "legacy-results")], check=True)
    subprocess.run([str(host)] + resources + [str(output / "region-results")], check=True)


if __name__ == "__main__":
    main()

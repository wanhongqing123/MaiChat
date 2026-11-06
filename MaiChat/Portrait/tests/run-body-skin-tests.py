#!/usr/bin/env python3
"""Build/run the portable body-skin core without loading an app, model, or user data.

Reuses an existing OpenCV core/imgproc cache and optionally its inpaint object.
The optional --compare inputs are read-only fixtures; fresh outputs go to --output.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-cache", type=Path, required=True)
    parser.add_argument("--opencv-config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inpaint-object", type=Path)
    parser.add_argument("--eigen-include", type=Path)
    parser.add_argument("--compare", type=Path, nargs=4, metavar=("SOURCE", "SCOPE", "CLEAN", "REPAIR"))
    parser.add_argument("--compare-whitening", type=Path, nargs=3, metavar=("CLEAN", "SCOPE", "BODY_WHITEN"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    command = ["c++", "-std=c++17", "-O3", "-D__OPENCV_BUILD"]
    for include in [root / "MaiChat/Portrait", root / "MaiAgent/third_party/llama.cpp/vendor/stb",
                    root / "MaiAgent/third_party/opencv/modules/core/include",
                    root / "MaiAgent/third_party/opencv/modules/imgproc/include",
                    root / "MaiAgent/third_party/opencv/modules/photo/include", args.opencv_config.resolve()]:
        command += ["-I", str(include)]
    if args.eigen_include:
        command += ["-I", str(args.eigen_include.resolve())]
    sources = [root / "MaiChat/Portrait/MaiBodySkinRetouch.cpp",
               root / "MaiChat/Portrait/MaiPortraitWhiteningLut.cpp",
               root / "MaiChat/Portrait/tests/MaiBodySkinRetouchTests.cpp"]
    if args.inpaint_object:
        sources.append(args.inpaint_object.resolve())
    else:
        sources.append(root / "MaiAgent/third_party/opencv/modules/photo/src/inpaint.cpp")
    cache = args.build_cache.resolve()
    libraries = [cache / "opencv/lib/libopencv_imgproc.a", cache / "opencv/lib/libopencv_core.a"]
    libraries += [cache / ("opencv/3rdparty/lib/" + name) for name in [
        "libtegra_hal.a", "libkleidicv_hal.a", "libkleidicv_thread.a", "libkleidicv.a", "libittnotify.a"]
                 if (cache / ("opencv/3rdparty/lib/" + name)).is_file()]
    binary = output / "MaiBodySkinRetouchTests"
    link = [str(item) for item in sources + libraries] + ["-lz"]
    if sys.platform == "darwin":
        link += ["-framework", "Accelerate"]
    build = command + link + ["-o", str(binary)]
    subprocess.run(build, check=True)
    source_identity = [item for item in sources if item.is_relative_to(root)] + [
        root / "MaiChat/Portrait/MaiBodySkinRetouch.h", root / "MaiChat/Portrait/MaiPortraitWhiteningLut.h",
        root / "MaiChat/Portrait/whitening/MaiPortraitWhiteningLutData.inc", Path(__file__).resolve()]
    fixtures = (args.compare or []) + (args.compare_whitening or [])
    summary = {"timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(), "platform": platform.platform(),
               "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
               "build_command": build, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "source_sha256": {str(item.relative_to(root)): hashlib.sha256(item.read_bytes()).hexdigest()
                                 for item in source_identity},
               "fixture_sha256": {str(item.resolve()): hashlib.sha256(item.read_bytes()).hexdigest()
                                  for item in fixtures}, "runs": []}

    def run(arguments):
        start = time.monotonic()
        result = subprocess.run([str(binary)] + arguments, capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        summary["runs"].append({"command": [str(binary)] + arguments, "exit_code": result.returncode,
                                "wall_seconds": time.monotonic() - start,
                                "report": [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]})
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        result.check_returncode()

    run([])
    if args.compare:
        run(["--compare"] + [str(item.resolve()) for item in args.compare] + [str(output / "parity")])
    if args.compare_whitening:
        run(["--compare-whitening"] + [str(item.resolve()) for item in args.compare_whitening] +
            [str(output / "sequential")])


if __name__ == "__main__":
    main()

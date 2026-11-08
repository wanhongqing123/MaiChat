#!/usr/bin/env python3
"""Build the pinned shared desktop Qt SDK once, then reuse its fixed directory.

Source is supplied by Git submodules, never fetched during configuration. Keeping
Qt's own builds separate avoids leaking its private CMake settings into MaiAgent.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


def run(command, cwd=None):
    print("[MaiChat Qt] " + subprocess.list2cmdline([str(x) for x in command]), flush=True)
    subprocess.run([str(x) for x in command], cwd=cwd, check=True)


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def parse_msvc_include_prefix(output):
    """Use the compiler's actual bytes rather than CMake's localized decoding."""
    try:
        text = output.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = output.decode("mbcs")
    for line in text.splitlines():
        if not line.rstrip().endswith("mai-qt-include-probe.h"):
            continue
        path = re.search(r"[A-Za-z]:[\\/]|\\\\", line)
        if path and path.start() > 0:
            return line[:path.start()].rstrip() + " "
    raise RuntimeError("MSVC /showIncludes did not report the private probe header")


def msvc_include_hook(compiler, build_root):
    probe = build_root / "msvc-include-probe"
    probe.mkdir(parents=True, exist_ok=True)
    (probe / "mai-qt-include-probe.h").write_text("/* private include probe */\n")
    source = probe / "probe.cpp"
    source.write_text('#include "mai-qt-include-probe.h"\n')
    result = subprocess.run([str(compiler), "/nologo", "/showIncludes", "/EP", str(source)],
                            capture_output=True, check=True)
    prefix = parse_msvc_include_prefix(result.stdout + result.stderr)
    hook = build_root / "msvc-showincludes.cmake"
    quoted = json.dumps(prefix, ensure_ascii=False)
    hook.write_text("if(MSVC)\n" + "".join(
        f"  set({name} {quoted})\n" for name in (
            "CMAKE_C_CL_SHOWINCLUDES_PREFIX", "CMAKE_CXX_CL_SHOWINCLUDES_PREFIX",
            "CMAKE_CL_SHOWINCLUDES_PREFIX")) + "endif()\n", encoding="utf-8")
    return prefix, hook


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--arch", default=platform.machine().lower())
    parser.add_argument("--c-compiler")
    parser.add_argument("--cxx-compiler")
    parser.add_argument("--sysroot")
    parser.add_argument("--deployment-target")
    parser.add_argument("--print-prefix", action="store_true")
    args = parser.parse_args()
    desktop = Path(__file__).resolve().parents[1]
    source_root = desktop / "qt"
    system = {"Darwin": "macos", "Windows": "windows"}.get(platform.system())
    if not system:
        parser.error("the pinned desktop SDK profile supports macOS and Windows")
    if system == "macos" and not args.deployment_target:
        args.deployment_target = "14.4"
    arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(args.arch.lower(), args.arch.lower())
    build_root = source_root / ".build" / (system + "-" + arch.replace(";", "_"))
    prefix = build_root / "install"
    if args.print_prefix:
        print(prefix)
        return
    if not args.cmake or not shutil.which("ninja"):
        parser.error("CMake 3.25+ and Ninja must be on PATH")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    lock = json.loads((source_root / "sources.lock.json").read_text())
    sources = []
    dirty = False
    for module in lock["modules"]:
        source = source_root / module["name"]
        if not (source / "CMakeLists.txt").is_file():
            parser.error("missing Qt source; run git submodule update --init --depth 1 -- MaiChat/desktop/qt")
        actual = git(source, "rev-parse", "HEAD")
        if actual != module["commit"]:
            parser.error(f"{module['name']} is {actual}, expected locked {module['commit']}")
        # A dirty source checkout always runs Ninja, including untracked files.
        # It must never be hidden by the installed SDK's otherwise valid stamp.
        dirty = dirty or bool(git(source, "status", "--porcelain"))
        sources.append((module["name"], actual))
    include_prefix = None
    include_hook = None
    if system == "windows":
        compiler = args.cxx_compiler or shutil.which("cl")
        if not compiler:
            parser.error("run the Qt source builder from an MSVC developer environment")
        include_prefix, include_hook = msvc_include_hook(compiler, build_root)
    fingerprint = {
        "sources": sources,
        "arch": arch,
        "cmake": str(Path(args.cmake).resolve()),
        "c": args.c_compiler,
        "cxx": args.cxx_compiler,
        "sysroot": args.sysroot,
        "deployment": args.deployment_target,
        "builder": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "msvcIncludePrefix": include_prefix,
    }
    stamp = build_root / "installed.json"
    required = [prefix / "lib/cmake/Qt6Core/Qt6CoreConfig.cmake",
                prefix / "lib/cmake/Qt6Widgets/Qt6WidgetsConfig.cmake",
                prefix / "lib/cmake/Qt6Svg/Qt6SvgConfig.cmake",
                prefix / "lib/cmake/Qt6Test/Qt6TestConfig.cmake"]
    if not dirty and stamp.exists() and all(p.is_file() for p in required):
        # JSON has lists rather than tuples; compare the serialized form.
        if json.loads(stamp.read_text()) == json.loads(json.dumps(fingerprint)):
            print(f"[MaiChat Qt] reusing {lock['version']} at {prefix}", flush=True)
            return
    build_root.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(build_root).free < 12 * 1024 ** 3:
        parser.error("less than 12 GiB free; clean old generated artifacts before building Qt")
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
              "-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_TESTS=OFF", "-DBUILD_WITH_PCH=OFF",
              "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"]
    for key, value in [("CMAKE_C_COMPILER", args.c_compiler),
                       ("CMAKE_CXX_COMPILER", args.cxx_compiler),
                       ("CMAKE_OSX_SYSROOT", args.sysroot),
                       ("CMAKE_OSX_DEPLOYMENT_TARGET", args.deployment_target)]:
        if value:
            common.append(f"-D{key}={value}")
    if system == "macos":
        common.append(f"-DCMAKE_OSX_ARCHITECTURES={arch}")
    if include_hook:
        # project() loads compiler metadata before this hook. Overriding the prefix
        # afterwards lets Ninja record headers with localized MSVC toolchains.
        common.append(f"-DCMAKE_PROJECT_INCLUDE={include_hook.as_posix()}")
    for name, _ in sources:
        build = build_root / name
        options = list(common)
        if name == "qtbase":
            options += ["-DBUILD_SHARED_LIBS=ON", "-DFEATURE_force_bundled_libs=ON",
                        "-DFEATURE_icu=OFF", "-DFEATURE_zstd=OFF", "-DFEATURE_brotli=OFF",
                        "-DFEATURE_openssl=OFF", "-DFEATURE_dbus=OFF"]
        else:
            options += [f"-DCMAKE_PREFIX_PATH={prefix}", f"-DQt6_DIR={prefix}/lib/cmake/Qt6"]
        if name == "qtimageformats":
            options += ["-DFEATURE_force_bundled_libs=ON"]
        run([args.cmake, "-S", source_root / name, "-B", build, *options])
        run([args.cmake, "--build", build, "--parallel", args.jobs])
        run([args.cmake, "--install", build])
    if not all(p.is_file() for p in required):
        raise RuntimeError("Qt build completed without the required desktop modules")
    stamp.write_text(json.dumps(fingerprint, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, OSError, ValueError, RuntimeError) as error:
        print(f"[MaiChat Qt] build failed: {error}", file=sys.stderr)
        sys.exit(1)

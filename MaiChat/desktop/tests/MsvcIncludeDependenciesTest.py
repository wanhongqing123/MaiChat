"""Exercise real MSVC/Ninja transitive header invalidation, including spaced paths."""

import argparse
import importlib.util
import os
import subprocess
import tempfile
from pathlib import Path


def run(command, **kwargs):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout.decode("utf-8", errors="replace"))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--module", type=Path)
    args = parser.parse_args()
    module = args.module or Path(__file__).resolve().parents[1] / "cmake/MaiChatMsvcIncludes.cmake"
    launcher = module.parent.parent / "scripts/msvc-includes.py"
    spec = importlib.util.spec_from_file_location("msvc_includes", launcher)
    includes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(includes)
    owned_header = b"maichat_include_probe.h"
    for encoding in ("utf-8", "cp936"):
        marker = "\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:".encode(encoding)
        path = "C:\\Users\\\u6d4b\u8bd5\\maichat_include_probe.h".encode(encoding)
        assert includes.include_prefix(marker + b"  " + path, owned_header) == marker
    with tempfile.TemporaryDirectory(prefix="maichat deps \u8def\u5f84 ") as directory:
        root = Path(directory)
        build = root / "build"
        header = root / "contract.h"
        header.write_text("#define CONTRACT_VALUE 1\n", encoding="ascii")
        (root / "bridge.h").write_text('#include "contract.h"\n', encoding="ascii")
        (root / "main.cpp").write_text(
            '#include "bridge.h"\nextern "C" int from_c(void);\n'
            'int main() { return CONTRACT_VALUE + from_c(); }\n', encoding="ascii")
        (root / "probe.c").write_text(
            '#include "bridge.h"\nint from_c(void) { return CONTRACT_VALUE; }\n', encoding="ascii")
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(IncludeProbe LANGUAGES C CXX)\n'
            f'include("{module.as_posix()}")\n'
            'add_subdirectory(child)\nenable_language(RC)\n'
            '# Imported project/compiler setup may overwrite the detected marker.\n'
            'set(CMAKE_CL_SHOWINCLUDES_PREFIX "obsolete localized marker")\n', encoding="utf-8")
        (root / "child").mkdir()
        (root / "child/CMakeLists.txt").write_text(
            'project(NestedConsumer LANGUAGES C CXX)\n'
            'add_executable(include_probe ../main.cpp ../probe.c)\n'
            'set_target_properties(include_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")\n',
            encoding="utf-8")
        run([args.cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_MAKE_PROGRAM=" + args.ninja,
             "-DCMAKE_C_COMPILER=" + args.compiler, "-DCMAKE_CXX_COMPILER=" + args.compiler])
        run([args.cmake, "--build", str(build)])
        exe = build / "include_probe.exe"
        assert subprocess.run([str(exe)]).returncode == 2
        dependencies = run([args.ninja, "-C", str(build), "-t", "deps"])
        assert dependencies.count(b"contract.h") == 2, dependencies
        # No source or command changes: only a transitive header changes.
        header.write_text("#define CONTRACT_VALUE 7\n", encoding="ascii")
        stat = header.stat()
        os.utime(header, ns=(stat.st_atime_ns, max(stat.st_mtime_ns, exe.stat().st_mtime_ns + 1_000_000_000)))
        run([args.cmake, "--build", str(build)])
        assert subprocess.run([str(exe)]).returncode == 14
        print("PASS: both C and C++ rebuild after a transitive header-only change")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build only the ordinary storage bridge/core and test synthetic temporary roots.

No Agent, model, app bundle, or host user directory is opened. An existing empty
output directory can be reused; the manifest records the exact source and binary.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    adapter = root / "MaiChat/MobileAgentAdapter"
    agent = root / "MaiAgent"
    filesystem = "MaiFileSystemWindows.cpp" if sys.platform == "win32" else "MaiFileSystemPosix.cpp"
    sources = [adapter / "MaiAppStorageBridge.cpp", Path(__file__).resolve().with_name("MaiAppStorageBridgeTests.cpp")]
    sources += [agent / "Agent" / name for name in [
        "MaiAppStorage.cpp", "MaiFilePath.cpp", filesystem, "MaiBlockingCheck.cpp",
        "MaiError.cpp", "MaiIdGenerator.cpp"]]
    binary = output / ("MaiAppStorageBridgeTests.exe" if sys.platform == "win32" else "MaiAppStorageBridgeTests")
    command = [args.compiler, "-std=c++17", "-O2", "-pthread", "-I", str(adapter),
               "-I", str(agent / "include"), "-I", str(agent / "third_party/nlohmann")]
    command += [str(path) for path in sources] + ["-o", str(binary)]
    subprocess.run(command, check=True)
    started = time.monotonic()
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    identity = sources + [adapter / "MaiAppStorageBridge.h", agent / "include/MaiAppStorage.h",
                          Path(__file__).resolve()]
    manifest = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "build_command": command,
        "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in identity},
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "exit_code": result.returncode,
        "run_seconds": time.monotonic() - started,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "boundary": "Synthetic temporary roots only; no mobile UI, JNI, or installed app validation."
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())

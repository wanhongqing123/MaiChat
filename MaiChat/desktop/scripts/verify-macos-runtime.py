#!/usr/bin/env python3
"""Check the load dependencies in a deployed Mac App, without using the build host's libraries."""
import argparse
import json
from pathlib import Path
import plistlib
import subprocess


MACHO_MAGIC = {
    b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
    b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}
LOAD_COMMANDS = {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB"}


def load_commands(binary):
    # LC_ID_DYLIB is the image's identity; universal-image headings are also not dependencies.
    # Reading the actual commands avoids both false positives from otool -L.
    result = subprocess.run(["otool", "-l", str(binary)], check=True,
                            capture_output=True, text=True)
    dependencies, rpaths = set(), set()
    command = ""
    for raw in result.stdout.splitlines():
        line = raw.strip()
        if line.startswith("cmd "):
            command = line[4:]
        elif command in LOAD_COMMANDS and line.startswith("name "):
            dependencies.add(line[5:].rsplit(" (offset ", 1)[0])
        elif command == "LC_RPATH" and line.startswith("path "):
            rpaths.add(line[5:].rsplit(" (offset ", 1)[0])
    return dependencies, rpaths


def verify(app, architecture):
    app = app.resolve(strict=True)
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
    _, main_rpaths = load_commands(executable)
    failures = []
    checked = 0
    architecture_results = []
    required_images = {executable.resolve()}

    def expand(value, declaring_binary):
        for prefix, base in (("@executable_path/", executable.parent),
                             ("@loader_path/", declaring_binary.parent)):
            if value.startswith(prefix):
                return base / value[len(prefix):]
        return Path(value) if value.startswith("/") else None

    def bundled(path):
        return path is not None and path.is_file() and path.resolve().is_relative_to(app)

    for binary in app.rglob("*"):
        if binary.is_symlink() or not binary.is_file():
            continue
        with binary.open("rb") as source:
            if source.read(4) not in MACHO_MAGIC:
                continue
        checked += 1
        result = subprocess.run(["lipo", "-verify_arch", architecture, str(binary)], capture_output=True)
        architecture_results.append((binary, result.returncode))
        dependencies, rpaths = load_commands(binary)
        for dependency in sorted(dependencies):
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                continue
            if dependency.startswith("@rpath/"):
                suffix = dependency[len("@rpath/"):]
                roots = [expand(value, binary) for value in rpaths]
                roots += [expand(value, executable) for value in main_rpaths]
                candidates = [root / suffix for root in roots if root is not None]
            else:
                candidates = [expand(dependency, binary)]
            resolved = [candidate.resolve() for candidate in candidates if bundled(candidate)]
            required_images.update(resolved)
            if not resolved:
                failures.append({"file": str(binary.relative_to(app)), "dependency": dependency})
    additional_sdk_resources = []
    for binary, status in architecture_results:
        if not status:
            continue
        relative = binary.relative_to(app)
        # A vendor framework also carries an auxiliary, separately installed CoreAudio driver.
        # It is not an App load image; keep its architecture visible without rejecting that SDK payload.
        driver_resource = "Resources" in relative.parts and any(part.endswith(".driver") for part in relative.parts)
        if driver_resource and binary.resolve() not in required_images:
            additional_sdk_resources.append(str(relative))
        else:
            failures.append({"file": str(relative), "reason": "missing " + architecture})
    return {"architecture": architecture, "machoFiles": checked, "failures": failures,
            "nonTargetSDKDriverResources": additional_sdk_resources}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--architecture", default="arm64")
    parser.add_argument("--json-output", type=Path)
    arguments = parser.parse_args()
    result = verify(arguments.app, arguments.architecture)
    if arguments.json_output:
        arguments.json_output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 1 if result["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

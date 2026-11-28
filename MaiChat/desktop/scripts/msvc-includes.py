"""Keep Ninja's MSVC include marker ASCII, independently of the console code page."""

import argparse
import ctypes
import locale
import re
import subprocess
import sys
import tempfile
from pathlib import Path


MARKER = b"Note: including file:"


def include_prefix(output, header_name):
    # The owned ASCII basename and Windows path boundary survive any localized
    # output encoding, including non-ASCII TEMP/user names printed in CP936.
    for line in output.splitlines():
        if line.lower().endswith(header_name.lower()):
            path = re.search(rb"(?<![A-Za-z0-9])(?:[A-Za-z]:[\\/]|\\\\)", line)
            if path and line[:path.start()].rstrip():
                return line[:path.start()].rstrip()
    raise RuntimeError("MSVC did not report the owned include probe header")


def probe(compiler):
    # Probe raw bytes instead of decoding localized diagnostics with the host
    # ANSI code page. cl can emit UTF-8 while CMake writes Ninja rules in ANSI.
    with tempfile.TemporaryDirectory(prefix="maichat-includes-") as directory:
        root = Path(directory)
        header = root / "maichat_include_probe.h"
        header.write_text("#pragma once\n", encoding="ascii")
        source = root / "probe.cpp"
        source.write_text('#include "maichat_include_probe.h"\n', encoding="ascii")
        result = subprocess.run(
            [compiler, "/nologo", "/showIncludes", "/c", str(source),
             "/Fo" + str(root / "probe.obj")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        return include_prefix(result.stdout, header.name.encode("ascii")).hex()


def include_prefixes(prefix):
    # CMake retains the original probe bytes across builds. cl can later emit
    # the same localized marker as UTF-8 instead of the Windows ANSI code page.
    # Decode only the marker, strictly; never transcode a path or diagnostic.
    ansi = ("cp" + str(ctypes.windll.kernel32.GetACP())
            if sys.platform == "win32" else locale.getpreferredencoding(False))
    encodings = ("utf-8", ansi)
    variants = {prefix}
    for encoding in encodings:
        try:
            marker = prefix.decode(encoding)
        except UnicodeError:
            continue
        for target in encodings:
            try:
                variants.add(marker.encode(target))
            except UnicodeError:
                pass
        break
    return sorted(variants, key=len, reverse=True)


def launch(prefix, command):
    # Stream diagnostics as bytes. Only the include marker changes; paths,
    # warnings, errors and the compiler's exit status remain intact.
    prefixes = include_prefixes(prefix)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for line in process.stdout:
        for candidate in prefixes:
            if line.startswith(candidate):
                line = MARKER + line[len(candidate):]
                break
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()
    return process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe")
    parser.add_argument("--include-prefix-hex")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.probe:
        print(probe(args.probe))
        return 0
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not args.include_prefix_hex or not command:
        parser.error("--include-prefix-hex and a compiler command are required")
    return launch(bytes.fromhex(args.include_prefix_hex), command)


if __name__ == "__main__":
    sys.exit(main())

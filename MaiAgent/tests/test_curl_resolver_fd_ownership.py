#!/usr/bin/env python3
"""Extract actual vendored curl resolver functions and compile Android fd probes.

No DNS, HTTP, credentials, device commands or complete core build are involved.
Each binary uses real eventfd/pipe descriptors and dup2 reuse after the first close.
"""
import argparse
from pathlib import Path
import re
import subprocess


def function(source: str, name: str) -> str:
    match = re.search(r"static\s+(?:void|int)\s+" + name + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Actual curl function not found: {name}")
    depth = 1
    cursor = match.end()
    while depth and cursor < len(source):
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    if depth:
        raise RuntimeError(f"Unbalanced actual curl function: {name}")
    return source[match.start():cursor]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sysroot", required=True)
    parser.add_argument("--target", default="aarch64-linux-android26")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--baseline-ref", required=True,
                        help="Unpatched source revision, for example b8234d71; current HEAD may already include the fix")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    relative = "MaiAgent/third_party/curl/lib/asyn-thread.c"
    sources = {
        "old": subprocess.run(["git", "show", f"{args.baseline_ref}:{relative}"], cwd=root,
                              check=True, capture_output=True, encoding="utf-8").stdout,
        "fixed": (root / relative).read_text(encoding="utf-8"),
    }
    template = (Path(__file__).parent / "fixtures/curl_resolver_fd_ownership.c").read_text(encoding="utf-8")
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    for label, source in sources.items():
        bodies = "\n\n".join(function(source, name) for name in (
            "destroy_thread_sync_data", "init_thread_sync_data", "destroy_async_data"))
        translation = output / f"curl-resolver-{label}.c"
        translation.write_text(template.replace("/* @CURL_FUNCTIONS@ */", bodies), encoding="utf-8")
        for mode in ("eventfd", "pipe"):
            binary = output / f"curl-resolver-{label}-{mode}"
            subprocess.run([args.compiler, f"--target={args.target}", f"--sysroot={args.sysroot}",
                            "-std=c11", "-D_GNU_SOURCE", f"-DFIXTURE_EVENTFD={int(mode == 'eventfd')}",
                            "-O0", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            str(translation), "-o", str(binary)], check=True)
            print(binary)


if __name__ == "__main__":
    main()

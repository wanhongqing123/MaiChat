#!/usr/bin/env python3
"""Verify and package the pinned bundled local primary model; network is opt-in only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import urllib.request


RESOURCE_DIRECTORY = Path(__file__).resolve().parent.parent / "BundledPrimaryModel"
EXPECTED = {
    "version": 1,
    "id": "builtin-qwen3.5-2b-q4_k_m",
    "name": "Qwen3.5 2B · Q4_K_M",
    "filename": "Qwen_Qwen3.5-2B-Q4_K_M.gguf",
    "bytes": 1396198496,
    "sha256": "57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8",
    "revision": "7d26695454df6de5fbcce2e58681e62dae06ce43",
}
MODEL_URL = (
    "https://huggingface.co/bartowski/Qwen_Qwen3.5-2B-GGUF/resolve/"
    + EXPECTED["revision"] + "/" + EXPECTED["filename"]
)
ATTRIBUTION_HASHES = {
    "LICENSE": "bbedc3fda3305820b977265f01b8619d87570a6739de3a5582c3464840f1e57a",
    "UPSTREAM_MODEL_CARD.md": "c0e83a849c776e6fa843d011f023132d942a0f0140d903205bf1c363adad2275",
}
# Only exact former product resources can be retired from an incremental build.
# Runtime Documents and unrelated files are never cleanup targets.
RETIRED_MODELS = {
    "qwen2.5-0.5b-instruct-q4_k_m.gguf": (
        491400032, "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"
    ),
}


def regular_file(path):
    """Never follow a model-file symlink or accept a directory as a weight."""
    try:
        info = path.lstat()
    except FileNotFoundError as error:
        raise RuntimeError(f"Missing {path}; run this script with --download or --source PATH") from error
    if not stat.S_ISREG(info.st_mode):
        raise RuntimeError(f"Expected a regular file, not a symlink or directory: {path}")
    return info


def digest(path, expected_bytes=None):
    info = regular_file(path)
    if expected_bytes is not None and info.st_size != expected_bytes:
        raise RuntimeError(f"Byte count mismatch for {path}: {info.st_size}, expected {expected_bytes}")
    fingerprint = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            count += len(chunk)
            if count > info.st_size:
                raise RuntimeError(f"File changed while checking: {path}")
            fingerprint.update(chunk)
    after = regular_file(path)
    if count != info.st_size or (after.st_size, after.st_mtime_ns, after.st_ino) != (
        info.st_size, info.st_mtime_ns, info.st_ino
    ):
        raise RuntimeError(f"File changed while checking: {path}")
    return fingerprint.hexdigest()


def verify_model(path):
    if digest(path, EXPECTED["bytes"]) != EXPECTED["sha256"]:
        raise RuntimeError(f"SHA-256 mismatch for {path}; existing files have been preserved")


def verify_metadata():
    manifest = RESOURCE_DIRECTORY / "manifest.json"
    regular_file(manifest)
    if json.loads(manifest.read_text(encoding="utf-8")) != EXPECTED:
        raise RuntimeError("Bundled primary manifest does not match the compiled model identity")
    source = RESOURCE_DIRECTORY / "SOURCE.json"
    regular_file(source)
    receipt = json.loads(source.read_text(encoding="utf-8"))
    if any(receipt.get(key) != EXPECTED[key] for key in ("revision", "bytes", "sha256")) or (
        receipt.get("model_url") != MODEL_URL or receipt.get("license") != "Apache-2.0"
    ):
        raise RuntimeError("Bundled primary source receipt does not match the pinned source")
    for name, expected in ATTRIBUTION_HASHES.items():
        if digest(RESOURCE_DIRECTORY / name) != expected:
            raise RuntimeError(f"Upstream attribution checksum mismatch: {name}")


def acquire_model(destination, source=None):
    """Publish only a complete verified file; preserve any existing local weight."""
    if destination.exists() or destination.is_symlink():
        verify_model(destination)
        return
    if source is not None:
        verify_model(source)
    with tempfile.NamedTemporaryFile(prefix=".primary-model-", dir=RESOURCE_DIRECTORY, delete=False) as output:
        temporary = Path(output.name)
        try:
            if source is not None:
                with source.open("rb") as input_stream:
                    shutil.copyfileobj(input_stream, output, length=1024 * 1024)
            else:
                # Only explicit --download reaches this branch; Xcode never downloads.
                request = urllib.request.Request(MODEL_URL, headers={"User-Agent": "MaiChat-model-prepare/1"})
                with urllib.request.urlopen(request, timeout=60) as response:
                    count = 0
                    while chunk := response.read(1024 * 1024):
                        count += len(chunk)
                        if count > EXPECTED["bytes"]:
                            raise RuntimeError("Downloaded primary model exceeds the pinned byte count")
                        output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
            # Windows cannot rename/unlink this named file while its writer is open.
            output.close()
            verify_model(temporary)
            try:
                os.link(temporary, destination)
            except FileExistsError:
                verify_model(destination)
        finally:
            output.close()
            temporary.unlink(missing_ok=True)


def package_model(destination):
    """Build artifacts can be replaced; runtime sandbox model files never are."""
    destination.mkdir(parents=True, exist_ok=True)
    retired = []
    for name, (size, fingerprint) in RETIRED_MODELS.items():
        path = destination / name
        if path.exists() or path.is_symlink():
            if digest(path, size) != fingerprint:
                raise RuntimeError(f"Superseded build resource changed; preserved for inspection: {name}")
            retired.append(path)
    unexpected = [path.name for path in destination.glob("*.gguf")
                  if path.name != EXPECTED["filename"] and path not in retired]
    if unexpected:
        raise RuntimeError("Unmanaged weight in primary build resources; preserved: " + ", ".join(sorted(unexpected)))
    for name in (EXPECTED["filename"], "manifest.json", "LICENSE", "SOURCE.json", "UPSTREAM_MODEL_CARD.md"):
        source = RESOURCE_DIRECTORY / name
        target = destination / name
        source_hash = EXPECTED["sha256"] if name == EXPECTED["filename"] else digest(source)
        if target.exists() and not target.is_symlink() and digest(target) == source_hash:
            continue
        with tempfile.NamedTemporaryFile(prefix=".primary-model-", dir=destination, delete=False) as output:
            temporary = Path(output.name)
            try:
                with source.open("rb") as input_stream:
                    shutil.copyfileobj(input_stream, output, length=1024 * 1024)
                output.flush()
                os.fsync(output.fileno())
                # Keep the same verified publication semantics on Windows build hosts.
                output.close()
                if digest(temporary) != source_hash:
                    raise RuntimeError(f"File changed while packaging: {name}")
                os.chmod(temporary, 0o644)
                os.replace(temporary, target)
            finally:
                output.close()
                temporary.unlink(missing_ok=True)
    # Publish and verify the replacement first. A failed copy retains the old resource.
    verify_model(destination / EXPECTED["filename"])
    for path in retired:
        size, fingerprint = RETIRED_MODELS[path.name]
        if digest(path, size) != fingerprint:
            raise RuntimeError(f"Superseded build resource changed during packaging: {path.name}")
        path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    acquisition = parser.add_mutually_exclusive_group()
    acquisition.add_argument("--download", action="store_true", help="Explicitly fetch the pinned public weight")
    acquisition.add_argument("--source", type=Path, help="Stage an existing exact weight without network access")
    acquisition.add_argument("--check", action="store_true", help="Verify local files only (also the default)")
    parser.add_argument("--bundle-directory", type=Path, help="Copy verified resources into an App bundle or asset directory")
    arguments = parser.parse_args()
    verify_metadata()
    model = RESOURCE_DIRECTORY / EXPECTED["filename"]
    if arguments.download or arguments.source is not None:
        acquire_model(model, arguments.source)
    verify_model(model)
    if arguments.bundle_directory is not None:
        package_model(arguments.bundle_directory)
    print(f"Bundled primary model verified: {EXPECTED['id']} ({EXPECTED['bytes']} bytes)")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError) as error:
        raise SystemExit(f"Bundled primary model preparation failed: {error}") from error

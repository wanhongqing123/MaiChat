#!/usr/bin/env python3
"""Generate owned EXIF fixtures; verify with stdlib and no Qt/product decoder."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys


OUTPUT = Path(__file__).resolve().parent
WIDTH, HEIGHT = 96, 64
TOLERANCE = 12
COLORS = (
    ((224, 32, 32), (32, 192, 64), (32, 64, 224)),
    ((32, 192, 192), (192, 32, 192), (224, 192, 32)),
)
TRANSFORMS = {
    1: "identity",
    2: "mirror horizontally",
    3: "rotate 180 degrees",
    4: "mirror vertically",
    5: "transpose across the top-left/bottom-right diagonal",
    6: "rotate 90 degrees clockwise",
    7: "transpose across the top-right/bottom-left diagonal",
    8: "rotate 90 degrees counterclockwise",
}
PROFILE_NAME = "sRGB2014.icc"
PROFILE_REPOSITORY_PATH = (
    "MaiChat/desktop/qt/qtbase/tests/auto/gui/painting/qcolorspace/resources/sRGB2014.icc"
)
PROFILE_SHA256 = "384b832de3412066743b52a75ee906b6fb9fb8d9e09e936fc2c43223815c6e0a"
QTBASE_COMMIT = "025bdad181de241e81bf853c8a2d7bf3d19261a9"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_rgb(x: int, y: int) -> tuple[int, int, int]:
    return COLORS[y // 32][x // 32]


def display_size(orientation: int) -> tuple[int, int]:
    return (WIDTH, HEIGHT) if orientation < 5 else (HEIGHT, WIDTH)


def display_to_source(orientation: int, x: int, y: int) -> tuple[int, int]:
    # Independent EXIF coordinate oracle; no ImageOps/Qt/FFmpeg transformation.
    return {
        1: (x, y),
        2: (WIDTH - 1 - x, y),
        3: (WIDTH - 1 - x, HEIGHT - 1 - y),
        4: (x, HEIGHT - 1 - y),
        5: (y, x),
        6: (y, HEIGHT - 1 - x),
        7: (WIDTH - 1 - y, HEIGHT - 1 - x),
        8: (WIDTH - 1 - y, x),
    }[orientation]


def expected_rgba(orientation: int) -> bytes:
    width, height = display_size(orientation)
    return bytes(
        value
        for y in range(height)
        for x in range(width)
        for value in (*source_rgb(*display_to_source(orientation, x, y)), 255)
    )


def exif_segment(orientation: int) -> bytes:
    # Little-endian TIFF IFD0 containing only SHORT Orientation (0x0112).
    tiff = b"II" + struct.pack("<HIH", 42, 8, 1)
    tiff += struct.pack("<HHIHHI", 0x0112, 3, 1, orientation, 0, 0)
    payload = b"Exif\0\0" + tiff
    return b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload


def icc_segment(profile: bytes) -> bytes:
    payload = b"ICC_PROFILE\0\x01\x01" + profile
    return b"\xff\xe2" + struct.pack(">H", len(payload) + 2) + payload


def metadata_and_scan(jpeg: bytes) -> tuple[list[tuple[int, bytes]], bytes]:
    assert jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9"
    segments = []
    offset = 2
    while offset + 4 <= len(jpeg):
        assert jpeg[offset] == 0xFF
        marker = jpeg[offset + 1]
        if marker == 0xDA:
            return segments, jpeg[offset:]
        length = int.from_bytes(jpeg[offset + 2 : offset + 4], "big")
        assert length >= 2 and offset + 2 + length <= len(jpeg)
        segments.append((marker, jpeg[offset + 4 : offset + 2 + length]))
        offset += 2 + length
    raise AssertionError("JPEG has no scan")


def stripped_jpeg(jpeg: bytes) -> bytes:
    segments, scan = metadata_and_scan(jpeg)
    output = bytearray(b"\xff\xd8")
    for marker, payload in segments:
        if marker == 0xE1 and payload.startswith(b"Exif\0\0"):
            continue
        if marker == 0xE2 and payload.startswith(b"ICC_PROFILE\0"):
            continue
        output += bytes((0xFF, marker)) + struct.pack(">H", len(payload) + 2) + payload
    return bytes(output) + scan


def read_orientation(jpeg: bytes) -> int:
    segments, _ = metadata_and_scan(jpeg)
    exif = [payload for marker, payload in segments if marker == 0xE1 and payload.startswith(b"Exif\0\0")]
    assert len(exif) == 1
    payload = exif[0]
    assert payload[:14] == b"Exif\0\0II\x2a\0\x08\0\0\0"
    assert struct.unpack_from("<H", payload, 14)[0] == 1
    assert struct.unpack_from("<HHI", payload, 16) == (0x0112, 3, 1)
    return struct.unpack_from("<H", payload, 24)[0]


def asset(path: Path) -> dict:
    data = path.read_bytes()
    return {"path": path.name, "bytes": len(data), "sha256": sha(data)}


def fixture_info(name: str, orientation: int, jpeg: bytes, has_icc: bool = False) -> dict:
    width, height = display_size(orientation)
    corners = (("top_left", 0, 0), ("top_right", width - 1, 0),
               ("bottom_left", 0, height - 1), ("bottom_right", width - 1, height - 1))
    samples = (("top_left", 4, 4), ("top_right", width - 5, 4),
               ("bottom_left", 4, height - 5), ("bottom_right", width - 5, height - 5))
    return {
        **asset(OUTPUT / name),
        "encoded_format": "JPEG",
        "source_size": [WIDTH, HEIGHT],
        "exif_orientation": orientation,
        "expected_display_size": [width, height],
        "display_transform": TRANSFORMS[orientation],
        "expected_corner_rgba": {
            label: [*source_rgb(*display_to_source(orientation, x, y)), 255]
            for label, x, y in corners
        },
        "sample_points": [
            {"label": label, "x": x, "y": y,
             "expected_rgba": [*source_rgb(*display_to_source(orientation, x, y)), 255]}
            for label, x, y in samples
        ],
        "rgb_max_absolute_error_per_channel": TOLERANCE,
        "alpha_expected_exactly": 255,
        "expected_rgba_file": f"expected-orientation-{orientation}.rgba",
        "jpeg_sos_through_eoi_sha256": sha(metadata_and_scan(jpeg)[1]),
        "stripped_jpeg_sha256": sha(stripped_jpeg(jpeg)),
        "icc_profile_sha256": PROFILE_SHA256 if has_icc else None,
    }


def generate() -> None:
    from PIL import Image, ImageCms, __version__ as pillow_version, features

    OUTPUT.mkdir(parents=True, exist_ok=True)
    rgb = bytes(value for y in range(HEIGHT) for x in range(WIDTH) for value in source_rgb(x, y))
    (OUTPUT / "source-asymmetric.rgb").write_bytes(rgb)
    source = Image.frombytes("RGB", (WIDTH, HEIGHT), rgb)
    buffer = io.BytesIO()
    source.save(buffer, format="JPEG", quality=95, subsampling=0, optimize=False, progressive=False)
    base = buffer.getvalue()
    (OUTPUT / "source-asymmetric.jpg").write_bytes(base)
    assert not any(marker == 0xE1 for marker, _ in metadata_and_scan(base)[0])
    decoded = Image.open(io.BytesIO(base)).convert("RGB").tobytes()
    maximum_error = max(abs(a - b) for a, b in zip(rgb, decoded))
    assert maximum_error <= TOLERANCE
    fixtures = []
    for orientation in range(1, 9):
        (OUTPUT / f"expected-orientation-{orientation}.rgba").write_bytes(expected_rgba(orientation))
        jpeg = base[:2] + exif_segment(orientation) + base[2:]
        name = f"exif-orientation-{orientation}.jpg"
        (OUTPUT / name).write_bytes(jpeg)
        fixtures.append(fixture_info(name, orientation, jpeg))

    profile_path = OUTPUT / PROFILE_NAME
    if profile_path.is_file():
        profile = profile_path.read_bytes()
    else:
        profile = (OUTPUT.parents[2] / PROFILE_REPOSITORY_PATH).read_bytes()
    assert sha(profile) == PROFILE_SHA256
    assert len(profile) == int.from_bytes(profile[:4], "big") and profile[36:40] == b"acsp"
    profile_path.write_bytes(profile)
    parsed = ImageCms.getOpenProfile(io.BytesIO(profile))
    # LittleCMS parses the real upstream profile and can build an RGB transform.
    ImageCms.buildTransformFromOpenProfiles(parsed, parsed, "RGB", "RGB")
    icc_jpeg = base[:2] + exif_segment(6) + icc_segment(profile) + base[2:]
    icc_name = "exif-orientation-6-srgb2014.jpg"
    (OUTPUT / icc_name).write_bytes(icc_jpeg)
    fixtures.append(fixture_info(icc_name, 6, icc_jpeg, True))

    generated = [OUTPUT / "source-asymmetric.rgb", OUTPUT / "source-asymmetric.jpg", profile_path]
    generated += [OUTPUT / f"expected-orientation-{o}.rgba" for o in range(1, 9)]
    manifest = {
        "schema_version": 1,
        "purpose": "Owned still JPEG orientation/ICC fixtures, independent pixel oracle; no user media",
        "source_generation": {
            "size": [WIDTH, HEIGHT], "layout": "3 columns by 2 rows; each solid cell is 32x32",
            "rgb_cells_top_to_bottom": COLORS,
            "pillow_version": pillow_version, "jpeg_library_version": features.version_codec("jpg"),
            "jpeg_encoder": {"quality": 95, "subsampling": "4:4:4", "progressive": False, "optimize": False},
            "pillow_base_decode_quality_control_max_rgb_error": maximum_error,
            "quality_control_is_orientation_oracle": False,
            "generator_sha256": sha(Path(__file__).read_bytes()),
        },
        "oracle": {
            "basis": "Original generated RGB layout plus explicit EXIF display-to-source coordinate formulas",
            "coordinates": "Origin at top left; x increases right, y increases down; no Qt/FFmpeg decoder used",
            "display_to_source": {
                "1": ["x", "y"], "2": ["W-1-x", "y"],
                "3": ["W-1-x", "H-1-y"], "4": ["x", "H-1-y"],
                "5": ["y", "x"], "6": ["y", "H-1-x"],
                "7": ["W-1-y", "H-1-x"], "8": ["W-1-y", "x"],
            },
            "expected_files": "Headerless, top-down tightly packed RGBA8; stride=display_width*4",
            "rgb_tolerance": TOLERANCE,
            "alpha_tolerance": 0,
            "dimensions_orientation_1_to_4": [WIDTH, HEIGHT],
            "dimensions_orientation_5_to_8": [HEIGHT, WIDTH],
            "spec_reference": "Exif Orientation tag 0x0112; TIFF/EP eight row/column orientations",
            "references": ["https://developer.adobe.com/xmp/docs/xmp-namespaces/tiff/",
                           "https://www.cipa.jp/std/documents/e/DC-X010-2017.pdf"],
        },
        "jpeg_identity": {
            "base_sha256": sha(base), "sos_through_eoi_sha256": sha(metadata_and_scan(base)[1]),
            "eight_exif_files_differ_only_by_inserted_app1_orientation": True,
            "encoder_invocations": 1,
        },
        "icc": {
            **asset(profile_path), "source_repository_file": PROFILE_REPOSITORY_PATH,
            "source_repository": "https://github.com/qt/qtbase", "source_commit": QTBASE_COMMIT,
            "upstream_url": "https://github.com/qt/qtbase/blob/" + QTBASE_COMMIT
                            + "/tests/auto/gui/painting/qcolorspace/resources/sRGB2014.icc",
            "description": ImageCms.getProfileDescription(parsed).strip(),
            "copyright": ImageCms.getProfileCopyright(parsed).strip(),
            "validation": "ICC size/acsp checked; LittleCMS parsed it and built RGB-to-RGB transform",
            "littlecms_version": features.version("littlecms2"),
            "generated_or_modified_profile": False,
        },
        "supporting_assets": [asset(path) for path in generated],
        "fixtures": fixtures,
        "not_claimed": ["FFmpeg decoder acceptance", "Qt decoder acceptance", "display color-management accuracy"],
    }
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def verify() -> None:
    manifest = json.loads((OUTPUT / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["source_generation"]["generator_sha256"] == sha(Path(__file__).read_bytes())
    base = (OUTPUT / "source-asymmetric.jpg").read_bytes()
    for entry in manifest["supporting_assets"] + manifest["fixtures"]:
        path = OUTPUT / entry["path"]
        assert path.parent == OUTPUT and asset(path) == {k: entry[k] for k in ("path", "bytes", "sha256")}
    assert sha(base) == manifest["jpeg_identity"]["base_sha256"]
    exif_entries = [f for f in manifest["fixtures"] if not f["icc_profile_sha256"]]
    assert [f["exif_orientation"] for f in exif_entries] == list(range(1, 9))
    for entry in manifest["fixtures"]:
        jpeg = (OUTPUT / entry["path"]).read_bytes()
        orientation = entry["exif_orientation"]
        assert read_orientation(jpeg) == orientation
        assert stripped_jpeg(jpeg) == base
        assert sha(metadata_and_scan(jpeg)[1]) == manifest["jpeg_identity"]["sos_through_eoi_sha256"]
        assert (OUTPUT / entry["expected_rgba_file"]).read_bytes() == expected_rgba(orientation)
        assert entry["expected_display_size"] == list(display_size(orientation))
        if entry["icc_profile_sha256"]:
            segments, _ = metadata_and_scan(jpeg)
            icc = [p[14:] for m, p in segments if m == 0xE2 and p.startswith(b"ICC_PROFILE\0\x01\x01")]
            assert len(icc) == 1 and sha(icc[0]) == PROFILE_SHA256
    print("Verified 8 same-scan EXIF JPEGs, 8 mathematical RGBA oracles, and 1 trusted-profile JPEG")
    print("No product decoder, Qt decode, build, camera, user media, or installation was used")


def write_sums() -> None:
    paths = sorted(p for p in OUTPUT.iterdir() if p.is_file() and p.name != "SHA256SUMS")
    (OUTPUT / "SHA256SUMS").write_text("".join(f"{sha(p.read_bytes())}  {p.name}\n" for p in paths), encoding="ascii")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true", help="stdlib only; read existing fixtures without writing")
    arguments = parser.parse_args()
    if not arguments.verify_only:
        generate()
    verify()
    if not arguments.verify_only:
        write_sums()

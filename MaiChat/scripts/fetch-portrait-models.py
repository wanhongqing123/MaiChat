#!/usr/bin/env python3
"""Stage revision-pinned portrait models; --check verifies without network access."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

MODELS = {
    "face_landmarks.onnx": (
        "https://huggingface.co/naklitechie/face-landmarks-onnx/resolve/575c33816c840c5b156398adb485e4f8d138adc2/face_landmarks.onnx",
        "f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c"),
    "selfie_multiclass.onnx": (
        "https://huggingface.co/senty-au/selfie_multiclass_256x256-ONNX/resolve/6db8421a7150ac20558f2c24675078eb3a1a04d0/onnx/model.onnx",
        "35ec1ecd9ee7f85073c99c00020b7f6751b69506eeacf683bc8665f6117f85b0"),
    "person_detection.onnx": (
        "https://media.githubusercontent.com/media/opencv/opencv_zoo/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/person_detection_mediapipe/person_detection_mediapipe_2023mar.onnx",
        "47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f"),
    "pose_estimation.onnx": (
        "https://media.githubusercontent.com/media/opencv/opencv_zoo/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/pose_estimation_mediapipe/pose_estimation_mediapipe_2023mar.onnx",
        "9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--test-fixtures", action="store_true", help="Also fetch the public MediaPipe pose test photograph")
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parent.parent / "Portrait" / "models"
    root.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in MODELS.items():
        target = root / name
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == expected:
            continue
        if arguments.check:
            raise SystemExit(f"Missing or invalid portrait model: {name}. Run {parser.prog}")
        partial = target.with_suffix(".download")
        try:
            with urllib.request.urlopen(url, timeout=30) as response, partial.open("wb") as output:
                size = 0
                while chunk := response.read(1024 * 1024):
                    size += len(chunk)
                    if size > 64 * 1024 * 1024:
                        raise RuntimeError(f"Model exceeds download limit: {name}")
                    output.write(chunk)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != expected:
                raise RuntimeError(f"Portrait model checksum mismatch: {name}")
            partial.replace(target)
        finally:
            partial.unlink(missing_ok=True)
    print("Portrait model checksums verified (4/4)")
    if arguments.test_fixtures:
        fixtures = root.parent / "tests" / "fixtures"
        fixtures.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen("https://storage.googleapis.com/mediapipe-assets/pose.jpg", timeout=30) as response:
            (fixtures / "pose.jpg").write_bytes(response.read(10 * 1024 * 1024))


if __name__ == "__main__":
    main()

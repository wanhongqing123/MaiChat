"""Exercise incremental replacement with real temporary files and no model/network."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "prepare-bundled-primary-model.py"


class BundledPrimaryPackagingTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("primary_packaging", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.temporary = tempfile.TemporaryDirectory(prefix="maichat-primary-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.destination = self.root / "build" / "Primary"
        self.destination.mkdir(parents=True)
        self.new = b"GGUF-new-public-model-fixture"
        self.old = b"GGUF-retired-public-model-fixture"
        self.module.RESOURCE_DIRECTORY = self.source
        self.module.EXPECTED = {
            "filename": "current.gguf", "bytes": len(self.new),
            "sha256": hashlib.sha256(self.new).hexdigest(),
        }
        self.module.RETIRED_MODELS = {
            "retired.gguf": (len(self.old), hashlib.sha256(self.old).hexdigest())
        }
        (self.source / "current.gguf").write_bytes(self.new)
        for name in ("manifest.json", "LICENSE", "SOURCE.json", "UPSTREAM_MODEL_CARD.md"):
            (self.source / name).write_text(json.dumps({"fixture": name}))
        (self.destination / "retired.gguf").write_bytes(self.old)

    def test_verified_replacement_retires_only_the_known_build_resource(self):
        unrelated = self.destination / "preserve.txt"
        unrelated.write_bytes(b"unrelated build evidence")
        self.module.package_model(self.destination)
        self.assertEqual((self.destination / "current.gguf").read_bytes(), self.new)
        self.assertFalse((self.destination / "retired.gguf").exists())
        self.assertEqual(unrelated.read_bytes(), b"unrelated build evidence")
        self.assertEqual((self.source / "current.gguf").read_bytes(), self.new)
        self.module.package_model(self.destination)
        self.assertEqual(list(self.destination.glob("*.gguf")), [self.destination / "current.gguf"])

    def test_changed_retired_file_is_preserved_and_blocks_publication(self):
        sentinel = b"existing unrelated changed weight"
        (self.destination / "retired.gguf").write_bytes(sentinel)
        with self.assertRaises(RuntimeError):
            self.module.package_model(self.destination)
        self.assertEqual((self.destination / "retired.gguf").read_bytes(), sentinel)
        self.assertFalse((self.destination / "current.gguf").exists())

    def test_failed_replacement_keeps_the_previous_complete_weight(self):
        (self.source / "current.gguf").write_bytes(b"incomplete candidate")
        with self.assertRaises(RuntimeError):
            self.module.package_model(self.destination)
        self.assertEqual((self.destination / "retired.gguf").read_bytes(), self.old)
        self.assertFalse((self.destination / "current.gguf").exists())

    def test_unmanaged_weight_is_preserved_and_blocks_publication(self):
        other = self.destination / "unmanaged.gguf"
        other.write_bytes(b"do not remove")
        with self.assertRaises(RuntimeError):
            self.module.package_model(self.destination)
        self.assertEqual(other.read_bytes(), b"do not remove")
        self.assertEqual((self.destination / "retired.gguf").read_bytes(), self.old)


if __name__ == "__main__":
    unittest.main()

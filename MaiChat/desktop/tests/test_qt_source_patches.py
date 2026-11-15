"""Exercise actual Git patch application and SDK source-cache provenance privately."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("maichat_qt_builder", Path(__file__).parents[1] / "scripts/build-qt.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class QtSourcePatchesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "qtbase"
        self.source.mkdir()
        self.git("init", "-q")
        self.git("config", "user.email", "owned-fixture@example.invalid")
        self.git("config", "user.name", "Owned build fixture")
        (self.source / "source.cpp").write_bytes(b"before\n")
        self.git("add", "source.cpp")
        self.git("commit", "-qm", "owned baseline")
        self.before = self.git("hash-object", "source.cpp")
        (self.source / "source.cpp").write_bytes(b"after\n")
        self.after = self.git("hash-object", "source.cpp")
        self.patch = self.root / "fix.patch"
        self.patch.write_bytes(subprocess.check_output(["git", "-C", str(self.source), "diff", "--binary"]))
        self.git("restore", "source.cpp")
        self.module = {"name": "qtbase", "patches": [{"platform": "windows", "file": "fix.patch",
            "sha256": hashlib.sha256(self.patch.read_bytes()).hexdigest(),
            "files": [{"path": "source.cpp", "before": self.before, "after": self.after}]}]}

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.source), *args], text=True).strip()

    def apply(self):
        function = getattr(builder, "apply_source_patches", None)
        self.assertTrue(callable(function), "Pinned SDK bootstrap must apply and fingerprint reviewed source overlays")
        return function(self.root, self.source, self.module, "windows")

    def test_reviewed_patch_is_idempotent_and_other_platforms_stay_unmodified(self):
        receipt = self.apply()
        self.assertEqual(self.git("hash-object", "source.cpp"), self.after)
        self.assertEqual(self.apply(), receipt)
        self.git("restore", "source.cpp")
        self.assertEqual(builder.apply_source_patches(self.root, self.source, self.module, "macos"), [])
        self.assertEqual(self.git("hash-object", "source.cpp"), self.before)

    def test_wrong_digest_and_unrecognized_preimage_never_modify_source(self):
        self.module["patches"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(RuntimeError, "digest"):
            self.apply()
        self.assertEqual(self.git("hash-object", "source.cpp"), self.before)
        self.module["patches"][0]["sha256"] = hashlib.sha256(self.patch.read_bytes()).hexdigest()
        (self.source / "source.cpp").write_text("someone else's work\n")
        with self.assertRaisesRegex(RuntimeError, "preimage"):
            self.apply()
        self.assertEqual((self.source / "source.cpp").read_text(), "someone else's work\n")

    def test_source_fingerprint_changes_for_tracked_and_untracked_bytes(self):
        function = getattr(builder, "source_state_fingerprint", None)
        self.assertTrue(callable(function), "A dirty SDK build must not share the clean SDK's cache identity")
        clean = function(self.source)
        (self.source / "source.cpp").write_text("dirty\n")
        self.assertNotEqual(function(self.source), clean)
        self.git("restore", "source.cpp")
        self.assertEqual(function(self.source), clean)
        (self.source / "new.h").write_text("one\n")
        first = function(self.source)
        self.assertNotEqual(first, clean)
        (self.source / "new.h").write_text("two\n")
        self.assertNotEqual(function(self.source), first)

    def test_clean_windows_checkout_keeps_the_locked_patch_bytes(self):
        store = self.root / "patch-store"
        store.mkdir()
        command = ["git", "-C", str(store)]
        subprocess.run(command + ["init", "-q"], check=True)
        for name, value in [("user.email", "owned-fixture@example.invalid"), ("user.name", "Owned fixture"),
                            ("core.autocrlf", "true")]:
            subprocess.run(command + ["config", name, value], check=True)
        attributes = Path(__file__).parents[1] / "qt/patches/.gitattributes"
        if attributes.is_file():
            (store / ".gitattributes").write_bytes(attributes.read_bytes())
        checked = store / "fix.patch"
        checked.write_bytes(self.patch.read_bytes())
        subprocess.run(command + ["add", "."], check=True)
        subprocess.run(command + ["commit", "-qm", "owned patch checkout"], check=True)
        checked.unlink()
        subprocess.run(command + ["checkout-index", "-f", "--", "fix.patch"], check=True)
        self.module["patches"][0]["file"] = "patch-store/fix.patch"
        try:
            self.apply()
        except RuntimeError as error:
            self.fail("A clean Windows checkout must apply its locked overlay: " + str(error))
        self.assertEqual(self.git("hash-object", "source.cpp"), self.after)
        # The same clean Git blob without the tracked policy reproduces the
        # rejected CRLF checkout; the helper must still fail closed on it.
        subprocess.run(command + ["rm", "-q", ".gitattributes"], check=True)
        subprocess.run(command + ["commit", "-qm", "owned negative EOL control"], check=True)
        checked.unlink()
        subprocess.run(command + ["checkout-index", "-f", "--", "fix.patch"], check=True)
        self.assertNotEqual(hashlib.sha256(checked.read_bytes()).hexdigest(), self.module["patches"][0]["sha256"])
        with self.assertRaisesRegex(RuntimeError, "digest"):
            self.apply()
        self.assertEqual(self.git("hash-object", "source.cpp"), self.after)


if __name__ == "__main__":
    unittest.main()

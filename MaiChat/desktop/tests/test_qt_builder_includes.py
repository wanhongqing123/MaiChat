"""Offline source-SDK metadata, layout and localized compiler regressions."""
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


script = Path(__file__).resolve().parents[1] / "scripts" / "build-qt.py"
spec = importlib.util.spec_from_file_location("maichat_qt_builder", script)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class IncludesPrefixTests(unittest.TestCase):
    def test_english_prefix_and_indentation(self):
        output = b"probe.cpp\r\nNote: including file:   E:\\private\\mai-qt-include-probe.h\r\n"
        self.assertEqual(builder.parse_msvc_include_prefix(output), "Note: including file: ")

    def test_utf8_chinese_prefix_is_not_double_decoded(self):
        prefix = "\u6ce8\u610f: \u5305\u542b\u6587\u4ef6: "
        output = (prefix + " E:/private/mai-qt-include-probe.h\r\n").encode("utf-8")
        self.assertEqual(builder.parse_msvc_include_prefix(output), prefix)

    def test_missing_probe_header_is_not_silently_accepted(self):
        with self.assertRaises(RuntimeError):
            builder.parse_msvc_include_prefix(b"compiler failed\r\n")


class SdkProfileTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="maichat-qt-profile-test-")
        self.addCleanup(temporary.cleanup)
        self.desktop = Path(temporary.name) / "desktop"
        self.source_root = self.desktop / "qt"
        self.fixture_script = self.desktop / "scripts" / "build-qt.py"
        self.fixture_script.parent.mkdir(parents=True)
        self.fixture_script.write_text("# private layout fixture\n", encoding="utf-8")
        self.source_root.mkdir()
        self.lock = {
            "version": "6.12.0",
            "sdk_profile": "widgets-native-images",
            "modules": [
                {"name": "qtbase", "commit": "base-commit"},
                {"name": "qtsvg", "commit": "svg-commit"},
            ],
        }
        for module in self.lock["modules"]:
            source = self.source_root / module["name"]
            source.mkdir()
            (source / "CMakeLists.txt").write_text("# never configured\n", encoding="utf-8")
        self.commands = []

    def invoke(self, *arguments, system="Darwin", tools=True):
        (self.source_root / "sources.lock.json").write_text(json.dumps(self.lock), encoding="utf-8")
        output = io.StringIO()
        errors = io.StringIO()
        install_prefix = None

        def fake_run(command, cwd=None):
            # Simulate only the four required SDK marker files, never execute CMake.
            nonlocal install_prefix
            values = [str(value) for value in command]
            self.commands.append(values)
            for value in values:
                if value.startswith("-DCMAKE_INSTALL_PREFIX="):
                    install_prefix = Path(value.partition("=")[2])
            if "--install" in values:
                for component in ("Core", "Widgets", "Svg", "Test"):
                    config = install_prefix / f"lib/cmake/Qt6{component}/Qt6{component}Config.cmake"
                    config.parent.mkdir(parents=True, exist_ok=True)
                    config.write_text("# synthetic SDK marker\n", encoding="utf-8")

        def fake_git(source, *arguments):
            if arguments == ("status", "--porcelain"):
                return ""
            if arguments == ("rev-parse", "HEAD"):
                return next(module["commit"] for module in self.lock["modules"] if module["name"] == source.name)
            raise AssertionError(f"unexpected git access: {arguments}")

        build_root = self.source_root / ".build" / ("windows-x86_64" if system == "Windows" else "macos-arm64")
        with ExitStack() as stack:
            stack.enter_context(patch.object(builder, "__file__", str(self.fixture_script)))
            stack.enter_context(patch.object(builder.platform, "system", return_value=system))
            stack.enter_context(patch.object(builder.shutil, "which", side_effect=lambda name: name if tools else None))
            stack.enter_context(patch.object(builder.shutil, "disk_usage", return_value=SimpleNamespace(free=100 * 1024 ** 3)))
            stack.enter_context(patch.object(builder, "run", side_effect=fake_run))
            stack.enter_context(patch.object(builder, "git", side_effect=fake_git))
            stack.enter_context(patch.object(builder.subprocess, "run", side_effect=AssertionError("real process forbidden")))
            stack.enter_context(patch.object(builder.subprocess, "check_output", side_effect=AssertionError("real git forbidden")))
            include_hook = stack.enter_context(patch.object(builder, "msvc_include_hook",
                return_value=("Note: including file: ", build_root / "msvc-showincludes.cmake")))
            stack.enter_context(patch("sys.argv", [str(self.fixture_script), "--arch",
                "AMD64" if system == "Windows" else "aarch64", *arguments]))
            stack.enter_context(redirect_stdout(output))
            stack.enter_context(redirect_stderr(errors))
            builder.main()
        return output.getvalue(), include_hook

    def test_print_prefix_reads_profile_without_build_tools(self):
        output, include_hook = self.invoke("--print-prefix", tools=False)
        expected = self.source_root / ".build/macos-arm64/install-widgets-native-images"
        self.assertEqual(output.strip(), str(expected))
        self.assertEqual(self.commands, [])
        include_hook.assert_not_called()
        self.assertFalse((self.source_root / ".build").exists())

    def test_profile_isolates_install_and_stamp_but_reuses_module_build_paths(self):
        root = self.source_root / ".build/macos-arm64"
        legacy_install = root / "install"
        legacy_install.mkdir(parents=True)
        legacy_marker = legacy_install / "preserved-sdk.txt"
        legacy_marker.write_text("legacy SDK\n", encoding="utf-8")
        legacy_stamp = root / "installed.json"
        legacy_stamp.write_text('{"legacy": "preserve"}\n', encoding="utf-8")
        old_stamp = legacy_stamp.read_bytes()
        self.invoke()
        prefix = root / "install-widgets-native-images"
        self.assertTrue((prefix / "lib/cmake/Qt6Core/Qt6CoreConfig.cmake").is_file())
        stamp = root / "installed-widgets-native-images.json"
        self.assertTrue(stamp.is_file())
        self.assertEqual(json.loads(stamp.read_text())["sdk_profile"], "widgets-native-images")
        self.assertEqual(legacy_stamp.read_bytes(), old_stamp)
        self.assertEqual(legacy_marker.read_text(), "legacy SDK\n")
        configure = [command for command in self.commands if "-S" in command]
        self.assertEqual([command[command.index("-B") + 1] for command in configure],
                         [str(root / "qtbase"), str(root / "qtsvg")])
        self.assertTrue(all(f"-DCMAKE_INSTALL_PREFIX={prefix}" in command for command in configure))
        self.assertIn(f"-DCMAKE_PREFIX_PATH={prefix}", configure[1])
        count = len(self.commands)
        output, _ = self.invoke()
        self.assertIn("reusing", output)
        self.assertEqual(len(self.commands), count)

    def test_absent_profile_retains_legacy_paths(self):
        del self.lock["sdk_profile"]
        output, _ = self.invoke("--print-prefix", tools=False)
        root = self.source_root / ".build/macos-arm64"
        self.assertEqual(output.strip(), str(root / "install"))
        self.invoke()
        self.assertTrue((root / "installed.json").is_file())
        self.assertFalse((root / "installed-widgets-native-images.json").exists())
        self.assertIsNone(json.loads((root / "installed.json").read_text()).get("sdk_profile"))
        self.assertTrue(all("-U" not in command for command in self.commands))

    def test_profile_external_module_resets_cached_qt_dirs_before_selecting_prefix(self):
        self.invoke()
        configure = [command for command in self.commands if "-S" in command]
        self.assertNotIn("-U", configure[0])
        external = configure[1]
        self.assertIn("-U", external)
        reset = external.index("-U")
        self.assertEqual(external[reset + 1], "Qt6*_DIR")
        prefix = self.source_root / ".build/macos-arm64/install-widgets-native-images"
        self.assertLess(reset, external.index(f"-DQt6_DIR={prefix}/lib/cmake/Qt6"))

    def test_safe_ascii_custom_profile_selects_its_own_prefix(self):
        self.lock["sdk_profile"] = "UI_2-native"
        output, _ = self.invoke("--print-prefix", tools=False)
        self.assertEqual(output.strip(), str(self.source_root / ".build/macos-arm64/install-UI_2-native"))

    def test_invalid_profiles_are_rejected_before_printing_or_writing(self):
        for profile in ("", ".", "..", "../escape", "a/b", "a\\b", "has space", "native.images",
                        "\u56fe\u7247", "native\n", None, 42, ["native"]):
            with self.subTest(profile=profile):
                self.lock["sdk_profile"] = profile
                with self.assertRaises(SystemExit) as rejected:
                    self.invoke("--print-prefix", tools=False)
                self.assertEqual(rejected.exception.code, 2)
                self.assertFalse((self.source_root / ".build").exists())

    def test_windows_profile_keeps_the_existing_include_probe_contract(self):
        _, include_hook = self.invoke(system="Windows")
        root = self.source_root / ".build/windows-x86_64"
        include_hook.assert_called_once_with("cl", root)
        configure = [command for command in self.commands if "-S" in command]
        self.assertTrue(all(f"-DCMAKE_PROJECT_INCLUDE={(root / 'msvc-showincludes.cmake').as_posix()}" in command
                            for command in configure))
        stamp = root / "installed-widgets-native-images.json"
        self.assertTrue(stamp.is_file())
        self.assertEqual(json.loads(stamp.read_text())["msvcIncludePrefix"], "Note: including file: ")

    def test_formal_lock_retains_only_pinned_ui_modules(self):
        lock = json.loads((script.parents[1] / "qt/sources.lock.json").read_text())
        self.assertEqual(lock.get("sdk_profile"), "widgets-native-images")
        self.assertEqual([(module["name"], module["commit"]) for module in lock["modules"]], [
            ("qtbase", "025bdad181de241e81bf853c8a2d7bf3d19261a9"),
            ("qtsvg", "883a58532078d003cefef0a549bfe9f885ce41f5"),
        ])


if __name__ == "__main__":
    unittest.main()

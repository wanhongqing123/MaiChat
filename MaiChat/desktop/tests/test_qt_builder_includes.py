"""Offline regression for localized compiler output used by the source SDK."""
import importlib.util
from pathlib import Path
import unittest


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


if __name__ == "__main__":
    unittest.main()

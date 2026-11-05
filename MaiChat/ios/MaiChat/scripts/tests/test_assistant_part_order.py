"""Guard the mobile rendering boundary when iOS cannot be built on this host.

These checks complement the Android grouping unit tests: neither platform may
hoist every tool in a message into a container before rendering its text parts.
They do not replace building or viewing either app on a target device.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[5]


class AssistantPartOrderTests(unittest.TestCase):
    def test_android_consecutive_tool_groups(self):
        java_root = ROOT / "MaiChat/android/MaiChat/app/src"
        helper = java_root / "main/java/assistant/com/kongshang/maichat/AIAssistantPartGroups.java"
        self.assertTrue(helper.is_file(), "An independently testable grouping boundary is missing")
        java_bin = Path(r"C:\Program Files\Android\Android Studio\jbr\bin")
        javac = shutil.which("javac") or str(java_bin / "javac.exe")
        java = shutil.which("java") or str(java_bin / "java.exe")
        if not Path(javac).is_file():
            self.skipTest("JDK is unavailable on this host")
        test = java_root / "test/java/com/kongshang/maichat/AIAssistantPartGroupsTest.java"
        with tempfile.TemporaryDirectory(prefix="maichat-part-groups-") as output:
            subprocess.run([javac, "-encoding", "UTF-8", "-d", output, str(helper), str(test)],
                           check=True)
            subprocess.run([java, "-cp", output, "com.kongshang.maichat.AIAssistantPartGroupsTest"],
                           check=True)

    def test_ios_does_not_hoist_all_tools_before_text(self):
        source = (ROOT / "MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift").read_text(
            encoding="utf-8"
        )
        row = source.split("struct AIMessageRow:", 1)[1].split(
            "private var hasVisibleAnswerText:", 1
        )[0]
        self.assertFalse("if !toolParts.isEmpty" in row,
                         "Text A, tool X, text B must keep A before the tool group")

    def test_ios_capture_receipt_with_media_ends_its_tool_run(self):
        source = (ROOT / "MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift").read_text(
            encoding="utf-8"
        )
        grouping = source.split("private var orderedPartGroups:", 1)[1].split(
            "private func mediaArtifacts(in", 1
        )[0]
        self.assertFalse("groups[last].isToolGroup" in grouping,
                         "A capture receipt with a media card must not absorb a later tool")
        self.assertIn("canJoinNextTool", grouping)

    def test_ios_canonical_media_uses_final_owner_order(self):
        source = (ROOT / "MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift").read_text(
            encoding="utf-8"
        )
        canonical = source.split("private var mediaArtifacts:", 1)[1].split(
            "private var textMediaMentions:", 1
        )[0]
        self.assertRegex(canonical, r"return result\.sorted\s*\{",
                         "Replacing an earlier capture with a later delivery must reorder its card")

    def test_ios_playlist_follows_artifacts_then_inline_mentions(self):
        source = (ROOT / "MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift").read_text(
            encoding="utf-8"
        )
        playlist = source.split("private var displayedMediaItems:", 1)[1].split(
            "private func fullMediaItems", 1
        )[0]
        self.assertIn("return artifacts + mentions", playlist,
                      "Assistant inline mention cards follow the ordered part cards in the row")

    def test_android_does_not_reuse_first_tool_container_for_later_runs(self):
        source = (ROOT / "MaiChat/android/MaiChat/app/src/main/java/assistant/"
                  "com/kongshang/maichat/AIAssistantPanel.java").read_text(encoding="utf-8")
        self.assertFalse("toolDetails == null" in source,
                         "A later tool run must get a separate container after its preceding text")


if __name__ == "__main__":
    unittest.main()

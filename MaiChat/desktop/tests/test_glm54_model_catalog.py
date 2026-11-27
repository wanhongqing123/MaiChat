"""Keep host model choices and provider routes aligned without contacting providers."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Glm54HostCatalogTest(unittest.TestCase):
    def test_desktop_choices_and_routes_replace_only_flagship(self):
        for relative in (
            "desktop/src/agent/AgentChatPanel.cpp",
            "desktop/src/agent/RemoteAgentPanel.cpp",
            "desktop/src/agent/DesktopAccountModelClient.cpp",
        ):
            with self.subTest(source=relative):
                source = (ROOT / relative).read_text(encoding="utf-8")
                self.assertTrue('"glm-5.4"' in source, "GLM-5.4 must be selectable and routed")
                self.assertIn('"glm-5.3-flash"', source)
                self.assertNotIn('"glm-5.3"', source)

    def test_ios_choices_and_provider_keep_flash_and_responses(self):
        source = (ROOT / "ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift").read_text(encoding="utf-8")
        self.assertIn('"glm-5.4", "glm-5.3-flash"', source)
        self.assertIn('case "glm-5.4": return "GLM-5.4"', source)
        self.assertIn('https://open.bigmodel.cn/api/v1', source)
        view = (ROOT / "ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift").read_text(encoding="utf-8")
        self.assertIn('ai-primary-model-glm-5.4"', view)
        self.assertIn('ai-primary-model-glm-5.3-flash"', view)
        self.assertNotIn('ai-primary-model-glm-5.3"', view)

    def test_ios_saved_alias_is_exact_and_read_only(self):
        source = (ROOT / "ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift").read_text(encoding="utf-8")
        self.assertRegex(source, r'model == "glm-5\.3" \? "glm-5\.4" : model')
        self.assertIn('settings.migrateLegacyCloudSelection()', source)
        self.assertNotRegex(source, r'replacingOccurrences\(of: "glm-5\.3"')

    def test_desktop_restores_preferences_through_shared_core(self):
        source = (ROOT / "desktop/src/agent/AgentController.cpp").read_text(encoding="utf-8")
        self.assertIn('maiCanonicalCloudModel(model.isEmpty()', source)


if __name__ == "__main__":
    unittest.main()

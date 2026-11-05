"""Check that both production hosts install the shared progress policy."""

from pathlib import Path
import re
import unittest


REPOSITORY = Path(__file__).resolve().parents[3]
HEADER = REPOSITORY / "MaiAgent/include/MaiAgentProgressInstructions.h"
SOURCE = REPOSITORY / "MaiAgent/Agent/MaiAgentProgressInstructions.cpp"
HOSTS = (
    REPOSITORY / "MaiChat/desktop/src/agent/AgentController.cpp",
    REPOSITORY / "MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp",
)


class AgentProgressInstructionsContract(unittest.TestCase):
    def test_production_hosts_append_the_shared_policy(self):
        for host in HOSTS:
            with self.subTest(host=host.relative_to(REPOSITORY)):
                text = host.read_text(encoding="utf-8")
                self.assertTrue(
                    '#include "MaiAgentProgressInstructions.h"' in text,
                    "Production host does not include the shared progress policy",
                )
                self.assertTrue(
                    re.search(
                        r"options\.baseInstructions\s*=\s*\w+\s*\+\s*"
                        r"maiAgentProgressInstructions\(\)\s*;",
                        text,
                    ),
                    "Production host does not append the shared progress policy",
                )

    def test_public_policy_is_linked_into_the_core(self):
        self.assertTrue(HEADER.is_file(), "Shared progress policy header is missing")
        self.assertTrue(SOURCE.is_file(), "Shared progress policy implementation is missing")
        self.assertRegex(
            HEADER.read_text(encoding="utf-8"),
            r"std::string\s+maiAgentProgressInstructions\(\)\s*;",
        )
        cmake = (REPOSITORY / "MaiAgent/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("Agent/MaiAgentProgressInstructions.cpp", cmake)

    def test_policy_requires_visible_truthful_bounded_updates(self):
        self.assertTrue(SOURCE.is_file(), "Shared progress policy implementation is missing")
        text = SOURCE.read_text(encoding="utf-8").lower()
        for requirement in (
            "before starting tool work",
            "visible assistant",
            "reasoning",
            "60 seconds",
            "observed facts",
            "do not invent",
            "paid tasks",
        ):
            with self.subTest(requirement=requirement):
                self.assertIn(requirement, text)


if __name__ == "__main__":
    unittest.main()

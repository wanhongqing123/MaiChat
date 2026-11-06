"""Check the iOS question presentation boundary on hosts without Xcode.

Native contract tests exercise question ownership and submission. These source
checks only prevent reintroducing a second question input or selectable options;
they do not replace an iOS build or device interaction.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[5]
ASSISTANT = ROOT / "MaiChat/ios/MaiChat/MaiChat/Assistant"


class QuestionConversationPresentationTests(unittest.TestCase):
    def test_question_has_no_separate_editor_or_option_controls(self):
        source = (ASSISTANT / "AIAssistantView.swift").read_text(encoding="utf-8")
        self.assertFalse("AIQuestionCard" in source,
                         "Question text belongs in the normal message stream, without a second answer editor")
        self.assertFalse('TextField("你的回答"' in source)
        self.assertFalse('model.action("answer"' in source,
                         "Only the normal composer may submit question answers")

    def test_question_is_rendered_as_part_of_its_owning_message(self):
        source = (ASSISTANT / "AIAssistantView.swift").read_text(encoding="utf-8")
        row = source.split("struct AIMessageRow:", 1)[1]
        self.assertTrue("questionText" in row,
                      "Pending and restored questions must stay at the tool's original message position")
        self.assertFalse("ForEach(model.questions)" in source,
                         "A pending question must not be appended again after the message history")

    def test_transient_answer_history_has_fixed_memory_limits(self):
        source = (ASSISTANT / "AIAssistantModel.swift").read_text(encoding="utf-8")
        self.assertTrue("acceptedQuestionAnswerOrder.count > 64" in source,
                        "Missing canonical pages must not grow accepted answer echoes without a limit")
        self.assertTrue("questionAnswerReservationOrder.count > 128" in source,
                        "Answered question tombstones must stay bounded across many sessions")
        reset = source.split("static func resetForAccountSwitch()", 1)[1].split("shared = AIAssistantModel()", 1)[0]
        self.assertTrue("shared.clearQuestionConversationState()" in reset,
                        "Switching accounts must clear the old answer presentation state")

    def test_outside_window_owner_updates_do_not_change_the_history_cursor(self):
        source = (ASSISTANT / "AIAssistantModel.swift").read_text(encoding="utf-8")
        self.assertTrue('"includeMessageUpdates": true' in source,
                        "An old assistant can continue asking after newer user inputs leave the recent page")
        self.assertTrue("var messageUpdates: [AIMessage]?" in source)
        self.assertTrue("mergeMessages(updates.filter { known.contains($0.id) })" in source,
                        "Unloaded old message point reads must not advance the oldest root history cursor")
        self.assertTrue("latest.suffix(20)" in source,
                        "Outside-window live owners need a bounded display window")

    def test_only_real_remote_publisher_consumes_owner_updates(self):
        source = (ASSISTANT / "AIAssistantModel.swift").read_text(encoding="utf-8")
        self.assertTrue("includeMessageUpdates: Bool = false" in source,
                        "Receipt decoration and root-owner lookup cannot consume the producer queue")
        publisher = source.split("private func pushLocalSessions", 1)[1].split("private func publishLocalPage", 1)[0]
        self.assertTrue("includeMessageUpdates: true" in publisher)
        self.assertTrue("for updated in state.messageUpdates ?? []" in source,
                        "Old assistant updates must be published with their actual canonical IDs")
        self.assertTrue("if state.hasMessageUpdates == true { localSessionChanged(sessionID) }" in source,
                        "A full twenty-owner update batch must not drop the remaining pending owners")


if __name__ == "__main__":
    unittest.main()

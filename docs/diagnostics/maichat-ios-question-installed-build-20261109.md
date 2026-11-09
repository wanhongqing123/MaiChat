# iOS question presentation and installed-build alignment

Started 2026-11-09; device handoff completed 2026-11-10.
Build input: `631f5d9c5d83910e68d9b9d8ae24b2443016879c`,
Core `69e18eb1e4b587b30ad8b467dd3172bea0443b8e`, plus the four iOS
Debug/test files recorded below. Later Windows curl commit `0f7a713e` is not
part of this phone build. The previously delivered desktop 2.2.54 candidates
remain unchanged.

## Report, expected operation and evidence

The user supplied an iPhone creation-center screenshot containing a blue
question card, selectable suggestions, a second “你的回答” field and a “回复”
button. The requested behavior remains: render the question and suggestions as
normal transcript text, and treat the next text sent from the existing bottom
composer as the answer to that question.

The screenshot precisely matches the `AIQuestionCard` rendering in the parent
of `f4890a4e87f08da791019db32ff1f9891ee46ddb`. That commit already removed the
card and connected ordinary composer intake to the original question ID.
Current `AIAssistantView.swift` contains no such card or second answer editor.
This investigation does not reintroduce that UI to manufacture a failing test.

Read-only CoreDevice inspection identifies the connected iPhone as iOS 27.2,
with `com.kongshang.maichat` version 2.2.4, build 202. The installed build number
does not identify its exact source commit; the old executable hash could not
be read through that metadata API. The earlier question change's diagnostic
explicitly recorded source-contract checks only for iOS, without an Xcode
build or device acceptance. Source correctness alone did not establish that
the implementation was present in the user's installed application.

## Regression coverage

Four files add only isolated test support and regression coverage:

- `AIAssistantModel.swift`: a Debug/Simulator-only native-answer receipt hook,
  after normal durable-queue, owner and question-binding validation.
- `AIAssistantModel+UITesting.swift`: owned temporary workspace and question
  DTO/message fixture; wrong routing through steering explicitly fails.
- `AIAssistantUITestFixtures.swift`: reuse the ordinary Root/composer and expose
  the synthetic receipt's question/session identity.
- `AIAssistantUITests.swift`: exercise transcript presentation, no second
  editor/option buttons, Return-to-answer, normal answer bubble and cleared
  composer. A separately opt-in connected-phone check only inspects the normal
  application; it never answers the user's real question or starts a model task.

The native question protocol, production presentation and release answer path
remain unchanged. The receipt hook is excluded from the device Release build.

Simulator iOS 26.5: normal build-for-testing succeeds. The new question case
and existing busy-composer steering case pass 2/2 in 26.494 seconds. The same
question case passes again while recording the required interaction, 1/1 in
11.988 seconds. Both screenshots and four recording frames were personally
viewed: normal question/options text, one bottom composer and the answer bubble.
The final native receipt is synthetic; these runs do not claim real-provider
execution. Existing question source-contract checks pass 5/5.

Two setup failures remain preserved: Xcode 27 rejects generated Pods deployment
targets 8/9/12, so the build invocation aligns them to the App/native bridge's
existing iOS 16 target. An unsigned Simulator closure then exits during loading
with CODESIGNING/Invalid Page; explicitly signing the private Simulator bundle
resolves it, and the same assertions pass. Neither setup failure is counted as
a behavior RED. No SDK/Pods source, dependency version or assertion is changed.

## Phone build and installation

Normal device Release build and inspection-runner build succeed. The invocation
sets `CURRENT_PROJECT_VERSION=203` without changing the shared Xcode project.
The signed application is version 2.2.4 (203), bundle ID
`com.kongshang.maichat`; deep/strict signature verification succeeds.
Executable SHA256:
`87ea9cc8afb7a0bb09d0373f8ff421d11a1b284032801912902670d478afe71b`.

The user connected the phone and authorized continuing the update. CoreDevice
confirms build 202 immediately before the operation. In-place installation
succeeds; subsequent device metadata reads back version 2.2.4 (203).
No uninstall or data deletion is performed. The normal application opens the
creation center and retains its existing conversation content. The actual device
UI recording and accessibility hierarchy show the ordinary bottom composer and
no second “你的回答” editor.

The opt-in exact-question check fails one assertion because startup restores a
different history conversation: the supplied natural question is not present in
the current page. Its original 1-failure result, hierarchy and device recording
remain preserved. This is not counted as a passing live-question case, and no
assertion is relaxed or question manufactured on the phone. Real answer
submission is verified only by the isolated Simulator case. The device check
does not send any real answer or model request. Existing user conversation
content is not copied into source-control diagnostics or shared proof files.
The final ordinary launch uses no fixture arguments and does not terminate an
existing instance. Its executable bundle URL matches the final installed-app
metadata, which again reports 2.2.4 (203). The phone is left on the normal App.

Private original evidence:
`/private/tmp/maichat-ios-inline-question-20261109/`.
No account tokens, private signing keys, user databases or provider requests
are exported. Device identifiers and user screenshots stay out of this document.

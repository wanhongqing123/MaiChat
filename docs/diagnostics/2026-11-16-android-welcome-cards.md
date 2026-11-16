# Android welcome cards

The user requested that ordinary AI chat remain available through the composer,
without a separate QA introduction card, and that the disabled image/video creation
introduction cards also disappear. The final change removes those three catalog
seeds and their unused switch branches. It does not enable or disable model tools,
change chat, or remove upload, gallery, camera or playback capabilities.

The actual welcome view tree now contains sixteen tool cards in eight complete
two-column rows, starting with file processing and screen capture. The first
QA-only candidate is superseded by the final three-card change.

The existing `AIAssistantMainNavigationTest` was extended against the real Panel
tree. QA absence first failed against the original implementation; image/video
absence then failed against the intermediate eighteen-card implementation. Final
offline JDK17 Gradle execution of the complete test class passed 3 tests with no
failures, errors or skips, including Debug Java compilation. Evidence is in
`.cache/android-welcome-no-qa-20261116/`; final patch SHA-256 is
`73dfadb2935e22524d77e7da15154adf69084128ec2a4ad1948960d44f4ddf6a`.

The first JBR21 environment attempt failed before tests in the existing AGP image
transform; no build configuration was changed. Verification used the already
cached portable JDK17 and Android SDK. No App, emulator or physical device was
started or installed, and no service or paid model was invoked. This is source and
JVM view-tree validation, not a device UI acceptance or an APK delivery.

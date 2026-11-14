# Android creation session drawer parity

## Reproduction and reference

On the normal Android 275 App, open the creation tab and drag inward from the left edge. The physical API 35 device navigated back to Messages rather than revealing sessions. The old session button opened an unrelated popup; it did not move the existing conversation. The iOS reference is AIAssistantView's shared reveal container, direction lock, 32-point edge admission and device-grouped session list.

Expected: reveal sessions and move the same header/message/composer page together, preserving draft and scroll state. Vertical scrolling must not open the drawer. Back while expanded closes it and stays in creation; Back while closed retains normal tab navigation. Devices show online state, session count, collapse and direct creation on that device.

## Change

Android now retains a stable content card and drawer in one container. Drawer width is min(360 dp, 78 percent), and the card scales to 95 percent at full reveal. Gesture direction and cancellation share a single progress state. A keyboard-only height change retains an active drag; a width/orientation change settles to an endpoint. Clicks are locked during tracking/settling.

The MainActivity change only delegates Back to an expanded/moving creation drawer after existing media/chat priorities. The left edge uses a limited central 200-dp system-gesture exclusion area and clears it when inactive; other system edges retain normal navigation. This follows Android's selective gesture-conflict mechanism, rather than disabling system Back globally. See the [official gesture navigation guidance](https://developer.android.com/develop/ui/views/touch-and-input/gestures/gesturenav).

Remote device grouping and pending selection/creation feedback reuse the existing same-account SessionSync Controller. This is distinct from the accepted-friend IM binding protocol. The remote image download/cache addition is documented separately in the accompanying remote-session-images record.

## Verification

The final current Java sources passed 44 tests: drawer/UI 14, remote-image policy/cache 8, remote Controller 7, attachment adapter 9 and the original history handoff 6. The original keyboard-height test and image sanitization cases were RED before the respective fixes. Normal arm64 APK 276 / 2.2.34 built successfully; signature, alignment, 26-library dependency closure, new JNI export, exact runtime and model gates passed.

Root installed 276 in place, without clearing/uninstalling the normal App or reading its database. On the actual PKD130 / Android 15 device, the original left-edge gesture now opened the drawer, moved the original page and showed the online remote device. Back closed the drawer while retaining the creation tab. The normal same-account remote conversation loaded real text, tool summaries and a Markdown table; the action menu showed its model/effort and owning device. Creating an empty remote session increased the source device count from one to two; deleting only that owned test session restored one. No prompt/model call was sent for this control check.

Evidence is retained under `.cache/android-creation-parity-20261115/normal276/`, including the physical screenshots, package and runtime receipts. Early attempts while a system installer was covering the App are excluded from gesture acceptance. Root observed the active App before the accepted runs.

Actual live remote-image display is a separate gate from the verified UI/control actions. Image parts with a valid temporary publication are supported by the new downloader. Historical images with no publication or expired publications without a retained copy remain source-device placeholders, as on iOS; the client does not silently scan and upload user history. Local fixtures and this distinction must not be reported as a live image row being accepted on the normal account.

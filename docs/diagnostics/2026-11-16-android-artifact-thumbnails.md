# Android delivered-document thumbnails: final candidate and validation

> Historical candidate evidence from 2026-11-16. The final reconciliation below
> supersedes the original unstaged/protected-WIP delivery notes; original test
> counts and runtime boundaries are retained as observations of that candidate.

2026-11-16, product HEAD 5d206ce6ec8c6e207a8812d183e2b48aa59a2adb plus the scoped candidate in candidate-manifest.json. Result: the existing Agent file Button/card shows real PDF/Markdown first-viewport pixels and retains its filename/size/caption. PDF click keeps the existing full paginated viewer. Markdown click uses the shared IM/creation full preview with explicit sharing; other file formats retain their prior chooser. Final evidence is **42 JVM/Robolectric cases, 0 failures/errors/skips**, instrumentation Java compilation success, and **7 actual Android29 native PDF tests, 0 failures** in an independent headless fixture. Normal MaiChat App UI/real-device/end-to-end model acceptance is not claimed.

Reproduce with owned fixtures: create a completed agent_send_file result for a two-page PDF and UTF-8 Markdown with headings/bold/list content and a far-tail sentinel, show the ordinary creation MessageRow, then click its file card. Expect a content thumbnail above the original metadata and a complete preview on click. The Markdown end sentinel remains available beyond the thumbnail. Its Share action remains available when the preview is too large/fails. Missing/unavailable files retain the previous unavailable-file state; a renderer/size/overload failure retains the ordinary card and its action.

The thumbnail pipeline uses two worker slots with an actual ArrayBlockingQueue capacity32. Rebinding cancels and removes the old queued FutureTask, keeping the latest pending request per binding; cancellation is also checked during snapshot copy and before publishing. When global capacity is exhausted, the ordinary card remains and a later bind can retry; I/O never runs on the caller as a saturation policy. No deferred unbounded thumbnail queue is added to MarkdownRenderer. Markdown thumbnails synchronously reuse its parser/spans/fonts on the bounded worker; a dedicated parser lock serializes shared parser access without holding the UI initialize monitor.

Owner safety: account generation, session/message/part/path, per-binding tokens, detach and away/back generation reject old results. Unchanged row rebinds retain visible pixels while validation happens. The8MiB image LRU keys the canonical source path, actual snapshot digest, owner and display/style identity. It is memory-only.

Immutable source identity: DocumentThumbnailSnapshot captures complete bounded Markdown bytes and computes their digest before rendering the same immutable text. PDF performs a bounded copy+digest into a random temporary file in the existing app cacheDir, opens a read-only PFD, immediately unlinks its name, then passes the verified snapshot to the ordinary PdfPreviewDialog.renderSnapshot core. This keeps rendering and digest tied to the same copied bytes even when the original inode is rewritten. PDF pages/renderer/descriptor and remaining temporary names close/delete in finally paths; no persistent thumbnail directory or disk thumbnail cache is introduced. The existing full PDF window/path behavior is unchanged.

Bounds: thumbnail Markdown parses the complete document up to256KiB before clipping a first viewport, avoiding cut fences/later-reference changes. Larger Markdown keeps its ordinary card/full preview. The shared full Markdown/text viewer has an explicit8MiB stat-and-stream limit and displays ??????????? above it, never a prefix called full content. UTF-8 replacement decoding and the original I/O error text are retained. PDF's prior100MiB limit is retained. Source delivery metadata's512MiB contract is unchanged.

The ordinary Markdown preview is shared by IM and creation. Its existing frame/size/close behavior is retained, with explicit ????. Sharing validates canonical allowedRoot on the worker and reuses ACTION_SEND, existing FileProvider content URI, matching ClipData and read-only URI permission. No write grant is added; the chooser is not advertised as guaranteed saving. Canonically out-of-root files are rejected even when FileProvider's broader app-cache root could address them. The card says ??????? for Markdown.

TDD evidence is retained under .cache/android-artifact-thumbnails-20261116: original red-integration/RED.xml proved missing document images, red-stable-rebind caught visible-image blanking, red-share rejected loss of sharing for normal/oversize Markdown, and red-snapshot-queue/RED-snapshot-queue.xml precisely demonstrated queue500 plus transientB pixels mislabeled under A after an A/B/A rewrite. Final green-snapshot-release.log freshly executed all42 cases and compiled product/instrumentation Java successfully. Tests include real Markdown pixels/full tail, stale owner and row recycling, replacement/cache, exact share URI/grants, root boundaries, UTF-8, read growth/cancel/size and prior file/Markdown/media/welcome/navigation regressions. FileProvider's static path-strategy cache is reset only by the Robolectric fixture because each test changes sandbox data roots; no production FileProvider configuration changes.

Command from MaiChat/android/MaiChat with cached JDK17 and existing SDK, offline:

`gradlew.bat --offline --no-daemon --init-script E:/OpenSource/MaiChat/.cache/android-artifact-thumbnails-20261116/pdf-test-jvm.gradle :app:testDebugUnitTest --tests com.kongshang.maichat.AIAssistantArtifactThumbnailTest --tests com.kongshang.maichat.MarkdownPreviewDialogTest --tests com.kongshang.maichat.AIAssistantFileDeliveryTest --tests com.kongshang.maichat.PdfPreviewDialogTest --tests com.kongshang.maichat.MarkdownRendererTest --tests com.kongshang.maichat.AIAssistantMediaCardLayoutTest --tests com.kongshang.maichat.AIAssistantMainNavigationTest :app:compileDebugAndroidTestJavaWithJavac`

The cache-only init script opens java.base/java.io for Robolectric FileDescriptor reflection. It changes no product build configuration. No model call/download, paid request, user document/database/credentials, normal App APK/native packaging, Qt/Ninja/CMake or host input occurred in this task.

Actual native evidence: .cache/android-artifact-native-20261116/native-snapshot-receipt.md and snapshot-native-receipt-lock.json. A single new private AOSP29/x86_64/WHPX AVD,1core/1024MiB/softwareGPU, ran -no-window via Start-Process Hidden. The separate applicationId com.kongshang.maichat.documentfixture contained byte-exact production PdfPreviewDialog, DocumentThumbnailSnapshot and the production PDF instrumentation test. Its only two unexecuted UI compile shells throw if called; the actual snapshot/PDF raster was never faked. Native7 repeats all original5 cases (page0 vs page1, first-viewport crop, repeated FD lifecycle, corruption, root/size bounds) plus original-inode rewrite after capture and canceled-copy temporary cleanup. The owned red/white first-viewport PNG was exported and visually inspected; offscreen green was clipped. Both runs' exact APKs, source locks, source copies, logs and PNGs are retained, including baseline-five/ with verified original APK/source SHA hashes. Only owned emulator5580 was stopped after each run; its userdata remains in this cache and existing AVD userdata was untouched. An unrelated real device appeared later and was not targeted by any command.

Boundary: this native evidence proves production static PDF snapshot/raster on an isolated Android29 emulator. It does not prove normal MaiChat App/account/Agent/LLM flow, full PDF/Markdown dialog UI, card scrolling/rotation/background behavior or real-device acceptance. Product APK delivery must exclude Protected MobileAgentAdapter WIP; no such APK was made or installed here. Protected WIP3 hashes remain byte exact. No welcome/tool registry/model/composer/directory-plan implementation changes were made. Work remains unstaged/uncommitted for Root review.

## Main reconciliation, 2026-11-17

The ten source/test files were rechecked against the frozen candidate manifest.
After Mac's `10fe0c64`, only MainActivity had an overlapping file change: its
video-orientation callback and this candidate's shared Markdown-preview entry
are separate hunks. A private three-way merge retains both. The other nine
files had no remote changes; the final iOS implementation was kept from Mac.

The merged source passed six focused JVM suites, 36 tests with no failures,
errors or skips. AndroidTest Java compilation first exposed two unrelated
stale cloud-limit references; switching those fixtures to native mediaInfo
made the same compile task pass in 9 seconds. The 36-case evidence was retained
without rerunning it. Logs/XML are under
`.cache/wip-reconcile-20261117-10fe0c64/android-validation/`.

The earlier 7-case native PDF snapshot evidence remains tied to its exact
source locks. The ordinary dual-ARM Debug APK from the CloudDrive integration
also compiled/packaged these pre-existing WIP sources; it was not a clean
release or a product UI acceptance run. No new App installation, live model
task, or claim about third-party/physical-device UI acceptance accompanies
this source reconciliation.

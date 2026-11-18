#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <functional>
#include <optional>

#include "MaiChatTools/DesktopComputerPlatform.h"

#include "MaiError.h"

class MaiToolRegistry;
class DesktopComputerBackend;

struct DesktopComputerObservation {
    std::string json;
    std::string imagePath;
};

struct DesktopComputerAction {
    std::string action;
    std::string snapshotId;
    std::uint64_t windowId = 0;
    std::uint64_t displayId = 0;
    bool desktopScope = false;
    // Required for display actions; a token from a different current workspace
    // cannot inject input or publish screen images into its former directory.
    std::string workspace;
    double xRatio = 0;
    double yRatio = 0;
    double toXRatio = 0;
    double toYRatio = 0;
    std::string text;
    std::string key;
    std::string expectedText;
    int scrollLines = 0;
    int durationMs = 200;
    DesktopMouseButton button = DesktopMouseButton::Left;
    DesktopScrollAxis scrollAxis = DesktopScrollAxis::Vertical;
    // Typed primitives share the same target, token and owner as legacy input.
    // Options are per call; native sessions never retain the cancel callback.
    std::vector<DesktopInputEvent> events;
    DesktopInputOptions inputOptions;
    // Stable task identity supplied by the host. Queued completion for an older
    // task must never release a newer task's holds in the same session.
    std::string turnId;
    bool dryRun = false;
    std::function<bool()> isCanceled;
    // Only click/double_click/right_click accept snapshot-local opaque IDs.
    // The ordinary API ignores ratios for this locator; tool JSON enforces XOR.
    std::string elementId;
};

struct DesktopComputerActionResult {
    std::string state;
    bool dispatched = false;
    bool verified = false;
    bool dryRun = false;
    std::string actualText;
    DesktopComputerObservation observation;
    std::string observationError;
    DesktopNativeInputState inputState;
    std::size_t acceptedEvents = 0;
    std::optional<DesktopNativeWindow> beforeWindow;
    std::optional<DesktopNativeWindow> afterWindow;
    std::optional<bool> focused;
};

// Attempt facts survive a failed Result; they do not assert receiver delivery
// or invent a completed-event count for a partially accepted native batch.
struct DesktopComputerActionProgress {
    bool inputDispatchAttempted = false;
    bool windowOperationAttempted = false;
    bool needsObservation = false;
};

// Session-owned observed window state. A later action must match the same
// window, title, geometry and snapshot token; the next observation verifies it.
class DesktopComputerController {
public:
    DesktopComputerController();
    explicit DesktopComputerController(std::shared_ptr<DesktopComputerBackend> backend);
    ~DesktopComputerController();
    DesktopComputerController(const DesktopComputerController&) = delete;
    DesktopComputerController& operator=(const DesktopComputerController&) = delete;

    // Cancellation is polled during observation and is never retained. A
    // minimized/cloaked target permits metadata only (includeImage=false).
    MaiResult<DesktopComputerObservation> observe(const std::string& sessionId, const std::string& windowTitle,
                                                  const std::string& workspace, bool includeElements,
                                                  std::uint64_t windowId = 0, bool includeImage = true,
                                                  std::function<bool()> isCanceled = {},
                                                  const std::string& turnId = {});
    MaiResult<std::vector<DesktopNativeWindow>> windows();
    MaiResult<std::vector<DesktopNativeDisplay>> displays();
    DesktopInputCapabilities capabilities() const;
    MaiResult<DesktopInputFeedbackBatch> feedback(const std::string& sessionId, std::uint64_t afterSequence = 0,
                                                  std::uint32_t limit = 64) const;
    // Observe a whole display using the resident capture worker. The token binds
    // this Session, input bounds and PNG dimensions. Desktop actions may switch
    // foreground applications and provide a subsequent screen observation.
    // elementWindowId optionally selects one native accessibility source; it
    // does not narrow or activate the display's existing native input scope.
    MaiResult<DesktopComputerObservation> observeDisplay(const std::string& sessionId, const std::string& workspace,
                                                         std::uint64_t displayId, std::function<bool()> isCanceled = {},
                                                         std::uint64_t afterFrameSequence = 0,
                                                         std::int64_t afterFrameReceivedAtMs = 0, int waitMs = 0,
                                                         const std::string& turnId = {},
                                                         std::uint64_t elementWindowId = 0);
    MaiResult<DesktopComputerActionResult> perform(const std::string& sessionId, const DesktopComputerAction& action,
                                                   DesktopComputerActionProgress* progress = nullptr);
    MaiError act(const std::string& sessionId, const std::string& action, const std::string& snapshotId,
                 std::uint64_t windowId, double xRatio, double yRatio, const std::string& text, const std::string& key,
                 int scrollLines);
    // Forget the selected window and snapshot; this never edits input text.
    MaiError clear(const std::string& sessionId);
    DesktopNativeInputState inputState(const std::string& sessionId) const;
    // Empty expectedTurn releases this ordinary caller's session state. Hosts
    // pass the actual completed/interrupted turn to protect a replacement task.
    MaiError releaseInput(const std::string& sessionId, const std::string& expectedTurn = {});
    // Reset owned controls while retaining the native context/configuration.
    // The same target and idle limit continue to apply. Busy while
    // another desktop operation is in flight; no reset occurs in that case.
    MaiError resetInput(const std::string& sessionId, const std::string& expectedTurn = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void registerDesktopComputerTools(MaiToolRegistry& registry, std::shared_ptr<DesktopComputerController> controller);

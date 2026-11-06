#pragma once

#include <memory>
#include <string>
#include <functional>

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
    double xRatio = 0;
    double yRatio = 0;
    double toXRatio = 0;
    double toYRatio = 0;
    std::string text;
    std::string key;
    std::string expectedText;
    int scrollLines = 0;
    int durationMs = 200;
    bool dryRun = false;
    std::function<bool()> isCanceled;
};

struct DesktopComputerActionResult {
    std::string state;
    bool dispatched = false;
    bool verified = false;
    bool dryRun = false;
    std::string actualText;
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

    MaiResult<DesktopComputerObservation> observe(const std::string& sessionId, const std::string& windowTitle,
                                                  const std::string& workspace, bool includeElements,
                                                  std::uint64_t windowId = 0, bool includeImage = true);
    MaiResult<std::vector<DesktopNativeWindow>> windows();
    MaiResult<DesktopComputerActionResult> perform(const std::string& sessionId, const DesktopComputerAction& action);
    MaiError act(const std::string& sessionId, const std::string& action, const std::string& snapshotId,
                 std::uint64_t windowId, double xRatio, double yRatio, const std::string& text, const std::string& key,
                 int scrollLines);
    // Forget the selected window and snapshot; this never edits input text.
    MaiError clear(const std::string& sessionId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void registerDesktopComputerTools(MaiToolRegistry& registry, std::shared_ptr<DesktopComputerController> controller);

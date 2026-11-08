#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiError.h"

class MaiToolRegistry;

// Owns an isolated, visible Chrome/Edge profile for each local Desktop Session.
// Ordinary App code and the Agent tool share these calls; no model-provided JS runs.
class DesktopBrowserManager {
public:
    DesktopBrowserManager();
    ~DesktopBrowserManager();
    DesktopBrowserManager(const DesktopBrowserManager&) = delete;
    DesktopBrowserManager& operator=(const DesktopBrowserManager&) = delete;

    // Network operations are blocking worker calls. Cancellation is checked during
    // HTTP/WS waits and is never retained after the call; callbacks must not throw.
    MaiError open(const std::string& sessionId, const std::string& url, std::function<bool()> isCanceled = {});
    MaiResult<std::string> observe(const std::string& sessionId, std::function<bool()> isCanceled = {});
    MaiError act(const std::string& sessionId, const std::string& action, const std::string& expectedUrl,
                 const std::string& snapshotId, const std::string& elementId, const std::string& expectedLabel,
                 const std::string& text, std::function<bool()> isCanceled = {});
    MaiError screenshot(const std::string& sessionId, const std::string& expectedUrl, const std::string& outputPath,
                        std::function<bool()> isCanceled = {});
    MaiError close(const std::string& sessionId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void registerDesktopBrowserTools(MaiToolRegistry& registry, std::shared_ptr<DesktopBrowserManager> manager);

// Ordinary image-file OCR and its read-only Agent tool share one local engine.
#pragma once

#include "MaiError.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class MaiToolRegistry;

struct DesktopOcrPoint {
    float x = 0;
    float y = 0;
};

struct DesktopOcrBlock {
    std::string text;
    float confidence = 0;
    std::array<DesktopOcrPoint, 4> quad{};
};

struct DesktopOcrResult {
    int imageWidth = 0;
    int imageHeight = 0;
    std::vector<DesktopOcrBlock> blocks;
};

struct DesktopOcrReadOptions {
    // Called synchronously on the calling thread, never retained. The callback
    // must not throw or recursively call the same service.
    std::function<bool()> isCanceled;
};

class DesktopOcrService final {
public:
    DesktopOcrService();
    ~DesktopOcrService();
    DesktopOcrService(const DesktopOcrService&) = delete;
    DesktopOcrService& operator=(const DesktopOcrService&) = delete;

    // absolutePath is a local UTF-8 path already authorized by the caller.
    // Reads at most 50 MiB / 12 MP, applies the codec's EXIF transform once and
    // keeps its full raster dimensions. Returned geometry is in those pixels;
    // it neither identifies a desktop snapshot nor establishes clickable UI.
    // Concurrent calls share a lazily initialized engine and run serially.
    MaiResult<DesktopOcrResult> recognizeImageFile(
        const std::string& absolutePath, const DesktopOcrReadOptions& options = {});

private:
    struct State;
    std::unique_ptr<State> mState;
};

// Supplying the ordinary service lets non-model callers reuse the same engine.
// The tool resolves every requested path through MaiToolContext before reading.
void registerDesktopOcrTools(MaiToolRegistry& registry,
                             std::shared_ptr<DesktopOcrService> service = {});

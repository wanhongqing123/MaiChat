// 文件作用：共用现有 ONNX Runtime 和 OpenCV 几何运行有界、可取消的本地 OCR。
#include "MaiOcr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <onnxruntime_c_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "MaiFilePath.h"
#include "MaiOcrInternal.h"

namespace {
constexpr size_t kMaxModelBytes = 64 * 1024 * 1024;
constexpr size_t kMaxDictionaryBytes = 1024 * 1024;
constexpr size_t kMaxClasses = 32768;
constexpr size_t kMaxSteps = 512;
constexpr size_t kMaxTextBytes = 8192;
constexpr size_t kMaxTotalTextBytes = 65536;
constexpr size_t kMaxCandidates = 512;
constexpr size_t kMaxRegions = 256;
constexpr int kMaxAxis = 16384;
constexpr uint64_t kMaxPixels = 16000000;
constexpr int kDetectorLongEdge = 960;
constexpr int kDetectorStride = 128;
constexpr int kMaxDetectorAxis = 1024;
constexpr int kRecognitionHeight = MAI_OCR_RECOGNITION_HEIGHT;
constexpr int kRecognitionMinWidth = 320;
constexpr int kRecognitionMaxWidth = MAI_OCR_MAX_RECOGNITION_WIDTH;
constexpr size_t kMaxLineFragments = 8;
constexpr size_t kMaxRecognitionCalls = 512;
constexpr int kFragmentTargetWidth = 1536;
constexpr int kBoundarySearchWidth = 192;
constexpr auto kRecognitionTimeLimit = std::chrono::seconds(30);

void copyError(char* destination, size_t capacity, const char* message) {
    if (destination && capacity) std::snprintf(destination, capacity, "%s", message);
}

bool isCanceled(const MaiOcrOptions* options) {
    return options && options->is_canceled && options->is_canceled(options->context);
}

// 字典条目允许多个 Unicode 标量（例如旗帜），但不接受非法 UTF-8 或 ASCII 控制字符。
bool isValidUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return false;
            continue;
        }
        int continuation = 0;
        uint32_t scalar = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
            scalar = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            scalar = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            scalar = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < static_cast<size_t>(continuation)) return false;
        while (continuation--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    }
    return !text.empty();
}

struct MaiOcrModelSession {
    OrtSession* value = nullptr;
    std::string inputName;
    std::string outputName;
};

struct MaiOcrTensor {
    const OrtApi* api = nullptr;
    OrtValue* value = nullptr;
    std::vector<int64_t> shape;
    size_t elements = 0;
    float* data = nullptr;
    ~MaiOcrTensor() {
        if (value) api->ReleaseValue(value);
    }
};

struct MaiOcrDetectedRegion {
    std::array<cv::Point2f, 4> quad;
};

struct MaiOcrRecognizedRegion {
    std::array<cv::Point2f, 4> quad;
    MaiOcrDecodedLine line;
};

std::array<MaiOcrPoint, 4> nativeQuad(const MaiOcrDetectedRegion& region) {
    std::array<MaiOcrPoint, 4> result;
    for (size_t i = 0; i < result.size(); ++i) result[i] = {region.quad[i].x, region.quad[i].y};
    return result;
}

MaiOcrDetectedRegion detectedQuad(const std::array<MaiOcrPoint, 4>& quad) {
    MaiOcrDetectedRegion result;
    for (size_t i = 0; i < quad.size(); ++i) result.quad[i] = {quad[i].x, quad[i].y};
    return result;
}

cv::Mat projectLine(const cv::Mat& bgr, const MaiOcrDetectedRegion& region, int width) {
    const std::array<cv::Point2f, 4> destination{
        cv::Point2f(0, 0), cv::Point2f(static_cast<float>(width - 1), 0),
        cv::Point2f(static_cast<float>(width - 1), kRecognitionHeight - 1),
        cv::Point2f(0, kRecognitionHeight - 1)};
    const cv::Mat transform = cv::getPerspectiveTransform(region.quad.data(), destination.data());
    if (!cv::checkRange(transform)) CV_Error(cv::Error::StsBadArg, "invalid OCR perspective transform");
    cv::Mat crop;
    cv::warpPerspective(bgr, crop, transform, cv::Size(width, kRecognitionHeight), cv::INTER_CUBIC,
                        cv::BORDER_REPLICATE);
    return crop;
}

// Only small windows around prospective boundaries are rectified. A complete
// over-wide crop is never allocated. Shared whitespace cuts avoid overlap text
// and ambiguous character stitching; an unsafe line stays explicitly partial.
bool planWideFragments(const cv::Mat& bgr, const MaiOcrLineLayout& layout, size_t availableCalls,
                       std::vector<MaiOcrDetectedRegion>& fragments) {
    const size_t count = static_cast<size_t>(std::ceil(layout.recognitionWidth / kFragmentTargetWidth));
    if (count > kMaxLineFragments || count > availableCalls) return false;
    std::vector<double> cuts{0};
    for (size_t i = 1; i < count; ++i) {
        const double nominal = static_cast<double>(i) / count;
        const double halfWindow = (kBoundarySearchWidth / 2.) / layout.recognitionWidth;
        const double begin = nominal - halfWindow;
        const double end = nominal + halfWindow;
        const cv::Mat crop = projectLine(bgr, detectedQuad(maiOcrSliceLine(layout, begin, end)),
                                        kBoundarySearchWidth);
        cv::Mat gray;
        cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
        const int best = maiOcrFindWhitespaceCut(gray.data, gray.cols, gray.rows, static_cast<int>(gray.step));
        if (best < 0) return false;
        cuts.push_back(begin + (end - begin) * best / (gray.cols - 1));
    }
    cuts.push_back(1);
    for (size_t i = 1; i < cuts.size(); ++i) {
        const auto quad = maiOcrSliceLine(layout, cuts[i - 1], cuts[i]);
        MaiOcrLineLayout fragment;
        if (maiOcrMeasureLine(quad, fragment) != MAI_OCR_OK || fragment.recognitionWidth > kRecognitionMaxWidth)
            return false;
        fragments.push_back(detectedQuad(quad));
    }
    return true;
}

// minAreaRect 的点序依角度变化；进入透视变换前统一为 TL、TR、BR、BL。
std::array<cv::Point2f, 4> orderedQuad(const cv::RotatedRect& rectangle) {
    std::array<cv::Point2f, 4> points;
    rectangle.points(points.data());
    std::sort(points.begin(), points.end(),
              [](const auto& a, const auto& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    const auto left = std::minmax(points[0], points[1], [](const auto& a, const auto& b) { return a.y < b.y; });
    const auto right = std::minmax(points[2], points[3], [](const auto& a, const auto& b) { return a.y < b.y; });
    return {left.first, right.first, right.second, left.second};
}

// Recognition reads a vertical slice from its right edge. Published quads keep
// the existing image-space TL/TR/BR/BL convention rather than that crop order.
std::array<cv::Point2f, 4> outputQuad(std::array<cv::Point2f, 4> points) {
    std::sort(points.begin(), points.end(),
              [](const auto& a, const auto& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    const auto left = std::minmax(points[0], points[1], [](const auto& a, const auto& b) { return a.y < b.y; });
    const auto right = std::minmax(points[2], points[3], [](const auto& a, const auto& b) { return a.y < b.y; });
    return {left.first, right.first, right.second, left.second};
}

float rectangleScore(const cv::Mat& probability, const std::array<cv::Point2f, 4>& quad) {
    std::vector<cv::Point> polygon;
    for (const auto& point : quad) polygon.emplace_back(cvRound(point.x), cvRound(point.y));
    const cv::Rect bounds = cv::boundingRect(polygon) & cv::Rect(0, 0, probability.cols, probability.rows);
    if (bounds.empty()) return 0;
    for (auto& point : polygon) point -= bounds.tl();
    cv::Mat mask = cv::Mat::zeros(bounds.size(), CV_8U);
    cv::fillConvexPoly(mask, polygon, cv::Scalar(1));
    return static_cast<float>(cv::mean(probability(bounds), mask)[0]);
}
}  // namespace

int maiOcrFindWhitespaceCut(const unsigned char* gray, int width, int height, int stride) {
    if (!gray || width < 2 || width > kBoundarySearchWidth || height < 8 ||
        height > kRecognitionHeight || stride < width) return -1;
    constexpr int tolerance = 12;
    const int margin = std::max(1, height / 12);
    const auto marginMedian = [&](int first) {
        std::array<int, 256> histogram{};
        for (int y = first; y < first + margin; ++y)
            for (int x = 0; x < width; ++x) ++histogram[gray[static_cast<size_t>(y) * stride + x]];
        int remaining = width * margin / 2;
        for (int value = 0; value < 256; ++value) {
            remaining -= histogram[value];
            if (remaining < 0) return value;
        }
        return 255;
    };
    const int top = marginMedian(0);
    const int bottom = marginMedian(height - margin);
    if (std::abs(top - bottom) > tolerance) return -1;
    const int background = (top + bottom) / 2;
    // Uniformity alone cannot distinguish a solid glyph stroke from a gap.
    // Both independent margins must mostly agree on one local background,
    // and the patch body must demonstrate contrasting ink. This is symmetric
    // for light/dark themes; textured, marginless or flat ambiguous patches
    // deliberately yield no safe cut instead of a confident partial glyph.
    for (const int first : {0, height - margin}) {
        int agreeing = 0;
        for (int y = first; y < first + margin; ++y)
            for (int x = 0; x < width; ++x)
                if (std::abs(static_cast<int>(gray[static_cast<size_t>(y) * stride + x]) - background) <= tolerance)
                    ++agreeing;
        if (agreeing * 5 < width * margin * 4) return -1;
    }
    int ink = 0;
    for (int y = margin; y < height - margin; ++y)
        for (int x = 0; x < width; ++x)
            if (std::abs(static_cast<int>(gray[static_cast<size_t>(y) * stride + x]) - background) >= 32) ++ink;
    if (ink < height) return -1;
    int best = -1;
    int runBegin = -1;
    for (int x = 0; x <= width; ++x) {
        int minimum = 255;
        int maximum = 0;
        if (x < width) {
            for (int y = 0; y < height; ++y) {
                const int value = gray[static_cast<size_t>(y) * stride + x];
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
        if (x < width && maximum - minimum <= 16 && minimum >= background - tolerance &&
            maximum <= background + tolerance) {
            if (runBegin < 0) runBegin = x;
            continue;
        }
        if (runBegin >= 0 && x - runBegin >= 2) {
            const int middle = (runBegin + x - 1) / 2;
            if (best < 0 || std::abs(middle - width / 2) < std::abs(best - width / 2)) best = middle;
        }
        runBegin = -1;
    }
    return best;
}

MaiOcrStatus maiOcrMeasureLine(const std::array<MaiOcrPoint, 4>& quad, MaiOcrLineLayout& layout) {
    layout = {};
    for (const auto& point : quad)
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return MAI_OCR_MODEL_ERROR;
    const auto length = [](const MaiOcrPoint& a, const MaiOcrPoint& b) {
        return std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y);
    };
    layout.quad = quad;
    layout.width = std::max(length(quad[0], quad[1]), length(quad[2], quad[3]));
    layout.height = std::max(length(quad[0], quad[3]), length(quad[1], quad[2]));
    if (layout.width < 1 || layout.height < 1) return MAI_OCR_MODEL_ERROR;
    if (layout.height >= layout.width * 1.5) {
        std::rotate(layout.quad.begin(), layout.quad.begin() + 1, layout.quad.end());
        std::swap(layout.width, layout.height);
    }
    layout.recognitionWidth = std::ceil(kRecognitionHeight * layout.width / layout.height);
    return std::isfinite(layout.recognitionWidth) ? MAI_OCR_OK : MAI_OCR_MODEL_ERROR;
}

std::array<MaiOcrPoint, 4> maiOcrSliceLine(const MaiOcrLineLayout& layout, double begin, double end) {
    const auto interpolate = [](const MaiOcrPoint& a, const MaiOcrPoint& b, double fraction) {
        return MaiOcrPoint{static_cast<float>(a.x + (b.x - a.x) * fraction),
                           static_cast<float>(a.y + (b.y - a.y) * fraction)};
    };
    return {interpolate(layout.quad[0], layout.quad[1], begin),
            interpolate(layout.quad[0], layout.quad[1], end),
            interpolate(layout.quad[3], layout.quad[2], end),
            interpolate(layout.quad[3], layout.quad[2], begin)};
}

struct MaiOcrEngine {
#if defined(_WIN32)
    HMODULE library = nullptr;
#else
    void* library = nullptr;
#endif
    const OrtApi* api = nullptr;
    OrtEnv* environment = nullptr;
    OrtSessionOptions* sessionOptions = nullptr;
    OrtMemoryInfo* memory = nullptr;
    OrtAllocator* allocator = nullptr;
    MaiOcrModelSession detection;
    MaiOcrModelSession recognition;
    std::vector<std::string> dictionary;
    MaiOcrStatus status = MAI_OCR_OK;
    std::string message;

    ~MaiOcrEngine() {
        if (api) {
            if (recognition.value) api->ReleaseSession(recognition.value);
            if (detection.value) api->ReleaseSession(detection.value);
            if (memory) api->ReleaseMemoryInfo(memory);
            if (sessionOptions) api->ReleaseSessionOptions(sessionOptions);
            if (environment) api->ReleaseEnv(environment);
        }
#if defined(_WIN32)
        if (library) FreeLibrary(library);
#else
        if (library) dlclose(library);
#endif
    }

    bool fail(MaiOcrStatus code, std::string detail) {
        status = code;
        message = std::move(detail);
        return false;
    }

    bool check(OrtStatus* failure, const char* action, MaiOcrStatus code = MAI_OCR_RUNTIME_ERROR) {
        if (!failure) return true;
        // 先释放 ORT 错误，再构造宿主字符串，分配失败时也不泄漏运行时对象。
        char detail[512]{};
        copyError(detail, sizeof(detail), api->GetErrorMessage(failure));
        api->ReleaseStatus(failure);
        return fail(code, std::string(action) + ": " + detail);
    }

    bool parseDictionary(const char* bytes, size_t size) {
        dictionary.emplace_back();
        std::unordered_set<std::string> seen;
        size_t begin = 0;
        while (begin < size) {
            size_t end = begin;
            while (end < size && bytes[end] != '\n') ++end;
            size_t length = end - begin;
            if (length && bytes[begin + length - 1] == '\r') --length;
            if (!length || length > 64) return fail(MAI_OCR_INVALID_INPUT, "invalid OCR dictionary entry size");
            std::string symbol(bytes + begin, length);
            if (!isValidUtf8(symbol) || symbol == " " || !seen.insert(symbol).second)
                return fail(MAI_OCR_INVALID_INPUT, "invalid OCR dictionary text or duplicate entry");
            dictionary.push_back(std::move(symbol));
            if (dictionary.size() >= kMaxClasses)
                return fail(MAI_OCR_LIMIT_EXCEEDED, "OCR dictionary class limit exceeded");
            begin = end < size ? end + 1 : size;
        }
        dictionary.emplace_back(" ");
        return dictionary.size() > 2 || fail(MAI_OCR_INVALID_INPUT, "empty OCR dictionary");
    }

    bool readShape(const OrtTensorTypeAndShapeInfo* info, std::vector<int64_t>& shape, size_t expectedRank) {
        ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        size_t rank = 0;
        if (!check(api->GetTensorElementType(info, &type), "read tensor type") ||
            !check(api->GetDimensionsCount(info, &rank), "read tensor rank"))
            return false;
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != expectedRank)
            return fail(MAI_OCR_MODEL_ERROR, "OCR model tensor type or rank mismatch");
        shape.resize(rank);
        return check(api->GetDimensions(info, shape.data(), shape.size()), "read tensor dimensions");
    }

    bool inspectSession(MaiOcrModelSession& model, bool isRecognition) {
        size_t inputs = 0;
        size_t outputs = 0;
        if (!check(api->SessionGetInputCount(model.value, &inputs), "read model inputs") ||
            !check(api->SessionGetOutputCount(model.value, &outputs), "read model outputs"))
            return false;
        if (inputs != 1 || outputs != 1)
            return fail(MAI_OCR_MODEL_ERROR, "OCR models must have one input and one output");
        for (int input = 0; input != 2; ++input) {
            char* name = nullptr;
            if (!check(input == 0 ? api->SessionGetInputName(model.value, 0, allocator, &name)
                                  : api->SessionGetOutputName(model.value, 0, allocator, &name),
                       "read model tensor name"))
                return false;
            auto freeName = [this](char* value) {
                if (value) allocator->Free(allocator, value);
            };
            std::unique_ptr<char, decltype(freeName)> ownedName(name, freeName);
            if (!name || !*name) return fail(MAI_OCR_MODEL_ERROR, "OCR model has an empty tensor name");
            (input == 0 ? model.inputName : model.outputName) = name;
            OrtTypeInfo* rawInfo = nullptr;
            if (!check(input == 0 ? api->SessionGetInputTypeInfo(model.value, 0, &rawInfo)
                                  : api->SessionGetOutputTypeInfo(model.value, 0, &rawInfo),
                       "read model tensor metadata"))
                return false;
            std::unique_ptr<OrtTypeInfo, decltype(api->ReleaseTypeInfo)> info(rawInfo, api->ReleaseTypeInfo);
            const OrtTensorTypeAndShapeInfo* tensor = nullptr;
            if (!check(api->CastTypeInfoToTensorInfo(info.get(), &tensor), "read tensor metadata")) return false;
            if (!tensor) return fail(MAI_OCR_MODEL_ERROR, "OCR model output is not a tensor");
            std::vector<int64_t> shape;
            if (!readShape(tensor, shape, input == 0 || !isRecognition ? 4 : 3)) return false;
            if (shape[0] != 1 && shape[0] != -1) return fail(MAI_OCR_MODEL_ERROR, "OCR model batch dimension mismatch");
            if (input == 0) {
                if (shape[1] != 3 || (isRecognition && shape[2] != kRecognitionHeight) ||
                    (!isRecognition && (shape[2] != -1 || shape[3] != -1)) || (isRecognition && shape[3] != -1))
                    return fail(MAI_OCR_MODEL_ERROR, "OCR model input geometry mismatch");
            } else if (isRecognition) {
                if (shape[2] != static_cast<int64_t>(dictionary.size()))
                    return fail(MAI_OCR_MODEL_ERROR, "OCR dictionary does not match model classes");
            } else if (shape[1] != 1 && shape[1] != -1) {
                return fail(MAI_OCR_MODEL_ERROR, "OCR detector output channel mismatch");
            }
        }
        return true;
    }

    bool initialize(const MaiOcrModelConfig& config) {
        if (!parseDictionary(config.dictionary_utf8, config.dictionary_size)) return false;
        const MaiFilePath runtime = MaiFilePath::fromUtf8(config.runtime_path);
        if (runtime.isEmpty() || !runtime.isAbsolute())
            return fail(MAI_OCR_NOT_CONFIGURED, "OCR runtime must have an absolute path");
#if defined(_WIN32)
        // 宿主指定的运行时只从绝对路径加载；其依赖仅允许同目录和 System32，
        // 不让工作目录或 PATH 中的同名 DLL 改变本地推理实现。
        library = LoadLibraryExW(runtime.value().c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        void* symbol = library ? reinterpret_cast<void*>(GetProcAddress(library, "OrtGetApiBase")) : nullptr;
#else
        library = dlopen(runtime.value().c_str(), RTLD_NOW | RTLD_LOCAL);
        void* symbol = library ? dlsym(library, "OrtGetApiBase") : nullptr;
#endif
        if (!symbol) return fail(MAI_OCR_NOT_CONFIGURED, "bundled ONNX Runtime is unavailable");
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        const OrtApiBase* base = reinterpret_cast<GetApiBase>(symbol)();
        if (!base || !base->GetVersionString() || std::strncmp(base->GetVersionString(), "1.26.", 5) != 0 ||
            !(api = base->GetApi(ORT_API_VERSION)))
            return fail(MAI_OCR_NOT_CONFIGURED, "OCR requires the bundled ONNX Runtime 1.26");
        if (!check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "MaiOcr", &environment), "create runtime") ||
            !check(api->CreateSessionOptions(&sessionOptions), "create session options") ||
            !check(api->SetIntraOpNumThreads(sessionOptions, 2), "bound runtime threads") ||
            !check(api->SetInterOpNumThreads(sessionOptions, 1), "bound graph threads") ||
            !check(api->SetSessionExecutionMode(sessionOptions, ORT_SEQUENTIAL), "set graph mode") ||
            !check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory),
                   "create CPU tensor memory") ||
            !check(api->GetAllocatorWithDefaultOptions(&allocator), "get runtime allocator"))
            return false;
        if (!check(api->CreateSessionFromArray(environment, config.detection_model, config.detection_model_size,
                                               sessionOptions, &detection.value),
                   "load OCR detection model", MAI_OCR_MODEL_ERROR) ||
            !check(api->CreateSessionFromArray(environment, config.recognition_model, config.recognition_model_size,
                                               sessionOptions, &recognition.value),
                   "load OCR recognition model", MAI_OCR_MODEL_ERROR))
            return false;
        return inspectSession(detection, false) && inspectSession(recognition, true);
    }

    bool run(MaiOcrModelSession& model, std::vector<float>& input, int width, int height, MaiOcrTensor& output,
             bool isRecognition);
    bool detect(const cv::Mat& bgr, const MaiOcrOptions* options, std::vector<MaiOcrDetectedRegion>& regions);
    bool recognizeRegion(const cv::Mat& bgr, const MaiOcrDetectedRegion& region, const MaiOcrOptions* options,
                         MaiOcrDecodedLine& line);
};

bool MaiOcrEngine::run(MaiOcrModelSession& model, std::vector<float>& input, int width, int height,
                       MaiOcrTensor& output, bool isRecognition) {
    const std::array<int64_t, 4> dimensions{1, 3, height, width};
    MaiOcrTensor source;
    source.api = api;
    output.api = api;
    if (!check(
            api->CreateTensorWithDataAsOrtValue(memory, input.data(), input.size() * sizeof(float), dimensions.data(),
                                                dimensions.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &source.value),
            "create OCR input"))
        return false;
    const OrtValue* inputs[] = {source.value};
    const char* inputNames[] = {model.inputName.c_str()};
    const char* outputNames[] = {model.outputName.c_str()};
    if (!check(api->Run(model.value, nullptr, inputNames, inputs, 1, outputNames, 1, &output.value),
               "run OCR inference"))
        return false;
    int isTensor = 0;
    if (!output.value || !check(api->IsTensor(output.value, &isTensor), "inspect OCR output"))
        return fail(MAI_OCR_MODEL_ERROR, "OCR model returned no tensor");
    if (!isTensor) return fail(MAI_OCR_MODEL_ERROR, "OCR output is not a tensor");
    OrtTensorTypeAndShapeInfo* rawInfo = nullptr;
    if (!check(api->GetTensorTypeAndShape(output.value, &rawInfo), "read OCR output shape")) return false;
    std::unique_ptr<OrtTensorTypeAndShapeInfo, decltype(api->ReleaseTensorTypeAndShapeInfo)> info(
        rawInfo, api->ReleaseTensorTypeAndShapeInfo);
    if (!readShape(info.get(), output.shape, isRecognition ? 3 : 4)) return false;
    size_t expected = 1;
    for (const int64_t dimension : output.shape) {
        if (dimension <= 0 || static_cast<uint64_t>(dimension) > std::numeric_limits<size_t>::max() / expected)
            return fail(MAI_OCR_MODEL_ERROR, "invalid OCR output tensor dimensions");
        expected *= static_cast<size_t>(dimension);
    }
    if (output.shape[0] != 1 ||
        (isRecognition && (output.shape[1] > static_cast<int64_t>(kMaxSteps) ||
                           output.shape[2] != static_cast<int64_t>(dictionary.size()))) ||
        (!isRecognition && (output.shape[1] != 1 || output.shape[2] != height || output.shape[3] != width)))
        return fail(MAI_OCR_MODEL_ERROR, "unexpected OCR output geometry or class count");
    if (!check(api->GetTensorShapeElementCount(info.get(), &output.elements), "read OCR output elements")) return false;
    const size_t maximum = isRecognition ? kMaxSteps * kMaxClasses : kMaxDetectorAxis * kMaxDetectorAxis;
    if (expected != output.elements || expected > maximum)
        return fail(MAI_OCR_MODEL_ERROR, "OCR output cardinality exceeds tensor budget");
    void* bytes = nullptr;
    if (!check(api->GetTensorMutableData(output.value, &bytes), "read OCR output data")) return false;
    output.data = static_cast<float*>(bytes);
    return output.data || fail(MAI_OCR_MODEL_ERROR, "OCR output data is missing");
}

bool MaiOcrEngine::detect(const cv::Mat& bgr, const MaiOcrOptions* options,
                          std::vector<MaiOcrDetectedRegion>& regions) {
    const double ratio = static_cast<double>(kDetectorLongEdge) / std::max(bgr.cols, bgr.rows);
    const auto aligned = [ratio](int axis) {
        const int resized = std::max(1, static_cast<int>(axis * ratio));
        return (resized + kDetectorStride - 1) / kDetectorStride * kDetectorStride;
    };
    const int width = aligned(bgr.cols);
    const int height = aligned(bgr.rows);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
    const size_t plane = static_cast<size_t>(width) * height;
    std::vector<float> normalized(plane * 3);
    constexpr std::array<float, 3> mean{.485f, .456f, .406f};
    constexpr std::array<float, 3> deviation{.229f, .224f, .225f};
    for (int y = 0; y < height; ++y) {
        const auto* row = resized.ptr<cv::Vec3b>(y);
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c)
                normalized[c * plane + static_cast<size_t>(y) * width + x] =
                    (row[x][c] / 255.f - mean[c]) / deviation[c];
    }
    if (isCanceled(options)) return fail(MAI_OCR_CANCELED, "OCR canceled");
    MaiOcrTensor output;
    if (!run(detection, normalized, width, height, output, false)) return false;
    if (isCanceled(options)) return fail(MAI_OCR_CANCELED, "OCR canceled");
    cv::Mat bitmap(height, width, CV_8U);
    for (int y = 0; y < height; ++y) {
        auto* row = bitmap.ptr<unsigned char>(y);
        for (int x = 0; x < width; ++x) {
            const float value = output.data[static_cast<size_t>(y) * width + x];
            if (!std::isfinite(value) || value < 0 || value > 1)
                return fail(MAI_OCR_MODEL_ERROR, "OCR detector returned invalid probability");
            row[x] = value > .3f ? 255 : 0;
        }
    }
    cv::Mat probability(height, width, CV_32F, output.data);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    if (contours.size() > kMaxCandidates) return fail(MAI_OCR_LIMIT_EXCEEDED, "OCR detection candidate limit exceeded");
    for (const auto& contour : contours) {
        if (isCanceled(options)) return fail(MAI_OCR_CANCELED, "OCR canceled");
        if (contour.size() < 3) continue;
        cv::RotatedRect rectangle = cv::minAreaRect(contour);
        if (std::min(rectangle.size.width, rectangle.size.height) < 3) continue;
        if (rectangleScore(probability, orderedQuad(rectangle)) < .6f) continue;
        // DB quad 路径先求最小矩形再扩张。圆角 offset 后再次求最小矩形的四个边界，
        // 恰为原矩形各边向外移动 area*1.5/perimeter；直接计算避免引入多边形库。
        const float area = rectangle.size.width * rectangle.size.height;
        const float perimeter = 2 * (rectangle.size.width + rectangle.size.height);
        const float distance = area * 1.5f / perimeter;
        if (!std::isfinite(distance) || distance <= 0 || distance > kMaxDetectorAxis)
            return fail(MAI_OCR_MODEL_ERROR, "invalid OCR polygon expansion");
        rectangle.size.width += 2 * distance;
        rectangle.size.height += 2 * distance;
        if (std::min(rectangle.size.width, rectangle.size.height) < 5) continue;
        MaiOcrDetectedRegion region{orderedQuad(rectangle)};
        for (auto& point : region.quad) {
            point.x = std::clamp(point.x * bgr.cols / width, 0.f, static_cast<float>(bgr.cols));
            point.y = std::clamp(point.y * bgr.rows / height, 0.f, static_cast<float>(bgr.rows));
        }
        if (cv::norm(region.quad[0] - region.quad[1]) < 2 || cv::norm(region.quad[0] - region.quad[3]) < 2) continue;
        regions.push_back(std::move(region));
    }
    return true;
}

bool MaiOcrEngine::recognizeRegion(const cv::Mat& bgr, const MaiOcrDetectedRegion& region, const MaiOcrOptions* options,
                                   MaiOcrDecodedLine& line) {
    MaiOcrLineLayout layout;
    if (maiOcrMeasureLine(nativeQuad(region), layout) != MAI_OCR_OK)
        return fail(MAI_OCR_MODEL_ERROR, "invalid OCR crop geometry");
    if (layout.recognitionWidth > kRecognitionMaxWidth) {
        char detail[256]{};
        std::snprintf(detail, sizeof(detail),
                      "OCR normalized text width %.0f exceeds %d at recognition height %d (quad %.3fx%.3f)",
                      layout.recognitionWidth, kRecognitionMaxWidth, kRecognitionHeight, layout.width, layout.height);
        return fail(MAI_OCR_LIMIT_EXCEEDED, detail);
    }
    const int resizedWidth = std::max(1, static_cast<int>(layout.recognitionWidth));
    const int paddedWidth = std::max(kRecognitionMinWidth, resizedWidth);
    // 直接投影到识别尺寸，避免先分配一个与源图同大的透视裁剪。
    const cv::Mat crop = projectLine(bgr, detectedQuad(layout.quad), resizedWidth);
    const size_t plane = static_cast<size_t>(paddedWidth) * kRecognitionHeight;
    std::vector<float> normalized(plane * 3, 0.f);
    for (int y = 0; y < kRecognitionHeight; ++y) {
        const auto* row = crop.ptr<cv::Vec3b>(y);
        for (int x = 0; x < resizedWidth; ++x)
            for (int c = 0; c < 3; ++c)
                normalized[c * plane + static_cast<size_t>(y) * paddedWidth + x] = (row[x][c] / 255.f - .5f) / .5f;
    }
    if (isCanceled(options)) return fail(MAI_OCR_CANCELED, "OCR canceled");
    MaiOcrTensor output;
    if (!run(recognition, normalized, paddedWidth, kRecognitionHeight, output, true)) return false;
    if (isCanceled(options)) return fail(MAI_OCR_CANCELED, "OCR canceled");
    const MaiOcrStatus decoded = maiOcrDecodeCtc(output.data, static_cast<size_t>(output.shape[1]),
                                                 static_cast<size_t>(output.shape[2]), dictionary, line);
    return decoded == MAI_OCR_OK || fail(decoded, "OCR CTC output violates probability or text limits");
}

MaiOcrStatus maiOcrDecodeCtc(const float* probabilities, size_t steps, size_t classes,
                             const std::vector<std::string>& dictionary, MaiOcrDecodedLine& output) {
    output = {};
    if (!probabilities || !steps || steps > kMaxSteps || classes < 2 || classes > kMaxClasses ||
        dictionary.size() != classes || !dictionary[0].empty())
        return MAI_OCR_MODEL_ERROR;
    MaiOcrDecodedLine line;
    double confidence = 0;
    size_t characters = 0;
    size_t previous = 0;
    for (size_t step = 0; step < steps; ++step) {
        size_t selected = 0;
        float best = -1;
        for (size_t index = 0; index < classes; ++index) {
            const float value = probabilities[step * classes + index];
            if (!std::isfinite(value) || value < 0 || value > 1) return MAI_OCR_MODEL_ERROR;
            if (value > best) {
                best = value;
                selected = index;
            }
        }
        if (selected && selected != previous) {
            const std::string& symbol = dictionary[selected];
            if (symbol.empty() || symbol.size() > kMaxTextBytes - line.text.size()) return MAI_OCR_LIMIT_EXCEEDED;
            line.text += symbol;
            confidence += best;
            ++characters;
        }
        previous = selected;
    }
    if (characters) line.confidence = static_cast<float>(confidence / characters);
    output = std::move(line);
    return MAI_OCR_OK;
}

MaiOcrStatus maiOcrCreate(const MaiOcrModelConfig* config, MaiOcrEngine** out_engine, char* error,
                          size_t error_capacity) {
    if (out_engine) *out_engine = nullptr;
    copyError(error, error_capacity, "");
    if (!config || !out_engine || !config->runtime_path || !*config->runtime_path || !config->detection_model ||
        !config->detection_model_size || !config->recognition_model || !config->recognition_model_size ||
        !config->dictionary_utf8 || !config->dictionary_size) {
        copyError(error, error_capacity, "missing OCR model configuration");
        return MAI_OCR_INVALID_INPUT;
    }
    if (config->detection_model_size > kMaxModelBytes || config->recognition_model_size > kMaxModelBytes ||
        config->dictionary_size > kMaxDictionaryBytes) {
        copyError(error, error_capacity, "OCR model or dictionary byte limit exceeded");
        return MAI_OCR_LIMIT_EXCEEDED;
    }
    try {
        auto engine = std::make_unique<MaiOcrEngine>();
        if (!engine->initialize(*config)) {
            copyError(error, error_capacity, engine->message.c_str());
            return engine->status;
        }
        *out_engine = engine.release();
        return MAI_OCR_OK;
    } catch (const std::bad_alloc&) {
        copyError(error, error_capacity, "OCR initialization allocation failed");
        return MAI_OCR_OUT_OF_MEMORY;
    } catch (const std::exception&) {
        copyError(error, error_capacity, "OCR initialization failed");
        return MAI_OCR_RUNTIME_ERROR;
    }
}

void maiOcrDestroy(MaiOcrEngine* engine) {
    delete engine;
}

MaiOcrStatus maiOcrRecognize(MaiOcrEngine* engine, const MaiOcrImage* image, const MaiOcrOptions* options,
                             MaiOcrResult* out_result, char* error, size_t error_capacity) {
    if (out_result) *out_result = {};
    copyError(error, error_capacity, "");
    if (!engine || !image || !out_result || !image->rgba || image->width <= 0 || image->height <= 0 ||
        image->stride <= 0) {
        copyError(error, error_capacity, "invalid OCR image or engine");
        return MAI_OCR_INVALID_INPUT;
    }
    if (image->width > kMaxAxis || image->height > kMaxAxis ||
        static_cast<uint64_t>(image->width) * image->height > kMaxPixels) {
        copyError(error, error_capacity, "OCR raster size limit exceeded");
        return MAI_OCR_LIMIT_EXCEEDED;
    }
    const size_t rowBytes = static_cast<size_t>(image->width) * 4;
    const size_t rowsBeforeLast = static_cast<size_t>(image->height - 1);
    if (static_cast<size_t>(image->stride) < rowBytes ||
        rowsBeforeLast > (std::numeric_limits<size_t>::max() - rowBytes) / static_cast<size_t>(image->stride) ||
        image->size < rowsBeforeLast * static_cast<size_t>(image->stride) + rowBytes) {
        copyError(error, error_capacity, "OCR RGBA buffer size or stride mismatch");
        return MAI_OCR_INVALID_INPUT;
    }
    MaiOcrResult owned{};
    try {
        engine->status = MAI_OCR_OK;
        engine->message.clear();
        if (isCanceled(options)) {
            copyError(error, error_capacity, "OCR canceled");
            return MAI_OCR_CANCELED;
        }
        const cv::Mat rgba(image->height, image->width, CV_8UC4, const_cast<unsigned char*>(image->rgba),
                           image->stride);
        cv::Mat bgr;
        cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
        std::vector<MaiOcrDetectedRegion> detected;
        if (!engine->detect(bgr, options, detected)) {
            copyError(error, error_capacity, engine->message.c_str());
            return engine->status;
        }
        std::sort(detected.begin(), detected.end(), [](const auto& a, const auto& b) {
            return a.quad[0].y != b.quad[0].y ? a.quad[0].y < b.quad[0].y : a.quad[0].x < b.quad[0].x;
        });
        std::vector<MaiOcrRecognizedRegion> recognized;
        size_t textBytes = 0;
        size_t recognitionCalls = 0;
        const auto recognitionStart = std::chrono::steady_clock::now();
        for (size_t index = 0; index < detected.size(); ++index) {
            if (isCanceled(options)) {
                copyError(error, error_capacity, "OCR canceled");
                return MAI_OCR_CANCELED;
            }
            if (std::chrono::steady_clock::now() - recognitionStart >= kRecognitionTimeLimit) {
                copyError(error, error_capacity, "OCR recognition time limit exceeded");
                return MAI_OCR_LIMIT_EXCEEDED;
            }
            const auto& region = detected[index];
            MaiOcrLineLayout layout;
            if (maiOcrMeasureLine(nativeQuad(region), layout) != MAI_OCR_OK) {
                copyError(error, error_capacity, "invalid OCR crop geometry");
                return MAI_OCR_MODEL_ERROR;
            }
            const bool wide = layout.recognitionWidth > kRecognitionMaxWidth;
            std::vector<MaiOcrDetectedRegion> fragments;
            if (wide) {
                // Reserve ordinary inference capacity for later independent
                // candidates. Only this specific recovery exhaustion is local;
                // model/CTC/result limits never become partial success.
                const size_t remaining = detected.size() - index - 1;
                const size_t available = kMaxRecognitionCalls - recognitionCalls - remaining;
                if (!planWideFragments(bgr, layout, available, fragments)) {
                    ++owned.skipped_lines;
                    continue;
                }
            } else {
                fragments.push_back(region);
            }
            bool incomplete = false;
            size_t lineCalls = 0;
            for (const auto& fragment : fragments) {
                if (isCanceled(options)) {
                    copyError(error, error_capacity, "OCR canceled");
                    return MAI_OCR_CANCELED;
                }
                if (recognitionCalls >= kMaxRecognitionCalls ||
                    std::chrono::steady_clock::now() - recognitionStart >= kRecognitionTimeLimit) {
                    copyError(error, error_capacity, "OCR recognition inference or time limit exceeded");
                    return MAI_OCR_LIMIT_EXCEEDED;
                }
                MaiOcrDecodedLine line;
                ++recognitionCalls;
                ++lineCalls;
                if (!engine->recognizeRegion(bgr, fragment, options, line)) {
                    copyError(error, error_capacity, engine->message.c_str());
                    return engine->status;
                }
                if (std::chrono::steady_clock::now() - recognitionStart >= kRecognitionTimeLimit) {
                    copyError(error, error_capacity, "OCR recognition time limit exceeded");
                    return MAI_OCR_LIMIT_EXCEEDED;
                }
                if (line.text.empty() || line.confidence < .5f) {
                    incomplete = wide;
                    continue;
                }
                if (recognized.size() >= kMaxRegions || line.text.size() > kMaxTotalTextBytes - textBytes) {
                    copyError(error, error_capacity, "OCR result count or text byte limit exceeded");
                    return MAI_OCR_LIMIT_EXCEEDED;
                }
                textBytes += line.text.size();
                recognized.push_back({wide ? outputQuad(fragment.quad) : fragment.quad, std::move(line)});
            }
            if (lineCalls >= 2) ++owned.segmented_lines;
            if (incomplete) ++owned.skipped_lines;
        }
        if (isCanceled(options)) {
            copyError(error, error_capacity, "OCR canceled");
            return MAI_OCR_CANCELED;
        }
        if (std::chrono::steady_clock::now() - recognitionStart >= kRecognitionTimeLimit) {
            copyError(error, error_capacity, "OCR recognition time limit exceeded");
            return MAI_OCR_LIMIT_EXCEEDED;
        }
        owned.width = image->width;
        owned.height = image->height;
        owned.partial = owned.skipped_lines != 0;
        if (!recognized.empty()) {
            owned.regions = static_cast<MaiOcrRegion*>(std::calloc(recognized.size(), sizeof(MaiOcrRegion)));
            if (!owned.regions) {
                copyError(error, error_capacity, "OCR result allocation failed");
                return MAI_OCR_OUT_OF_MEMORY;
            }
            owned.count = recognized.size();
            for (size_t i = 0; i < recognized.size(); ++i) {
                const auto& source = recognized[i];
                auto& destination = owned.regions[i];
                destination.text = static_cast<char*>(std::malloc(source.line.text.size() + 1));
                if (!destination.text) {
                    maiOcrResultFree(&owned);
                    copyError(error, error_capacity, "OCR text allocation failed");
                    return MAI_OCR_OUT_OF_MEMORY;
                }
                std::memcpy(destination.text, source.line.text.c_str(), source.line.text.size() + 1);
                destination.confidence = source.line.confidence;
                for (size_t point = 0; point < 4; ++point)
                    destination.quad[point] = {source.quad[point].x, source.quad[point].y};
            }
        }
        if (isCanceled(options)) {
            maiOcrResultFree(&owned);
            copyError(error, error_capacity, "OCR canceled");
            return MAI_OCR_CANCELED;
        }
        *out_result = owned;
        return MAI_OCR_OK;
    } catch (const std::bad_alloc&) {
        maiOcrResultFree(&owned);
        copyError(error, error_capacity, "OCR allocation failed");
        return MAI_OCR_OUT_OF_MEMORY;
    } catch (const cv::Exception&) {
        maiOcrResultFree(&owned);
        copyError(error, error_capacity, "OCR image geometry failed");
        return MAI_OCR_RUNTIME_ERROR;
    } catch (const std::exception&) {
        maiOcrResultFree(&owned);
        copyError(error, error_capacity, "OCR recognition failed");
        return MAI_OCR_RUNTIME_ERROR;
    }
}

void maiOcrResultFree(MaiOcrResult* result) {
    if (!result) return;
    for (size_t i = 0; i < result->count; ++i) std::free(result->regions[i].text);
    std::free(result->regions);
    *result = {};
}

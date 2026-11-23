// 文件作用：发送前把模型请求副本适配到请求体字节上限；按模型记住 HTTP 413 学到的上限。
#include "MaiRequestBodyFitter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMinTextCap = 512;
// 预览准备把每张图限制在 2 MB（见 makeMaiFfmpegImagePreview），base64 再放大 4/3。
constexpr std::uint64_t kMaxPreparedImageBytes = 2u * 1024u * 1024u;

std::mutex& limitsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::size_t>& learnedLimits() {
    static std::map<std::string, std::size_t> limits;
    return limits;
}

// 宿主工具观察（截图组、执行观察）以 user 角色出现，但不是用户本人的输入。
bool isHostObservation(const MaiModelMessage& message) {
    if (message.role != MaiModelRole::User || message.content.empty() ||
        message.content.front() != '{')
        return false;
    const auto parsed = Json::parse(message.content, nullptr, false);
    if (!parsed.is_object()) return false;
    const auto type = parsed.value("type", std::string{});
    return type.rfind("tool_", 0) == 0;
}

std::size_t utf8Floor(const std::string& text, std::size_t index) {
    index = std::min(index, text.size());
    while (index > 0 && index < text.size() &&
           (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80)
        --index;
    return index;
}

std::size_t utf8Ceil(const std::string& text, std::size_t index) {
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80)
        ++index;
    return index;
}

std::string locatorFor(const MaiModelMessage& message) {
    if (message.sourceMessageId.empty()) return {};
    std::string locator = " (source_message_id=" + message.sourceMessageId;
    if (!message.sourcePartId.empty()) locator += ", part_id=" + message.sourcePartId;
    return locator + ")";
}

// 保留首尾、中间换成明确说明；不足上限时原样返回。
std::string truncateMiddle(const std::string& text, std::size_t cap, const std::string& locator,
                           bool toolOutput) {
    if (text.size() <= cap) return text;
    const std::size_t head = utf8Floor(text, cap * 2 / 3);
    const std::size_t tailStart = utf8Ceil(text, text.size() - (cap - cap * 2 / 3));
    if (tailStart <= head) return text;
    const std::size_t omitted = tailStart - head;
    return text.substr(0, head) + "\n[" + std::to_string(omitted) +
           " bytes omitted from this request to fit the provider's request-size limit; the full "
           "text is kept in the conversation" +
           locator + ". Use " + (toolOutput ? "tool_output_read or session_history" : "session_history") +
           " to read it if needed.]\n" + text.substr(tailStart);
}

std::string replaceArguments(const std::string& arguments, std::size_t cap) {
    if (arguments.size() <= cap) return arguments;
    const std::size_t previewBytes = utf8Floor(arguments, std::min<std::size_t>(cap / 2, 256));
    return Json{{"omitted_for_request_size", true},
                {"original_bytes", arguments.size()},
                {"preview", arguments.substr(0, previewBytes)}}
        .dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string imagePlaceholder(const MaiModelImage& image) {
    return "[Image omitted from this request to fit the provider's request-size limit; the "
           "original file is kept at workspace_path=" +
           Json(image.path).dump(-1, ' ', false, Json::error_handler_t::replace) +
           ". Call view_image with that path if you need to look at it again.]";
}

std::uint64_t approximateImageBytes(const MaiModelRequest& request, const MaiModelImage& image) {
    std::uint64_t bytes = 0;
    auto path = MaiFilePath::fromUtf8(image.path);
    if (!path.isAbsolute() && !request.workingDirectory.empty())
        path = MaiFilePath::fromUtf8(request.workingDirectory).append(path);
    if (!MaiFileSystem::fileSize(path, bytes) || bytes == 0) bytes = kMaxPreparedImageBytes;
    bytes = std::min(bytes, kMaxPreparedImageBytes);
    return bytes / 3 * 4 + 4 + 64;
}

std::size_t approximateBodyBytes(const MaiModelRequest& request) {
    const auto text = [](std::size_t bytes) { return bytes + bytes / 16; };
    std::uint64_t total = 512 + text(request.baseInstructions.size());
    for (const auto& tool : request.tools)
        total += 64 + text(tool.name.size() + tool.description.size() + tool.parametersJson.size());
    for (const auto& message : request.messages) {
        total += 64 + text(message.content.size() + message.reasoning.size() +
                           message.toolCallId.size());
        for (const auto& call : message.invocations)
            total += 64 + text(call.id.size() + call.name.size() + call.arguments.size());
        if (message.continuation) total += 64 + text(message.continuation->payload.size());
        for (const auto& image : message.images) total += approximateImageBytes(request, image);
    }
    return total > std::numeric_limits<std::size_t>::max()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(total);
}

struct ImageRef {
    std::size_t message = 0;
    std::size_t image = 0;
};

struct FitPlan {
    std::size_t earlyImages = 0;  // 从 early 列表头部省略的数量
    bool dropContinuations = false;
    std::size_t textCap = std::numeric_limits<std::size_t>::max();
    std::size_t lateImages = 0;  // 从 late 列表头部省略的数量
    std::size_t currentCap = std::numeric_limits<std::size_t>::max();
};

class Fitter {
public:
    Fitter(MaiModelClient& model, const MaiModelRequest& request)
        : mModel(model), mRequest(request) {
        const auto& messages = mRequest.messages;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (messages[index].role == MaiModelRole::User && !isHostObservation(messages[index]))
                mCurrentUser = index;
            if (messages[index].role == MaiModelRole::Assistant) mLatestAssistant = index;
        }
        std::size_t newestObservation = npos;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (!messages[index].images.empty() && isHostObservation(messages[index]) &&
                (mCurrentUser == npos || index > mCurrentUser))
                newestObservation = index;
        }
        // early：当前输入之前的全部图片，然后是当前轮较早的工具截图（均由旧到新）。
        std::vector<ImageRef> earlier, turn;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index == mCurrentUser || index == newestObservation) continue;
            for (std::size_t image = 0; image < messages[index].images.size(); ++image) {
                if (mCurrentUser != npos && index > mCurrentUser)
                    turn.push_back({index, image});
                else
                    earlier.push_back({index, image});
            }
        }
        mEarly = earlier;
        mEarly.insert(mEarly.end(), turn.begin(), turn.end());
        // late：最新一组工具截图，然后是当前用户附件。
        if (newestObservation != npos)
            for (std::size_t image = 0; image < messages[newestObservation].images.size(); ++image)
                mLate.push_back({newestObservation, image});
        if (mCurrentUser != npos)
            for (std::size_t image = 0; image < messages[mCurrentUser].images.size(); ++image)
                mLate.push_back({mCurrentUser, image});
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto& message = messages[index];
            if (index != mLatestAssistant && message.continuation) mHasOldContinuation = true;
            if (message.role == MaiModelRole::System) continue;
            if (index == mCurrentUser) {
                mCurrentTextBytes = message.content.size();
                continue;
            }
            mMaxBlob = std::max(mMaxBlob, message.content.size());
            if (index != mLatestAssistant) mMaxBlob = std::max(mMaxBlob, message.reasoning.size());
            for (const auto& call : message.invocations)
                mMaxBlob = std::max(mMaxBlob, call.arguments.size());
        }
    }

    MaiModelRequest build(const FitPlan& plan, MaiRequestBodyFitResult* stats = nullptr) const {
        MaiModelRequest out = mRequest;
        std::vector<std::vector<bool>> omit(out.messages.size());
        for (std::size_t index = 0; index < out.messages.size(); ++index)
            omit[index].assign(out.messages[index].images.size(), false);
        for (std::size_t i = 0; i < plan.earlyImages && i < mEarly.size(); ++i)
            omit[mEarly[i].message][mEarly[i].image] = true;
        for (std::size_t i = 0; i < plan.lateImages && i < mLate.size(); ++i)
            omit[mLate[i].message][mLate[i].image] = true;
        std::size_t omittedImages = 0, truncated = 0, dropped = 0;
        for (std::size_t index = 0; index < out.messages.size(); ++index) {
            auto& message = out.messages[index];
            if (!message.images.empty()) {
                std::vector<MaiModelImage> kept;
                std::string notes;
                for (std::size_t image = 0; image < message.images.size(); ++image) {
                    if (!omit[index][image]) {
                        kept.push_back(message.images[image]);
                        continue;
                    }
                    ++omittedImages;
                    notes += "\n" + imagePlaceholder(message.images[image]);
                }
                message.images = std::move(kept);
                message.content += notes;
            }
            if (plan.dropContinuations && index != mLatestAssistant && message.continuation) {
                message.continuation.reset();
                ++dropped;
            }
            if (message.role == MaiModelRole::System) continue;
            const std::size_t cap = index == mCurrentUser ? plan.currentCap : plan.textCap;
            if (cap == std::numeric_limits<std::size_t>::max()) continue;
            const bool toolOutput = message.role == MaiModelRole::ToolResult;
            const auto before = message.content.size();
            message.content = truncateMiddle(message.content, cap, locatorFor(message), toolOutput);
            if (message.content.size() != before) ++truncated;
            if (index != mCurrentUser) {
                if (index != mLatestAssistant && message.reasoning.size() > cap) {
                    message.reasoning = truncateMiddle(message.reasoning, cap, {}, false);
                    ++truncated;
                }
                for (auto& call : message.invocations) {
                    if (call.arguments.size() <= cap) continue;
                    call.arguments = replaceArguments(call.arguments, cap);
                    ++truncated;
                }
            }
        }
        if (stats) {
            stats->omittedImages = omittedImages;
            stats->truncatedTexts = truncated;
            stats->droppedContinuations = dropped;
        }
        return out;
    }

    std::size_t measure(const FitPlan& plan) {
        bool measured = true;
        const auto bytes = maiMeasureRequestBody(mModel, build(plan), &measured);
        if (!measured) mMeasured = false;
        return bytes;
    }

    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
    MaiModelClient& mModel;
    const MaiModelRequest& mRequest;
    std::size_t mCurrentUser = npos;
    std::size_t mLatestAssistant = npos;
    std::vector<ImageRef> mEarly;
    std::vector<ImageRef> mLate;
    std::size_t mMaxBlob = 0;
    std::size_t mCurrentTextBytes = 0;
    bool mHasOldContinuation = false;
    bool mMeasured = true;
};

}  // namespace

std::size_t maiMeasureRequestBody(MaiModelClient& model, const MaiModelRequest& request,
                                  bool* measured) {
    const auto bytes = model.requestBodyBytes(request);
    if (bytes && bytes.value() != 0) {
        if (measured) *measured = true;
        return bytes.value();
    }
    // 测量失败（如本地传输没有编码器）不能当作 0 字节，也不能让适配失败：用保守近似。
    if (measured) *measured = false;
    return approximateBodyBytes(request);
}

MaiResult<MaiRequestBodyFitResult> maiFitRequestBody(MaiModelClient& model,
                                                     const MaiModelRequest& request,
                                                     std::size_t byteLimit,
                                                     const std::atomic<bool>& cancel) {
    MaiRequestBodyFitResult result;
    bool measured = true;
    result.originalBytes = maiMeasureRequestBody(model, request, &measured);
    result.measured = measured;
    if (byteLimit == 0 || result.originalBytes <= byteLimit) {
        result.request = request;
        result.fittedBytes = result.originalBytes;
        result.fits = true;
        return result;
    }
    Fitter fitter(model, request);
    FitPlan plan;
    const auto canceled = [&] { return cancel.load(std::memory_order_relaxed); };
    const auto fitsNow = [&](const FitPlan& candidate) {
        return fitter.measure(candidate) <= byteLimit;
    };
    // 在 [0, n] 中找最少的省略数量使请求适配；省略越多请求越小（单调）。
    const auto searchCount = [&](std::size_t FitPlan::*field, std::size_t n) -> bool {
        FitPlan full = plan;
        full.*field = n;
        if (!fitsNow(full)) {
            plan = full;
            return false;
        }
        std::size_t low = 0, high = n;
        while (low < high && !canceled()) {
            const std::size_t mid = low + (high - low) / 2;
            FitPlan trial = plan;
            trial.*field = mid;
            if (fitsNow(trial))
                high = mid;
            else
                low = mid + 1;
        }
        plan.*field = high;
        return true;
    };
    // 在 [kMinTextCap, maxBytes] 中找最大的统一截断上限使请求适配；上限越小请求越小。
    const auto searchCap = [&](std::size_t FitPlan::*field, std::size_t maxBytes) -> bool {
        if (maxBytes <= kMinTextCap) return false;
        FitPlan tightest = plan;
        tightest.*field = kMinTextCap;
        if (!fitsNow(tightest)) {
            plan = tightest;
            return false;
        }
        std::size_t low = kMinTextCap, high = maxBytes;
        while (low < high && !canceled()) {
            const std::size_t mid = low + (high - low + 1) / 2;
            FitPlan trial = plan;
            trial.*field = mid;
            if (fitsNow(trial))
                low = mid;
            else
                high = mid - 1;
        }
        plan.*field = low;
        return true;
    };

    bool done = false;
    if (!fitter.mEarly.empty()) done = searchCount(&FitPlan::earlyImages, fitter.mEarly.size());
    if (!done && !canceled() && fitter.mHasOldContinuation) {
        plan.dropContinuations = true;
        done = fitsNow(plan);
    }
    if (!done && !canceled()) done = searchCap(&FitPlan::textCap, fitter.mMaxBlob);
    if (!done && !canceled() && !fitter.mLate.empty())
        done = searchCount(&FitPlan::lateImages, fitter.mLate.size());
    if (!done && !canceled()) done = searchCap(&FitPlan::currentCap, fitter.mCurrentTextBytes);
    if (canceled()) return MaiError{MaiErrorCode::Canceled, "Request-body fitting was canceled."};

    result.request = fitter.build(plan, &result);
    result.fittedBytes = maiMeasureRequestBody(model, result.request, &measured);
    result.measured = result.measured && measured && fitter.mMeasured;
    result.fits = result.fittedBytes <= byteLimit;
    result.exhausted = !result.fits;
    result.reduced = result.omittedImages != 0 || result.truncatedTexts != 0 ||
                     result.droppedContinuations != 0;
    return result;
}

std::size_t maiLearnedRequestBodyLimit(const std::string& model) {
    std::lock_guard<std::mutex> lock(limitsMutex());
    const auto found = learnedLimits().find(model);
    return found == learnedLimits().end() ? 0 : found->second;
}

std::size_t maiParseStatedRequestBodyLimit(const std::string& providerMessage) {
    // 只接受紧跟在 max/maximum/limit 之后、单位为字节的数字，避免把“被拒 N 字节”误当上限。
    std::string lower = providerMessage;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t best = 0;
    for (const char* keyword : {"maximum", "max", "limit"}) {
        std::size_t at = 0;
        while ((at = lower.find(keyword, at)) != std::string::npos) {
            at += std::char_traits<char>::length(keyword);
            std::size_t digit = at;
            const std::size_t window = std::min(lower.size(), at + 40);
            while (digit < window && !std::isdigit(static_cast<unsigned char>(lower[digit])))
                ++digit;
            if (digit >= window) continue;
            std::size_t end = digit;
            std::uint64_t value = 0;
            while (end < lower.size() && std::isdigit(static_cast<unsigned char>(lower[end])) &&
                   value < (std::uint64_t{1} << 50))
                value = value * 10 + static_cast<std::uint64_t>(lower[end++] - '0');
            std::size_t unit = end;
            while (unit < lower.size() && lower[unit] == ' ') ++unit;
            if (lower.compare(unit, 4, "byte") != 0 || value < 1024) continue;
            if (best == 0 || value < best) best = static_cast<std::size_t>(value);
        }
    }
    return best;
}

std::size_t maiLearnRequestBodyRejection(const std::string& model, std::size_t rejectedBytes,
                                         const std::string& providerMessage,
                                         unsigned int keepPercent) {
    keepPercent = std::clamp(keepPercent, 1u, 99u);
    std::size_t proposed = rejectedBytes / 100 * keepPercent + (rejectedBytes % 100) * keepPercent / 100;
    const std::size_t stated = maiParseStatedRequestBodyLimit(providerMessage);
    if (stated != 0 && stated < rejectedBytes) proposed = stated - stated / 50;
    // 每次都严格变小，保证重复 413 时上限单调收敛，不会原地打转。
    proposed = std::max<std::size_t>(1, std::min(proposed, rejectedBytes - (rejectedBytes > 1)));
    std::lock_guard<std::mutex> lock(limitsMutex());
    auto& limit = learnedLimits()[model];
    if (limit == 0 || proposed < limit) limit = proposed;
    return limit;
}

void maiResetLearnedRequestBodyLimits() {
    std::lock_guard<std::mutex> lock(limitsMutex());
    learnedLimits().clear();
}

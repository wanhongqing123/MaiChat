// 文件作用：统一直接模型调用与 Agent 恢复的退避、等待和取消语义。
#include "MaiModelRetry.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <json.hpp>

#include "MaiModelClient.h"

bool MaiModelRetry::isTransient(long status, bool transportRetryable, std::string_view body) {
    if (status >= 400 && status < 500 && status != 408 && status != 429) return false;
    if (status == 429) {
        const auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (parsed.is_object()) {
            const auto error = parsed.value("error", nlohmann::json{});
            if (error.is_object()) {
                for (const char* key : {"code", "type"}) {
                    const auto found = error.find(key);
                    if (found != error.end() && found->is_string()) {
                        const auto value = found->get<std::string>();
                        if (value == "insufficient_quota" ||
                            value == "billing_hard_limit_reached" || value == "budget_exhausted")
                            return false;
                    }
                }
            }
        }
    }
    return transportRetryable || status == 408 || status == 429 || status >= 500;
}

bool MaiModelRetry::isContextLengthCode(std::string_view code) {
    return code == "context_length_exceeded" || code == "context_window_exceeded";
}
bool MaiModelRetry::isContextLengthError(std::string_view body) {
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_object()) return false;
    const auto error = parsed.value("error", nlohmann::json{});
    if (!error.is_object()) return false;
    const auto code = error.value("code", nlohmann::json{});
    return code.is_string() && isContextLengthCode(code.get_ref<const std::string&>());
}

bool MaiModelRetry::isTransientProviderCode(std::string_view code) {
    return code == "server_error" || code == "rate_limit_exceeded" || code == "overloaded_error";
}

MaiError MaiModelRetry::advise(MaiError error, const MaiModelConfig& config, bool sawOutput,
                               long retryAfterMs) {
    return error.withRetryAdvice(
        {std::max(0, config.maxRetries), config.retryInitialDelayMs, retryAfterMs, sawOutput});
}

long MaiModelRetry::delayMilliseconds(const MaiRetryAdvice& advice, int retryIndex) {
    long delay = std::clamp(advice.initialDelayMs, 0L, 60000L);
    for (int index = 0; index < retryIndex && delay > 0 && delay < 60000; ++index)
        delay = std::min(60000L, delay * 2);
    return std::max(delay, advice.retryAfterMs);
}

long MaiModelRetry::parseDelayMilliseconds(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return -1;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        long seconds = 0;
        for (char c : value) {
            if (seconds > (86400 - (c - '0')) / 10) return -1;
            seconds = seconds * 10 + (c - '0');
        }
        return seconds * 1000;
    }
    // 日期只接受标准 GMT 形式，避免日期解析器把符号/小数解释成相对日期。
    if (value.size() != 29 || value.substr(value.size() - 3) != "GMT") return -1;
    const std::string text(value);
    const auto deadline = curl_getdate(text.c_str(), nullptr);
    if (deadline < 0) return -1;
    const auto seconds = std::max(std::difftime(deadline, std::time(nullptr)), 0.0);
    return seconds <= 86400 ? static_cast<long>(seconds * 1000) : -1;
}

MaiRetryWaitResult MaiModelRetry::wait(long milliseconds, const std::atomic<bool>& cancel,
                                       const std::function<bool()>& hasPendingInput) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0L, milliseconds));
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return MaiRetryWaitResult::Canceled;
        if (hasPendingInput && hasPendingInput()) return MaiRetryWaitResult::InputPending;
        // 向上取整剩余毫秒，不能因截断小数而早于服务方截止时间返回 Ready。
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) return MaiRetryWaitResult::Ready;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(25, remaining)));
    }
}

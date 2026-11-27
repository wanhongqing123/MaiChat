// 文件作用：生成 rollout 的 UUIDv7、UTC 相对路径与毫秒时间文本。
#include "MaiRolloutPath.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <limits>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// 四位年份的格式边界比 UUIDv7 的 48 位时间边界更窄，先拒绝再做单位转换。
constexpr MaiMillis kLastTimestamp = 253402300799999LL;

std::mt19937_64& rolloutRandom() {
    // 沿用 MaiIdGenerator 的线程局部标准随机源，不引入共享状态或鉴权密钥路径。
    // 多个种子字避免把每线程引擎的初始状态压缩成单个 32 位值。
    static thread_local std::mt19937_64 generator = [] {
        std::random_device device;
        std::array<std::uint32_t, 8> values{};
        for (auto& value : values) value = device();
        std::seed_seq seed(values.begin(), values.end());
        return std::mt19937_64(seed);
    }();
    return generator;
}

std::string createUuid(MaiMillis timestamp) {
    std::array<std::uint8_t, 16> bytes{};
    auto encodedTime = static_cast<std::uint64_t>(timestamp);
    for (int index = 5; index >= 0; --index) {
        bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(encodedTime & 0xffu);
        encodedTime >>= 8;
    }
    std::uint64_t randomBits = 0;
    for (std::size_t index = 6; index < bytes.size(); ++index) {
        if ((index - 6) % 8 == 0) randomBits = rolloutRandom()();
        bytes[index] = static_cast<std::uint8_t>(randomBits & 0xffu);
        randomBits >>= 8;
    }
    // 按 RFC 9562 的位布局，和 Chromium base::Uuid 一样先写版本、变体，再转小写十六进制。
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fu) | 0x70u);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fu) | 0x80u);
    constexpr char kHex[] = "0123456789abcdef";
    std::string id;
    id.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) id.push_back('-');
        id.push_back(kHex[bytes[index] >> 4]);
        id.push_back(kHex[bytes[index] & 0x0fu]);
    }
    return id;
}

}  // namespace

MaiResult<std::string> MaiRolloutPath::formatTimestamp(MaiMillis timestamp) {
    if (timestamp < 0 || timestamp > kLastTimestamp) {
        return {MaiErrorCode::InvalidInput,
                "Rollout timestamp must be within UTC years 1970..9999"};
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
#if defined(_WIN32)
    // Windows 的 gmtime_s 只支持到 3000 年，FILETIME 的 UTC 转换可覆盖完整四位年份。
    // 输入范围已检查，毫秒转 100ns 和加 1601 年起点均不会溢出。
    const std::uint64_t ticks =
        static_cast<std::uint64_t>(timestamp) * 10000u + 116444736000000000ULL;
    const FILETIME fileTime{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME broken{};
    if (!::FileTimeToSystemTime(&fileTime, &broken)) {
        return {MaiErrorCode::Internal, "Rollout timestamp UTC conversion failed"};
    }
    year = broken.wYear;
    month = broken.wMonth;
    day = broken.wDay;
    hour = broken.wHour;
    minute = broken.wMinute;
    second = broken.wSecond;
#else
    const auto seconds = timestamp / 1000;
    if (static_cast<std::uint64_t>(seconds) >
        static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max())) {
        return {MaiErrorCode::Internal, "Rollout timestamp exceeds platform UTC conversion range"};
    }
    const auto nativeSeconds = static_cast<std::time_t>(seconds);
    std::tm broken{};
    // 与 MaiTime 和 Chromium 的 UTC 分支一致，使用独立缓冲，避免 gmtime 的共享静态状态。
    if (!::gmtime_r(&nativeSeconds, &broken)) {
        return {MaiErrorCode::Internal, "Rollout timestamp UTC conversion failed"};
    }
    year = broken.tm_year + 1900;
    month = broken.tm_mon + 1;
    day = broken.tm_mday;
    hour = broken.tm_hour;
    minute = broken.tm_min;
    second = broken.tm_sec;
#endif
    char text[25]{};
    const int length =
        std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", year, month, day,
                      hour, minute, second, static_cast<int>(timestamp % 1000));
    if (length != 24) {
        return {MaiErrorCode::Internal, "Rollout timestamp formatting failed"};
    }
    return std::string(text, 24);
}

MaiResult<MaiRolloutFileIdentity> MaiRolloutPath::create(MaiMillis timestamp) {
    const auto formatted = formatTimestamp(timestamp);
    if (!formatted) return formatted.error();
    const auto& text = formatted.value();
    std::string filenameTime = text;
    filenameTime[13] = '-';
    filenameTime[16] = '-';
    MaiRolloutFileIdentity identity;
    identity.id = createUuid(timestamp);
    identity.relativePath = MaiFilePath::fromUtf8("sessions")
                                .append(MaiFilePath::fromUtf8(text.substr(0, 4)))
                                .append(MaiFilePath::fromUtf8(text.substr(5, 2)))
                                .append(MaiFilePath::fromUtf8(text.substr(8, 2)))
                                .append(MaiFilePath::fromUtf8("rollout-" + filenameTime + "-" +
                                                              identity.id + ".jsonl"));
    return identity;
}

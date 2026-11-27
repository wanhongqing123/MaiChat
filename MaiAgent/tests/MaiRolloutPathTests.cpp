// 文件作用：验证 rollout 时间、相对文件路径与 UUIDv7 的对外契约。
#include "MaiRolloutPath.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "MaiThread.h"

namespace {

int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

// 由独立 UTC 日历样例给定，禁止用被测格式化器构造期望值。
constexpr MaiMillis kTimestamp = 1795185882657LL;

void testFixedUtcTimestamp() {
    const auto result = MaiRolloutPath::formatTimestamp(kTimestamp);
    CHECK(result.isOk());
    if (result) CHECK(result.value() == "2026-11-20T14:44:42.657Z");
    const auto epoch = MaiRolloutPath::formatTimestamp(0);
    CHECK(epoch.isOk());
    if (epoch) CHECK(epoch.value() == "1970-01-01T00:00:00.000Z");
}

void testRelativePathAndNativeParent() {
    const auto result = MaiRolloutPath::create(kTimestamp);
    CHECK(result.isOk());
    if (!result) return;
    const auto& identity = result.value();
    CHECK(!identity.relativePath.isAbsolute());
    CHECK(identity.relativePath.toGenericUtf8() ==
          "sessions/2026/11/20/rollout-2026-11-20T14-44-42.657Z-" + identity.id + ".jsonl");
    CHECK(identity.relativePath.baseName().toUtf8().find(':') == std::string::npos);
    for (const auto& component : identity.relativePath.components()) {
        CHECK(component != MaiFilePath::fromUtf8("..").value());
        CHECK(component != MaiFilePath::fromUtf8(".").value());
    }
    const auto parent = MaiFilePath::fromUtf8(u8"/root/\u6d4b\u8bd5/\u5bf9\u8bdd");
    const auto composed = parent.append(identity.relativePath);
    CHECK(parent.isParentOf(composed));
    CHECK(composed.toGenericUtf8() ==
          parent.toGenericUtf8() + "/" + identity.relativePath.toGenericUtf8());
}

void testUtcDayRollover() {
    const auto last = MaiRolloutPath::create(1795219199999LL);
    const auto next = MaiRolloutPath::create(1795219200000LL);
    CHECK(last.isOk());
    CHECK(next.isOk());
    if (last) {
        CHECK(last.value().relativePath.toGenericUtf8().find(
                  "sessions/2026/11/20/rollout-2026-11-20T23-59-59.999Z-") == 0);
    }
    if (next) {
        CHECK(next.value().relativePath.toGenericUtf8().find(
                  "sessions/2026/11/21/rollout-2026-11-21T00-00-00.000Z-") == 0);
    }
}

void testUuidVersionVariantAndTimestamp() {
    for (const MaiMillis timestamp : {MaiMillis{0}, kTimestamp, MaiMillis{253402300799999LL}}) {
        const auto result = MaiRolloutPath::create(timestamp);
        CHECK(result.isOk());
        if (!result) continue;
        const auto& id = result.value().id;
        CHECK(id.size() == 36);
        if (id.size() != 36) continue;
        CHECK(id[14] == '7');
        CHECK(std::string("89ab").find(id[19]) != std::string::npos);
        std::uint64_t decodedTimestamp = 0;
        std::size_t digitCount = 0;
        for (std::size_t index = 0; index < id.size(); ++index) {
            if (index == 8 || index == 13 || index == 18 || index == 23) {
                CHECK(id[index] == '-');
                continue;
            }
            const auto digit = std::string("0123456789abcdef").find(id[index]);
            CHECK(digit != std::string::npos);
            if (digitCount++ < 12) decodedTimestamp = decodedTimestamp * 16 + digit;
        }
        CHECK(decodedTimestamp == static_cast<std::uint64_t>(timestamp));
    }
}

void testConcurrentUniqueness() {
    constexpr std::size_t kThreadCount = 8;
    constexpr std::size_t kCountPerThread = 1024;
    std::vector<std::vector<std::string>> ids(kThreadCount);
    std::vector<std::thread> workers;
    std::atomic<int> errors{0};
    for (std::size_t thread = 0; thread < kThreadCount; ++thread) {
        workers.emplace_back([&, thread] {
            MaiThread::setCurrentName("mai-rollout-test");
            for (std::size_t index = 0; index < kCountPerThread; ++index) {
                const auto identity = MaiRolloutPath::create(kTimestamp);
                const auto text = MaiRolloutPath::formatTimestamp(kTimestamp);
                if (!identity || !text || text.value() != "2026-11-20T14:44:42.657Z") {
                    ++errors;
                    continue;
                }
                ids[thread].push_back(identity.value().id);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    CHECK(errors.load() == 0);
    std::set<std::string> unique;
    for (const auto& threadIds : ids) unique.insert(threadIds.begin(), threadIds.end());
    CHECK(unique.size() == kThreadCount * kCountPerThread);
}

void testInvalidTimestamp() {
    for (const MaiMillis timestamp :
         {MaiMillis{-1}, MaiMillis{253402300800000LL}, std::numeric_limits<MaiMillis>::max(),
          std::numeric_limits<MaiMillis>::min()}) {
        const auto identity = MaiRolloutPath::create(timestamp);
        const auto text = MaiRolloutPath::formatTimestamp(timestamp);
        CHECK(!identity.isOk());
        CHECK(!text.isOk());
        CHECK(identity.error().code() == MaiErrorCode::InvalidInput);
        CHECK(text.error().code() == MaiErrorCode::InvalidInput);
        CHECK(!identity.error().message().empty());
        CHECK(!text.error().message().empty());
    }
    const auto last = MaiRolloutPath::formatTimestamp(253402300799999LL);
    CHECK(last.isOk());
    if (last) CHECK(last.value() == "9999-12-31T23:59:59.999Z");
}

}  // namespace

int main() {
    testFixedUtcTimestamp();
    testRelativePathAndNativeParent();
    testUtcDayRollover();
    testUuidVersionVariantAndTimestamp();
    testConcurrentUniqueness();
    testInvalidTimestamp();
    std::printf("MaiRolloutPathTests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

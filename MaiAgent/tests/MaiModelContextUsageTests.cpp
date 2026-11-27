// 文件作用：以固定 Codex 算式验证实测窗口、缓存费用和缺失值边界。
#include "MaiModelContextUsage.h"
#include "MaiSessionRecordCodec.h"

#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* name) {
        if (!condition) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    };
    MaiModelUsage measured;
    measured.inputTokens = 15000;
    measured.outputTokens = 1000;
    measured.totalTokens = 16000;
    measured.cachedInputTokens = 14000;
    measured.totalSource = MaiUsageValueSource::Provider;
    measured.cachedInputSource = MaiUsageValueSource::Provider;
    MaiModelContextUsage usage;
    usage.latestSampling = maiTokenUsageSnapshot(measured);
    usage.windowTokens = 20000;
    usage.windowSource = MaiContextWindowSource::Configuration;
    usage.samplingValid = true;
    check(maiContextRemainingPercent(usage) == 50, "Codex last total with baseline 12000");
    check(maiNonCachedInput(usage.latestSampling).tokens == 1000, "noncached subtracts cache once");
    check(maiBlendedTotal(usage.latestSampling).tokens == 2000, "blended is noncached plus output");
    check(maiBlendedTotal(usage.latestSampling).source == MaiUsageValueSource::Derived,
          "arithmetic reports derived source");
    usage.thread.total = {999999, MaiUsageValueSource::Derived};
    usage.turn.total = {30000, MaiUsageValueSource::Derived};
    check(maiContextRemainingPercent(usage) == 50, "cumulative consumption never occupies window");
    usage.latestSampling.cachedInput.tokens = 99999;
    check(maiNonCachedInput(usage.latestSampling).tokens == 0, "cache larger than input clamps");
    check(maiContextRemainingPercent(usage) == 50, "cache never reduces window occupancy");
    usage.samplingValid = false;
    check(!maiContextRemainingPercent(usage), "stale source is unknown");
    usage.samplingValid = true;
    usage.windowTokens = 0;
    check(!maiContextRemainingPercent(usage), "unknown window never fakes zero percent");
    usage.windowTokens = 12000;
    check(maiContextRemainingPercent(usage) == 0, "known baseline sized window follows Codex");
    usage.windowTokens = 20000;
    usage.latestSampling.total.source = MaiUsageValueSource::Missing;
    check(!maiContextRemainingPercent(usage), "missing total never becomes zero occupancy");
    usage.latestSampling.total = {0, MaiUsageValueSource::Provider};
    check(maiContextRemainingPercent(usage) == 100, "provider zero is a real measurement");
    usage.latestSampling.total.tokens = 18000;
    check(maiContextRemainingPercent(usage, 0) == 10, "baseline is display configurable");
    usage.latestSampling.cachedInput.source = MaiUsageValueSource::Missing;
    check(maiBlendedTotal(usage.latestSampling).tokens == 16000 &&
              maiBlendedTotal(usage.latestSampling).source == MaiUsageValueSource::Derived,
          "missing cache falls back to zero only for arithmetic and remains derived");
    measured.inputTokens = std::numeric_limits<std::size_t>::max();
    measured.outputTokens = 1;
    measured.totalSource = MaiUsageValueSource::Missing;
    const auto overflow = maiTokenUsageSnapshot(measured);
    check(overflow.total.source == MaiUsageValueSource::Missing, "overflow cannot fabricate total");
    measured.inputTokens = 10;
    measured.outputTokens = 2;
    const auto derived = maiTokenUsageSnapshot(measured);
    check(derived.total.tokens == 12 && derived.total.source == MaiUsageValueSource::Derived,
          "custom client known input output derive actual total");
    const auto cumulative = maiAccumulateTokenUsage(derived, derived);
    check(cumulative.total.tokens == 24 && cumulative.total.source == MaiUsageValueSource::Derived,
          "two requests accumulate separately from window");
    usage.model = "fixture-model";
    usage.provider = "opaque-provider";
    usage.turnId = "fixture-turn";
    usage.responseId = "provider-response";
    usage.windowId = "fixture-window";
    usage.windowNumber = 4;
    usage.checkpointRevision = 7;
    usage.latestOperation = "compaction";
    const auto encoded = MaiSessionRecordCodec::encodeContextUsage(usage);
    check(static_cast<bool>(encoded), "typed usage codec encodes");
    const auto restored = MaiSessionRecordCodec::decodeContextUsage(encoded ? encoded.value() : "");
    check(static_cast<bool>(restored), "typed usage codec decodes");
    if (restored) {
        check(restored.value().latestSampling.total.tokens == 18000,
              "codec retains sampling total");
        check(restored.value().latestSampling.cachedInput.source == MaiUsageValueSource::Missing,
              "codec retains missing cache source");
        check(restored.value().model == usage.model &&
                  restored.value().provider == usage.provider &&
                  restored.value().turnId == usage.turnId &&
                  restored.value().responseId == usage.responseId &&
                  restored.value().windowId == usage.windowId &&
                  restored.value().windowNumber == 4 && restored.value().checkpointRevision == 7,
              "codec retains exact sampling origin");
        check(restored.value().turn.total.tokens == 30000 &&
                  restored.value().thread.total.tokens == 999999,
              "codec retains independent cumulative scopes");
        check(restored.value().latestOperation == "compaction" && restored.value().samplingValid,
              "summary scope does not replace sampling origin");
    }
    check(!MaiSessionRecordCodec::decodeContextUsage("{\"version\":999}"),
          "codec rejects invalid usage");
    if (encoded) {
        auto corrupt = encoded.value();
        const std::string known = "\"total_tokens_source\":\"provider\"";
        const auto offset = corrupt.find(known);
        check(offset != std::string::npos, "fixture locates the measured source field");
        if (offset != std::string::npos) {
            corrupt.replace(offset, known.size(), "\"total_tokens_source\":\"estimate\"");
            check(!MaiSessionRecordCodec::decodeContextUsage(corrupt),
                  "codec rejects estimates posing as actual usage");
        }
        corrupt = encoded.value();
        const std::string count = "\"total_tokens\":18000";
        const auto countOffset = corrupt.find(count);
        check(countOffset != std::string::npos, "fixture locates the actual total field");
        if (countOffset != std::string::npos) {
            corrupt.replace(countOffset, count.size(), "\"total_tokens\":-1");
            check(!MaiSessionRecordCodec::decodeContextUsage(corrupt),
                  "negative counts cannot wrap into unsigned occupancy");
        }
    }
    std::printf("Model context usage: %d failures\n", failures);
    return failures ? 1 : 0;
}

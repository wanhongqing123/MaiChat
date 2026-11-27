#include "agent/AgentContextUsagePresentation.h"

#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* description) {
        if (!passed) {
            ++failures;
            std::printf("FAIL %s\n", description);
        }
    };
    MaiModelContextUsage usage;
    usage.samplingValid = true;
    usage.windowTokens = 100000;
    usage.windowSource = MaiContextWindowSource::Configuration;
    usage.latestSampling.input = {39000, MaiUsageValueSource::Provider};
    usage.latestSampling.cachedInput = {35000, MaiUsageValueSource::Provider};
    usage.latestSampling.output = {1000, MaiUsageValueSource::Provider};
    usage.latestSampling.total = {40000, MaiUsageValueSource::Provider};
    usage.turn.total = {80000, MaiUsageValueSource::Derived};
    usage.thread.total = {3000000, MaiUsageValueSource::Derived};
    auto view = presentAgentContextUsage(usage);
    check(view.text.contains(QStringLiteral("68% left (40,000 used / 100,000)")),
          "latest total and Codex display baseline determine remaining percent");
    check(view.usedPercent == 32, "bar uses the same normalized remaining ratio");
    check(view.detail.contains(QStringLiteral("4,000")) && view.detail.contains(QStringLiteral("3,000,000")),
          "noncached input and recorded cumulative total are independently visible");
    check(view.detail.contains(QStringLiteral("思考：未提供")), "missing reasoning never masquerades as measured zero");
    usage.latestSampling.cachedInput.tokens = 38999;
    check(presentAgentContextUsage(usage).text == view.text, "cached input still occupies the context window");
    usage.latestSampling.total.source = MaiUsageValueSource::Derived;
    check(presentAgentContextUsage(usage).text.contains(QStringLiteral("推算")),
          "derived total is explicitly identified");
    usage.latestSampling.total.source = MaiUsageValueSource::Missing;
    view = presentAgentContextUsage(usage);
    check(!view.usedPercent && !view.text.contains(QLatin1Char('%')), "missing server total produces no fake ratio");
    usage.latestSampling.total = {40000, MaiUsageValueSource::Provider};
    usage.windowTokens = 0;
    view = presentAgentContextUsage(usage);
    check(!view.usedPercent && view.text.contains(QStringLiteral("窗口未知")),
          "unknown model window is not zero or a guessed common window");
    usage.windowTokens = 100000;
    usage.windowSource = MaiContextWindowSource::Missing;
    view = presentAgentContextUsage(usage);
    check(!view.usedPercent && view.text.contains(QStringLiteral("窗口未知")),
          "numeric window without provenance does not produce a blank or fabricated ratio");
    usage.windowSource = MaiContextWindowSource::Configuration;
    usage.samplingValid = false;
    view = presentAgentContextUsage(usage);
    check(!view.usedPercent && view.text.contains(QStringLiteral("等待")),
          "changed window or model invalidates old sampling occupancy");
    // 调用方已绘制上下文标签，等待状态拼入同一 header 后不能重复该前缀。
    const QString waitingHeader = QStringLiteral("上下文 · %1").arg(view.text);
    check(waitingHeader.count(QStringLiteral("上下文")) == 1,
          "waiting status does not repeat the caller's context header label");
    usage.samplingValid = true;
    usage.latestSampling.total = {0, MaiUsageValueSource::Provider};
    view = presentAgentContextUsage(usage);
    check(view.text.contains(QStringLiteral("100% left (0 used / 100,000)")),
          "measured zero remains distinguishable from missing data");
    std::printf("AgentContextUsagePresentationTest failures=%d\n", failures);
    return failures ? 1 : 0;
}

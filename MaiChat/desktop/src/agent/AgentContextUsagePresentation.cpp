#include "agent/AgentContextUsagePresentation.h"

#include <QLocale>
#include <QStringList>

namespace {
QString number(std::size_t value) {
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(qulonglong(value));
}

QString measured(const MaiTokenUsageCount& count) {
    if (count.source == MaiUsageValueSource::Missing) return QStringLiteral("未提供");
    return count.source == MaiUsageValueSource::Derived ? QStringLiteral("推算 %1").arg(number(count.tokens))
                                                        : number(count.tokens);
}
}  // namespace

AgentContextUsagePresentation presentAgentContextUsage(const MaiModelContextUsage& usage, std::size_t baselineTokens) {
    AgentContextUsagePresentation result;
    QStringList details;
    details << QStringLiteral("窗口占用取最近一次主请求的 total_tokens；缓存仍占窗口，累计消耗不作为窗口占用。");
    if (!usage.samplingValid) {
        result.text = QStringLiteral("上下文：等待新的服务端用量");
    } else if (usage.latestSampling.total.source == MaiUsageValueSource::Missing) {
        result.text = QStringLiteral("服务端未提供上下文用量");
    } else {
        const auto total = usage.latestSampling.total;
        const QString derived = total.source == MaiUsageValueSource::Derived ? QStringLiteral("推算 · ") : QString();
        if (!usage.windowTokens || usage.windowSource == MaiContextWindowSource::Missing) {
            result.text = derived + QStringLiteral("%1 used / 窗口未知").arg(number(total.tokens));
        } else if (usage.windowTokens <= baselineTokens) {
            // 小窗口无法容纳显示基线，不把 upstream 的零余量哨兵画成“已经用满”。
            result.text =
                derived + QStringLiteral("%1 used / %2").arg(number(total.tokens), number(usage.windowTokens));
            details << QStringLiteral("窗口不大于显示基线，未计算归一化剩余百分比。");
        } else if (const auto remaining = maiContextRemainingPercent(usage, baselineTokens)) {
            result.text = derived + QStringLiteral("%1% left (%2 used / %3)")
                                        .arg(*remaining)
                                        .arg(number(total.tokens), number(usage.windowTokens));
            result.usedPercent = 100 - *remaining;
        }
        details << QStringLiteral("最近主请求总量：%1 tokens。").arg(measured(total));
        details << QStringLiteral("输入：%1；缓存命中：%2；新输入：%3。")
                       .arg(measured(usage.latestSampling.input), measured(usage.latestSampling.cachedInput),
                            measured(maiNonCachedInput(usage.latestSampling)));
        details << QStringLiteral("缓存写入：%1；输出：%2；思考：%3。")
                       .arg(measured(usage.latestSampling.cacheWriteInput), measured(usage.latestSampling.output),
                            measured(usage.latestSampling.reasoningOutput));
        details << QStringLiteral("本次新输入加输出：%1 tokens；这是消费统计，不是窗口大小。")
                       .arg(measured(maiBlendedTotal(usage.latestSampling)));
        if (usage.latestSampling.cachedInput.source == MaiUsageValueSource::Missing)
            details << QStringLiteral("供应商未提供缓存明细，新输入计算按缓存 0 退化。");
    }
    if (usage.windowTokens && usage.windowSource != MaiContextWindowSource::Missing) {
        details << QStringLiteral("%1：%2 tokens（%3）；显示基线：%4。")
                       .arg(usage.samplingValid ? QStringLiteral("模型窗口")
                                                : QStringLiteral("上次测量窗口（当前已失效）"),
                            number(usage.windowTokens),
                            usage.windowSource == MaiContextWindowSource::Provider ? QStringLiteral("服务端")
                                                                                   : QStringLiteral("模型配置"),
                            number(baselineTokens));
    } else {
        details << QStringLiteral("模型窗口未知，未硬编码一个默认窗口或伪造百分比。");
    }
    details << QStringLiteral("本轮累计：%1；会话累计（已记录部分）：%2。")
                   .arg(measured(usage.turn.total), measured(usage.thread.total));
    if (usage.latestOperation == "compaction")
        details << QStringLiteral("最近一条记录为压缩请求；其消耗计入累计，不能覆盖主请求窗口占用。");
    result.detail = details.join(QLatin1Char('\n'));
    return result;
}

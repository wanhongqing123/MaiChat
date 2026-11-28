// 文件作用：在真实 Agent 工具循环中验证声明时序、隐藏工具拒绝和库默认行为。
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "MaiAgent.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"

static std::atomic<int> failures{0};
#define CHECK(value)                                       \
    do {                                                   \
        if (!(value)) {                                    \
            std::printf("FAIL %d %s\n", __LINE__, #value); \
            ++failures;                                    \
        }                                                  \
    } while (0)

namespace {
class MaiDiscoveryTestModel final : public MaiModelClient {
public:
    std::function<void(const MaiModelRequest&, const MaiStreamSink&, int)> respond;
    int requests = 0;
    bool expectSummary = false;
    mutable int measuredRequests = 0;
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
    MaiModelContextLimits contextLimits(const std::string&) const override {
        return {4096, 512};
    }
    MaiModelTokenEstimate estimateInputTokens(const MaiModelRequest& request) const override {
        ++measuredRequests;
        if (expectSummary)
            CHECK(request.baseInstructions.find("catalog_action") != std::string::npos);
        return maiEstimateModelInputTokens(request);
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        respond(request, sink, ++requests);
        return {};
    }
};
class MaiDiscoveryTestAction final : public MaiTool {
public:
    MaiDiscoveryTestAction(std::string toolName, std::atomic<int>& count)
        : mName(std::move(toolName)), mCount(count) {}
    std::string name() const override {
        return mName;
    }
    std::string description() const override {
        return "Test catalog action.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++mCount;
        return MaiToolResult::success("action executed");
    }

private:
    std::string mName;
    std::atomic<int>& mCount;
};
bool declared(const MaiModelRequest& request, const std::string& name) {
    return std::any_of(request.tools.begin(), request.tools.end(),
                       [&](const auto& tool) { return tool.name == name; });
}
void run(bool enabled) {
    auto model = std::make_unique<MaiDiscoveryTestModel>();
    model->expectSummary = enabled;
    auto* measuredModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    std::atomic<int> actions{0}, internals{0};
    tools->add(std::make_unique<MaiDiscoveryTestAction>("catalog_action", actions));
    tools->addInternal(std::make_unique<MaiDiscoveryTestAction>("internal_action", internals));
    std::string initialInstructions;
    std::string sessionId;
    auto store = makeMaiMemoryStore();
    MaiModelToolCatalog directCatalog(*store, *tools);
    model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int step) {
        CHECK(!declared(request, "internal_action"));
        if (enabled) {
            CHECK(request.baseInstructions.find("catalog_action") != std::string::npos);
            CHECK(request.baseInstructions.find("Test catalog action.") != std::string::npos);
            CHECK(request.baseInstructions.find("internal_action") == std::string::npos);
            CHECK(request.baseInstructions.find("next request") != std::string::npos);
            CHECK(request.baseInstructions.find("tool_search") != std::string::npos);
            const auto direct =
                directCatalog.capabilitySummary({sessionId, {}, request.model, {}, {}});
            CHECK(direct && request.baseInstructions == direct.value());
            auto withoutSummary = request;
            withoutSummary.baseInstructions.clear();
            CHECK(maiEstimateModelInputTokens(request).tokens >
                  maiEstimateModelInputTokens(withoutSummary).tokens);
            if (step == 1) {
                initialInstructions = request.baseInstructions;
                std::printf("Initial discovery request: model=%s tools=%zu summary_bytes=%zu\n%s\n",
                            request.model.c_str(), request.tools.size(),
                            request.baseInstructions.size(), request.baseInstructions.c_str());
            } else {
                CHECK(request.baseInstructions == initialInstructions);
            }
        }
        if (!enabled) {
            CHECK(declared(request, "catalog_action"));
            CHECK(!declared(request, "tool_search"));
            sink.onText("Default library behavior retained.");
        } else if (step == 1) {
            CHECK(declared(request, "tool_search"));
            CHECK(!declared(request, "catalog_action"));
            sink.onToolCall({"premature", "catalog_action", "{}"});
        } else if (step == 2) {
            CHECK(actions == 0);
            sink.onToolCall({"discover", "tool_search",
                             R"({"query":"catalog_action","select":["catalog_action"]})"});
            // 同一响应中即使搜索已经执行，后续未声明调用仍须拒绝，等待下次请求。
            sink.onToolCall({"same_batch", "catalog_action", "{}"});
            sink.onToolCall({"internal", "internal_action", "{}"});
        } else if (step == 3) {
            CHECK(actions == 0);
            CHECK(declared(request, "catalog_action"));
            sink.onToolCall({"allowed", "catalog_action", "{}"});
        } else {
            CHECK(actions == 1);
            sink.onText("Loaded action ran after declaration.");
        }
    };
    MaiAgent::Options options;
    options.toolDiscoveryEnabled = enabled;
    options.skillsEnabled = false;
    MaiAgent agent(std::move(store), std::move(model), std::move(tools), options);
    const auto created = agent.submit(MaiCreateSession{{}, "Tool discovery", "glm-5.4", {}});
    CHECK(created);
    if (!created) return;
    sessionId = created.value();
    CHECK(agent.submit(MaiSendPrompt{created.value(), "Exercise tool discovery."}));
    agent.waitIdle();
    CHECK(measuredModel->measuredRequests > 0);
    CHECK(internals == 0);
    CHECK(actions == (enabled ? 1 : 0));
    if (enabled) {
        MaiToolCatalogQuery query;
        query.query = "catalog_action";
        const auto page = agent.searchTools(created.value(), query);
        CHECK(page && page.value().tools.size() == 1 && page.value().tools[0].loaded);
        const auto second = agent.submit(MaiCreateSession{{}, "Other catalog", "glm-5.4", {}});
        CHECK(second);
        if (second) {
            const auto isolated = agent.searchTools(second.value(), query);
            CHECK(isolated && isolated.value().tools.size() == 1 &&
                  !isolated.value().tools[0].loaded);
        }
    }
}

// 用真实内置声明对比固定摘要开销；模型只返回文本，从不执行工具或调用供应商。
class MaiBuiltinWindowModel final : public MaiModelClient {
public:
    int streams = 0;
    mutable std::size_t largestInput = 0;
    mutable std::size_t summaryBytes = 0;
    mutable std::size_t schemaCount = 0;
    std::size_t outputTokens = 128;
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
    MaiModelContextLimits contextLimits(const std::string&) const override {
        // 同时设置输出上限和实际输入预算预留；第二字段本身不代表预留。
        return {4096, outputTokens, outputTokens};
    }
    MaiModelTokenEstimate estimateInputTokens(const MaiModelRequest& request) const override {
        const auto estimate = MaiModelClient::estimateInputTokens(request);
        largestInput = std::max(largestInput, estimate.tokens);
        summaryBytes = std::max(summaryBytes, request.baseInstructions.size());
        schemaCount = std::max(schemaCount, request.tools.size());
        return estimate;
    }
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        ++streams;
        sink.onText("Owned builtin-window test reply.");
        return {};
    }
};

class MaiHelpRoundTripModel final : public MaiModelClient {
public:
    std::size_t window = 4096;
    std::size_t reserve = 128;
    std::size_t sampledMaximum = 0;
    int requests = 0;
    int summaries = 0;
    bool readSkill = false;
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
    MaiModelContextLimits contextLimits(const std::string&) const override {
        return {window, reserve, reserve};
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        const auto input = estimateInputTokens(request).tokens;
        sampledMaximum = std::max(sampledMaximum, input);
        CHECK(input <= std::min(window * 95 / 100, window - reserve));
        if (request.tools.empty()) {
            ++summaries;
            sink.onText("Owned context summary; the task is to inspect builtin help.");
            return {};
        }
        const auto resultContains = [&](const char* text) {
            return std::any_of(request.messages.begin(), request.messages.end(),
                               [&](const auto& message) {
                                   return message.role == MaiModelRole::ToolResult &&
                                          message.content.find(text) != std::string::npos;
                               });
        };
        switch (++requests) {
            case 1:
                sink.onToolCall({"help_select", "tool_search",
                                 R"({"query":"tool_help","limit":1,"select":["tool_help"]})"});
                break;
            case 2:
                CHECK(declared(request, "tool_help"));
                sink.onToolCall({"help_groups", "tool_help", R"({"arguments":["-h"]})"});
                break;
            case 3:
                CHECK(resultContains("Builtin tool groups"));
                if (readSkill)
                    sink.onToolCall(
                        {"help_skill", "skill_read", R"({"skill":"builtin:builtin-media"})"});
                else
                    sink.onText("Owned grouped help round trip completed.");
                break;
            case 4:
                CHECK(readSkill && resultContains("# Local media workflows"));
                sink.onText("Owned help and bundled Skill round trip completed.");
                break;
            default:
                CHECK(false);
                sink.onText("Unexpected owned request.");
                break;
        }
        return {};
    }
};

void testBuiltinHelpRoundTrip(std::size_t window, std::size_t reserve, std::size_t baseBytes) {
    auto model = std::make_unique<MaiHelpRoundTripModel>();
    auto* observed = model.get();
    observed->window = window;
    observed->reserve = reserve;
    observed->readSkill = window >= 8192;
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);
    MaiAgent::Options options;
    options.toolDiscoveryEnabled = true;
    options.skillsEnabled = true;
    options.skillOptions.includeBuiltinSkills = true;
    options.skillOptions.homeDirectory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-owned-help-roundtrip-home"));
    options.baseInstructions = std::string(baseBytes, 'b');
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto created =
        agent.submit(MaiCreateSession{{}, "Builtin help round trip", "glm-5.4", {}});
    CHECK(created);
    if (!created) return;
    CHECK(agent.submit(MaiSendPrompt{created.value(), "Discover grouped builtin help."}));
    agent.waitIdle();
    const auto outcome = agent.getTurnOutcome(created.value());
    CHECK(outcome);
    if (!outcome) return;
    const bool completed = outcome.value().status == MaiTurnStatus::Completed;
    std::printf(
        "Builtin help roundtrip: window=%zu reserve=%zu base=%zu requests=%d summaries=%d "
        "sampled_max=%zu status=%s error_code=%s error=%s\n",
        window, reserve, baseBytes, observed->requests, observed->summaries,
        observed->sampledMaximum, maiTurnStatusToString(outcome.value().status),
        maiErrorCodeToString(outcome.value().error.code()),
        outcome.value().error.message().c_str());
    if (window >= 8192) CHECK(completed);
    if (completed)
        CHECK(observed->requests == (observed->readSkill ? 4 : 3));
    else {
        // Local capacity guards use InvalidInput; ContextWindowExceeded denotes
        // a provider-reported window error. Do not invent a different local code.
        // Platform registries differ (for example OpenSSL CLI versus typed
        // crypto tools), so the capacity guard can stop at either tool step.
        CHECK(window == 4096 && observed->requests >= 1 && observed->requests < 3);
        CHECK(outcome.value().status == MaiTurnStatus::Failed);
        CHECK(outcome.value().error.code() == MaiErrorCode::InvalidInput);
        const auto& message = outcome.value().error.message();
        CHECK(message.find("safe context budget") != std::string::npos ||
              message.find("protected recent tool observation exceed") != std::string::npos);
        CHECK(agent.listMessages(created.value()).size() >= 2);
    }
}

void testBuiltinSummaryFitsSmallWindow(bool baselineWithoutSummary, std::size_t outputTokens = 128,
                                       std::size_t baseInstructionBytes = 0,
                                       bool bundledSkills = false) {
    auto store = makeMaiMemoryStore();
    auto* observedStore = store.get();
    auto model = std::make_unique<MaiBuiltinWindowModel>();
    auto* observedModel = model.get();
    observedModel->outputTokens = outputTokens;
    auto tools = std::make_unique<MaiToolRegistry>();
    auto* observedTools = tools.get();
    registerMaiBuiltinTools(*tools);
    MaiAgent::Options options;
    options.toolDiscoveryEnabled = true;
    options.skillsEnabled = bundledSkills;
    options.skillOptions.includeBuiltinSkills = bundledSkills;
    options.skillOptions.homeDirectory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-owned-help-budget-home"));
    options.baseInstructions = std::string(baseInstructionBytes, 'b');
    std::atomic<std::size_t> inputBudget{0};
    MaiAgent agent(std::move(store), std::move(model), std::move(tools), options);
    const auto contextToken = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.contextStatus && event.contextStatus->inputBudgetTokens)
            inputBudget.store(event.contextStatus->inputBudgetTokens);
    });
    const auto created = agent.submit(MaiCreateSession{{}, "Builtin window", "glm-5.4", {}});
    CHECK(created);
    if (!created) return;
    if (baselineWithoutSummary) {
        // 独立元数据目录在新会话先保存空摘要，模拟旧请求的同一组声明，不改写产品代码。
        std::atomic<int> unusedActions{0};
        MaiToolRegistry bootstrapOnly;
        bootstrapOnly.add(std::make_unique<MaiDiscoveryTestAction>("tool_search", unusedActions));
        MaiModelToolCatalog baseline(*observedStore, bootstrapOnly);
        const auto summary = baseline.capabilitySummary({created.value(), {}, "glm-5.4", {}, {}});
        CHECK(summary && summary.value().empty());
        CHECK(unusedActions == 0);
    }
    CHECK(agent.submit(MaiSendPrompt{created.value(), "Reply with one short sentence."}));
    agent.waitIdle();
    MaiModelToolCatalog verification(*observedStore, *observedTools);
    const MaiTurnContext context{created.value(), {}, "glm-5.4", {}, {}};
    const auto summary = verification.capabilitySummary(context);
    CHECK(summary);
    std::size_t deferredNames = 0;
    if (!baselineWithoutSummary && summary) {
        CHECK(summary.value().size() <= 1536);
        CHECK(summary.value().find("omitted") == std::string::npos);
        const auto initial = verification.exposedSpecs(context);
        CHECK(initial);
        if (initial) {
            MaiModelRequest declaredRequest;
            declaredRequest.tools = initial.value();
            for (const auto& item : observedTools->metadata(context.model)) {
                if (declared(declaredRequest, item.name)) continue;
                ++deferredNames;
                CHECK(summary.value().find("- " + item.name + "\n") != std::string::npos ||
                      summary.value().find("- " + item.name + ": ") != std::string::npos);
            }
        }
    }
    const auto outcome = agent.getTurnOutcome(created.value());
    std::printf(
        "Builtin 4096-token window: baseline=%d output_limit=%zu reserve=%zu input_budget=%zu "
        "base_bytes=%zu streams=%d "
        "max_input_tokens=%zu summary_bytes=%zu instruction_bytes=%zu schemas=%zu "
        "deferred_names=%zu status=%s error=%s\n",
        baselineWithoutSummary, outputTokens, outputTokens, inputBudget.load(),
        baseInstructionBytes, observedModel->streams, observedModel->largestInput,
        summary ? summary.value().size() : 0, observedModel->summaryBytes,
        observedModel->schemaCount, deferredNames,
        outcome ? maiTurnStatusToString(outcome.value().status) : "missing",
        outcome ? outcome.value().error.message().c_str() : outcome.error().message().c_str());
    CHECK(observedModel->streams == 1);
    CHECK(outcome && outcome.value().status == MaiTurnStatus::Completed);
    CHECK(inputBudget == std::min<std::size_t>(4096 * 95 / 100, 4096 - outputTokens));
    agent.eventBus().unsubscribe(contextToken);
}
}  // namespace
int main() {
    testBuiltinHelpRoundTrip(4096, 128, 0);
    testBuiltinHelpRoundTrip(4096, 512, 550);
    testBuiltinHelpRoundTrip(8192, 512, 550);
    run(true);
    run(false);
    testBuiltinSummaryFitsSmallWindow(true);
    testBuiltinSummaryFitsSmallWindow(false);
    testBuiltinSummaryFitsSmallWindow(true, 512, 550);
    testBuiltinSummaryFitsSmallWindow(false, 512, 550);
    testBuiltinSummaryFitsSmallWindow(true, 128, 0, true);
    testBuiltinSummaryFitsSmallWindow(false, 128, 0, true);
    testBuiltinSummaryFitsSmallWindow(true, 512, 550, true);
    testBuiltinSummaryFitsSmallWindow(false, 512, 550, true);
    std::printf("MaiToolDiscoveryRuntimeTests: %d failures\n", failures.load());
    return failures ? 1 : 0;
}

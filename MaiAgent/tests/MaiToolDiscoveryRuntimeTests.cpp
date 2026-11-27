// 文件作用：在真实 Agent 工具循环中验证声明时序、隐藏工具拒绝和库默认行为。
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "MaiAgent.h"
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
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
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
    auto tools = std::make_unique<MaiToolRegistry>();
    std::atomic<int> actions{0}, internals{0};
    tools->add(std::make_unique<MaiDiscoveryTestAction>("catalog_action", actions));
    tools->addInternal(std::make_unique<MaiDiscoveryTestAction>("internal_action", internals));
    model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int step) {
        CHECK(!declared(request, "internal_action"));
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
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto created = agent.submit(MaiCreateSession{{}, "Tool discovery", "glm-5.4", {}});
    CHECK(created);
    if (!created) return;
    CHECK(agent.submit(MaiSendPrompt{created.value(), "Exercise tool discovery."}));
    agent.waitIdle();
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
}  // namespace
int main() {
    run(true);
    run(false);
    std::printf("MaiToolDiscoveryRuntimeTests: %d failures\n", failures.load());
    return failures ? 1 : 0;
}

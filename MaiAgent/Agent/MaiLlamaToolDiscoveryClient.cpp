// 文件作用：将工具声明延迟到真实模型选中后；不改历史、不执行用户操作、不提供脚本化答案。
#include "MaiLlamaToolDiscoveryClient.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiLlamaToolCatalog.h"

namespace {
MaiError canceledError() {
    return {MaiErrorCode::Canceled, "Local tool discovery was canceled"};
}

bool isCanceled(const std::atomic<bool>& cancel) {
    return cancel.load(std::memory_order_relaxed);
}

// 先检查完整批次，再向宿主交付。逐个检查并交付会让后续无效调用之前的操作先执行。
MaiError validateCalls(const std::vector<MaiToolInvocation>& calls,
                       const std::vector<MaiToolSpec>& advertised) {
    std::set<std::string> ids;
    for (const auto& call : calls) {
        if (call.id.empty() || !ids.insert(call.id).second ||
            !nlohmann::json::parse(call.arguments, nullptr, false).is_object() ||
            std::none_of(advertised.begin(), advertised.end(),
                         [&](const auto& tool) { return tool.name == call.name; }))
            return {MaiErrorCode::Protocol,
                    "Local model returned a call outside its advertised tools"};
    }
    return {};
}

class MaiLlamaToolDiscoveryClient final : public MaiModelClient {
public:
    explicit MaiLlamaToolDiscoveryClient(std::unique_ptr<MaiModelClient> client)
        : mClient(std::move(client)) {}

    MaiWireApi wireApi() const override {
        return MaiWireApi::Local;
    }

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (isCanceled(cancel)) return canceledError();
        if (!mClient) return {MaiErrorCode::NotConfigured, "Local inference client is unavailable"};
        if (mClient->wireApi() != MaiWireApi::Local)
            return {MaiErrorCode::NotSupported, "Tool discovery requires a local model client"};
        if (request.tools.empty()) return mClient->stream(request, sink, cancel);

        const MaiLlamaToolCatalog catalog(request.tools);
        if (catalog.error()) return catalog.error();
        // 目录延迟不会放宽原始请求的内存边界；所有原始声明和历史仍合计检查。
        constexpr std::size_t maximumBytes = 2 * 1024 * 1024;
        std::size_t bytes = 0;
        const auto fits = [&](const std::string& value) {
            if (value.size() > maximumBytes - bytes) return false;
            bytes += value.size();
            return true;
        };
        if (!fits(request.baseInstructions))
            return {MaiErrorCode::InvalidInput, "Complete local chat exceeds its byte budget"};
        for (const auto& message : request.messages) {
            if (!fits(message.content) || !fits(message.reasoning))
                return {MaiErrorCode::InvalidInput, "Complete local chat exceeds its byte budget"};
            for (const auto& call : message.invocations)
                if (!fits(call.id) || !fits(call.name) || !fits(call.arguments))
                    return {MaiErrorCode::InvalidInput,
                            "Complete local chat exceeds its byte budget"};
        }
        for (const auto& tool : request.tools)
            if (!fits(tool.name) || !fits(tool.description) || !fits(tool.parametersJson))
                return {MaiErrorCode::InvalidInput, "Complete local chat exceeds its byte budget"};
        // 原始请求保留在调用方；内部声明选择不追加 assistant/tool/user 合成历史。
        // 小模型会把 metadata 工具结果误当成用户任务的结果，重新使用原始请求避免该歧义。
        MaiModelRequest selection = request;
        selection.tools = {catalog.specification()};
        std::vector<MaiToolInvocation> calls;
        MaiStreamSink observed = sink;
        observed.onToolCall = [&](const MaiToolInvocation& call) { calls.push_back(call); };
        const auto selected = mClient->stream(selection, observed, cancel);
        if (isCanceled(cancel)) return canceledError();
        if (selected) return selected;
        if (calls.empty()) return {};
        if (calls.size() > 8)
            return {MaiErrorCode::Protocol, "Local model exceeded the tool discovery batch limit"};
        const auto validSelection = validateCalls(calls, selection.tools);
        if (validSelection) return validSelection;

        std::vector<MaiToolSpec> definitions;
        std::set<std::string> names;
        for (const auto& call : calls) {
            const auto resolved = catalog.resolve(call.arguments);
            if (!resolved) return resolved.error();
            for (const auto& definition : resolved.value()) {
                if (names.insert(definition.name).second) definitions.push_back(definition);
            }
        }
        if (isCanceled(cancel)) return canceledError();
        MaiModelRequest task = request;
        task.tools = std::move(definitions);
        calls.clear();
        const auto completed = mClient->stream(task, observed, cancel);
        if (isCanceled(cancel)) return canceledError();
        if (completed) return completed;
        const auto validActions = validateCalls(calls, task.tools);
        if (validActions) return validActions;
        // 这里仅交付已完成的真实模型调用；原 TurnRunner 继续负责审批、执行和结果回灌。
        for (const auto& call : calls) {
            if (isCanceled(cancel)) return canceledError();
            if (sink.onToolCall) sink.onToolCall(call);
        }
        return isCanceled(cancel) ? canceledError() : MaiError{};
    }

private:
    std::unique_ptr<MaiModelClient> mClient;
};
}  // namespace

std::unique_ptr<MaiModelClient> makeMaiLlamaToolDiscoveryClient(
    std::unique_ptr<MaiModelClient> client) {
    return std::make_unique<MaiLlamaToolDiscoveryClient>(std::move(client));
}

// 文件作用：本地推理仅适配中立的模型接口，不另建 Agent 或工具执行循环。
#include "MaiLlamaModelClient.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "MaiLlamaTools.h"

namespace {
class MaiLlamaModelClient final : public MaiModelClient {
public:
    MaiLlamaModelClient(MaiLlamaModelConfig config, std::shared_ptr<MaiLlamaRuntime> runtime)
        : mConfig(std::move(config)), mRuntime(std::move(runtime)) {}

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        auto config = mConfig;
        if (request.maxOutputTokens != 0) {
            const auto maximum =
                config.maximumTokens != 0 ? config.maximumTokens : config.contextSize - 1;
            config.maximumTokens =
                static_cast<std::uint32_t>(std::min<std::size_t>(maximum, request.maxOutputTokens));
        }
        return mRuntime->streamChat(config, request, sink, cancel);
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::Local;
    }

    MaiModelContextLimits contextLimits(const std::string&) const override {
        return {mConfig.contextSize,
                mConfig.maximumTokens != 0 ? mConfig.maximumTokens : mConfig.contextSize - 1,
                mConfig.maximumTokens != 0 ? mConfig.maximumTokens : mConfig.contextSize / 8};
    }

private:
    MaiLlamaModelConfig mConfig;
    std::shared_ptr<MaiLlamaRuntime> mRuntime;
};
}  // namespace

std::unique_ptr<MaiModelClient> makeMaiLlamaModelClient(MaiLlamaModelConfig config,
                                                        std::shared_ptr<MaiLlamaRuntime> runtime) {
    if (!runtime) runtime = getSharedMaiLlamaRuntime();
    // 与其他主模型共用完整声明和同一执行循环，不另加只看工具名的预选推理。
    return std::make_unique<MaiLlamaModelClient>(std::move(config), std::move(runtime));
}

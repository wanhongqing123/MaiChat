// 文件作用：本地推理仅适配中立的模型接口，不另建 Agent 或工具执行循环。
#include "MaiLlamaModelClient.h"

#include <utility>

#include "MaiLlamaTools.h"

namespace {
class MaiLlamaModelClient final : public MaiModelClient {
public:
    MaiLlamaModelClient(MaiLlamaModelConfig config, std::shared_ptr<MaiLlamaRuntime> runtime)
        : mConfig(std::move(config)), mRuntime(std::move(runtime)) {}

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        return mRuntime->streamChat(mConfig, request, sink, cancel);
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::Local;
    }

private:
    MaiLlamaModelConfig mConfig;
    std::shared_ptr<MaiLlamaRuntime> mRuntime;
};
}  // namespace

std::unique_ptr<MaiModelClient> makeMaiLlamaModelClient(MaiLlamaModelConfig config,
                                                        std::shared_ptr<MaiLlamaRuntime> runtime) {
    if (!runtime) runtime = getSharedMaiLlamaRuntime();
    return std::make_unique<MaiLlamaModelClient>(std::move(config), std::move(runtime));
}

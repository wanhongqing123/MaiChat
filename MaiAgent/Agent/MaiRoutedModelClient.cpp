// 文件作用：把不可变型号目录和每请求凭据回调组合成共享模型客户端。
#include "MaiRoutedModelClient.h"

#include <algorithm>
#include <set>
#include <mutex>
#include <map>
#include <utility>

#include "MaiOpenAiClient.h"

namespace {
class RoutedModelClient final : public MaiModelClient {
public:
    explicit RoutedModelClient(MaiRoutedModelConfig config) : mConfig(std::move(config)) {
        std::set<std::string> models;
        bool first = true;
        if (mConfig.routes.empty()) reject("The model route directory is empty.");
        for (const auto& route : mConfig.routes) {
            if (route.modelNames.empty() || route.config.wire == MaiWireApi::Mixed ||
                (route.config.wire == MaiWireApi::Local && !route.clientFactory))
                reject("A model route has no models or a usable transport.");
            for (const auto& model : route.modelNames) {
                if (model.empty() || !models.insert(model).second)
                    reject("Model routes contain an empty or duplicate model.");
            }
            if (first) {
                mWire = route.config.wire;
                first = false;
            } else if (mWire != route.config.wire) {
                mWire = MaiWireApi::Mixed;
            }
        }
    }

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (cancel.load(std::memory_order_relaxed))
            return {MaiErrorCode::Canceled, "Model request canceled."};
        if (mError) return mError;
        const auto* route = findRoute(request.model);
        if (!route)
            return {MaiErrorCode::InvalidInput,
                    "The selected model is not in the route directory."};
        // 只读取已选供应商的凭据，不尝试其余路由；失败不能改变模型、会话或供应商。
        MaiModelConfig config = route->config;
        std::shared_ptr<MaiModelClient> client;
        try {
            if (route->clientFactory) {
                client = route->clientFactory();
            } else {
                if (route->apiKeyProvider) config.apiKey = route->apiKeyProvider();
                if (config.apiKey.empty() || config.apiKey.size() > 8192 ||
                    config.apiKey.find_first_of("\r\n") != std::string::npos ||
                    config.apiKey.find('\0') != std::string::npos) {
                    std::lock_guard<std::mutex> lock(mClientsMutex);
                    mClients.erase(route);
                    return {MaiErrorCode::NotConfigured,
                            "The selected provider credential is unavailable."};
                }
                // 凭据每次仍由宿主读取；同一路由同凭据复用客户端及连接池。
                // 新凭据/退出登录丢弃旧缓存，已在途请求仅持有自己的 shared_ptr。
                std::lock_guard<std::mutex> lock(mClientsMutex);
                auto& cached = mClients[route];
                if (!cached.client || cached.credential != config.apiKey) {
                    const auto credential = config.apiKey;
                    auto replacement = makeMaiModelClient(std::move(config));
                    // 构造成功后才一起替换，异常不能把旧客户端标记成新凭据。
                    cached.credential = credential;
                    cached.client = std::move(replacement);
                }
                client = cached.client;
            }
        } catch (...) {
            return {MaiErrorCode::NotConfigured, "The selected provider could not be configured."};
        }
        if (!client)
            return {MaiErrorCode::NotConfigured, "The selected model client is unavailable."};
        const auto wire = client->wireApiForModel(request.model);
        if (!wire || *wire != route->config.wire)
            return {MaiErrorCode::InvalidInput,
                    "The selected model transport does not match its route."};
        return client->stream(request, sink, cancel);
    }

    MaiWireApi wireApi() const override {
        return mWire;
    }

    MaiResult<std::size_t> requestBodyBytes(const MaiModelRequest& request) override {
        const auto* route = mError ? nullptr : findRoute(request.model);
        if (!route) return std::size_t{0};
        std::shared_ptr<MaiModelClient> client;
        {
            std::lock_guard<std::mutex> lock(mClientsMutex);
            const auto found = mClients.find(route);
            if (found != mClients.end()) client = found->second.client;
        }
        // HTTP 413 后只复用刚才实际使用的传输；不额外取凭据、切换供应商或启动模型。
        return client ? client->requestBodyBytes(request) : MaiResult<std::size_t>{std::size_t{0}};
    }

    std::optional<MaiWireApi> wireApiForModel(const std::string& model) const override {
        const auto* route = mError ? nullptr : findRoute(model);
        return route ? std::optional<MaiWireApi>(route->config.wire) : std::nullopt;
    }

    MaiModelContextLimits contextLimits(const std::string& model) const override {
        const auto* route = mError ? nullptr : findRoute(model);
        if (!route) return {};
        return route->config.contextLimits
                   ? route->config.contextLimits(model)
                   : maiOfficialModelContextLimits(route->config.baseUrl, model);
    }

private:
    struct CachedClient {
        std::string credential;
        std::shared_ptr<MaiModelClient> client;
    };
    std::mutex mClientsMutex;
    std::map<const MaiModelRoute*, CachedClient> mClients;
    void reject(const char* reason) {
        if (!mError) mError = {MaiErrorCode::InvalidInput, reason};
    }

    const MaiModelRoute* findRoute(const std::string& model) const {
        for (const auto& route : mConfig.routes) {
            if (std::find(route.modelNames.begin(), route.modelNames.end(), model) !=
                route.modelNames.end())
                return &route;
        }
        return nullptr;
    }

    MaiRoutedModelConfig mConfig;
    MaiWireApi mWire = MaiWireApi::Mixed;
    MaiError mError;
};
}  // namespace

std::unique_ptr<MaiModelClient> makeMaiRoutedModelClient(MaiRoutedModelConfig config) {
    return std::make_unique<RoutedModelClient>(std::move(config));
}

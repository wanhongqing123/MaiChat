// 文件作用：按会话的固定型号选择共享路由，将原生协议请求直连对应供应商。
// 账号 Key 回调在模型工作线程调用；宿主须保证回调线程安全且不记录密钥。
#pragma once

#include <QString>
#include <functional>
#include <memory>
#include <optional>

#include "MaiModelClient.h"

class DesktopAccountModelClient final : public MaiModelClient {
public:
    struct Config {
        QString glmBaseUrl;
        QString deepseekBaseUrl;
        // 只读取宿主缓存，不发网络请求；纯配置查询也可在 UI 线程调用。
        std::function<QString(const QString&)> keyProvider;
        MaiModelImagePreparer prepareImage;
        // 测试可注入确定的逐跳网络路由；正式配置为空时在模型工作线程查询系统代理。
        std::function<bool(const std::string&, MaiModelProxy&)> proxyResolver;
    };

    explicit DesktopAccountModelClient(Config config);
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override;
    MaiWireApi wireApi() const override;
    std::optional<MaiWireApi> wireApiForModel(const std::string& model) const override;
    MaiModelContextLimits contextLimits(const std::string& model) const override;
    bool isConfiguredForModel(const std::string& model) const;

private:
    Config config_;
    std::unique_ptr<MaiModelClient> routed_;
};

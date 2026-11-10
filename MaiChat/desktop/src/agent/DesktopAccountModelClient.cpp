// 文件作用：每次模型请求读取当前账号 Key，按模型名选供应商地址；不经模型代理。
#include "agent/DesktopAccountModelClient.h"

#include <utility>

#include <QUrl>

#include "MaiCloudModelCatalog.h"
#include "MaiRoutedModelClient.h"
#include "MaiSystemProxy.h"

namespace {

std::string toUtf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString providerBaseUrl(const DesktopAccountModelClient::Config& config, const QString& provider) {
    if (provider == QStringLiteral("glm")) return config.glmBaseUrl;
    if (provider == QStringLiteral("deepseek")) return config.deepseekBaseUrl;
    return {};
}

// 系统代理按目标 URL 查询，每次重定向也重新查询，遵守 PAC 和绕过规则。
// 代理凭据只交给 curl 的代理认证选项，不能写入模型地址或日志。
bool resolveSystemModelProxy(const std::string& url, MaiModelProxy& result) {
    result = {};
    const QUrl target = QUrl::fromEncoded(QByteArray::fromStdString(url));
    if (!target.isValid() || target.host().isEmpty()) return false;
    // 本机测试服务不应把请求和测试令牌送到系统代理，和附件下载保持一致。
    if (target.host() == QStringLiteral("localhost") || target.host() == QStringLiteral("127.0.0.1") ||
        target.host() == QStringLiteral("::1"))
        return true;
    const auto proxy = maiSystemProxyForUrl(url);
    if (proxy.mode == MaiSystemProxyMode::Direct) return true;
    if (proxy.mode != MaiSystemProxyMode::Http && proxy.mode != MaiSystemProxyMode::Socks5) return false;
    QUrl address = QUrl::fromEncoded(QByteArray::fromStdString(proxy.url));
    if (!address.isValid() || address.host().isEmpty()) return false;
    // Core sets the explicit curl authentication fields, so remove userinfo
    // from the address after decoding it into those fields.
    result.username = toUtf8(address.userName(QUrl::FullyDecoded));
    result.password = toUtf8(address.password(QUrl::FullyDecoded));
    address.setUserInfo({});
    result.url = toUtf8(address.toString(QUrl::FullyEncoded));
    return true;
}

}  // namespace

DesktopAccountModelClient::DesktopAccountModelClient(Config config) : config_(std::move(config)) {
    MaiRoutedModelConfig routed;
    // 这里只列产品提供的型号，供应商、协议和能力统一来自 Core 目录。
    // 每条 route 的凭据回调在 stream 时执行，不能捕获构造时的旧 Key。
    for (const char* model : {"glm-5.3", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro"}) {
        const auto info = maiCloudModelInfo(model);
        if (!info) continue;
        const QString provider = QString::fromStdString(info->provider);
        MaiModelRoute route;
        route.modelNames = {info->model};
        route.config.baseUrl = toUtf8(providerBaseUrl(config_, provider));
        route.config.wire = info->wire;
        route.config.contextLimits = [modelName = info->model, limits = info->contextLimits](const std::string& model) {
            return model == modelName ? limits : MaiModelContextLimits{};
        };
        route.config.prepareImage = config_.prepareImage;
        route.config.proxyResolver = config_.proxyResolver ? config_.proxyResolver : resolveSystemModelProxy;
        route.apiKeyProvider = [provider, keyProvider = config_.keyProvider,
                                baseConfigured = !route.config.baseUrl.empty()] {
            return baseConfigured && keyProvider ? toUtf8(keyProvider(provider)) : std::string();
        };
        routed.routes.push_back(std::move(route));
    }
    routed_ = makeMaiRoutedModelClient(std::move(routed));
}

MaiWireApi DesktopAccountModelClient::wireApi() const {
    return routed_->wireApi();
}

std::optional<MaiWireApi> DesktopAccountModelClient::wireApiForModel(const std::string& model) const {
    return routed_->wireApiForModel(model);
}

MaiModelContextLimits DesktopAccountModelClient::contextLimits(const std::string& model) const {
    return routed_->contextLimits(model);
}

bool DesktopAccountModelClient::isConfiguredForModel(const std::string& model) const {
    const auto info = maiCloudModelInfo(model);
    if (!info || !config_.keyProvider) return false;
    const QString provider = QString::fromStdString(info->provider);
    const QString baseUrl = providerBaseUrl(config_, provider);
    return !baseUrl.isEmpty() && !config_.keyProvider(provider).isEmpty();
}

MaiError DesktopAccountModelClient::stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                                           const std::atomic<bool>& cancel) {
    return routed_->stream(request, sink, cancel);
}

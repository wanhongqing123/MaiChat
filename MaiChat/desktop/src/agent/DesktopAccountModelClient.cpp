// 文件作用：每次模型请求读取当前账号 Key，按模型名选供应商地址；不经模型代理。
#include "agent/DesktopAccountModelClient.h"

#include <utility>

#include <QUrl>

#include "MaiOpenAiClient.h"
#include "MaiSystemProxy.h"

namespace {

std::string toUtf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
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

DesktopAccountModelClient::DesktopAccountModelClient(Config config) : config_(std::move(config)) {}

MaiWireApi DesktopAccountModelClient::wireApi() const {
    return MaiWireApi::Responses;
}

MaiError DesktopAccountModelClient::stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                                           const std::atomic<bool>& cancel) {
    const QString model = QString::fromUtf8(request.model.c_str(), static_cast<int>(request.model.size()));
    QString provider;
    QString baseUrl;
    if (model == QStringLiteral("glm-5.3") || model == QStringLiteral("glm-5.3-flash")) {
        provider = QStringLiteral("glm");
        baseUrl = config_.glmBaseUrl;
    } else if (model == QStringLiteral("deepseek-flash")) {
        provider = QStringLiteral("deepseek");
        baseUrl = config_.deepseekBaseUrl;
    } else {
        return MaiError::make(MaiErrorCode::InvalidInput, "unsupported account model");
    }
    if (!config_.keyProvider || baseUrl.isEmpty())
        return MaiError::make(MaiErrorCode::NotConfigured, "account model unavailable");
    const QString key = config_.keyProvider(provider);
    if (key.isEmpty()) return MaiError::make(MaiErrorCode::NotConfigured, "account model key unavailable");
    MaiModelConfig config;
    config.baseUrl = toUtf8(baseUrl);
    config.apiKey = toUtf8(key);
    config.wire = MaiWireApi::Responses;
    config.prepareImage = config_.prepareImage;
    config.proxyResolver = config_.proxyResolver ? config_.proxyResolver : resolveSystemModelProxy;
    auto client = makeMaiModelClient(config);
    if (!client) return MaiError::make(MaiErrorCode::Internal, "model client creation failed");
    return client->stream(request, sink, cancel);
}

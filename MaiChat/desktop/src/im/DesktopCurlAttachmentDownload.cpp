#include "im/DesktopCurlAttachmentDownload.h"

#include <QFile>
#include <QResource>

#include <utility>

#include "MaiSystemProxy.h"

namespace {

struct ProxyResolverContext {
    std::string url;
};

// The shared downloader calls this for the initial URL and every redirect on
// its worker. A discovery failure aborts the transfer rather than selecting Direct.
int resolveProxy(const char* currentUrl, void* context, MaiDownloadProxy* output) {
    auto* resolver = static_cast<ProxyResolverContext*>(context);
    if (!currentUrl || !resolver || !output) return 0;
    const QUrl location = QUrl::fromEncoded(QByteArray(currentUrl));
    MaiSystemProxyResult proxy;
    // Keep local fixture traffic and its credentials on the local machine.
    if (location.host() == QStringLiteral("localhost") || location.host() == QStringLiteral("127.0.0.1"))
        proxy.mode = MaiSystemProxyMode::Direct;
    else
        proxy = maiSystemProxyForUrl(currentUrl);
    if (proxy.mode != MaiSystemProxyMode::Direct && proxy.mode != MaiSystemProxyMode::Http &&
        proxy.mode != MaiSystemProxyMode::Socks5)
        return 0;
    resolver->url = std::move(proxy.url);
    output->url_utf8 = resolver->url.c_str();
    // curl decodes userinfo from the proxy URL. Null fields avoid overriding it.
    output->username_utf8 = nullptr;
    output->password_utf8 = nullptr;
    return 1;
}

DesktopCurlAttachmentResult::Status statusFrom(MaiDownloadStatus status) {
    switch (status) {
        case MAI_DOWNLOAD_OK: return DesktopCurlAttachmentResult::Status::Downloaded;
        case MAI_DOWNLOAD_INVALID_ARGUMENT: return DesktopCurlAttachmentResult::Status::InvalidUrl;
        case MAI_DOWNLOAD_WRITE_ERROR: return DesktopCurlAttachmentResult::Status::WriteError;
        case MAI_DOWNLOAD_CANCELED: return DesktopCurlAttachmentResult::Status::Canceled;
        default: return DesktopCurlAttachmentResult::Status::NetworkError;
    }
}

}  // namespace

DesktopCurlAttachmentResult DesktopCurlAttachmentDownload::perform(const QUrl& url, const QString& targetPath,
                                                                   qint64 maximumBytes,
                                                                   const QByteArray& certificatePem,
                                                                   const std::shared_ptr<MaiDownloadCancel>& canceled) {
    DesktopCurlAttachmentResult output;
    if (maximumBytes <= 0 || targetPath.isEmpty()) {
        output.status = DesktopCurlAttachmentResult::Status::InvalidUrl;
        return output;
    }

    QByteArray trustedRoots = certificatePem;
#ifdef Q_OS_MACOS
    Q_INIT_RESOURCE(AuthCertificates);
    if (trustedRoots.isEmpty()) {
        QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
        if (certificates.open(QIODevice::ReadOnly)) trustedRoots = certificates.readAll();
    }
    if (trustedRoots.isEmpty()) return output;
#endif

    const QByteArray encodedUrl = url.toString(QUrl::FullyEncoded).toUtf8();
    const QByteArray encodedPath = targetPath.toUtf8();
    MaiDownloadOptions options{};
    options.url_utf8 = encodedUrl.constData();
    options.target_path_utf8 = encodedPath.constData();
    options.max_bytes = static_cast<uint64_t>(maximumBytes);
    options.ca_pem = trustedRoots.isEmpty() ? nullptr : trustedRoots.constData();
    options.ca_pem_length = static_cast<size_t>(trustedRoots.size());
    ProxyResolverContext resolver;
    options.proxy_resolver = resolveProxy;
    options.proxy_context = &resolver;

    MaiDownloadResult result{};
    mai_download_file(&options, canceled.get(), &result);
    output.status = statusFrom(result.status);
    output.transferCode = result.curl_code;
    output.bytes = static_cast<qint64>(result.bytes);
    return output;
}

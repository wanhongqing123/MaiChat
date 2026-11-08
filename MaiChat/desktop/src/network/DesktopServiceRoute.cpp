#include "network/DesktopServiceRoute.h"

#include <QMutex>
#include <QMutexLocker>

#include "MaiSystemProxy.h"

namespace {

QMutex& routeMutex() {
    static QMutex mutex;
    return mutex;
}

DesktopServiceProxyRoute& configuredRoute() {
    static DesktopServiceProxyRoute route;
    return route;
}

DesktopServiceProxyRoute& selectedRoute() {
    static DesktopServiceProxyRoute route;
    return route;
}

bool sameRoute(const DesktopServiceProxyRoute& left, const DesktopServiceProxyRoute& right) {
    return left.mode == right.mode && left.proxyUrl == right.proxyUrl;
}

void remember(const DesktopServiceProxyRoute& configured, const DesktopServiceProxyRoute& selected) {
    QMutexLocker lock(&routeMutex());
    configuredRoute() = configured;
    selectedRoute() = selected;
}

}  // namespace

DesktopServiceProxyRoute DesktopServiceRoute::configured(const QUrl& url) {
    const auto proxy = maiSystemProxyForUrl(url.toEncoded().toStdString());
    if (proxy.mode == MaiSystemProxyMode::Direct) {
        return {DesktopServiceProxyMode::Direct, {}};
    }
    if (proxy.mode == MaiSystemProxyMode::Unavailable) return {DesktopServiceProxyMode::Unavailable, {}};
    // The shared WebSocket transport supports plain HTTP CONNECT proxies only.
    // Preserve credentials for its separate proxy authentication setup.
    if (proxy.mode != MaiSystemProxyMode::Http || proxy.url.rfind("http://", 0) != 0) {
        return {DesktopServiceProxyMode::Unsupported, {}};
    }
    return {DesktopServiceProxyMode::HttpProxy, QString::fromStdString(proxy.url)};
}

DesktopServiceProxyRoute DesktopServiceRoute::choose(const DesktopServiceProxyRoute& configured, const Probe& probe) {
    if (configured.mode != DesktopServiceProxyMode::HttpProxy) return configured;
    {
        QMutexLocker lock(&routeMutex());
        if (sameRoute(configuredRoute(), configured) && selectedRoute().mode != DesktopServiceProxyMode::Unavailable) {
            return selectedRoute();
        }
    }
    if (probe(configured)) {
        remember(configured, configured);
        return configured;
    }
    const DesktopServiceProxyRoute direct{DesktopServiceProxyMode::Direct, {}};
    if (probe(direct)) {
        remember(configured, direct);
        return direct;
    }
    // A slow but required proxy might still complete the real request. Do not
    // select unverified direct access when neither short probe succeeded.
    return configured;
}

DesktopServiceProxyRoute DesktopServiceRoute::current(const DesktopServiceProxyRoute& configured) {
    QMutexLocker lock(&routeMutex());
    if (sameRoute(configuredRoute(), configured) && selectedRoute().mode != DesktopServiceProxyMode::Unavailable) {
        return selectedRoute();
    }
    return configured;
}

void DesktopServiceRoute::invalidate(const DesktopServiceProxyRoute& route) {
    QMutexLocker lock(&routeMutex());
    if (sameRoute(selectedRoute(), route)) {
        selectedRoute() = {};
    }
}

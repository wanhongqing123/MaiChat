// 文件作用：只读 macOS 系统代理，并把 CFNetwork 的首选规则转换为普通 curl/WebSocket 代理。
// 系统例外列表交给 CFNetwork；PAC 在工作线程的私有 run-loop mode 执行，超时明确失败。
#include "MaiNativeSystemProxy.h"

#import <CFNetwork/CFNetwork.h>
#import <CoreFoundation/CoreFoundation.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>

namespace {
template<class T> class CfOwned {
public:
    explicit CfOwned(T value = nullptr) : value_(value) {}
    ~CfOwned() { if (value_) CFRelease(value_); }
    CfOwned(const CfOwned&) = delete;
    CfOwned& operator=(const CfOwned&) = delete;
    T get() const { return value_; }
private:
    T value_;
};

MaiSystemProxyResult unavailable() { return {MaiSystemProxyMode::Unavailable, {}}; }
MaiSystemProxyResult unsupported() { return {MaiSystemProxyMode::Unsupported, {}}; }

bool stringValue(CFTypeRef value, std::string& output) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return false;
    auto text = static_cast<CFStringRef>(value);
    const CFIndex length = CFStringGetLength(text);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (capacity < 0) return false;
    output.resize(static_cast<size_t>(capacity));
    CFIndex used = 0;
    if (CFStringGetBytes(text, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false,
            reinterpret_cast<UInt8*>(output.data()), capacity, &used) != length)
        return false;
    output.resize(static_cast<size_t>(used));
    return output.find('\0') == std::string::npos;
}

bool flag(CFDictionaryRef settings, CFStringRef key, bool& output) {
    CFTypeRef value = CFDictionaryGetValue(settings, key);
    output = false;
    if (!value) return true;
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        output = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
        return true;
    }
    if (CFGetTypeID(value) != CFNumberGetTypeID()) return false;
    long long number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number))
        return false;
    output = number != 0;
    return true;
}

bool portValue(CFTypeRef value, int& port) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return false;
    long long number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number) ||
        number <= 0 || number > 65535)
        return false;
    port = static_cast<int>(number);
    return true;
}

bool unreserved(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

std::string escapeCredential(const std::string& text) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char byte : text) {
        if (unreserved(byte)) output += static_cast<char>(byte);
        else {
            output += '%';
            output += digits[byte >> 4];
            output += digits[byte & 15];
        }
    }
    return output;
}

bool proxyHost(CFTypeRef value, std::string& output) {
    std::string host;
    if (!stringValue(value, host) || host.empty()) return false;
    if (host.front() == '[' || host.back() == ']') {
        if (host.front() != '[' || host.back() != ']') return false;
        host = host.substr(1, host.size() - 2);
        // 方括号只包 IPv6，不能把带 userinfo 或 path 的普通字符串伪装成主机。
        if (host.find(':') == std::string::npos) return false;
    }
    if (host.find(':') != std::string::npos) {
        const size_t scopeAt = host.find('%');
        const std::string address = host.substr(0, scopeAt);
        in6_addr parsed{};
        if (inet_pton(AF_INET6, address.c_str(), &parsed) != 1) return false;
        output = '[' + address;
        if (scopeAt != std::string::npos) {
            const std::string scope = host.substr(scopeAt + 1);
            if (scope.empty()) return false;
            for (unsigned char byte : scope) if (!unreserved(byte)) return false;
            output += "%25" + scope;
        }
        output += ']';
        return true;
    }
    for (unsigned char byte : host) {
        if (byte <= 0x20 || byte == 0x7f || byte == '/' || byte == '\\' || byte == '@' ||
            byte == '?' || byte == '#' || byte == '%' || byte == '[' || byte == ']')
            return false;
    }
    output = host;
    return true;
}

bool httpUrl(CFURLRef url) {
    if (!url || CFGetTypeID(url) != CFURLGetTypeID()) return false;
    CfOwned scheme(CFURLCopyScheme(url));
    CfOwned host(CFURLCopyHostName(url));
    return scheme.get() && host.get() && CFStringGetLength(host.get()) > 0 &&
        (CFStringCompare(scheme.get(), CFSTR("http"), kCFCompareCaseInsensitive) == kCFCompareEqualTo ||
         CFStringCompare(scheme.get(), CFSTR("https"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
}

bool pacUrl(CFURLRef url) {
    if (!url || CFGetTypeID(url) != CFURLGetTypeID()) return false;
    if (httpUrl(url)) return true;
    CfOwned scheme(CFURLCopyScheme(url));
    CfOwned path(CFURLCopyPath(url));
    return scheme.get() && path.get() && CFStringGetLength(path.get()) > 0 &&
        CFStringCompare(scheme.get(), CFSTR("file"), kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

bool validManual(CFDictionaryRef settings, bool enabled, CFStringRef hostKey, CFStringRef portKey) {
    if (!enabled) return true;
    std::string host;
    int port = 0;
    return proxyHost(CFDictionaryGetValue(settings, hostKey), host) &&
        portValue(CFDictionaryGetValue(settings, portKey), port);
}

bool validSettings(CFDictionaryRef settings, CFURLRef target, bool& discovery, bool& automatic) {
    CfOwned scheme(CFURLCopyScheme(target));
    const bool secure = CFStringCompare(scheme.get(), CFSTR("https"),
        kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    bool manual = false, socks = false;
    if (!flag(settings, secure ? kSCPropNetProxiesHTTPSEnable : kSCPropNetProxiesHTTPEnable, manual) ||
        !flag(settings, kSCPropNetProxiesSOCKSEnable, socks) ||
        !flag(settings, kSCPropNetProxiesProxyAutoConfigEnable, automatic) ||
        !flag(settings, kSCPropNetProxiesProxyAutoDiscoveryEnable, discovery))
        return false;
    // CFNetwork 对缺失 host/port 的 enabled 规则可能返回 DIRECT；先验证，不能因此绕过系统代理。
    if (!validManual(settings, manual,
            secure ? kSCPropNetProxiesHTTPSProxy : kSCPropNetProxiesHTTPProxy,
            secure ? kSCPropNetProxiesHTTPSPort : kSCPropNetProxiesHTTPPort) ||
        !validManual(settings, socks, kSCPropNetProxiesSOCKSProxy, kSCPropNetProxiesSOCKSPort))
        return false;
    if (automatic) {
        std::string script;
        if (stringValue(CFDictionaryGetValue(settings, kSCPropNetProxiesProxyAutoConfigJavaScript),
                script) && !script.empty())
            return true;
        std::string url;
        if (!stringValue(CFDictionaryGetValue(settings, kSCPropNetProxiesProxyAutoConfigURLString),
                url) || url.empty())
            return false;
        CfOwned text(CFStringCreateWithBytes(kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(url.data()), static_cast<CFIndex>(url.size()),
            kCFStringEncodingUTF8, false));
        CfOwned location(text.get() ? CFURLCreateWithString(kCFAllocatorDefault, text.get(), nullptr) : nullptr);
        return pacUrl(location.get());
    }
    return true;
}

struct PacState {
    std::atomic<unsigned> references{1};
    std::mutex mutex;
    bool finished = false;
    CFArrayRef proxies = nullptr;
    ~PacState() { if (proxies) CFRelease(proxies); }
};
void* retainPac(void* pointer) {
    auto* state = static_cast<PacState*>(pointer);
    state->references.fetch_add(1, std::memory_order_relaxed);
    return pointer;
}
void releasePac(void* pointer) {
    auto* state = static_cast<PacState*>(pointer);
    if (state->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state;
}
void pacComplete(void* pointer, CFArrayRef proxies, CFErrorRef error) {
    auto* state = static_cast<PacState*>(pointer);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->finished) return;
    if (!error && proxies && CFGetTypeID(proxies) == CFArrayGetTypeID())
        state->proxies = static_cast<CFArrayRef>(CFRetain(proxies));
    state->finished = true;
}

template<class Start> CFArrayRef copyPacResult(Start start) {
    auto* state = new PacState;
    struct Release { PacState* value; ~Release() { releasePac(value); } } release{state};
    CFStreamClientContext context{};
    context.info = state;
    context.retain = retainPac;
    context.release = releasePac;
    CfOwned source(start(pacComplete, &context));
    if (!source.get()) return nullptr;
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    CFStringRef mode = CFSTR("org.maichat.system-proxy.pac");
    CFRunLoopAddSource(runLoop, source.get(), mode);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->finished) break;
        }
        if (std::chrono::steady_clock::now() >= deadline || !CFRunLoopSourceIsValid(source.get()))
            break;
        CFRunLoopRunInMode(mode, 0.05, true);
    }
    // 退出前终止 URL 下载/脚本并移除 source。context 自持有，避免迟到的原生清理引用栈变量。
    CFRunLoopSourceInvalidate(source.get());
    CFRunLoopRemoveSource(runLoop, source.get(), mode);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->proxies ? static_cast<CFArrayRef>(CFRetain(state->proxies)) : nullptr;
}

MaiSystemProxyResult concreteProxy(CFDictionaryRef proxy, MaiSystemProxyMode mode) {
    std::string host;
    int port = 0;
    if (!proxyHost(CFDictionaryGetValue(proxy, kCFProxyHostNameKey), host) ||
        !portValue(CFDictionaryGetValue(proxy, kCFProxyPortNumberKey), port))
        return unavailable();
    std::string url = mode == MaiSystemProxyMode::Socks5 ? "socks5h://" : "http://";
    CFTypeRef username = CFDictionaryGetValue(proxy, kCFProxyUsernameKey);
    CFTypeRef password = CFDictionaryGetValue(proxy, kCFProxyPasswordKey);
    if (username || password) {
        std::string user, secret;
        if ((username && !stringValue(username, user)) || (password && !stringValue(password, secret)))
            return unavailable();
        url += escapeCredential(user);
        if (password) url += ':' + escapeCredential(secret);
        url += '@';
    }
    url += host + ':' + std::to_string(port);
    return {mode, url};
}

MaiSystemProxyResult selectProxy(CFArrayRef proxies, CFURLRef target, unsigned depth = 0) {
    if (!proxies || CFGetTypeID(proxies) != CFArrayGetTypeID() || CFArrayGetCount(proxies) == 0 || depth > 4)
        return unavailable();
    // 冻结接口只返回一次连接策略；保留系统首选顺序，不能把不支持/坏配置静默换成后面的 DIRECT。
    // 多代理连接失败后的重试需要上层请求 replay 语义，此处不发起目标请求，也不探测代理可用性。
    CFTypeRef first = CFArrayGetValueAtIndex(proxies, 0);
    if (!first || CFGetTypeID(first) != CFDictionaryGetTypeID()) return unavailable();
    auto proxy = static_cast<CFDictionaryRef>(first);
    CFTypeRef type = CFDictionaryGetValue(proxy, kCFProxyTypeKey);
    if (!type || CFGetTypeID(type) != CFStringGetTypeID()) return unavailable();
    if (CFEqual(type, kCFProxyTypeNone)) return {MaiSystemProxyMode::Direct, {}};
    // Apple 的 HTTPS 类型表示 HTTPS 目标使用的 CONNECT 隧道，不能据此开启 TLS-to-proxy。
    if (CFEqual(type, kCFProxyTypeHTTP) || CFEqual(type, kCFProxyTypeHTTPS))
        return concreteProxy(proxy, MaiSystemProxyMode::Http);
    if (CFEqual(type, kCFProxyTypeSOCKS)) return concreteProxy(proxy, MaiSystemProxyMode::Socks5);
    if (CFEqual(type, kCFProxyTypeAutoConfigurationURL)) {
        CFTypeRef value = CFDictionaryGetValue(proxy, kCFProxyAutoConfigurationURLKey);
        if (!value || CFGetTypeID(value) != CFURLGetTypeID()) return unavailable();
        auto location = static_cast<CFURLRef>(value);
        if (!pacUrl(location)) return unavailable();
        CfOwned result(copyPacResult([&](CFProxyAutoConfigurationResultCallback callback,
                                        CFStreamClientContext* context) {
            return CFNetworkExecuteProxyAutoConfigurationURL(location, target, callback, context);
        }));
        return selectProxy(result.get(), target, depth + 1);
    }
    if (CFEqual(type, kCFProxyTypeAutoConfigurationJavaScript)) {
        CFTypeRef value = CFDictionaryGetValue(proxy, kCFProxyAutoConfigurationJavaScriptKey);
        if (!value || CFGetTypeID(value) != CFStringGetTypeID() ||
            CFStringGetLength(static_cast<CFStringRef>(value)) == 0)
            return unavailable();
        CfOwned result(copyPacResult([&](CFProxyAutoConfigurationResultCallback callback,
                                        CFStreamClientContext* context) {
            return CFNetworkExecuteProxyAutoConfigurationScript(
                static_cast<CFStringRef>(value), target, callback, context);
        }));
        return selectProxy(result.get(), target, depth + 1);
    }
    return unsupported();
}
} // namespace

MaiSystemProxyResult maiNativeSystemProxyForUrl(const std::string& targetUrl) {
    if (targetUrl.empty() || targetUrl.size() > static_cast<size_t>(std::numeric_limits<CFIndex>::max()))
        return unavailable();
    for (unsigned char byte : targetUrl) if (byte <= 0x20 || byte == 0x7f) return unavailable();
    CfOwned text(CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(targetUrl.data()), static_cast<CFIndex>(targetUrl.size()),
        kCFStringEncodingUTF8, false));
    CfOwned target(text.get() ? CFURLCreateWithString(kCFAllocatorDefault, text.get(), nullptr) : nullptr);
    if (!httpUrl(target.get())) return unavailable();
    CfOwned settings(CFNetworkCopySystemProxySettings());
    if (!settings.get() || CFGetTypeID(settings.get()) != CFDictionaryGetTypeID()) return unavailable();
    bool discovery = false, automatic = false;
    if (!validSettings(settings.get(), target.get(), discovery, automatic)) return unavailable();
    CfOwned proxies(CFNetworkCopyProxiesForURL(target.get(), settings.get()));
    // CFNetwork 已按系统 manual/PAC/bypass 规则返回有序决策。
    // WPAD 开关不能覆盖首项明确的 DIRECT 或具体代理；坏配置与 PAC 失败仍由原路径报告。
    return selectProxy(proxies.get(), target.get());
}

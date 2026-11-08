// Private offline fixture: replace only OS I/O; the production URL, schema,
// decision conversion and PAC source lifecycle execute unchanged.
#import <CFNetwork/CFNetwork.h>
#import <CoreFoundation/CoreFoundation.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace fixture {
CFDictionaryRef settings = nullptr;
CFTypeRef decisions = nullptr;
bool replaceDecisions = false;
CFArrayRef pacDecisions = nullptr;
enum class Pac { Success, Error, NullList, NullSource, Timeout, RealScript };
Pac pac = Pac::Success;
int settingsReads = 0, decisionReads = 0, pacCalls = 0;
int contextsRetained = 0, contextsReleased = 0, sourcesCancelled = 0;
bool sawPacUrl = false, sawPacScript = false;

CFDictionaryRef copySettings() {
    ++settingsReads;
    return settings ? static_cast<CFDictionaryRef>(CFRetain(settings)) : nullptr;
}
CFArrayRef copyDecisions(CFURLRef url, CFDictionaryRef value) {
    ++decisionReads;
    if (replaceDecisions)
        return static_cast<CFArrayRef>(decisions ? CFRetain(decisions) : nullptr);
    return CFNetworkCopyProxiesForURL(url, value);
}
struct Source {
    CFProxyAutoConfigurationResultCallback callback;
    CFStreamClientContext client;
    CFArrayRef result;
    Pac behavior;
    bool delivered = false;
    bool cancelled = false;
};
void sourceRelease(const void* pointer) {
    auto* value = const_cast<Source*>(static_cast<const Source*>(pointer));
    if (value->client.release) value->client.release(value->client.info);
    ++contextsReleased;
    if (value->result) CFRelease(value->result);
    delete value;
}
void sourceCancel(void* pointer, CFRunLoopRef, CFRunLoopMode) {
    auto* value = static_cast<Source*>(pointer);
    if (!value->cancelled) ++sourcesCancelled;
    value->cancelled = true;
}
void sourcePerform(void* pointer) {
    auto* value = static_cast<Source*>(pointer);
    value->delivered = true;
    CFErrorRef error = nullptr;
    if (value->behavior == Pac::Error)
        error = CFErrorCreate(kCFAllocatorDefault, kCFErrorDomainCFNetwork, 123, nullptr);
    value->callback(value->client.info,
                    value->behavior == Pac::NullList ? nullptr : value->result, error);
    if (error) CFRelease(error);
}
CFRunLoopSourceRef source(CFProxyAutoConfigurationResultCallback callback,
                          CFStreamClientContext* client) {
    if (pac == Pac::NullSource) return nullptr;
    auto* value = new Source{callback, *client,
        static_cast<CFArrayRef>(pacDecisions ? CFRetain(pacDecisions) : nullptr), pac};
    if (value->client.retain)
        value->client.info = const_cast<void*>(value->client.retain(value->client.info));
    ++contextsRetained;
    CFRunLoopSourceContext context{};
    context.info = value;
    context.release = sourceRelease;
    context.cancel = sourceCancel;
    context.perform = sourcePerform;
    auto result = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    if (pac != Pac::Timeout) CFRunLoopSourceSignal(result);
    return result;
}
CFRunLoopSourceRef executeUrl(CFURLRef, CFURLRef,
                              CFProxyAutoConfigurationResultCallback callback,
                              CFStreamClientContext* client) {
    ++pacCalls;
    sawPacUrl = true;
    return source(callback, client);
}
CFRunLoopSourceRef executeScript(CFStringRef script, CFURLRef target,
                                 CFProxyAutoConfigurationResultCallback callback,
                                 CFStreamClientContext* client) {
    ++pacCalls;
    sawPacScript = true;
    if (pac == Pac::RealScript)
        return CFNetworkExecuteProxyAutoConfigurationScript(script, target, callback, client);
    return source(callback, client);
}
} // namespace fixture

#define CFNetworkCopySystemProxySettings fixture::copySettings
#define CFNetworkCopyProxiesForURL fixture::copyDecisions
#define CFNetworkExecuteProxyAutoConfigurationURL fixture::executeUrl
#define CFNetworkExecuteProxyAutoConfigurationScript fixture::executeScript
#include MAI_NATIVE_PROXY_SOURCE
#undef CFNetworkCopySystemProxySettings
#undef CFNetworkCopyProxiesForURL
#undef CFNetworkExecuteProxyAutoConfigurationURL
#undef CFNetworkExecuteProxyAutoConfigurationScript

namespace {
int passed = 0, failed = 0;
void check(bool success, const char* name) {
    if (success) ++passed;
    else { ++failed; std::printf("FAIL %s\n", name); }
}
template<class T> class Owned {
public:
    explicit Owned(T value) : value_(value) {}
    ~Owned() { if (value_) CFRelease(value_); }
    operator T() const { return value_; }
private:
    T value_;
};
CFMutableDictionaryRef dict() {
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}
void setInt(CFMutableDictionaryRef dictionary, CFStringRef key, int value) {
    Owned number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value));
    CFDictionarySetValue(dictionary, key, number);
}
CFMutableDictionaryRef proxy(CFStringRef type, CFStringRef host = CFSTR("proxy.example.test"),
                             int port = 8080) {
    auto result = dict();
    CFDictionarySetValue(result, kCFProxyTypeKey, type);
    if (host) CFDictionarySetValue(result, kCFProxyHostNameKey, host);
    setInt(result, kCFProxyPortNumberKey, port);
    return result;
}
CFMutableArrayRef list(CFTypeRef first = nullptr, CFTypeRef second = nullptr) {
    auto result = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (first) CFArrayAppendValue(result, first);
    if (second) CFArrayAppendValue(result, second);
    return result;
}
CFMutableDictionaryRef manual(CFStringRef enable = kCFNetworkProxiesHTTPEnable,
                              CFStringRef hostKey = kCFNetworkProxiesHTTPProxy,
                              CFStringRef portKey = kCFNetworkProxiesHTTPPort) {
    auto result = dict();
    setInt(result, enable, 1);
    CFDictionarySetValue(result, hostKey, CFSTR("proxy.example.test"));
    setInt(result, portKey, 8080);
    return result;
}
void reset(CFDictionaryRef settings) {
    fixture::settings = settings;
    fixture::decisions = nullptr;
    fixture::replaceDecisions = false;
    fixture::pacDecisions = nullptr;
    fixture::pac = fixture::Pac::Success;
    fixture::settingsReads = fixture::decisionReads = fixture::pacCalls = 0;
    fixture::contextsRetained = fixture::contextsReleased = fixture::sourcesCancelled = 0;
    fixture::sawPacUrl = fixture::sawPacScript = false;
}
void force(CFTypeRef decisions) {
    fixture::decisions = decisions;
    fixture::replaceDecisions = true;
}
MaiSystemProxyResult resolve(const std::string& url = "https://target.example.test/path") {
    return maiNativeSystemProxyForUrl(url);
}
void expect(const char* name, MaiSystemProxyMode mode, const char* url = "",
            const std::string& target = "https://target.example.test/path") {
    auto result = resolve(target);
    // Do not print URLs, even for synthetic inputs: this fixture must stay safe
    // to use when a failed assertion is copied to an integration log.
    check(result.mode == mode && result.url == url, name);
}
void directAndManual() {
    reset(nullptr);
    expect("missing settings explicit", MaiSystemProxyMode::Unavailable);
    Owned empty(dict()); reset(empty);
    expect("empty settings direct", MaiSystemProxyMode::Direct);
    Owned http(manual()); reset(http);
    expect("manual HTTP", MaiSystemProxyMode::Http, "http://proxy.example.test:8080",
        "http://target.example.test/path");
    expect("protocol isolation", MaiSystemProxyMode::Direct);
    setInt(http, kCFNetworkProxiesHTTPEnable, 0);
    expect("disabled HTTP", MaiSystemProxyMode::Direct, "", "http://target.example.test/");
    Owned https(manual(kCFNetworkProxiesHTTPSEnable, kCFNetworkProxiesHTTPSProxy,
        kCFNetworkProxiesHTTPSPort)); reset(https);
    expect("HTTPS is CONNECT over HTTP", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080");
    Owned socks(manual(kCFNetworkProxiesSOCKSEnable, kCFNetworkProxiesSOCKSProxy,
        kCFNetworkProxiesSOCKSPort)); reset(socks);
    expect("SOCKS remote DNS", MaiSystemProxyMode::Socks5,
        "socks5h://proxy.example.test:8080");
    Owned exceptions(list(CFSTR("*.internal.example.test"), CFSTR("192.0.2.0/24")));
    CFDictionarySetValue(https, kCFNetworkProxiesExceptionsList, exceptions);
    reset(https);
    expect("system wildcard bypass", MaiSystemProxyMode::Direct, "",
        "https://one.internal.example.test/");
    expect("system CIDR bypass", MaiSystemProxyMode::Direct, "", "https://192.0.2.5/");
    expect("bypass hostname boundary", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080", "https://notinternal.example.test/");
    setInt(https, kCFNetworkProxiesExcludeSimpleHostnames, 1);
    expect("system simple host bypass", MaiSystemProxyMode::Direct, "", "https://printer/");
    expect("system dotted host retained", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080", "https://printer.example.test/");
    reset(empty);
    for (const std::string& target : std::initializer_list<std::string>{"", "ftp://target.example.test/", "ws://target.example.test/",
            "https:///", "no-scheme", std::string("https://x\0.example.test/", 24),
            std::string("https://\xff.example.test/")}) {
        expect("invalid native target rejected", MaiSystemProxyMode::Unavailable, "", target);
    }
    check(fixture::settingsReads == 0, "invalid targets never read settings");
}
void selectedDecisions() {
    Owned settings(dict()); reset(settings);
    Owned http(proxy(kCFProxyTypeHTTP));
    Owned direct(proxy(kCFProxyTypeNone));
    Owned both(list(http, direct)); force(both);
    expect("first proxy wins", MaiSystemProxyMode::Http, "http://proxy.example.test:8080");
    Owned reverse(list(direct, http)); force(reverse);
    expect("first direct wins", MaiSystemProxyMode::Direct);
    Owned unknown(proxy(CFSTR("FutureProxyType")));
    Owned unsupported(list(unknown, direct)); force(unsupported);
    expect("unsupported is not later direct", MaiSystemProxyMode::Unsupported);
    force(CFSTR("bad array"));
    expect("non-array explicit", MaiSystemProxyMode::Unavailable);
    force(nullptr);
    expect("null array explicit", MaiSystemProxyMode::Unavailable);
    Owned empty(list()); force(empty);
    expect("empty array explicit", MaiSystemProxyMode::Unavailable);
    Owned nondictionary(list(CFSTR("bad entry"))); force(nondictionary);
    expect("bad entry explicit", MaiSystemProxyMode::Unavailable);
    Owned noType(dict()); Owned untyped(list(noType)); force(untyped);
    expect("missing type explicit", MaiSystemProxyMode::Unavailable);
    for (CFStringRef host : {CFSTR(""), CFSTR("proxy/path"), CFSTR("user@proxy"),
            CFSTR("proxy?x"), CFSTR("proxy#x"), CFSTR("proxy\\x"), CFSTR("proxy x"),
            CFSTR("[broken"), CFSTR("bad:ipv6"), CFSTR("proxy%40host")}) {
        Owned bad(proxy(kCFProxyTypeHTTP, host)); Owned value(list(bad, direct)); force(value);
        expect("unsafe host explicit", MaiSystemProxyMode::Unavailable);
    }
    for (int port : {-1, 0, 65536}) {
        Owned bad(proxy(kCFProxyTypeHTTP, CFSTR("proxy.example.test"), port));
        Owned value(list(bad, direct)); force(value);
        expect("invalid port explicit", MaiSystemProxyMode::Unavailable);
    }
    Owned badPort(proxy(kCFProxyTypeHTTP));
    CFDictionarySetValue(badPort, kCFProxyPortNumberKey, CFSTR("8080"));
    Owned portValue(list(badPort)); force(portValue);
    expect("string port explicit", MaiSystemProxyMode::Unavailable);
    double fraction = 8080.5;
    Owned fractionValue(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &fraction));
    CFDictionarySetValue(badPort, kCFProxyPortNumberKey, fractionValue);
    expect("fractional port explicit", MaiSystemProxyMode::Unavailable);
    Owned ipv6(proxy(kCFProxyTypeHTTP, CFSTR("2001:db8::1"), 3128));
    Owned ipv6Value(list(ipv6)); force(ipv6Value);
    expect("IPv6 bracketed", MaiSystemProxyMode::Http, "http://[2001:db8::1]:3128");
    CFDictionarySetValue(ipv6, kCFProxyHostNameKey, CFSTR("[2001:db8::1]"));
    expect("existing IPv6 brackets", MaiSystemProxyMode::Http, "http://[2001:db8::1]:3128");
    CFDictionarySetValue(ipv6, kCFProxyHostNameKey, CFSTR("fe80::1%en0"));
    expect("IPv6 scope escaped", MaiSystemProxyMode::Http, "http://[fe80::1%25en0]:3128");
    Owned credential(proxy(kCFProxyTypeHTTP));
    CFDictionarySetValue(credential, kCFProxyUsernameKey, CFSTR("user:@%"));
    CFDictionarySetValue(credential, kCFProxyPasswordKey, CFSTR("test /?#@"));
    Owned credentialValue(list(credential)); force(credentialValue);
    expect("credentials percent encoded", MaiSystemProxyMode::Http,
        "http://user%3A%40%25:test%20%2F%3F%23%40@proxy.example.test:8080");
    CFDictionarySetValue(credential, kCFProxyUsernameKey, kCFBooleanTrue);
    expect("bad credential type explicit", MaiSystemProxyMode::Unavailable);
    CFDictionaryRemoveValue(credential, kCFProxyUsernameKey);
    expect("password without username preserved", MaiSystemProxyMode::Http,
        "http://:test%20%2F%3F%23%40@proxy.example.test:8080");
}
void malformedSettings() {
    Owned settings(manual(kCFNetworkProxiesHTTPSEnable, kCFNetworkProxiesHTTPSProxy,
        kCFNetworkProxiesHTTPSPort)); reset(settings);
    CFDictionarySetValue(settings, kCFNetworkProxiesHTTPSProxy, CFSTR(""));
    expect("enabled missing host not implicit direct", MaiSystemProxyMode::Unavailable);
    CFDictionarySetValue(settings, kCFNetworkProxiesHTTPSProxy, CFSTR("proxy.example.test"));
    setInt(settings, kCFNetworkProxiesHTTPSPort, 0);
    expect("enabled invalid port not implicit direct", MaiSystemProxyMode::Unavailable);
    CFDictionarySetValue(settings, kCFNetworkProxiesHTTPSEnable, CFSTR("yes"));
    expect("invalid enable flag explicit", MaiSystemProxyMode::Unavailable);
    Owned pacSettings(dict()); reset(pacSettings);
    setInt(pacSettings, kCFNetworkProxiesProxyAutoConfigEnable, 1);
    expect("enabled PAC missing source explicit", MaiSystemProxyMode::Unavailable);
    CFDictionarySetValue(pacSettings, kCFNetworkProxiesProxyAutoConfigURLString,
        CFSTR("not a URL"));
    expect("enabled PAC malformed source explicit", MaiSystemProxyMode::Unavailable);
    Owned discovery(dict()); reset(discovery);
    setInt(discovery, kCFNetworkProxiesProxyAutoDiscoveryEnable, 1);
    Owned none(proxy(kCFProxyTypeNone)); Owned direct(list(none)); force(direct);
    expect("OS explicit DIRECT with WPAD is respected", MaiSystemProxyMode::Direct);
}
void pacLifecycle() {
    Owned settings(dict()); reset(settings);
    Owned automatic(proxy(kCFProxyTypeAutoConfigurationURL));
    Owned url(CFURLCreateWithString(kCFAllocatorDefault,
        CFSTR("https://pac.example.test/rules.pac"), nullptr));
    CFDictionarySetValue(automatic, kCFProxyAutoConfigurationURLKey, url);
    Owned choices(list(automatic)); force(choices);
    Owned http(proxy(kCFProxyTypeHTTP, CFSTR("pac.example.test"), 3128));
    Owned answer(list(http)); fixture::pacDecisions = answer;
    expect("PAC URL decision", MaiSystemProxyMode::Http, "http://pac.example.test:3128");
    check(fixture::sawPacUrl && fixture::pacCalls == 1, "PAC URL used native executor");
    check(fixture::contextsRetained == fixture::contextsReleased &&
        fixture::sourcesCancelled == 1, "PAC success source released");
    Owned none(proxy(kCFProxyTypeNone)); Owned direct(list(none));
    fixture::pacDecisions = direct;
    expect("PAC explicit direct", MaiSystemProxyMode::Direct);
    fixture::pac = fixture::Pac::Error;
    expect("PAC error explicit", MaiSystemProxyMode::Unavailable);
    fixture::pac = fixture::Pac::NullList;
    expect("PAC missing result explicit", MaiSystemProxyMode::Unavailable);
    fixture::pac = fixture::Pac::NullSource;
    expect("PAC source failure explicit", MaiSystemProxyMode::Unavailable);
    fixture::pac = fixture::Pac::Success;
    Owned noAnswer(list()); fixture::pacDecisions = noAnswer;
    expect("PAC empty result explicit", MaiSystemProxyMode::Unavailable);
    fixture::pacDecisions = answer;
    fixture::pac = fixture::Pac::Timeout;
    const auto started = std::chrono::steady_clock::now();
    expect("PAC timeout explicit", MaiSystemProxyMode::Unavailable);
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(8),
        "PAC timeout bounded");
    check(fixture::contextsRetained == fixture::contextsReleased &&
        fixture::sourcesCancelled == fixture::contextsRetained,
        "PAC timeout cancels and releases every source");
    CFDictionarySetValue(automatic, kCFProxyAutoConfigurationURLKey, CFSTR("wrong type"));
    fixture::pac = fixture::Pac::Success;
    expect("PAC URL wrong type explicit", MaiSystemProxyMode::Unavailable);
    CFDictionarySetValue(automatic, kCFProxyTypeKey, kCFProxyTypeAutoConfigurationJavaScript);
    CFDictionarySetValue(automatic, kCFProxyAutoConfigurationJavaScriptKey,
        CFSTR("function FindProxyForURL(url,host){return 'DIRECT';}"));
    fixture::pacDecisions = answer;
    expect("PAC inline decision", MaiSystemProxyMode::Http, "http://pac.example.test:3128");
    check(fixture::sawPacScript, "PAC inline native executor");
    fixture::pacDecisions = choices;
    expect("recursive PAC explicit", MaiSystemProxyMode::Unavailable);
    check(fixture::pacCalls < 20, "recursive PAC bounded");
    fixture::pac = fixture::Pac::RealScript;
    CFDictionarySetValue(automatic, kCFProxyAutoConfigurationJavaScriptKey,
        CFSTR("function FindProxyForURL(url,host){return host==='target.example.test' ? "
              "'PROXY native-pac.example.test:8888' : 'DIRECT';}"));
    expect("real native PAC script proxy", MaiSystemProxyMode::Http,
        "http://native-pac.example.test:8888");
    expect("real native PAC script direct", MaiSystemProxyMode::Direct, "",
        "https://other.example.test/");
    fixture::settings = nullptr;
    fixture::decisions = nullptr;
    fixture::pacDecisions = nullptr;
}
void discoveryWithConcreteRules() {
    Owned https(manual(kCFNetworkProxiesHTTPSEnable, kCFNetworkProxiesHTTPSProxy,
        kCFNetworkProxiesHTTPSPort));
    setInt(https, kCFNetworkProxiesProxyAutoDiscoveryEnable, 1);
    reset(https);
    expect("WPAD preserves concrete manual HTTPS CONNECT", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080");
    Owned exceptions(list(CFSTR("*.internal.example.test"), CFSTR("192.0.2.0/24")));
    CFDictionarySetValue(https, kCFNetworkProxiesExceptionsList, exceptions);
    expect("WPAD preserves explicit wildcard bypass", MaiSystemProxyMode::Direct, "",
        "https://one.internal.example.test/");
    expect("WPAD bypass host boundary keeps manual proxy", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080", "https://notinternal.example.test/");
    expect("WPAD preserves explicit CIDR bypass", MaiSystemProxyMode::Direct, "", "https://192.0.2.5/");
    expect("WPAD CIDR boundary keeps manual proxy", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080", "https://192.0.3.5/");
    setInt(https, kCFNetworkProxiesExcludeSimpleHostnames, 1);
    expect("WPAD preserves simple-host bypass", MaiSystemProxyMode::Direct, "", "https://printer/");
    expect("WPAD dotted-host boundary keeps manual proxy", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080", "https://printer.example.test/");
    Owned http(manual());
    setInt(http, kCFNetworkProxiesProxyAutoDiscoveryEnable, 1);
    reset(http);
    expect("WPAD preserves concrete manual HTTP", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080", "http://target.example.test/");
    Owned socks(manual(kCFNetworkProxiesSOCKSEnable, kCFNetworkProxiesSOCKSProxy,
        kCFNetworkProxiesSOCKSPort));
    setInt(socks, kCFNetworkProxiesProxyAutoDiscoveryEnable, 1);
    reset(socks);
    expect("WPAD preserves concrete SOCKS with remote DNS", MaiSystemProxyMode::Socks5,
        "socks5h://proxy.example.test:8080");

    Owned settings(dict()); setInt(settings, kCFNetworkProxiesProxyAutoDiscoveryEnable, 1);
    reset(settings);
    Owned direct(proxy(kCFProxyTypeNone)); Owned concrete(proxy(kCFProxyTypeHTTP));
    Owned firstDirect(list(direct, concrete)); force(firstDirect);
    expect("WPAD honors first explicit DIRECT before proxy", MaiSystemProxyMode::Direct);
    Owned firstProxy(list(concrete, direct)); force(firstProxy);
    expect("WPAD honors first concrete proxy before DIRECT", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080");
    Owned malformed(dict()); Owned firstMalformed(list(malformed, direct)); force(firstMalformed);
    expect("WPAD malformed first choice stays unavailable", MaiSystemProxyMode::Unavailable);
    Owned unknown(proxy(CFSTR("FutureProxyType"))); Owned firstUnknown(list(unknown, direct)); force(firstUnknown);
    expect("WPAD unknown first choice stays unsupported", MaiSystemProxyMode::Unsupported);
    force(nullptr);
    expect("WPAD missing OS decisions stays unavailable", MaiSystemProxyMode::Unavailable);
    force(firstDirect);
    CFDictionarySetValue(settings, kCFNetworkProxiesProxyAutoDiscoveryEnable, CFSTR("yes"));
    expect("WPAD malformed flag stays unavailable before OS DIRECT", MaiSystemProxyMode::Unavailable);
    reset(https);
    CFDictionarySetValue(https, kCFNetworkProxiesHTTPSProxy, CFSTR(""));
    force(firstDirect);
    expect("WPAD malformed enabled manual rule stays unavailable", MaiSystemProxyMode::Unavailable);

    reset(settings);
    setInt(settings, kCFNetworkProxiesProxyAutoDiscoveryEnable, 1);
    Owned automatic(proxy(kCFProxyTypeAutoConfigurationURL));
    Owned url(CFURLCreateWithString(kCFAllocatorDefault,
        CFSTR("https://pac.example.test/rules.pac"), nullptr));
    CFDictionarySetValue(automatic, kCFProxyAutoConfigurationURLKey, url);
    Owned automaticChoices(list(automatic, direct)); force(automaticChoices);
    Owned pacAnswer(list(concrete)); fixture::pacDecisions = pacAnswer;
    expect("WPAD resolved PAC still executes native decision", MaiSystemProxyMode::Http,
        "http://proxy.example.test:8080");
    check(fixture::pacCalls == 1 && fixture::sawPacUrl, "WPAD PAC URL uses injected native executor");
    fixture::pac = fixture::Pac::Error;
    expect("WPAD PAC download/execution error cannot fall back to DIRECT", MaiSystemProxyMode::Unavailable);
}
} // namespace

int main() {
    discoveryWithConcreteRules();
    directAndManual();
    selectedDecisions();
    malformedSettings();
    pacLifecycle();
    std::printf("Mac system proxy offline: %d PASS, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}

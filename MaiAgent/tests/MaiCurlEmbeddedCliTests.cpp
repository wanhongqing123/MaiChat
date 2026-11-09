// 文件作用：验证“MaiCurlEmbeddedCli”的正确行为和边界条件。
// 验证入口：阅读 main 或测试注册处，核对输入、预期结果与失败输出。
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <json.hpp>
#include <curl/curl.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiCurlCliTool.h"
#include "MaiTool.h"
#include "mai_curl_embed.h"

#if defined(_WIN32) && defined(_MSC_VER)
namespace {
using MaiTestCertOpenStore = HCERTSTORE(WINAPI*)(HCRYPTPROV_LEGACY, LPCSTR);
std::atomic<bool> sPrivateNativeStoreEnabled{false};
std::atomic<unsigned> sPrivateNativeStoreOpens{0};
std::atomic<unsigned> sForwardedRootStoreOpens{0};
std::atomic<unsigned> sForwardedCaStoreOpens{0};
std::atomic<unsigned> sForwardedStoreFailures{0};
std::atomic<DWORD> sForwardedStoreLastError{0};
std::string sPrivateNativeRoot;

// 只在本测试进程启用内存库；curl 仍执行原生证书导入和真实 TLS 校验。
// 非夹具请求转发真实 API，不注册或修改用户的系统证书库。
HCERTSTORE WINAPI openCurlTestSystemStore(HCRYPTPROV_LEGACY provider, LPCSTR name) {
    if (!sPrivateNativeStoreEnabled.load()) {
        static const auto original = reinterpret_cast<MaiTestCertOpenStore>(
            GetProcAddress(GetModuleHandleW(L"crypt32.dll"), "CertOpenSystemStoreA"));
        if (original) {
            HCERTSTORE store = original(provider, name);
            const DWORD error = GetLastError();
            if (name && std::strcmp(name, "ROOT") == 0) ++sForwardedRootStoreOpens;
            if (name && std::strcmp(name, "CA") == 0) ++sForwardedCaStoreOpens;
            if (!store) {
                ++sForwardedStoreFailures;
                sForwardedStoreLastError = error;
            }
            SetLastError(error);
            return store;
        }
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    ++sPrivateNativeStoreOpens;
    if (!name || (std::strcmp(name, "ROOT") != 0 && std::strcmp(name, "CA") != 0)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    HCERTSTORE store =
        CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (store && std::strcmp(name, "ROOT") == 0 && !sPrivateNativeRoot.empty() &&
        !CertAddEncodedCertificateToStore(
            store, X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(sPrivateNativeRoot.data()),
            static_cast<DWORD>(sPrivateNativeRoot.size()), CERT_STORE_ADD_ALWAYS, nullptr)) {
        CertCloseStore(store, 0);
        return nullptr;
    }
    return store;
}
}  // namespace

// 静态 libcurl 的导入槽仅在这个测试 EXE 中替换；产品不链接此适配器。
extern "C" {
MaiTestCertOpenStore __imp_CertOpenSystemStoreA = openCurlTestSystemStore;
}
#endif

namespace {

int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

int run(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    return mai_curl_execute(static_cast<int>(arguments.size()), argv.data());
}

int cancelImmediately(void*) {
    return 1;
}

size_t discardResponse(char*, size_t size, size_t count, void*) {
    if (size != 0 && count > SIZE_MAX / size) return 0;
    return size * count;
}

struct Server {
    httplib::Server server;
    std::thread worker;
    int port = 0;

    Server() {
        server.Get("/ok", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("embedded curl", "text/plain");
        });
        server.Get("/html", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("<p>Readable <b>page</b></p><script>bad()</script>", "text/html");
        });
        server.Get("/binary", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string(1, static_cast<char>(0xff)),
                                 "application/octet-stream");
        });
        server.Get("/missing", [](const httplib::Request&, httplib::Response& response) {
            response.status = 404;
        });
        server.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
            response.set_content(request.body, "text/plain");
        });
        server.Put("/upload", [](const httplib::Request& request, httplib::Response& response) {
            response.set_content(std::to_string(request.body.size()), "text/plain");
        });
        port = server.bind_to_any_port("127.0.0.1");
        worker = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~Server() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::string url(const char* route) const {
        return "http://127.0.0.1:" + std::to_string(port) + route;
    }
};

int toolCurlStatus(const MaiToolResult& result) {
    const auto value = nlohmann::json::parse(
        result.hasError() ? result.error().message() : result.output(), nullptr, false);
    return value.is_object() ? value.value("exit_status", -1) : -1;
}

MaiToolResult requestHead(MaiTool& tool, const std::string& url, const MaiToolContext& context) {
    return tool.execute(nlohmann::json{{"arguments", {"--head", url}}, {"timeout_s", 10}}.dump(),
                        context);
}

#if defined(_WIN32) && defined(_MSC_VER)
class MaiPrivateNativeStoreScope {
public:
    explicit MaiPrivateNativeStoreScope(std::string authority) {
        sPrivateNativeRoot = std::move(authority);
        sPrivateNativeStoreOpens = 0;
        sPrivateNativeStoreEnabled = true;
    }
    ~MaiPrivateNativeStoreScope() {
        sPrivateNativeStoreEnabled = false;
        sPrivateNativeRoot.clear();
    }
};

void checkLocalTls(const MaiToolContext& context, const MaiFilePath& root) {
    const char* url = std::getenv("MAI_CURL_TEST_TLS_URL");
    const char* mismatch = std::getenv("MAI_CURL_TEST_TLS_MISMATCH_URL");
    const char* authority = std::getenv("MAI_CURL_TEST_CA_FILE");
    const char* authorityDer = std::getenv("MAI_CURL_TEST_CA_DER");
    const char* wrong = std::getenv("MAI_CURL_TEST_WRONG_CA_FILE");
    const char* malformed = std::getenv("MAI_CURL_TEST_BAD_CA_FILE");
    CHECK(url && mismatch && authority && authorityDer && wrong && malformed);
    if (!url || !mismatch || !authority || !authorityDer || !wrong || !malformed) return;
    std::string nativeAuthority;
    CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(authorityDer), nativeAuthority));
    CHECK(!nativeAuthority.empty() && nativeAuthority.size() < 1024 * 1024);
    if (nativeAuthority.empty() || nativeAuthority.size() >= 1024 * 1024) return;
    MaiPrivateNativeStoreScope privateStore(nativeAuthority);
    const int control =
        run({"curl", "-q", "--silent", "--show-error", "--noproxy", "*", "--max-time", "10",
             "--ca-native", "--output",
             root.append(MaiFilePath::fromUtf8("native-control.txt")).toUtf8(), url});
    std::printf("local TLS native importer control: exit=%d native_store_opens=%u\n", control,
                sPrivateNativeStoreOpens.load());
    CHECK(control == 0 && sPrivateNativeStoreOpens.load() > 0);
    if (control != 0) return;

    const auto verify = [&](const char* label, const std::string& ca, const std::string& endpoint,
                            int expected, bool native) {
        sPrivateNativeStoreOpens = 0;
        auto tool = makeMaiCurlCliTool(ca);
        const auto result = requestHead(*tool, endpoint, context);
        const int status = toolCurlStatus(result);
        const unsigned opens = sPrivateNativeStoreOpens.load();
        std::printf("local TLS %s: exit=%d expected=%d native_store_opens=%u\n", label, status,
                    expected, opens);
        CHECK(status == expected);
        CHECK((!result.hasError()) == (expected == 0));
        CHECK(native ? opens > 0 : opens == 0);
    };
    verify("default-native", {}, url, 0, true);
    verify("explicit-authority", authority, url, 0, false);
    verify("explicit-untrusted", wrong, url, CURLE_PEER_FAILED_VERIFICATION, false);
    verify("explicit-malformed", malformed, url, CURLE_SSL_CACERT_BADFILE, false);
    verify("explicit-missing", root.append(MaiFilePath::fromUtf8("missing-ca.pem")).toUtf8(), url,
           CURLE_SSL_CACERT_BADFILE, false);
    verify("explicit-hostname", authority, mismatch, CURLE_PEER_FAILED_VERIFICATION, false);
    verify("native-hostname", {}, mismatch, CURLE_PEER_FAILED_VERIFICATION, true);
    sPrivateNativeRoot.clear();
    verify("native-untrusted", {}, url, CURLE_PEER_FAILED_VERIFICATION, true);
}
#endif

int publicTlsDiagnostics(CURL*, curl_infotype type, char* bytes, size_t size, void*) {
    if (type != CURLINFO_TEXT) return 0;
    const std::string text(bytes, size);
    // 只保留导入结果和 TLS 状态；不打印请求头、响应头、证书内容或环境变量。
    if (text.find("successfully imported Windows ") == 0 ||
        text.find("error importing Windows ") == 0 || text.find("SSL certificate problem:") == 0 ||
        text.find("SSL connection using ") == 0)
        std::printf("public TLS detail: %s", text.c_str());
    return 0;
}

void checkPublicTls(const MaiToolContext& context) {
#if defined(_WIN32)
    CURL* curl = curl_easy_init();
    CHECK(curl != nullptr);
    if (!curl) return;
    char error[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, publicTlsDiagnostics);
#if defined(_MSC_VER)
    sForwardedRootStoreOpens = 0;
    sForwardedCaStoreOpens = 0;
    sForwardedStoreFailures = 0;
    sForwardedStoreLastError = 0;
#endif
    curl_easy_setopt(curl, CURLOPT_URL, "https://example.com/");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
    const CURLcode control = curl_easy_perform(curl);
    long http = 0;
    long verify = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
    curl_certinfo* certificates = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CERTINFO, &certificates);
    std::printf(
        "public example.com TLS diagnostics: verify_result=%ld peer_chain_count=%d error=%s\n",
        verify, certificates ? certificates->num_of_certs : 0, error);
#if defined(_MSC_VER)
    std::printf("public Windows store API: ROOT=%u CA=%u failures=%u last_error=%lu\n",
                sForwardedRootStoreOpens.load(), sForwardedCaStoreOpens.load(),
                sForwardedStoreFailures.load(), sForwardedStoreLastError.load());
#endif
    curl_easy_cleanup(curl);
    std::printf("public example.com native control: curl=%d HTTP=%ld\n", int(control), http);
    CHECK(control == CURLE_OK && http == 200);
    if (control != CURLE_OK) {
        std::printf("public TLS environment control failed; builtin trust was not validated\n");
        return;
    }
    auto tool = makeMaiCurlCliTool();
    const auto result = requestHead(*tool, "https://example.com/", context);
    const auto value = nlohmann::json::parse(result.output(), nullptr, false);
    std::printf("public example.com default tool: curl=%d success=%d\n", toolCurlStatus(result),
                int(!result.hasError()));
    CHECK(!result.hasError());
    CHECK(value.is_object() && value.value("status", 0) == 200);
#else
    (void)context;
    CHECK(false);
#endif
}

}  // namespace

// 默认用本地服务验证内嵌 curl；显式启用时才请求正式账号只读端点。
int main() {
#if defined(_WIN32)
    const char* sslVersion = curl_version_info(CURLVERSION_NOW)->ssl_version;
    std::printf("embedded curl TLS backend: %s\n", sslVersion ? sslVersion : "missing");
    CHECK(sslVersion != nullptr);
    if (sslVersion) CHECK(std::string(sslVersion).find("OpenSSL/") != std::string::npos);
    if (std::getenv("MAI_CURL_LIVE_AUTH_TEST") != nullptr) {
        CURL* curl = curl_easy_init();
        CHECK(curl != nullptr);
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, "https://ichat.life/api/v1/auth/me");
            curl_easy_setopt(curl, CURLOPT_PROXY, "");
            CHECK(curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA) == CURLE_OK);
            CHECK(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK);
            CHECK(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
            const CURLcode transfer = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            std::printf("live auth TLS result: curl=%d HTTP=%ld\n", int(transfer), status);
            CHECK(transfer == CURLE_OK);
            CHECK(status == 401);
            curl_easy_cleanup(curl);
        }
    }
#endif
    Server server;
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("curl-cli-test-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const auto invoke = [&](const char* route, const char* output) {
        const MaiFilePath path = root.append(MaiFilePath::fromUtf8(output));
        const int status =
            run({"curl", "--silent", "--show-error", "--fail", "--proto", "=http,https",
                 "--max-time", "5", "--output", path.toUtf8(), server.url(route)});
        return std::pair<int, MaiFilePath>{status, path};
    };
    const auto first = invoke("/ok", "first.txt");
    CHECK(first.first == 0);
    std::string content;
    CHECK(!MaiFileSystem::readFile(first.second, content));
    CHECK(content == "embedded curl");

    MaiToolContext context;
    context.root = root.toUtf8();
    // 本地真实 TLS 矩阵由私有脚本显式启用，不能计入普通 HTTP suite 的覆盖。
    if (std::getenv("MAI_CURL_LOCAL_TLS_TEST") != nullptr) {
#if defined(_WIN32) && defined(_MSC_VER)
        checkLocalTls(context, root);
#else
        CHECK(false);
#endif
    }
    // 只有该开关允许请求公网；不使用账号、模型服务或认证信息。
    if (std::getenv("MAI_CURL_LIVE_PUBLIC_TLS_TEST") != nullptr) checkPublicTls(context);
    auto tool = makeMaiCurlCliTool();
    CHECK(tool->requiresApproval("{}"));
    CHECK(!tool->requiresPerCallApproval("{}"));
    CHECK(tool->approvalKey(R"({"arguments":["https://a.example"]})") !=
          tool->approvalKey(R"({"arguments":["https://b.example"]})"));
    const auto fetched =
        tool->execute(nlohmann::json{{"arguments", {server.url("/ok")}}}.dump(), context);
    CHECK(!fetched.hasError());
    if (!fetched.hasError()) {
        const auto result = nlohmann::json::parse(fetched.output());
        CHECK(result.value("status", 0) == 200);
        CHECK(result.value("body", std::string{}) == "embedded curl");
        CHECK(result["headers"].value("content-type", std::string{}) == "text/plain");
    }
    const auto html = tool->execute(
        nlohmann::json{{"arguments", {server.url("/html")}}, {"text_only", true}}.dump(), context);
    CHECK(!html.hasError());
    if (!html.hasError()) {
        const auto text = nlohmann::json::parse(html.output()).value("body", std::string{});
        CHECK(text.find("Readable page") != std::string::npos);
        CHECK(text.find("bad()") == std::string::npos);
    }
    const auto binary =
        tool->execute(nlohmann::json{{"arguments", {server.url("/binary")}}}.dump(), context);
    CHECK(!binary.hasError());
    if (!binary.hasError())
        CHECK(nlohmann::json::parse(binary.output()).value("body_base64", std::string{}) == "/w==");
    const auto saved = tool->execute(
        nlohmann::json{{"arguments", {server.url("/ok")}}, {"output_path", "saved.txt"}}.dump(),
        context);
    CHECK(!saved.hasError());
    CHECK(MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("saved.txt"))));
    CHECK(tool->execute(
                  nlohmann::json{{"arguments", {server.url("/ok")}}, {"output_path", "saved.txt"}}
                      .dump(),
                  context)
              .hasError());
    const auto posted = tool->execute(
        nlohmann::json{{"arguments", {"--data", "hello", server.url("/echo")}}}.dump(), context);
    CHECK(!posted.hasError());
    if (!posted.hasError())
        CHECK(nlohmann::json::parse(posted.output()).value("body", std::string{}) == "hello");
    const MaiFilePath upload = root.append(MaiFilePath::fromUtf8("input.bin"));
    CHECK(!MaiFileSystem::writeFile(upload, std::string(4096, 'u')));
    const auto uploaded = tool->execute(
        nlohmann::json{{"arguments", {"--upload-file", "input.bin", server.url("/upload")}}}.dump(),
        context);
    CHECK(!uploaded.hasError());
    if (!uploaded.hasError())
        CHECK(nlohmann::json::parse(uploaded.output()).value("body", std::string{}) == "4096");
    const auto missing =
        tool->execute(nlohmann::json{{"arguments", {server.url("/missing")}}}.dump(), context);
    CHECK(!missing.hasError());
    if (!missing.hasError())
        CHECK(nlohmann::json::parse(missing.output()).value("status", 0) == 404);
    CHECK(tool->execute(nlohmann::json{{"arguments", {server.url("/missing")}},
                                       {"output_path", "missing.bin"}}
                            .dump(),
                        context)
              .hasError());
    CHECK(!MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("missing.bin"))));
    CHECK(
        tool->execute(nlohmann::json{{"arguments", {"--config", "bad", server.url("/ok")}}}.dump(),
                      context)
            .hasError());
    CHECK(tool->execute(nlohmann::json{{"arguments",
                                        {"--upload-file", "../outside", server.url("/upload")}}}
                            .dump(),
                        context)
              .hasError());
    FILE* errors = std::tmpfile();
    CHECK(errors != nullptr);
    if (errors) mai_curl_set_error_file(errors);
    const auto failed = invoke("/missing", "failed.txt");
    mai_curl_set_error_file(nullptr);
    CHECK(failed.first != 0);
    if (errors) {
        std::rewind(errors);
        char text[256]{};
        const std::size_t count = std::fread(text, 1, sizeof(text) - 1, errors);
        CHECK(std::string(text, count).find("404") != std::string::npos);
        std::fclose(errors);
    }
    mai_curl_set_cancel_check(cancelImmediately, nullptr);
    const auto canceled = invoke("/ok", "canceled.txt");
    mai_curl_set_cancel_check(nullptr, nullptr);
    CHECK(canceled.first != 0);
    const auto second = invoke("/ok", "second.txt");
    CHECK(second.first == 0);
    content.clear();
    CHECK(!MaiFileSystem::readFile(second.second, content));
    CHECK(content == "embedded curl");
    MaiFileSystem::removeRecursively(root);
    return failures == 0 ? 0 : 1;
}

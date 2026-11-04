// 文件作用：用 loopback 假服务验证普通检索客户端、只读工具的协议、权限、边界、重试及 TLS。
// 所有请求均为本地合成 JSON；凭据仅为进程内假值，不访问官方服务或真实账号。
#include "MaiDeepSeekWebTools.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#if defined(MAI_DEEPSEEK_WEB_TLS_TESTS)
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#endif

#include "MaiAgent.h"
#include "MaiArkMediaTools.h"
#include "MaiFakeModelClient.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiThread.h"
#include "MaiTool.h"

namespace {
using Json = nlohmann::json;
int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

Json searchItem() {
    // 官方 api-search.md 示例字段；未知元数据保留 null/-1，不猜测日期或权威等级。
    return {{"rerank_score", 2.4},
            {"title", "DeepSeek V4 model released"},
            {"url", "https://example.com/deepseek-v4"},
            {"snippet", "Release notes"},
            {"summary", "A structured public result"},
            {"content", ""},
            {"site_name", nullptr},
            {"site_icon", nullptr},
            {"date", nullptr},
            {"authority", -1}};
}

MaiWebSearchRequest searchRequest(const std::string& query, unsigned count = 10) {
    MaiWebSearchRequest request;
    request.query = query;
    request.count = count;
    return request;
}

struct MaiWebReply {
    int status = 200;
    std::string body;
    std::string location;
    unsigned delayMilliseconds = 0;
};

struct MaiWebCapturedRequest {
    std::string path;
    Json body;
    bool authenticated = false;
};

class MaiDeepSeekFakeProvider {
public:
    explicit MaiDeepSeekFakeProvider(std::unique_ptr<httplib::Server> server = {},
                                     bool secure = false)
        : mServer(server ? std::move(server) : std::make_unique<httplib::Server>()),
          mSecure(secure),
          mCredential(MaiIdGenerator::generate("fixture_")) {
        mServer->new_task_queue = [] { return new httplib::ThreadPool(2); };
        mServer->set_read_timeout(2, 0);
        mServer->set_write_timeout(2, 0);
        mServer->Post("/v1/tools/search", [this](const auto& request, auto& response) {
            handle(request, response, true);
        });
        mServer->Post("/v1/tools/fetch", [this](const auto& request, auto& response) {
            handle(request, response, false);
        });
        mServer->Get("/collected", [this](const auto& request, auto& response) {
            handle(request, response, false);
        });
        mServer->Post("/collected", [this](const auto& request, auto& response) {
            handle(request, response, false);
        });
        mSearch = {{200, Json{{"results", Json::array({searchItem()})}}.dump(), {}, 0}};
        mFetch = {{200,
                   Json{{"title", "Article"}, {"content", "# Public article\n\nMarkdown."}}.dump(),
                   {},
                   0}};
        mPort = mServer->bind_to_any_port("127.0.0.1");
        CHECK(mPort > 0);
        if (mPort <= 0) return;
        mThread = std::thread([this] {
            MaiThread::setCurrentName("mai-web-server");
            mServer->listen_after_bind();
        });
        for (unsigned index = 0; index < 200 && !mServer->is_running(); ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(mServer->is_running());
    }
    ~MaiDeepSeekFakeProvider() {
        mServer->stop();
        if (mThread.joinable()) mThread.join();
    }
    MaiDeepSeekWebOptions options() {
        MaiDeepSeekWebOptions options;
        options.baseUrl = address();
        options.allowLoopbackTestServer = true;
        options.timeoutSeconds = 3;
        options.apiKey = [this] {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mKeyReads;
            return mCredential;
        };
        return options;
    }
    std::string address() const {
        return std::string(mSecure ? "https" : "http") + "://127.0.0.1:" + std::to_string(mPort);
    }
    void searchReplies(std::vector<MaiWebReply> replies) {
        std::lock_guard<std::mutex> lock(mMutex);
        mSearch = std::move(replies);
        mSearchIndex = 0;
        mRequests.clear();
    }
    void fetchReplies(std::vector<MaiWebReply> replies) {
        std::lock_guard<std::mutex> lock(mMutex);
        mFetch = std::move(replies);
        mFetchIndex = 0;
        mRequests.clear();
    }
    std::vector<MaiWebCapturedRequest> requests() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mRequests;
    }
    unsigned keyReads() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mKeyReads;
    }
    void rotateCredential() {
        std::lock_guard<std::mutex> lock(mMutex);
        mCredential = MaiIdGenerator::generate("fixture_rotated_");
    }

private:
    void handle(const httplib::Request& request, httplib::Response& response, bool search) {
        MaiWebReply reply;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mRequests.push_back(
                {request.path, Json::parse(request.body, nullptr, false),
                 request.get_header_value("Authorization") == "Bearer " + mCredential});
            auto& replies = search ? mSearch : mFetch;
            auto& index = search ? mSearchIndex : mFetchIndex;
            reply = replies[std::min(index++, replies.size() - 1)];
        }
        if (reply.delayMilliseconds)
            std::this_thread::sleep_for(std::chrono::milliseconds(reply.delayMilliseconds));
        response.status = reply.status;
        if (!reply.location.empty()) response.set_header("Location", reply.location);
        response.set_content(reply.body, "application/json");
    }
    std::unique_ptr<httplib::Server> mServer;
    bool mSecure;
    int mPort = 0;
    std::thread mThread;
    mutable std::mutex mMutex;
    std::string mCredential;
    unsigned mKeyReads = 0;
    std::size_t mSearchIndex = 0, mFetchIndex = 0;
    std::vector<MaiWebReply> mSearch, mFetch;
    std::vector<MaiWebCapturedRequest> mRequests;
};

void test_unicode_operators_unknown_metadata_and_empty_results() {
    MaiDeepSeekFakeProvider server;
    MaiDeepSeekWebClient client(server.options());
    MaiWebSearchRequest request;
    request.query =
        "site:example.com -site:other.example \"exact phrase\" lang:zh after:2026-01-01 "
        "before:2026-11-03";
    const auto response = client.search(request);
    CHECK(response.error.code.empty() && response.attempts == 1 && response.results.size() == 1);
    if (!response.results.empty()) {
        CHECK(!response.results[0].date && !response.results[0].site);
        CHECK(response.results[0].authority == -1);
        CHECK(response.results[0].rerankScore == 2.4);
    }
    auto sent = server.requests();
    CHECK(sent.size() == 1 && sent[0].authenticated);
    if (!sent.empty()) {
        CHECK(sent[0].body["query"] == request.query);
        CHECK(sent[0].body["need_content"] == false);
        CHECK(!sent[0].body.contains("localization") && !sent[0].body.contains("ori_question"));
    }
    request.query.clear();
    for (unsigned index = 0; index < 127; ++index) request.query += u8"\u4e2d";
    request.query += u8"\U0001f9ea";
    request.oriQuestion = request.query;
    request.localization = "2001:db8::1";
    request.needContent = true;
    request.count = 50;
    CHECK(client.search(request).error.code.empty());
    sent = server.requests();
    CHECK(sent.size() == 2 && sent.back().body["query"] == request.query);
    CHECK(sent.back().body["ori_question"] == request.oriQuestion);
    CHECK(sent.back().body["localization"] == request.localization);
    CHECK(sent.back().body["need_content"] == true);
    CHECK(sent.back().body["count"] == 50);
    request.query += "x";
    CHECK(client.search(request).error.code == "INVALID_INPUT");
    CHECK(server.requests().size() == 2);
    server.searchReplies({{200, R"({"results":[]})", {}, 0}});
    request = {};
    request.query = "no matching public results";
    const auto empty = client.search(request);
    CHECK(empty.error.code.empty() && empty.results.empty() && empty.attempts == 1);
}

void test_invalid_typed_inputs_never_read_credentials_or_send_requests() {
    MaiDeepSeekFakeProvider server;
    MaiDeepSeekWebClient client(server.options());
    for (const auto& query : std::vector<std::string>{
             "", " \t\n", u8"\u3000\u00a0", std::string("\xc0\xaf", 2), std::string("a\0b", 3)}) {
        MaiWebSearchRequest request;
        request.query = query;
        CHECK(client.search(request).error.code == "INVALID_INPUT");
    }
    MaiWebSearchRequest request;
    request.query = "valid";
    for (const unsigned count : {0u, 51u}) {
        request.count = count;
        CHECK(client.search(request).error.code == "INVALID_INPUT");
    }
    request.count = 1;
    request.oriQuestion = std::string(129, 'x');
    CHECK(client.search(request).error.code == "INVALID_INPUT");
    request.oriQuestion = u8"\u3000";
    CHECK(client.search(request).error.code == "INVALID_INPUT");
    request.oriQuestion.clear();
    request.localization = std::string("127.0.0.1\0other", 15);
    CHECK(client.search(request).error.code == "INVALID_INPUT");
    for (const auto& url :
         {"file:///tmp/page", "https://user:password@example.invalid/", "https:///", ""})
        CHECK(client.fetch(url).error.code == "INVALID_INPUT");
    CHECK(client.fetch("https://example.invalid/", 0).error.code == "INVALID_INPUT");
    CHECK(client.fetch("https://example.invalid/", 64001).error.code == "INVALID_INPUT");
    try {
        CHECK(client.fetch(std::string("https://example.invalid/") + char(0xff)).error.code ==
              "INVALID_INPUT");
    } catch (...) {
        CHECK(false && "Malformed UTF-8 in the ordinary API must return an error, not throw");
    }
    CHECK(server.requests().empty() && server.keyReads() == 0);
    auto options = server.options();
    options.allowLoopbackTestServer = false;
    CHECK(MaiDeepSeekWebClient(options).search(searchRequest("valid")).error.code ==
          "INVALID_CONFIGURATION");
    options = server.options();
    options.apiKey = [] { return std::string{}; };
    CHECK(MaiDeepSeekWebClient(options).search(searchRequest("valid")).error.code ==
          "NOT_CONFIGURED");
    CHECK(server.requests().empty());
}

void test_localization_uses_a_known_ip_or_is_omitted() {
    MaiDeepSeekFakeProvider server;
    MaiDeepSeekWebClient ordinary(server.options());
    MaiToolRegistry tools;
    registerMaiDeepSeekWebTools(tools, server.options());
    auto* search = tools.find("web_search");
    CHECK(search != nullptr);
    if (!search) return;
    const auto schema = Json::parse(search->parametersSchema());
    const auto& localization = schema["properties"]["localization"];
    CHECK(localization.value("maxLength", 0) == 45);
    const auto description = localization.value("description", std::string{});
    CHECK(description.find("IPv4") != std::string::npos &&
          description.find("IPv6") != std::string::npos);
    CHECK(description.find("language or region code") != std::string::npos);
    CHECK(description.find("Omit") != std::string::npos &&
          description.find("Never guess or look up") != std::string::npos);

    // 地域、语言标签不能悄悄当 IP，也不能在提示修复前取凭据或发送 HTTP。
    for (const auto& invalid : {"zh-CN", "China", "en", "256.1.1.1", "[2001:db8::1]"}) {
        auto request = searchRequest("public news");
        request.localization = invalid;
        const auto direct = ordinary.search(request);
        CHECK(direct.error.code == "INVALID_INPUT" && direct.error.attempts == 0);
        CHECK(direct.error.reason.find("IPv4 or IPv6") != std::string::npos &&
              direct.error.reason.find("omit") != std::string::npos);
        const auto result =
            search->execute(Json{{"query", request.query}, {"localization", invalid}}.dump(), {});
        CHECK(result.hasError() && result.error().code() == MaiErrorCode::InvalidInput);
        if (result.hasError()) {
            const auto detail = Json::parse(result.error().message());
            CHECK(detail["code"] == "INVALID_INPUT" && detail["attempts"] == 0);
            CHECK(detail["reason"] == direct.error.reason);
        }
    }
    CHECK(server.requests().empty() && server.keyReads() == 0);

    // 普通入口和工具入口保留同一协议：两种 IP 原样传递，未提供时不补造定位。
    for (const auto& ip : {"192.0.2.1", "2001:db8::1"}) {
        auto request = searchRequest("public news");
        request.localization = ip;
        CHECK(ordinary.search(request).error.code.empty());
        CHECK(!search->execute(Json{{"query", request.query}, {"localization", ip}}.dump(), {})
                   .hasError());
        const auto sent = server.requests();
        CHECK(sent.size() >= 2);
        if (sent.size() >= 2) {
            CHECK(sent[sent.size() - 2].body["localization"] == ip);
            CHECK(sent.back().body["localization"] == ip);
        }
    }
    CHECK(ordinary.search(searchRequest("public news")).error.code.empty());
    CHECK(!search->execute(R"({"query":"public news"})", {}).hasError());
    CHECK(!search->execute(R"({"query":"public news","localization":""})", {}).hasError());
    const auto sent = server.requests();
    CHECK(sent.size() == 7 && server.keyReads() == 7);
    if (sent.size() == 7) {
        for (std::size_t index = 4; index < sent.size(); ++index)
            CHECK(!sent[index].body.contains("localization"));
    }
}

void test_response_schema_numeric_ranges_and_fetch_markdown_failures() {
    MaiDeepSeekFakeProvider server;
    MaiDeepSeekWebClient client(server.options());
    auto malformed = searchItem();
    for (const auto& value :
         std::vector<Json>{-2, 5, 1.5, nullptr, std::numeric_limits<std::uint64_t>::max(),
                           std::uint64_t{4294967297ULL}}) {
        malformed["authority"] = value;
        server.searchReplies(
            {{200, Json{{"results", Json::array({searchItem(), malformed})}}.dump(), {}, 0}});
        const auto response = client.search(searchRequest("range checks", 2));
        CHECK(response.error.code == "INVALID_RESPONSE" && response.results.empty());
        CHECK(response.error.httpStatus == 200 && response.error.attempts == 1);
    }
    for (const auto& content : {std::string{}, std::string(" \r\n\t")}) {
        server.fetchReplies({{200, Json{{"title", "Empty"}, {"content", content}}.dump(), {}, 0}});
        CHECK(client.fetch("https://example.invalid/article").error.code == "EMPTY_CONTENT");
    }
    const std::vector<std::pair<std::string, std::string>> pages = {
        {"Just a moment...", "BOT_CHALLENGE"},
        {"CAPTCHA", "BOT_CHALLENGE"},
        {u8"\u9a8c\u8bc1\u7801", "BOT_CHALLENGE"},
        {"Sign in", "LOGIN_REQUIRED"},
        {u8"\u767b\u5f55", "LOGIN_REQUIRED"}};
    for (const auto& page : pages) {
        server.fetchReplies(
            {{200,
              Json{{"title", page.first}, {"content", "Verification required"}}.dump(),
              {},
              0}});
        const auto response = client.fetch("https://example.invalid/article");
        CHECK(response.error.code == page.second && response.content.empty());
    }
    for (const auto& html : {"<!DOCTYPE html><html>Raw page</html>", "  \n<HTML>Raw page</HTML>"}) {
        server.fetchReplies({{200, Json{{"title", "Article"}, {"content", html}}.dump(), {}, 0}});
        CHECK(client.fetch("https://example.invalid/article").error.code == "HTML_NOT_EXTRACTED");
    }
    server.fetchReplies({{200, R"({"title":"Article","content":"A\u4e2d\ud83e\uddeaZ"})", {}, 0}});
    const auto clipped = client.fetch("https://example.invalid/article", 3);
    CHECK(clipped.error.code.empty() && clipped.truncated &&
          clipped.content == u8"A\u4e2d\U0001f9ea");
    CHECK(!Json(clipped.content).dump().empty());
    server.fetchReplies({{200, R"({"title":"Article"})", {}, 0}});
    CHECK(client.fetch("https://example.invalid/article").error.code == "INVALID_RESPONSE");
}

void test_bounded_retries_http_stage_and_recovery() {
    MaiDeepSeekFakeProvider server;
    MaiDeepSeekWebClient client(server.options());
    for (const int status : {408, 429, 500, 502, 503, 504}) {
        server.fetchReplies({{status, R"({"error":"synthetic provider failure"})", {}, 0}});
        const auto response = client.fetch("https://example.invalid/page");
        CHECK(response.error.httpStatus == status && response.attempts == 2);
        CHECK(response.error.attempts == 2 && response.error.retryable);
        CHECK(server.requests().size() == 2);
    }
    for (const auto& item :
         std::vector<std::pair<int, std::string>>{{402, "INSUFFICIENT_BALANCE"},
                                                  {403, "PROVIDER_SECURITY_POLICY"},
                                                  {404, "FETCH_UNAVAILABLE"}}) {
        server.fetchReplies({{item.first, R"({"error":"synthetic provider failure"})", {}, 0}});
        const auto response = client.fetch("https://example.invalid/page");
        CHECK(response.error.code == item.second && response.error.httpStatus == item.first);
        CHECK(response.attempts == 1 && !response.error.retryable && server.requests().size() == 1);
    }
    server.searchReplies(
        {{429, R"({"error":"limited"})", {}, 0}, {200, R"({"results":[]})", {}, 0}});
    const auto recovered = client.search(searchRequest("transient recovery"));
    CHECK(recovered.error.code.empty() && recovered.attempts == 2 && server.requests().size() == 2);
}

void test_cancellation_and_response_byte_limit() {
    MaiDeepSeekFakeProvider server;
    MaiDeepSeekWebClient client(server.options());
    std::atomic<bool> cancel{true};
    const auto before = client.search(searchRequest("already canceled"), &cancel);
    CHECK(before.error.code == "CANCELED" && before.attempts == 0);
    CHECK(server.requests().empty() && server.keyReads() == 0);
    cancel.store(false);
    server.fetchReplies({{200, R"({"title":"Slow","content":"Ready"})", {}, 1200}});
    MaiWebFetchResponse result;
    std::thread worker([&] {
        MaiThread::setCurrentName("mai-web-cancel");
        result = client.fetch("https://example.invalid/slow", 20000, &cancel);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server.requests().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(server.requests().size() == 1);
    cancel.store(true);
    worker.join();
    CHECK(result.error.code == "CANCELED" && result.attempts == 1);
    CHECK(server.requests().size() == 1);
    std::string exactBoundary = R"({"title":"Boundary","content":"usable"})";
    exactBoundary.append(2 * 1024 * 1024 - exactBoundary.size(), ' ');
    server.fetchReplies({{200, std::move(exactBoundary), {}, 0}});
    const auto accepted = client.fetch("https://example.invalid/exact-boundary", 1);
    CHECK(accepted.error.code.empty() && accepted.content == "u" && accepted.truncated);
    server.fetchReplies({{200, std::string(2 * 1024 * 1024 + 1, 'x'), {}, 0}});
    const auto oversized = client.fetch("https://example.invalid/large");
    CHECK(oversized.error.code == "RESPONSE_TOO_LARGE" && oversized.attempts == 1);
    CHECK(!oversized.error.retryable && oversized.content.empty());
}

void test_tools_share_the_client_contract_and_strict_read_only_protocol() {
    MaiDeepSeekFakeProvider server;
    const auto options = server.options();
    MaiDeepSeekWebClient ordinary(options);
    MaiToolRegistry registry;
    registerMaiDeepSeekWebTools(registry, options);
    auto* search = registry.find("web_search");
    auto* fetch = registry.find("web_fetch");
    CHECK(search && fetch && server.keyReads() == 0);
    if (!search || !fetch) return;
    CHECK(!search->requiresApproval(R"({"query":"public news","count":15})"));
    CHECK(!search->requiresPaidApproval(R"({"query":"public news","count":15})"));
    CHECK(!fetch->requiresApproval(R"({"url":"https://example.invalid/page"})"));
    CHECK(!fetch->requiresPaidApproval(R"({"url":"https://example.invalid/page"})"));
    CHECK(Json::parse(search->parametersSchema())["additionalProperties"] == false);
    const auto direct = ordinary.search(searchRequest("same contract"));
    server.rotateCredential();
    const auto tool = search->execute(R"({"query":"same contract","ori_question":null})", {});
    CHECK(!tool.hasError() && direct.error.code.empty());
    if (!tool.hasError()) {
        const auto json = Json::parse(tool.output());
        CHECK(json["results"][0]["title"] == direct.results[0].title);
        CHECK(json["results"][0]["site"].is_null() && json["results"][0]["date"].is_null());
        CHECK(json["results"][0]["authority"] == -1);
    }
    CHECK(!fetch->execute(R"({"url":"https://example.invalid/page","max_characters":1})", {})
               .hasError());
    CHECK(server.keyReads() == 3);
    for (const auto& request : server.requests()) CHECK(request.authenticated);
    const auto requestCount = server.requests().size();
    const std::vector<std::string> invalidSearch = {
        "[]",
        "null",
        "{",
        "{}",
        R"({"query":1})",
        R"({"query":false})",
        R"({"query":[]})",
        R"({"query":"valid","unknown":true})",
        R"({"query":"valid","api_key":"forbidden"})",
        R"({"query":"valid","base_url":"https://example.invalid"})",
        R"({"query":"valid","count":0})",
        R"({"query":"valid","count":51})",
        R"({"query":"valid","count":1.5})",
        R"({"query":"valid","count":"1"})",
        R"({"query":"valid","count":true})",
        R"({"query":"valid","ori_question":1})",
        R"({"query":"valid","ori_question":""})",
        R"({"query":"valid","localization":null})",
        R"({"query":"valid","need_content":1})"};
    for (const auto& input : invalidSearch) {
        const auto response = search->execute(input, {});
        CHECK(response.hasError() && response.error().code() == MaiErrorCode::InvalidInput);
    }
    for (const auto& input : {R"({"url":null})", R"({"url":"https://example.invalid","unknown":1})",
                              R"({"url":"https://example.invalid","max_characters":0})",
                              R"({"url":"https://example.invalid","max_characters":64001})",
                              R"({"url":"https://example.invalid","max_characters":2.5})"})
        CHECK(fetch->execute(input, {}).error().code() == MaiErrorCode::InvalidInput);
    CHECK(server.requests().size() == requestCount && server.keyReads() == 3);
    server.fetchReplies({{404, R"({"error":"origin explanation unavailable"})", {}, 0}});
    const auto failed = fetch->execute(R"({"url":"https://example.invalid/page"})", {});
    CHECK(failed.hasError());
    const auto detail = Json::parse(failed.error().message(), nullptr, false);
    CHECK(detail["code"] == "FETCH_UNAVAILABLE" && detail["http_status"] == 404);
    CHECK(detail["http_stage"] == "provider_api" && detail["attempts"] == 1 &&
          detail["retryable"] == false);
}

void test_read_only_retrieval_runs_without_approval_under_every_policy() {
    // 走实际 Agent 权限链并观察发出的请求，避免只验证工具元数据而漏掉闸门调用。
    // 每种模式都连续搜索、抓取、再搜索；超时只用于旧实现红灯时防止测试无限等待。
    for (const auto policy : {MaiApprovalPolicy::Never, MaiApprovalPolicy::UnlessTrusted,
                              MaiApprovalPolicy::OnRequest}) {
        MaiDeepSeekFakeProvider server;
        auto tools = std::make_unique<MaiToolRegistry>();
        registerMaiDeepSeekWebTools(*tools, server.options());
        std::vector<MaiFakeModelClient::Turn> script;
        const auto call = [&](const std::string& name, const std::string& arguments) {
            MaiFakeModelClient::Turn turn;
            turn.invocations.push_back({"call_" + std::to_string(script.size()), name, arguments});
            script.push_back(std::move(turn));
        };
        // 现场查询“今日新闻 头条 要闻”及 count=15；保持原始 Unicode 字节。
        call("web_search",
             Json{{"query", "\u4eca\u65e5\u65b0\u95fb \u5934\u6761 \u8981\u95fb"}, {"count", 15}}
                 .dump());
        call("web_fetch", R"({"url":"https://example.invalid/page"})");
        call("web_search", R"({"query":"follow-up public news"})");
        MaiFakeModelClient::Turn done;
        done.textChunks = {"Done."};
        script.push_back(std::move(done));

        auto model = std::make_unique<MaiFakeModelClient>(std::move(script));
        const auto* observer = model.get();
        std::atomic<unsigned> approvalRequests{0};
        MaiAgent::Options options;
        options.defaultModel = "test-model";
        options.approvalPolicy = policy;
        options.permissionTimeoutMs = 30;
        MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
        agent.eventBus().subscribe([&](const MaiEvent& event) {
            if (event.type == MaiEventType::PermissionAsked) ++approvalRequests;
        });
        const auto session =
            agent.submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(), "", ""});
        CHECK(session.isOk());
        if (!session.isOk()) continue;
        CHECK(agent.submit(MaiSendPrompt{session.value(), "Read current public news."}).isOk());
        agent.waitIdle();

        CHECK(approvalRequests == 0 && agent.listPendingPermissions().empty());
        CHECK(server.requests().size() == 3 && server.keyReads() == 3);
        CHECK(observer->requestCount() == 4);
        const auto requests = server.requests();
        if (requests.size() == 3) {
            CHECK(requests[0].path == "/v1/tools/search" && requests[0].body["count"] == 15);
            CHECK(requests[1].path == "/v1/tools/fetch");
            CHECK(requests[2].path == "/v1/tools/search");
        }
    }
}

void test_full_access_keeps_paid_media_confirmation() {
    // 使用实际媒体工具和空凭据，确认只读检索的修复没有放宽付费提交闸门。
    // 在权限事件中直接拒绝；即使回归使闸门失效，空凭据也保证不会发送真实付费请求。
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(makeMaiSeedreamImageTool([] { return std::string(); }));
    MaiFakeModelClient::Turn submit;
    submit.invocations.push_back(
        {"call_paid", "seedream_image", R"({"action":"delegate","message":"Create an image"})"});
    MaiFakeModelClient::Turn done;
    done.textChunks = {"Skipped."};
    auto model = std::make_unique<MaiFakeModelClient>(
        std::vector<MaiFakeModelClient::Turn>{std::move(submit), std::move(done)});
    std::atomic<unsigned> approvalRequests{0};
    MaiAgent::Options options;
    options.defaultModel = "test-model";
    options.approvalPolicy = MaiApprovalPolicy::Never;
    options.permissionTimeoutMs = 30;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type != MaiEventType::PermissionAsked) return;
        ++approvalRequests;
        const auto pending = agent.listPendingPermissions();
        CHECK(pending.size() == 1);
        if (pending.empty()) return;
        CHECK(pending[0].toolName == "seedream_image" && !pending[0].allowForSession);
        CHECK(
            agent.submit(MaiReplyPermission{pending[0].id, MaiPermissionDecision::Denied}).isOk());
    });
    const auto session =
        agent.submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(), "", ""});
    CHECK(session.isOk());
    if (!session.isOk()) return;
    CHECK(agent.submit(MaiSendPrompt{session.value(), "Create an image."}).isOk());
    agent.waitIdle();
    CHECK(approvalRequests == 1 && agent.listPendingPermissions().empty());
}

void test_redirect_never_forwards_authorization() {
    MaiDeepSeekFakeProvider destination;
    MaiDeepSeekFakeProvider provider;
    MaiDeepSeekWebClient client(provider.options());
    for (const int status : {301, 302, 307, 308}) {
        provider.fetchReplies({{status, "{}", destination.address() + "/collected", 0}});
        const auto response = client.fetch("https://example.invalid/page");
        CHECK(response.error.httpStatus == status && response.attempts == 1);
        CHECK(destination.requests().empty());
    }
}

#if defined(MAI_DEEPSEEK_WEB_TLS_TESTS)
class MaiWebTestCertificate {
public:
    MaiWebTestCertificate()
        : mDirectory(MaiFileSystem::temporaryDirectory().append(
              MaiFilePath::fromUtf8(MaiIdGenerator::generate("web_tls_test_")))) {}
    ~MaiWebTestCertificate() {
        MaiFileSystem::removeRecursively(mDirectory);
    }
    bool prepare(bool matchingHost) {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* raw = nullptr;
        if (!generator || EVP_PKEY_keygen_init(generator.get()) != 1 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) != 1 ||
            EVP_PKEY_keygen(generator.get(), &raw) != 1)
            return false;
        key.reset(raw);
        certificate.reset(X509_new());
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
            !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
            !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
            X509_set_pubkey(certificate.get(), key.get()) != 1)
            return false;
        auto* name = X509_get_subject_name(certificate.get());
        if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>("Loopback test"), -1,
                                       -1, 0) != 1 ||
            X509_set_issuer_name(certificate.get(), name) != 1)
            return false;
        X509V3_CTX context{};
        X509V3_set_ctx(&context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        char authority[] = "critical,CA:TRUE";
        char address[] = "IP:127.0.0.1";
        char different[] = "DNS:wrong-host.invalid";
        for (const auto& extension : std::vector<std::pair<int, char*>>{
                 {NID_basic_constraints, authority},
                 {NID_subject_alt_name, matchingHost ? address : different}}) {
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> value(
                X509V3_EXT_conf_nid(nullptr, &context, extension.first, extension.second),
                X509_EXTENSION_free);
            if (!value || X509_add_ext(certificate.get(), value.get(), -1) != 1) return false;
        }
        if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) return false;
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
        if (!output || PEM_write_bio_X509(output.get(), certificate.get()) != 1) return false;
        char* bytes = nullptr;
        const auto length = BIO_get_mem_data(output.get(), &bytes);
        if (length <= 0 || MaiFileSystem::createDirectories(mDirectory)) return false;
        mAuthority = mDirectory.append(MaiFilePath::fromUtf8("public-ca.pem"));
        // 仅公有证书落临时盘供 CURLOPT_CAINFO 读取；私钥始终留在测试内存。
        return !MaiFileSystem::writeFile(mAuthority,
                                         std::string(bytes, static_cast<std::size_t>(length)));
    }
    std::string authorityPath() const {
        return mAuthority.toUtf8();
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::unique_ptr<X509, decltype(&X509_free)> certificate{nullptr, X509_free};

private:
    MaiFilePath mDirectory, mAuthority;
};

void test_tls_peer_hostname_and_https_to_http_redirect() {
    MaiWebTestCertificate certificate;
    CHECK(certificate.prepare(true));
    if (!certificate.certificate) return;
    auto server =
        std::make_unique<httplib::SSLServer>(certificate.certificate.get(), certificate.key.get());
    CHECK(server->is_valid());
    if (!server->is_valid()) return;
    MaiDeepSeekFakeProvider provider(std::move(server), true);
    auto options = provider.options();
    options.caBundlePath = certificate.authorityPath();
    CHECK(MaiDeepSeekWebClient(options)
              .search(searchRequest("trusted loopback TLS"))
              .error.code.empty());
    CHECK(provider.requests().size() == 1 && provider.requests()[0].authenticated);
    auto untrusted = options;
    untrusted.caBundlePath.clear();
    const auto rejected =
        MaiDeepSeekWebClient(untrusted).search(searchRequest("untrusted loopback TLS"));
    CHECK(rejected.error.code == "TLS_FAILURE" && rejected.attempts == 1);
    CHECK(provider.requests().size() == 1);
    MaiDeepSeekFakeProvider destination;
    provider.fetchReplies({{302, "{}", destination.address() + "/collected", 0}});
    const auto redirect = MaiDeepSeekWebClient(options).fetch("https://example.invalid/article");
    CHECK(redirect.error.httpStatus == 302 && redirect.attempts == 1);
    CHECK(destination.requests().empty());
    MaiWebTestCertificate mismatch;
    CHECK(mismatch.prepare(false));
    auto wrongServer =
        std::make_unique<httplib::SSLServer>(mismatch.certificate.get(), mismatch.key.get());
    MaiDeepSeekFakeProvider wrong(std::move(wrongServer), true);
    auto wrongOptions = wrong.options();
    wrongOptions.caBundlePath = mismatch.authorityPath();
    const auto hostname =
        MaiDeepSeekWebClient(wrongOptions).search(searchRequest("hostname validation"));
    CHECK(hostname.error.code == "TLS_FAILURE" && hostname.attempts == 1 &&
          wrong.requests().empty());
    std::printf(
        "Local TLS peer/hostname verification and HTTPS redirect credential boundary passed\n");
}
#endif
}  // namespace

int main() {
    MaiThread::setCurrentName("mai-web-tests");
    // 只改本测试进程，loopback 假服务不交给环境代理，避免任何外网请求。
#if defined(_WIN32)
    _putenv_s("NO_PROXY", "*");
#else
    setenv("NO_PROXY", "*", 1);
    setenv("no_proxy", "*", 1);
#endif
    test_unicode_operators_unknown_metadata_and_empty_results();
    test_invalid_typed_inputs_never_read_credentials_or_send_requests();
    test_localization_uses_a_known_ip_or_is_omitted();
    test_response_schema_numeric_ranges_and_fetch_markdown_failures();
    test_bounded_retries_http_stage_and_recovery();
    test_cancellation_and_response_byte_limit();
    test_tools_share_the_client_contract_and_strict_read_only_protocol();
    test_read_only_retrieval_runs_without_approval_under_every_policy();
    test_full_access_keeps_paid_media_confirmation();
    test_redirect_never_forwards_authorization();
#if defined(MAI_DEEPSEEK_WEB_TLS_TESTS)
    test_tls_peer_hostname_and_https_to_http_redirect();
#else
    std::printf("SKIP local TLS: SSL test target support was not configured\n");
#endif
    std::printf("MaiDeepSeekWebToolsTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

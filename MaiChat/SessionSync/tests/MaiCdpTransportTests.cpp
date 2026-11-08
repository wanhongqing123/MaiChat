// 文件作用：以本机 WebSocket fixture 验证普通 CDP 通道，不启动浏览器或访问外部网络。
#include "MaiCdpTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include "MaiThread.h"

static int failures = 0;
#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                              \
        }                                                                            \
    } while (false)

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Server = websocketpp::server<websocketpp::config::asio>;

// 单个期限覆盖实现不返回、连接线程或 fixture 收尾挂起；只终止这个自有测试进程。
class MaiCdpTestDeadline {
public:
    MaiCdpTestDeadline()
        : mWorker([this] {
              MaiThread::setCurrentName("mai-cdp-limit");
              std::unique_lock<std::mutex> lock(mMutex);
              if (!mChanged.wait_for(lock, std::chrono::seconds(50), [this] { return mFinished; })) {
                  std::fprintf(stderr, "FAIL CDP tests exceeded their 50 second deadline\n");
                  std::fflush(stderr);
                  std::_Exit(1);
              }
          }) {}

    ~MaiCdpTestDeadline() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFinished = true;
        }
        mChanged.notify_one();
        mWorker.join();
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    bool mFinished = false;
    std::thread mWorker;
};

enum class ReplyMode { Matched, Fragmented, Error, Ignore, Close, InvalidJson, Large, FragmentedLarge };

class MaiCdpLoopbackFixture {
public:
    explicit MaiCdpLoopbackFixture(ReplyMode mode, std::size_t payloadBytes = 0)
        : mMode(mode), mPayloadBytes(payloadBytes) {
        mServer.clear_access_channels(websocketpp::log::alevel::all);
        mServer.clear_error_channels(websocketpp::log::elevel::all);
        mServer.init_asio();
        mServer.set_open_handshake_timeout(1000);
        mServer.set_close_handshake_timeout(100);
        mServer.set_max_message_size(34u * 1024u * 1024u);
        mServer.set_open_handler([this](websocketpp::connection_hdl) {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                ++mConnections;
            }
            mChanged.notify_all();
        });
        mServer.set_message_handler([this](websocketpp::connection_hdl handle, Server::message_ptr message) {
            const Json request = Json::parse(message->get_payload(), nullptr, false);
            if (!request.is_object() || !request.value("id", Json{}).is_number_unsigned() ||
                !request.value("method", Json{}).is_string() || !request.value("params", Json{}).is_object()) {
                mHealthy.store(false);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mRequests.push_back(request);
            }
            mChanged.notify_all();
            reply(handle, request["id"]);
        });
        websocketpp::lib::error_code error;
        mServer.listen(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
        if (error) {
            mHealthy.store(false);
            return;
        }
        mPort = mServer.get_local_endpoint(error).port();
        if (error || mPort == 0) {
            mHealthy.store(false);
            return;
        }
        mServer.start_accept(error);
        if (error) {
            mHealthy.store(false);
            return;
        }
        mWorker = std::thread([this] {
            MaiThread::setCurrentName("mai-cdp-server");
            try {
                mServer.run();
            } catch (...) {
                mHealthy.store(false);
            }
        });
    }

    ~MaiCdpLoopbackFixture() {
        websocketpp::lib::error_code error;
        mServer.stop_listening(error);
        mServer.stop();
        if (mWorker.joinable()) mWorker.join();
    }

    std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(mPort) + "/devtools/page/owned-fixture";
    }
    bool isHealthy() const {
        return mPort != 0 && mHealthy.load();
    }
    bool waitForCommands(std::size_t count) {
        std::unique_lock<std::mutex> lock(mMutex);
        return mChanged.wait_for(lock, std::chrono::milliseconds(600), [&] { return mRequests.size() >= count; });
    }
    std::size_t commands() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mRequests.size();
    }
    int connections() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mConnections;
    }
    Json request(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mMutex);
        return index < mRequests.size() ? mRequests[index] : Json{};
    }

private:
    void send(websocketpp::connection_hdl handle, const Json& value) {
        websocketpp::lib::error_code error;
        mServer.send(handle, value.dump(), websocketpp::frame::opcode::text, error);
        if (error) mHealthy.store(false);
    }

    void reply(websocketpp::connection_hdl handle, const Json& id) {
        if (mMode == ReplyMode::Ignore) return;
        if (mMode == ReplyMode::Close) {
            websocketpp::lib::error_code error;
            mServer.close(handle, websocketpp::close::status::normal, "fixture closed", error);
            if (error) mHealthy.store(false);
            return;
        }
        if (mMode == ReplyMode::InvalidJson) {
            websocketpp::lib::error_code error;
            mServer.send(handle, "not-json", websocketpp::frame::opcode::text, error);
            if (error) mHealthy.store(false);
            return;
        }
        if (mMode == ReplyMode::Error) {
            send(handle, Json{{"id", id}, {"error", {{"code", -32000}, {"message", "owned fixture CDP error"}}}});
            return;
        }
        const Json result = (mMode == ReplyMode::Large || mMode == ReplyMode::FragmentedLarge)
                                ? Json{{"payload", std::string(mPayloadBytes, 'x')}}
                                : Json{{"value", "owned fixture result"}};
        const std::string body = Json{{"id", id}, {"result", result}}.dump();
        if (mMode == ReplyMode::Fragmented || mMode == ReplyMode::FragmentedLarge) {
            websocketpp::lib::error_code error;
            auto connection = mServer.get_con_from_hdl(handle, error);
            if (error) {
                mHealthy.store(false);
                return;
            }
            const auto split = body.size() / 2;
            auto first = connection->get_message(websocketpp::frame::opcode::text, split);
            first->set_payload(body.substr(0, split));
            first->set_fin(false);
            error = connection->send(first);
            if (error) {
                mHealthy.store(false);
                return;
            }
            auto last = connection->get_message(websocketpp::frame::opcode::continuation, body.size() - split);
            last->set_payload(body.substr(split));
            last->set_fin(true);
            error = connection->send(last);
            if (error) mHealthy.store(false);
            return;
        }
        if (mMode == ReplyMode::Matched) {
            send(handle, Json{{"method", "Page.fixtureNotification"}, {"params", Json::object()}});
            send(handle, Json{{"id", id.get<std::uint64_t>() + 100}, {"result", {{"value", "wrong id"}}}});
        }
        send(handle, Json{{"id", id}, {"result", result}});
    }

    ReplyMode mMode;
    std::size_t mPayloadBytes;
    Server mServer;
    std::thread mWorker;
    unsigned short mPort = 0;
    std::atomic<bool> mHealthy{true};
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    int mConnections = 0;
    std::vector<Json> mRequests;
};

// TCP 已接受连接但不发送 Upgrade 回复，独立验证连接期限，不依赖不可达公网地址。
class MaiCdpHandshakeBlackhole {
public:
    MaiCdpHandshakeBlackhole()
        : mAcceptor(mIo, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)), mSocket(mIo) {
        mPort = mAcceptor.local_endpoint().port();
        mAcceptor.async_accept(mSocket, [this](const asio::error_code& error) {
            if (!error) mAccepted.store(true);
        });
        mWorker = std::thread([this] {
            MaiThread::setCurrentName("mai-cdp-stall");
            mIo.run();
        });
    }
    ~MaiCdpHandshakeBlackhole() {
        mIo.stop();
        if (mWorker.joinable()) mWorker.join();
    }
    std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(mPort) + "/devtools/page/blackhole";
    }
    bool accepted() const {
        return mAccepted.load();
    }

private:
    asio::io_service mIo;
    asio::ip::tcp::acceptor mAcceptor;
    asio::ip::tcp::socket mSocket;
    std::thread mWorker;
    unsigned short mPort = 0;
    std::atomic<bool> mAccepted{false};
};

long long millisecondsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

void test_pairing_and_fragments() {
    for (const ReplyMode mode : {ReplyMode::Matched, ReplyMode::Fragmented}) {
        MaiCdpLoopbackFixture fixture(mode);
        CHECK(fixture.isHealthy());
        MaiCdpTransport transport;
        const auto result = transport.call(fixture.url(), "Runtime.evaluate", R"({"expression":"owned fixture"})");
        if (!result) std::fprintf(stderr, "pairing call failed: %s\n", result.error().message().c_str());
        CHECK(result.isOk());
        if (result) CHECK(Json::parse(result.value(), nullptr, false) == Json({{"value", "owned fixture result"}}));
        CHECK(fixture.commands() == 1);
        CHECK(fixture.connections() == 1);
        const Json request = fixture.request(0);
        if (request.is_object()) {
            CHECK(request["method"] == "Runtime.evaluate");
            CHECK(request["params"] == Json({{"expression", "owned fixture"}}));
        }
        CHECK(fixture.isHealthy());
    }
}

void test_input_validation_does_not_connect() {
    MaiCdpLoopbackFixture fixture(ReplyMode::Matched);
    MaiCdpTransport transport;
    for (const std::string url : {"", "ws://localhost:9/page", "wss://127.0.0.1:9/page", "ws://192.0.2.1:9/page"}) {
        CHECK(transport.call(url, "Runtime.evaluate").error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(transport.call(fixture.url(), "").error().code() == MaiErrorCode::InvalidInput);
    CHECK(transport.call(fixture.url(), "Runtime.evaluate", "not-json").error().code() == MaiErrorCode::InvalidInput);
    CHECK(transport.call(fixture.url(), "Runtime.evaluate", "[]").error().code() == MaiErrorCode::InvalidInput);
    CHECK(fixture.connections() == 0);
    MaiCdpTransport::Options invalid;
    invalid.connectTimeoutMs = 0;
    CHECK(MaiCdpTransport(invalid).call(fixture.url(), "Runtime.evaluate").error().code() ==
          MaiErrorCode::InvalidInput);
}

void test_errors_and_disconnect_do_not_replay() {
    for (const ReplyMode mode : {ReplyMode::Error, ReplyMode::InvalidJson, ReplyMode::Close}) {
        MaiCdpLoopbackFixture fixture(mode);
        MaiCdpTransport transport;
        const auto result = transport.call(fixture.url(), "Runtime.evaluate");
        CHECK(!result);
        CHECK(result.error().code() == (mode == ReplyMode::Close ? MaiErrorCode::Network : MaiErrorCode::Protocol));
        if (mode == ReplyMode::Error)
            CHECK(result.error().message().find("owned fixture CDP error") != std::string::npos);
        CHECK(fixture.commands() == 1);
        CHECK(fixture.connections() == 1);
        transport.stop();
        CHECK(fixture.commands() == 1);
    }
}

void test_connection_and_response_deadlines() {
    MaiCdpTransport::Options options;
    options.connectTimeoutMs = 150;
    options.responseTimeoutMs = 150;
    {
        MaiCdpHandshakeBlackhole fixture;
        MaiCdpTransport transport(options);
        const auto start = Clock::now();
        const auto result = transport.call(fixture.url(), "Runtime.evaluate");
        CHECK(result.error().code() == MaiErrorCode::Network);
        CHECK(fixture.accepted());
        CHECK(millisecondsSince(start) >= 50 && millisecondsSince(start) < 2000);
    }
    {
        MaiCdpLoopbackFixture fixture(ReplyMode::Ignore);
        MaiCdpTransport transport(options);
        const auto start = Clock::now();
        const auto result = transport.call(fixture.url(), "Runtime.evaluate");
        CHECK(result.error().code() == MaiErrorCode::Network);
        CHECK(fixture.commands() == 1);
        CHECK(millisecondsSince(start) >= 50 && millisecondsSince(start) < 2000);
    }
}

void test_message_limits_cover_large_cdp_images() {
    CHECK(MaiCdpTransport::Options{}.maximumMessageBytes == 32u * 1024u * 1024u);
    {
        // 大于 20 MiB PNG 的 Base64 体积，防止误用 SessionSync 的 1 MiB 接收上限。
        constexpr std::size_t payloadBytes = 28u * 1024u * 1024u;
        MaiCdpLoopbackFixture fixture(ReplyMode::Large, payloadBytes);
        MaiCdpTransport transport;
        const auto result = transport.call(fixture.url(), "Page.captureScreenshot");
        CHECK(result.isOk());
        if (result) {
            const Json value = Json::parse(result.value(), nullptr, false);
            CHECK(value.is_object() && value.value("payload", Json{}).is_string());
            if (value.is_object() && value.value("payload", Json{}).is_string()) {
                const auto& payload = value["payload"].get_ref<const std::string&>();
                CHECK(payload.size() == payloadBytes);
                CHECK(payload.find_first_not_of('x') == std::string::npos);
            }
        }
    }
    {
        MaiCdpLoopbackFixture fixture(ReplyMode::FragmentedLarge, 6000);
        MaiCdpTransport::Options options;
        options.maximumMessageBytes = 4096;
        MaiCdpTransport transport(options);
        const auto result = transport.call(fixture.url(), "Runtime.evaluate");
        CHECK(result.error().code() == MaiErrorCode::Protocol);
        CHECK(fixture.commands() == 1);
    }
    {
        MaiCdpLoopbackFixture fixture(ReplyMode::Matched);
        MaiCdpTransport::Options options;
        options.maximumMessageBytes = 128;
        MaiCdpTransport transport(options);
        const auto result =
            transport.call(fixture.url(), "Runtime.evaluate", Json{{"expression", std::string(512, 'x')}}.dump());
        CHECK(result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(fixture.commands() == 0);
    }
}

void test_cancel_stop_and_concurrent_call() {
    {
        MaiCdpLoopbackFixture fixture(ReplyMode::Matched);
        MaiCdpTransport transport;
        const auto result = transport.call(fixture.url(), "Runtime.evaluate", "{}", [] { return true; });
        CHECK(result.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.connections() == 0);
    }
    for (const bool useStop : {false, true}) {
        MaiCdpLoopbackFixture fixture(ReplyMode::Ignore);
        MaiCdpTransport transport;
        std::atomic<bool> canceled{false};
        std::atomic<bool> returned{false};
        std::atomic<int> lateCancellationQueries{0};
        std::optional<MaiResult<std::string>> result;
        std::thread caller([&] {
            MaiThread::setCurrentName("mai-cdp-caller");
            result.emplace(transport.call(fixture.url(), "Runtime.evaluate", "{}", [&] {
                if (returned.load()) ++lateCancellationQueries;
                return canceled.load();
            }));
        });
        const bool commandReceived = fixture.waitForCommands(1);
        CHECK(commandReceived);
        if (commandReceived) {
            const auto concurrent = transport.call(fixture.url(), "Runtime.evaluate");
            CHECK(concurrent.error().code() == MaiErrorCode::Busy);
        }
        const auto start = Clock::now();
        if (useStop)
            transport.stop();
        else
            canceled.store(true);
        caller.join();
        returned.store(true);
        CHECK(millisecondsSince(start) < 2000);
        CHECK(result.has_value());
        if (result) CHECK(result->error().code() == MaiErrorCode::Canceled);
        transport.stop();
        transport.stop();
        CHECK(lateCancellationQueries.load() == 0);
        CHECK(fixture.commands() == 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool pairingOnly = argc == 2 && std::string(argv[1]) == "--pairing-only";
    if (argc > 1 && !pairingOnly) {
        std::fprintf(stderr, "usage: MaiCdpTransportTests [--pairing-only]\n");
        return 2;
    }
    MaiCdpTestDeadline deadline;
    test_pairing_and_fragments();
    if (!pairingOnly) {
        test_input_validation_does_not_connect();
        test_errors_and_disconnect_do_not_replay();
        test_connection_and_response_deadlines();
        test_message_limits_cover_large_cdp_images();
        test_cancel_stop_and_concurrent_call();
    }
    if (failures == 0) std::printf("CDP transport tests passed\n");
    return failures == 0 ? 0 : 1;
}

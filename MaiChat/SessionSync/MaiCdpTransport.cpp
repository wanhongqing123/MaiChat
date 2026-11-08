// 文件作用：通过已有 WebSocket++/Asio 执行一次本机 CDP 命令；不依赖 Qt 或账号重连策略。
#include "MaiCdpTransport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>

#include "MaiThread.h"

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Client = websocketpp::client<websocketpp::config::asio_client>;

// 每次 call 拥有独立的端点、线程及结果。回调只持弱引用，不借用调用栈或公开对象地址。
struct MaiCdpCall {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<MaiResult<std::string>> result;
    bool opened = false;
    Clock::time_point responseStarted;
    std::uint64_t requestId = 0;
    std::string command;
    std::size_t maximumMessageBytes = 0;

    // 启动和 join 共用一个锁，防止 stop 返回后，尚未完成启动的 call 又建立新线程。
    std::mutex lifecycleMutex;
    std::shared_ptr<Client> endpoint;
    std::thread worker;
};

void complete(const std::shared_ptr<MaiCdpCall>& call, MaiResult<std::string> result) {
    {
        std::lock_guard<std::mutex> lock(call->mutex);
        if (call->result) return;
        call->result.emplace(std::move(result));
    }
    call->changed.notify_all();
}

bool isComplete(const std::shared_ptr<MaiCdpCall>& call) {
    std::lock_guard<std::mutex> lock(call->mutex);
    return call->result.has_value();
}

void stopAndJoin(const std::shared_ptr<MaiCdpCall>& call) {
    std::lock_guard<std::mutex> lifecycle(call->lifecycleMutex);
    if (call->endpoint) call->endpoint->stop();
    if (call->worker.joinable()) call->worker.join();
    call->endpoint.reset();
}

bool validLocalUrl(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos || value.find_first_of("\r\n\t\\#") != std::string::npos)
        return false;
    const websocketpp::uri uri(value);
    if (!uri.get_valid() || uri.get_scheme() != "ws" || uri.get_port() == 0) return false;
    asio::error_code error;
    const auto address = asio::ip::make_address(uri.get_host(), error);
    return !error && address.is_loopback();
}

void connectionEnded(const std::shared_ptr<MaiCdpCall>& call, const std::weak_ptr<Client>& weakEndpoint,
                     websocketpp::connection_hdl handle) {
    const auto endpoint = weakEndpoint.lock();
    if (endpoint) {
        websocketpp::lib::error_code error;
        const auto connection = endpoint->get_con_from_hdl(handle, error);
        if (!error && connection && connection->get_local_close_code() == websocketpp::close::status::message_too_big) {
            complete(call, {MaiErrorCode::Protocol, "CDP reply exceeds the message limit"});
            return;
        }
    }
    complete(call, {MaiErrorCode::Network, "CDP connection closed before a matching reply"});
}

void receiveReply(const std::shared_ptr<MaiCdpCall>& call, Client::message_ptr message) {
    if (isComplete(call)) return;
    if (message->get_opcode() != websocketpp::frame::opcode::text) {
        complete(call, {MaiErrorCode::Protocol, "CDP reply must be a text message"});
        return;
    }
    const std::string& payload = message->get_payload();
    if (payload.size() > call->maximumMessageBytes) {
        complete(call, {MaiErrorCode::Protocol, "CDP reply exceeds the message limit"});
        return;
    }
    const Json reply = Json::parse(payload, nullptr, false);
    if (!reply.is_object()) {
        complete(call, {MaiErrorCode::Protocol, "CDP reply is not a JSON object"});
        return;
    }
    const auto id = reply.find("id");
    // 事件通知没有 ID；其他命令的回复也不能完成当前请求。
    if (id == reply.end() || !id->is_number_integer() || *id != call->requestId) return;
    const auto error = reply.find("error");
    if (error != reply.end()) {
        complete(call, {MaiErrorCode::Protocol, error->dump()});
        return;
    }
    const auto result = reply.find("result");
    if (result == reply.end() || !result->is_object()) {
        complete(call, {MaiErrorCode::Protocol, "CDP reply has no result object"});
        return;
    }
    complete(call, result->dump());
}

void start(const std::shared_ptr<MaiCdpCall>& call, const std::string& url, const MaiCdpTransport::Options& options) {
    std::lock_guard<std::mutex> lifecycle(call->lifecycleMutex);
    if (isComplete(call)) return;
    auto endpoint = std::make_shared<Client>();
    endpoint->clear_access_channels(websocketpp::log::alevel::all);
    endpoint->clear_error_channels(websocketpp::log::elevel::all);
    endpoint->init_asio();
    endpoint->set_max_message_size(options.maximumMessageBytes);
    endpoint->set_open_handshake_timeout(options.connectTimeoutMs);
    endpoint->set_close_handshake_timeout(250);
    const std::weak_ptr<MaiCdpCall> weakCall(call);
    const std::weak_ptr<Client> weakEndpoint(endpoint);
    endpoint->set_open_handler([weakCall, weakEndpoint](websocketpp::connection_hdl handle) {
        const auto active = weakCall.lock();
        const auto connection = weakEndpoint.lock();
        if (!active || !connection) return;
        {
            std::lock_guard<std::mutex> lock(active->mutex);
            if (active->result || active->opened) return;
            active->opened = true;
            active->responseStarted = Clock::now();
        }
        active->changed.notify_all();
        websocketpp::lib::error_code error;
        // 每个端点只在首次握手成功后发送一次，不重连，也不在失败后重发业务命令。
        connection->send(handle, active->command, websocketpp::frame::opcode::text, error);
        if (error) complete(active, {MaiErrorCode::Network, "CDP command could not be sent"});
    });
    endpoint->set_message_handler([weakCall](websocketpp::connection_hdl, Client::message_ptr message) {
        if (const auto active = weakCall.lock()) receiveReply(active, std::move(message));
    });
    const auto ended = [weakCall, weakEndpoint](websocketpp::connection_hdl handle) {
        if (const auto active = weakCall.lock()) connectionEnded(active, weakEndpoint, handle);
    };
    endpoint->set_close_handler(ended);
    endpoint->set_fail_handler(ended);
    websocketpp::lib::error_code error;
    const auto connection = endpoint->get_connection(url, error);
    if (error) {
        complete(call, {MaiErrorCode::Network, "CDP WebSocket connection could not be created"});
        return;
    }
    call->endpoint = endpoint;
    endpoint->connect(connection);
    call->worker = std::thread([endpoint, weakCall] {
        MaiThread::setCurrentName("mai-cdp-ws");
        try {
            endpoint->run();
        } catch (const std::exception&) {
            if (const auto active = weakCall.lock())
                complete(active, {MaiErrorCode::Internal, "CDP WebSocket worker failed"});
        } catch (...) {
            if (const auto active = weakCall.lock())
                complete(active, {MaiErrorCode::Internal, "CDP WebSocket worker failed"});
        }
        if (const auto active = weakCall.lock())
            complete(active, {MaiErrorCode::Network, "CDP connection ended before a matching reply"});
    });
}

}  // namespace

struct MaiCdpTransport::Runtime {
    explicit Runtime(Options value) : options(value) {}

    const Options options;
    std::mutex mutex;
    std::shared_ptr<MaiCdpCall> active;
    std::uint64_t nextRequestId = 1;
};

MaiCdpTransport::MaiCdpTransport() : MaiCdpTransport(Options{}) {}

MaiCdpTransport::MaiCdpTransport(Options options) : mRuntime(std::make_shared<Runtime>(options)) {}

MaiCdpTransport::~MaiCdpTransport() {
    stop();
}

MaiResult<std::string> MaiCdpTransport::call(const std::string& socketUrl, const std::string& method,
                                             const std::string& parametersJson, std::function<bool()> isCanceled) {
    const auto runtime = mRuntime;
    const auto& options = runtime->options;
    if (options.connectTimeoutMs <= 0 || options.responseTimeoutMs <= 0 || options.maximumMessageBytes == 0 ||
        socketUrl.size() > options.maximumMessageBytes || !validLocalUrl(socketUrl) || method.empty() ||
        method.size() > options.maximumMessageBytes || parametersJson.size() > options.maximumMessageBytes)
        return {MaiErrorCode::InvalidInput,
                "CDP needs a local ws URL, method, positive deadlines and a bounded message"};
    const Json parameters = Json::parse(parametersJson, nullptr, false);
    if (!parameters.is_object()) return {MaiErrorCode::InvalidInput, "CDP parameters must be a JSON object"};
    if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "CDP command canceled"};

    const auto active = std::make_shared<MaiCdpCall>();
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (runtime->active) return {MaiErrorCode::Busy, "CDP transport already has an active command"};
        active->requestId = runtime->nextRequestId++;
        active->maximumMessageBytes = options.maximumMessageBytes;
        active->command = Json{{"id", active->requestId}, {"method", method}, {"params", parameters}}.dump();
        if (active->command.size() > options.maximumMessageBytes)
            return {MaiErrorCode::InvalidInput, "CDP command exceeds the message limit"};
        runtime->active = active;
    }
    const auto connectionDeadline = Clock::now() + std::chrono::milliseconds(options.connectTimeoutMs);
    try {
        start(active, socketUrl, options);
        while (!isComplete(active)) {
            if (isCanceled && isCanceled()) {
                complete(active, {MaiErrorCode::Canceled, "CDP command canceled"});
                break;
            }
            std::unique_lock<std::mutex> lock(active->mutex);
            if (active->result) break;
            const auto deadline = active->opened
                                      ? active->responseStarted + std::chrono::milliseconds(options.responseTimeoutMs)
                                      : connectionDeadline;
            if (Clock::now() >= deadline) {
                const bool opened = active->opened;
                lock.unlock();
                complete(active, {MaiErrorCode::Network,
                                  opened ? "CDP command timed out" : "CDP WebSocket handshake timed out"});
                break;
            }
            active->changed.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(10)));
        }
    } catch (const std::exception&) {
        complete(active, {MaiErrorCode::Internal, "CDP request failed"});
    } catch (...) {
        complete(active, {MaiErrorCode::Internal, "CDP request failed"});
    }
    stopAndJoin(active);
    MaiResult<std::string> result(MaiErrorCode::Internal, "CDP request ended without a result");
    {
        std::lock_guard<std::mutex> lock(active->mutex);
        if (active->result) result = std::move(*active->result);
    }
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (runtime->active == active) runtime->active.reset();
    }
    return result;
}

void MaiCdpTransport::stop() {
    std::shared_ptr<MaiCdpCall> active;
    {
        std::lock_guard<std::mutex> lock(mRuntime->mutex);
        active = mRuntime->active;
    }
    if (!active) return;
    complete(active, {MaiErrorCode::Canceled, "CDP command canceled"});
    stopAndJoin(active);
}

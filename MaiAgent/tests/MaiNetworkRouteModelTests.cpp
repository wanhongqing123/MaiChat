// 文件作用：用已接收 POST 的假服务端验证换网通知不会重投付费请求或中断仍可完成的流。
#include "MaiResponsesClient.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include <httplib.h>

#include "MaiCurlNetworkRoute.h"

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::cerr << __FILE__ << static_cast<char>(58) << __LINE__ << std::endl; \
            std::abort();                                                            \
        }                                                                            \
    } while (false)

int main() {
    uint64_t generation = 100;
    for (bool callerManagesRetries : {false, true}) {
        for (int phase : {0, 1, 2}) {
            httplib::Server server;
            std::atomic<int> requests{0};
            server.Post("/responses", [&](const httplib::Request&, httplib::Response& response) {
                ++requests;
                // The server has received the complete POST before publishing the route event.
                MaiCurlNetworkRoute::publish(0, ++generation);
                std::string data =
                    "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\"}}\n\n";
                if (phase >= 1)
                    data +=
                        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"kept\"}\n\n";
                if (phase == 2)
                    data +=
                        "data: "
                        "{\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":"
                        "\"completed\",\"output\":[]}}\n\n";
                response.set_content(data, "text/event-stream");
            });
            const int port = server.bind_to_any_port("127.0.0.1");
            CHECK(port > 0);
            std::thread worker([&] { server.listen_after_bind(); });
            while (!server.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            MaiModelConfig config;
            config.baseUrl = "http://127.0.0.1:" + std::to_string(port);
            config.maxRetries = 2;
            auto client = makeMaiResponsesClient(config);
            MaiModelRequest request;
            request.model = "route-test";
            request.callerManagesRetries = callerManagesRetries;
            request.messages.push_back({MaiModelRole::User, "synthetic"});
            std::string text;
            MaiStreamSink sink;
            sink.onText = [&](std::string_view delta) { text.append(delta); };
            const std::atomic<bool> cancel{false};
            const auto result = client->stream(request, sink, cancel);
            CHECK(requests == 1);
            if (phase < 2) {
                CHECK(result);
                CHECK(!result.retryAdvice());
            } else {
                CHECK(!result);
            }
            if (phase == 1) CHECK(text == "kept");
            server.stop();
            worker.join();
        }
    }
    MaiCurlNetworkRoute::publish(0, ++generation);
    std::cout << "network POST replay contracts passed\n";
}

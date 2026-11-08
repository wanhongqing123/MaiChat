#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "MaiChatTools/DesktopBrowserTools.h"
#include "MaiThread.h"
#include "MaiTool.h"

namespace {

#define CHECK(value)                                                                       \
    do {                                                                                   \
        if (!(value)) {                                                                    \
            std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
            return 1;                                                                      \
        }                                                                                  \
    } while (false)

struct FixtureServer {
    httplib::Server server;
    std::thread worker;
    int port = 0;

    FixtureServer() {
        // 使用同一份 vendored HTTP fixture；浏览器测试不依赖 Qt Network。
        server.Get("/page", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"(<!doctype html><html><head><meta name="viewport"
                content="width=device-width,initial-scale=1"><title>Agent Browser Fixture</title></head>
                <body style="background:white;font-size:22px"><h1>Agent Browser Fixture</h1>
                <input aria-label="Note" id="note"><input aria-label="Password" type="password">
                <button aria-label="Run" onclick="document.getElementById('result').textContent=
                'clicked:'+document.getElementById('note').value">Run</button>
                <p id="result">idle</p></body></html>)",
                                 "text/html; charset=utf-8");
        });
        port = server.bind_to_any_port("127.0.0.1");
        if (port > 0)
            worker = std::thread([this] {
                MaiThread::setCurrentName("mai-browser-http");
                server.listen_after_bind();
            });
    }
    ~FixtureServer() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
};

std::string element(const nlohmann::json& snapshot, const std::string& label) {
    for (const auto& value : snapshot["elements"])
        if (value.value("label", "") == label) return value["id"].get<std::string>();
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    FixtureServer server;
    const int port = server.port;
    CHECK(port > 0);
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/page";
    QTemporaryDir workspace;
    CHECK(workspace.isValid());
    auto manager = std::make_shared<DesktopBrowserManager>();
    MaiToolRegistry registry;
    registerDesktopBrowserTools(registry, manager);
    MaiTool* tool = registry.find("desktop_browser");
    CHECK(tool != nullptr);
    CHECK(nlohmann::json::parse(tool->parametersSchema()).is_object());
    CHECK(tool->requiresPerCallApproval(R"({"action":"open"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"click"})"));
    CHECK(!tool->requiresApproval(R"({"action":"observe"})"));

    // A canceled request must not launch a profile or acquire a browser Session.
    CHECK(manager->open("canceled-owner", url, [] { return true; }).code() == MaiErrorCode::Canceled);
    CHECK(manager->observe("canceled-owner").error().code() == MaiErrorCode::NotFound);

    const MaiError opened = manager->open("owner", url);
    if (opened) std::fprintf(stderr, "open failed: %s\n", opened.message().c_str());
    CHECK(!opened);
    CHECK(manager->observe("owner", [] { return true; }).error().code() == MaiErrorCode::Canceled);
    CHECK(manager->observe("other").error().code() == MaiErrorCode::NotFound);
    nlohmann::json first;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const auto observed = manager->observe("owner");
        if (observed) {
            const auto value = nlohmann::json::parse(observed.value());
            if (value.value("state", "") == "ready" && value.value("text", "").find("idle") != std::string::npos) {
                first = value;
                break;
            }
        }
        QThread::msleep(100);
    }
    CHECK(first.is_object());
    CHECK(first["url"] == url);
    const std::string note = element(first, "Note");
    const std::string password = element(first, "Password");
    CHECK(!note.empty() && !password.empty());
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "canceled text", [] { return true; })
              .code() == MaiErrorCode::Canceled);
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], password, "Password", "secret").code() ==
          MaiErrorCode::InvalidInput);
    CHECK(!manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "desktop OK"));
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "stale").code() ==
          MaiErrorCode::InvalidInput);
    const auto second = manager->observe("owner");
    CHECK(second);
    const auto next = nlohmann::json::parse(second.value());
    const std::string button = element(next, "Run");
    CHECK(!button.empty());
    CHECK(!manager->act("owner", "click", url, next["snapshot_id"], button, "Run", ""));
    bool clicked = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        const auto observed = manager->observe("owner");
        if (observed && observed.value().find("clicked:desktop OK") != std::string::npos) {
            clicked = true;
            break;
        }
        QThread::msleep(50);
    }
    CHECK(clicked);
    CHECK(manager
              ->screenshot("owner", "https://wrong.example/",
                           workspace.filePath(QStringLiteral("wrong.png")).toStdString())
              .code() == MaiErrorCode::InvalidInput);
    const QString image = workspace.filePath(QStringLiteral("page.png"));
    const QString canceledImage = workspace.filePath(QStringLiteral("canceled.png"));
    CHECK(manager->screenshot("owner", url, canceledImage.toUtf8().toStdString(), [] { return true; }).code() ==
          MaiErrorCode::Canceled);
    CHECK(!QFile::exists(canceledImage));
    const MaiError captured = manager->screenshot("owner", url, image.toUtf8().toStdString());
    if (captured) std::fprintf(stderr, "screenshot failed: %s\n", captured.message().c_str());
    CHECK(!captured && QFile::exists(image) && QFileInfo(image).size() > 100);
    const QString copy = qEnvironmentVariable("MAICHAT_BROWSER_TEST_SCREENSHOT");
    if (!copy.isEmpty()) {
        QFile::remove(copy);
        CHECK(QFile::copy(image, copy));
    }
    CHECK(!manager->close("owner"));
    CHECK(manager->observe("owner").error().code() == MaiErrorCode::NotFound);
    return 0;
}

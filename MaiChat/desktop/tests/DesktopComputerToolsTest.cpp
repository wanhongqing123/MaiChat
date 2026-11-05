#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cstdio>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

namespace {

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    return 1; } } while (false)

int unavailable(const char* reason) {
    std::fprintf(stderr, "computer test SKIP: %s; native click/type was not fully verified\n", reason);
    return 77;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#if defined(Q_OS_WIN)
    if (QApplication::platformName() != QStringLiteral("windows"))
        return unavailable("a real Windows QPA session is required");
#endif
    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>();
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool && nlohmann::json::parse(tool->parametersSchema()).is_object());
    CHECK(tool->requiresPerCallApproval(R"({"action":"observe"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"click"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"clear"})"));
    MaiToolContext absent;
    CHECK(tool->execute(R"({"action":"observe","window_title":"test"})", absent)
              .error().code() == MaiErrorCode::NotConfigured);
    CHECK(controller->act("no-observation", "click", "unknown", 1,
                          0.5, 0.5, "", "", 0).code() == MaiErrorCode::NotFound);
    QWidget window;
    // 不按固定标题寻找可能属于其它进程的窗口；原生输入只能落在本次测试自己的 fixture。
    const QString fixtureTitle = QStringLiteral("MaiChat Computer Tool Fixture %1")
                                     .arg(QApplication::applicationPid());
    window.setWindowTitle(fixtureTitle);
    auto* layout = new QVBoxLayout(&window);
    auto* result = new QLabel(QStringLiteral("Waiting"), &window);
    auto* input = new QLineEdit(&window);
    input->setAccessibleName(QStringLiteral("Fixture input"));
    auto* button = new QPushButton(QStringLiteral("Confirm"), &window);
    QObject::connect(button, &QPushButton::clicked, &window,
                     [result, input] { result->setText(input->text()); });
    layout->addWidget(result);
    layout->addWidget(input);
    layout->addWidget(button);
    window.resize(500, 320);
    window.show();
    app.processEvents();
    QThread::msleep(300);
    app.processEvents();
    const auto windows = maiDesktopWindows();
    if (!windows) {
        std::fprintf(stderr, "computer test unavailable: %s\n",
                     windows.error().message().c_str());
        CHECK(windows.error().code() == MaiErrorCode::NotConfigured);
        return unavailable("native window enumeration is not configured");
    }
    DesktopNativeWindow native;
    int count = 0;
    for (const auto& candidate : windows.value()) {
        if (candidate.title == fixtureTitle.toUtf8().toStdString()) {
            native = candidate;
            ++count;
        }
    }
    CHECK(count == 1);
    CHECK(native.processId == std::uint64_t(QApplication::applicationPid()));
#if defined(Q_OS_WIN)
    const HWND fixtureHandle = reinterpret_cast<HWND>(window.winId());
    CHECK(native.id == std::uint64_t(reinterpret_cast<std::uintptr_t>(fixtureHandle)));
    DWORD nativeProcess = 0;
    CHECK(GetWindowThreadProcessId(fixtureHandle, &nativeProcess) != 0);
    CHECK(nativeProcess == GetCurrentProcessId());
    std::fprintf(stderr, "computer own fixture: pid=%lu hwnd=%llu qpa=windows\n",
                 static_cast<unsigned long>(nativeProcess),
                 static_cast<unsigned long long>(native.id));
#endif
    QTemporaryDir workspace;
    CHECK(workspace.isValid());
    const auto observed = controller->observe("owner", native.title,
        workspace.path().toUtf8().toStdString(), true);
    if (!observed) {
        std::fprintf(stderr, "computer observe unavailable: %s\n",
                     observed.error().message().c_str());
        CHECK(observed.error().code() == MaiErrorCode::NotConfigured ||
              observed.error().code() == MaiErrorCode::NotSupported);
        return unavailable("native fixture observation is unavailable");
    }
    const auto snapshot = nlohmann::json::parse(observed.value().json);
    CHECK(snapshot["window_id"] == std::to_string(native.id));
    CHECK(QFileInfo(QString::fromStdString(observed.value().imagePath)).size() > 100);
    const QString copy = qEnvironmentVariable("MAICHAT_COMPUTER_TEST_SCREENSHOT");
    if (!copy.isEmpty()) {
        QFile::remove(copy);
        CHECK(QFile::copy(QString::fromStdString(observed.value().imagePath), copy));
    }
    CHECK(controller->act("other", "click", snapshot["snapshot_id"],
        native.id, 0.5, 0.5, "", "", 0).code() == MaiErrorCode::NotFound);
    CHECK(controller->act("owner", "click", "stale", native.id,
        0.5, 0.5, "", "", 0).code() == MaiErrorCode::InvalidInput);
#if defined(Q_OS_WIN)
    // 后台测试进程没有权限强抢前台。只激活已核对的自有窗口，等待真实系统状态后再发送输入。
    window.raise();
    window.activateWindow();
    if (window.windowHandle()) window.windowHandle()->requestActivate();
    const MaiError activation = maiDesktopActivateWindow(native);
    const std::string foregroundDenied = "Windows did not allow the selected window to become foreground";
    if (activation) {
        CHECK(activation.code() == MaiErrorCode::Busy);
        CHECK(activation.message() == foregroundDenied);
    }
    QElapsedTimer activationWait;
    activationWait.start();
    while (GetForegroundWindow() != fixtureHandle && activationWait.elapsed() < 1500) {
        app.processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(10);
    }
    if (GetForegroundWindow() != fixtureHandle) {
        CHECK(activation.code() == MaiErrorCode::Busy);
        CHECK(activation.message() == foregroundDenied);
        CHECK(input->text().isEmpty());
        CHECK(result->text() == QStringLiteral("Waiting"));
        return unavailable("Windows denied the verified fixture foreground; no mouse input was sent");
    }
#endif
    const QPoint inputCenter = input->mapToGlobal(input->rect().center());
    const double inputX = (inputCenter.x() - native.x) / native.width;
    const double inputY = (inputCenter.y() - native.y) / native.height;
    const MaiError clicked = controller->act("owner", "click",
        snapshot["snapshot_id"], native.id, inputX, inputY, "", "", 0);
    if (clicked) {
        std::fprintf(stderr, "computer click unavailable: %s\n", clicked.message().c_str());
#if defined(Q_OS_WIN)
        if (clicked.code() == MaiErrorCode::Busy && clicked.message() == foregroundDenied) {
            CHECK(GetForegroundWindow() != fixtureHandle);
            CHECK(input->text().isEmpty());
            return unavailable("verified fixture lost foreground before mouse injection");
        }
#endif
        CHECK(clicked.code() == MaiErrorCode::NotConfigured);
        return unavailable("native click is not configured");
    }
    QThread::msleep(150);
    app.processEvents();
    const auto selected = controller->observe("owner", native.title,
        workspace.path().toUtf8().toStdString(), false);
    CHECK(selected);
    const auto second = nlohmann::json::parse(selected.value().json);
    const MaiError typed = controller->act("owner", "type",
        second["snapshot_id"], native.id, 0, 0, "computer OK", "", 0);
    if (typed) {
        std::fprintf(stderr, "computer type unavailable: %s\n", typed.message().c_str());
#if defined(Q_OS_WIN)
        if (typed.code() == MaiErrorCode::Busy && typed.message() == foregroundDenied) {
            CHECK(GetForegroundWindow() != fixtureHandle);
            return unavailable("verified fixture lost foreground before text injection");
        }
#endif
        CHECK(typed.code() == MaiErrorCode::NotConfigured);
        return unavailable("native typing is not configured");
    }
    QEventLoop inputDelivery;
    QObject::connect(input, &QLineEdit::textChanged, &inputDelivery,
                     [&inputDelivery](const QString& text) {
                         if (text == QStringLiteral("computer OK")) inputDelivery.quit();
                     });
    QTimer::singleShot(2000, &inputDelivery, &QEventLoop::quit);
    if (input->text() != QStringLiteral("computer OK")) inputDelivery.exec();
    CHECK(input->text() == QStringLiteral("computer OK"));
    CHECK(!controller->clear("owner"));
    std::fprintf(stderr, "computer native input verified: own pid=%llu hwnd=%llu text=computer OK\n",
                 static_cast<unsigned long long>(native.processId),
                 static_cast<unsigned long long>(native.id));
    return 0;
}

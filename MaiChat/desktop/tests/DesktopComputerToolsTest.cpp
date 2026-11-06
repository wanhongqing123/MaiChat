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
    // Keep right-click delivery inside this fixture without opening a menu
    // that could take focus from the following controlled input checks.
    input->setContextMenuPolicy(Qt::CustomContextMenu);
    int contextRequests = 0;
    QObject::connect(input, &QLineEdit::customContextMenuRequested, &window,
                     [&contextRequests](const QPoint&) { ++contextRequests; });
    auto* button = new QPushButton(QStringLiteral("Confirm"), &window);
    QObject::connect(button, &QPushButton::clicked, &window,
                     [result, input] { result->setText(input->text()); });
    QObject::connect(input, &QLineEdit::returnPressed, &window,
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
        std::fprintf(stderr, "computer native click failed: %s\n", clicked.message().c_str());
    }
    // An input-capable call may partially dispatch before reporting failure.
    // Readiness skips end before this first native mouse attempt.
    CHECK(!clicked);
    QThread::msleep(150);
    app.processEvents();
    const auto selected = controller->observe("owner", native.title,
        workspace.path().toUtf8().toStdString(), false);
    CHECK(selected);
    const auto second = nlohmann::json::parse(selected.value().json);
    const MaiError typed = controller->act("owner", "type",
        second["snapshot_id"], native.id, 0, 0, "computer OK", "", 0);
    if (typed) {
        std::fprintf(stderr, "computer native type failed: %s\n", typed.message().c_str());
    }
    CHECK(!typed);
    QEventLoop inputDelivery;
    QObject::connect(input, &QLineEdit::textChanged, &inputDelivery,
                     [&inputDelivery](const QString& text) {
                         if (text == QStringLiteral("computer OK")) inputDelivery.quit();
                     });
    QTimer::singleShot(2000, &inputDelivery, &QEventLoop::quit);
    if (input->text() != QStringLiteral("computer OK")) inputDelivery.exec();
    CHECK(input->text() == QStringLiteral("computer OK"));

    // Every operation observes this exact self-owned ID again. The production
    // controller and native backend perform the actual Unicode/key/gesture
    // dispatch; direct widget state is used only to check what was delivered.
    const QString chineseDraft = QStringLiteral("电脑操作中文草稿");
    const QString selectedReplacement = QStringLiteral("组合键替换中文");
    const QString finalText = QStringLiteral("已验证中文输入与替换");
    const std::vector<std::string> actions = {
        "replace_text", "key", "type", "replace_text", "replace_text",
        "double_click", "right_click", "drag", "key", "verify"};
    for (std::size_t step = 0; step < actions.size(); ++step) {
        const auto fresh = controller->observe("owner", "",
            workspace.path().toUtf8().toStdString(), false, native.id);
        CHECK(fresh);
        const auto freshSnapshot = nlohmann::json::parse(fresh.value().json);
        DesktopComputerAction action;
        action.action = actions[step];
        action.snapshotId = freshSnapshot["snapshot_id"];
        action.windowId = native.id;
        action.xRatio = inputX;
        action.yRatio = inputY;
        if (step == 0) action.text = chineseDraft.toUtf8().toStdString();
        else if (step == 1) {
#if defined(Q_OS_MAC)
            action.key = "Cmd+A";
#else
            action.key = "Ctrl+A";
#endif
        }
        else if (step == 2) action.text = selectedReplacement.toUtf8().toStdString();
        else if (step == 4) action.text = finalText.toUtf8().toStdString();
        else if (step == 8) action.key = "Enter";
        else if (step == 9) action.expectedText = finalText.toUtf8().toStdString();
        if (step == 5 || step == 6 || step == 7) {
            const QPoint start = input->mapToGlobal(QPoint(12, input->height() / 2));
            const QPoint end = input->mapToGlobal(QPoint(130, input->height() / 2));
            action.xRatio = (start.x() - native.x) / native.width;
            action.yRatio = (start.y() - native.y) / native.height;
            action.toXRatio = (end.x() - native.x) / native.width;
            action.toYRatio = (end.y() - native.y) / native.height;
            action.durationMs = 100;
        }
        const auto performed = controller->perform("owner", action);
        if (!performed) {
            const auto& error = performed.error();
            std::fprintf(stderr, "computer native step %zu %s failed: %s\n",
                         step, action.action.c_str(), error.message().c_str());
        }
        CHECK(performed);
        QElapsedTimer delivery;
        delivery.start();
        while (delivery.elapsed() < 180) {
            app.processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(5);
        }
        if (step == 0 || step == 1) CHECK(input->text() == chineseDraft);
        if (step == 1) CHECK(input->selectedText() == chineseDraft);
        if (step == 2) CHECK(input->text() == selectedReplacement);
        if (step == 3) CHECK(input->text().isEmpty());
        if (step >= 4) CHECK(input->text() == finalText);
        if (step == 5 || step == 7) CHECK(!input->selectedText().isEmpty());
        if (step == 6) CHECK(contextRequests == 1);
        if (step >= 8) CHECK(result->text() == finalText);
        if (step == 9) {
            CHECK(performed.value().verified);
            CHECK(!performed.value().dispatched);
            CHECK(performed.value().actualText == finalText.toUtf8().toStdString());
        } else {
            CHECK(performed.value().dispatched);
            CHECK(!performed.value().verified);
        }
    }
    const auto finalObservation = controller->observe("owner", "",
        workspace.path().toUtf8().toStdString(), true, native.id);
    CHECK(finalObservation);
    if (!copy.isEmpty()) {
        QFile::remove(copy);
        CHECK(QFile::copy(QString::fromStdString(finalObservation.value().imagePath), copy));
    }
    CHECK(!controller->clear("owner"));
    std::fprintf(stderr, "computer native input verified: own pid=%llu hwnd=%llu Unicode/shortcuts/empty replacement/gestures/Enter/readback\n",
                 static_cast<unsigned long long>(native.processId),
                 static_cast<unsigned long long>(native.id));
    return 0;
}

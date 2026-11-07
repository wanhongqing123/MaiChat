#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <json.hpp>

#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"
#include "MaiAgentSendMediaTool.h"

namespace {

using Json = nlohmann::json;

int failures = 0;

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    ++failures; } } while (false)

bool contains(const Json& values, const char* expected) {
    if (!values.is_array()) return false;
    for (const auto& value : values) {
        if (value.is_string() && value.get<std::string>() == expected) return true;
    }
    return false;
}

// This backend supplies actual PNG data and records the controller's native
// boundary calls. It never enumerates, captures or injects into the user's OS.
class RecordingBackend final : public DesktopComputerBackend {
public:
    std::vector<DesktopNativeWindow> visible{
        {101, 5001, "Isolated editor", "Fixture", 100, 200, 400, 300}};
    std::vector<DesktopNativeInput> inputs;
    std::vector<std::string> events;
    std::string textValue;
    bool focused = true;
    bool secure = false;
    bool ownedPopup = false;
    bool requirePopupForKey = false;
    int activationCount = 0;
    int captureCount = 0;
    int readCount = 0;
    std::uint64_t capturedWindowId = 0;
    MaiError activationError;
    MaiError inputError;
    MaiError readError;
    std::function<void()> onActivate;
    std::function<void()> onCapture;

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        events.push_back("windows");
        return visible;
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) override {
        return std::vector<DesktopNativeElement>{
            {"text", "Isolated editor input", window.x + 20, window.y + 20, 100, 30}};
    }
    MaiError capture(const DesktopNativeWindow& window, MaiScreenshot& output) override {
        ++captureCount;
        capturedWindowId = window.id;
        if (onCapture) onCapture();
        QImage image(20, 20, QImage::Format_RGB32);
        image.fill(Qt::white);
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {MaiErrorCode::Internal, "fixture PNG encoding failed"};
        output.width = image.width();
        output.height = image.height();
        output.pngBytes.assign(png.begin(), png.end());
        return {};
    }
    MaiError activate(const DesktopNativeWindow&) override {
        if (ownedPopup) focused = true;
        ownedPopup = false;
        events.push_back("activate");
        ++activationCount;
        if (onActivate) onActivate();
        return activationError;
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        events.push_back("isFocused");
        return focused;
    }
    MaiError activateForInput(const DesktopNativeWindow& window) override {
        return ownedPopup ? MaiError{} : activate(window);
    }
    MaiResult<bool> inputFocused(const DesktopNativeWindow& window) override {
        return ownedPopup ? MaiResult<bool>(true) : isFocused(window);
    }
    MaiResult<bool> secureInput() override {
        events.push_back("secureInput");
        return secure;
    }
    MaiError input(const DesktopNativeInput& value) override {
        events.push_back("input");
        inputs.push_back(value);
        if (inputError) return inputError;
        if (value.kind == DesktopNativeInputKind::RightClick) { ownedPopup = true; focused = false; }
        if (value.kind == DesktopNativeInputKind::Key && requirePopupForKey && !ownedPopup)
            return {MaiErrorCode::Busy, "fixture popup was dismissed before its key input"};
        if (value.kind == DesktopNativeInputKind::ReplaceFocusedText) textValue = value.text;
        if (value.kind == DesktopNativeInputKind::Type) textValue += value.text;
        return {};
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        events.push_back("focusedText");
        ++readCount;
        if (readError) return readError;
        return textValue;
    }
};

struct Fixture {
    std::shared_ptr<RecordingBackend> backend = std::make_shared<RecordingBackend>();
    DesktopComputerController controller{backend};
    QTemporaryDir workspace;

    DesktopComputerAction observe(const std::string& kind = "click") {
        DesktopComputerAction action;
        action.action = kind;
        action.windowId = backend->visible.front().id;
        action.xRatio = 0.25;
        action.yRatio = 0.5;
        action.toXRatio = 0.75;
        action.toYRatio = 0.75;
        CHECK(workspace.isValid());
        const auto observed = controller.observe("owner", "",
            workspace.path().toUtf8().toStdString(), true, action.windowId);
        CHECK(observed);
        if (observed) {
            const Json output = Json::parse(observed.value().json, nullptr, false);
            CHECK(output.is_object());
            if (output.is_object()) action.snapshotId = output.value("snapshot_id", std::string());
            CHECK(!action.snapshotId.empty());
            CHECK(QFileInfo(QString::fromStdString(observed.value().imagePath)).size() > 0);
        }
        return action;
    }
};

void verifyOwnedPopupInput() {
    Fixture fixture;
    auto menu = fixture.observe("right_click");
    CHECK(fixture.controller.perform("owner", menu));
    CHECK(fixture.backend->ownedPopup);
    CHECK(!fixture.backend->focused);
    const int activated = fixture.backend->activationCount;
    fixture.backend->requirePopupForKey = true;
    auto key = fixture.observe("key");
    key.key = "ArrowDown";
    const auto sent = fixture.controller.perform("owner", key);
    CHECK(sent);
    if (sent) CHECK(sent.value().dispatched);
    CHECK(fixture.backend->ownedPopup);
    CHECK(fixture.backend->activationCount == activated);
    // A menu owner is valid for keyboard navigation, not for focused-value verification.
    auto verify = fixture.observe("verify");
    const auto checked = fixture.controller.perform("owner", verify);
    CHECK(!checked);
    if (!checked) CHECK(checked.error().code() == MaiErrorCode::Busy);
    CHECK(fixture.backend->readCount == 0);
}

void verifySecureKeyboardInput() {
    for (const char* key : {"A", "Space", "Backspace", "Delete", "Ctrl+V", "Shift+Insert"}) {
        Fixture fixture;
        auto action = fixture.observe("key");
        action.key = key;
        fixture.backend->secure = true;
        const auto result = fixture.controller.perform("owner", action);
        CHECK(!result);
        if (!result) CHECK(result.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->inputs.empty());
    }
    // Navigating away from a protected field remains available without writing it.
    for (const char* key : {"Tab", "Escape"}) {
        Fixture fixture;
        auto action = fixture.observe("key");
        action.key = key;
        fixture.backend->secure = true;
        CHECK(fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.size() == 1);
    }
}

void verifyKeyGrammar() {
    const auto selectAll = maiDesktopParseKey("Ctrl+A");
    CHECK(selectAll);
    if (selectAll) {
        CHECK(selectAll.value().key == DesktopKeyCode::Character);
        CHECK(selectAll.value().character == 'A');
        CHECK(selectAll.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::Control});
    }
    const auto selection = maiDesktopParseKey("shift+home");
    CHECK(selection);
    if (selection) {
        CHECK(selection.value().key == DesktopKeyCode::Home);
        CHECK(selection.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::Shift});
    }
    for (const char* shortcut : {"Control+Shift+End", "Alt+F4", "Cmd+A", "Meta+A",
                                  "Super+A", "Enter", "Tab", "Escape", "Delete",
                                  "Backspace", "ArrowLeft", "ArrowRight", "ArrowUp",
                                  "ArrowDown", "PageUp", "PageDown", "Space", "Insert",
                                  "End", "F1", "F24", "9"}) {
        const auto parsed = maiDesktopParseKey(shortcut);
        CHECK(parsed);
    }
    for (const std::string& invalid : {std::string(), std::string("Ctrl"), std::string("Ctrl+"),
                                       std::string("+A"), std::string("Ctrl++A"),
                                       std::string("Ctrl+Control+A"), std::string("Cmd+Meta+A"),
                                       std::string("Ctrl+A+B"), std::string("Shift+Ctrl"),
                                       std::string("F0"), std::string("F25"),
                                       std::string("Hyper+A"), std::string("\xe4\xb8\xad")}) {
        const auto parsed = maiDesktopParseKey(invalid);
        CHECK(!parsed);
        if (!parsed) CHECK(parsed.error().code() == MaiErrorCode::InvalidInput);
    }
}

void verifyWindowSelection() {
    Fixture fixture;
    auto duplicate = fixture.backend->visible.front();
    duplicate.id = 202;
    duplicate.processId = 5002;
    fixture.backend->visible.push_back(duplicate);
    const auto listed = fixture.controller.windows();
    CHECK(listed);
    if (listed) {
        CHECK(listed.value().size() == 2);
        CHECK(listed.value()[0].id == 101);
        CHECK(listed.value()[1].id == 202);
        CHECK(listed.value()[1].processId == 5002);
        CHECK(listed.value()[1].title == "Isolated editor");
        CHECK(listed.value()[1].width == 400);
    }
    const auto ambiguous = fixture.controller.observe("owner", duplicate.title,
        fixture.workspace.path().toUtf8().toStdString(), true);
    CHECK(!ambiguous);
    if (!ambiguous) CHECK(ambiguous.error().code() == MaiErrorCode::NotFound);
    CHECK(fixture.backend->captureCount == 0);
    const auto selected = fixture.controller.observe("owner", "",
        fixture.workspace.path().toUtf8().toStdString(), true, duplicate.id);
    CHECK(selected);
    if (selected) {
        const Json output = Json::parse(selected.value().json);
        CHECK(output["window_id"] == "202");
        CHECK(output["process_id"] == 5002);
        CHECK(output["bounds"]["width"] == 400);
        CHECK(output["elements"].size() == 1);
        CHECK(QImage(QString::fromStdString(selected.value().imagePath)).size() == QSize(20, 20));
    }
    CHECK(fixture.backend->capturedWindowId == 202);
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
}

void verifyScreenshotHandoff() {
    Fixture fixture;
    // The ordinary API retains a controlled native-fixture capture for host
    // integration; model-facing observe is metadata only.
    MaiToolContext context;
    context.sessionId = "screenshot-delivery";
    context.root = fixture.workspace.path().toUtf8().toStdString();
    context.model = "deepseek-flash";
    const auto captured = fixture.controller.observe(context.sessionId, "", context.root, true, 101);
    CHECK(captured);
    if (!captured) return;
    const Json output = Json::parse(captured.value().json);
    const std::string relative = output.value("image_path", std::string());
    CHECK(!relative.empty());
    CHECK(QDir::isRelativePath(QString::fromStdString(relative)));
    if (relative.empty()) return;
    const QString saved = fixture.workspace.filePath(QString::fromStdString(relative));
    CHECK(QFileInfo(saved).canonicalFilePath() ==
          QFileInfo(QString::fromStdString(captured.value().imagePath)).canonicalFilePath());
    const auto delivered =
        makeMaiAgentSendMediaTool()->execute(Json{{"file_path", relative}, {"type", "image"}}.dump(), context);
    CHECK(!delivered.hasError());
    if (!delivered.hasError()) {
        const Json receipt = Json::parse(delivered.output());
        CHECK(receipt["path"] == relative);
        CHECK(receipt["type"] == "image");
    }
    CHECK(fixture.backend->captureCount == 1);
    CHECK(fixture.backend->inputs.empty());
}

void verifyScreenshotDirectoryBoundary() {
#if defined(Q_OS_WIN)
    Fixture fixture;
    QTemporaryDir outside;
    CHECK(outside.isValid());
    const QString link = fixture.workspace.filePath(QStringLiteral("computer"));
    if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()), reinterpret_cast<LPCWSTR>(outside.path().utf16()),
                             0x3)) {
        std::fprintf(stderr, "computer directory link boundary not exercised: Win32 %lu\n", GetLastError());
        return;
    }
    const auto observed =
        fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(), false, 101);
    CHECK(!observed);
    if (!observed) CHECK(observed.error().code() == MaiErrorCode::InvalidInput);
    CHECK(fixture.backend->captureCount == 0);
    CHECK(QDir(outside.path()).entryList(QDir::Files).isEmpty());
    CHECK(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16())) != 0);
#endif
}

void verifyScreenshotDirectoryIsValidatedBeforeCapture() {
    Fixture fixture;
    // A file occupying the output directory must fail before requesting pixels.
    QFile blocker(fixture.workspace.filePath(QStringLiteral("computer")));
    const bool opened = blocker.open(QIODevice::WriteOnly);
    CHECK(opened);
    if (!opened) return;
    blocker.close();

    const auto captured = fixture.controller.observe("screenshot-invalid-directory", "",
                                                     fixture.workspace.path().toUtf8().toStdString(), true, 101, true);
    CHECK(!captured);
    CHECK(fixture.backend->captureCount == 0);
    CHECK(QFileInfo(blocker).isFile());
}

void verifyScreenshotCancellationDoesNotPublishPixels() {
    Fixture fixture;
    std::atomic<bool> canceled{false};
    // Cancellation during a successful capture must be observed before saving
    // or publishing its pixels; an empty output directory may already exist.
    fixture.backend->onCapture = [&canceled] { canceled.store(true, std::memory_order_relaxed); };
    const auto captured = fixture.controller.observe("screenshot-canceled-during-capture", "",
                                                     fixture.workspace.path().toUtf8().toStdString(), true, 101, true,
                                                     [&canceled] { return canceled.load(std::memory_order_relaxed); });
    CHECK(canceled.load(std::memory_order_relaxed));
    CHECK(fixture.backend->captureCount == 1);
    CHECK(!captured);
    if (!captured) CHECK(captured.error().code() == MaiErrorCode::Canceled);
    CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
}

void verifyObservationWithoutImage() {
    Fixture fixture;
    // Element-only observation still issues a fresh usable observation without
    // depending on capture support or creating a screenshot on disk.
    const auto observed =
        fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(), true, 101, false);
    CHECK(observed);
    std::string firstSnapshot;
    if (observed) {
        CHECK(observed.value().imagePath.empty());
        const Json output = Json::parse(observed.value().json);
        CHECK(output["has_image"] == false);
        CHECK(output.value("image_path", std::string()).empty());
        CHECK(output["window_id"] == "101");
        CHECK(output["elements"].size() == 1);
        firstSnapshot = output.value("snapshot_id", std::string());
        CHECK(!firstSnapshot.empty());
    }
    CHECK(fixture.backend->captureCount == 0);
    CHECK(!QFileInfo::exists(fixture.workspace.filePath(QStringLiteral("computer"))));

    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>(fixture.backend);
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;
    MaiToolContext context;
    context.sessionId = "element-only-tool-session";
    context.root = fixture.workspace.path().toUtf8().toStdString();
    context.model = "deepseek-flash";
    const auto result = tool->execute(R"({"action":"observe","window_id":"101","include_elements":true})", context);
    CHECK(!result.hasError());
    CHECK(result.images().empty());
    if (!result.hasError()) {
        const Json output = Json::parse(result.output());
        CHECK(output["has_image"] == false);
        CHECK(!output.contains("image_path"));
        CHECK(output["elements"].size() == 1);
        CHECK(!output.value("snapshot_id", std::string()).empty());
        CHECK(output.value("snapshot_id", std::string()) != firstSnapshot);
    }
    // Text-only and image-capable models receive the same metadata contract.
    for (const std::string model : {"glm-5.3", "local:qa"}) {
        context.model = model;
        const auto defaultResult =
            tool->execute(R"({"action":"observe","window_id":"101","include_elements":true})", context);
        CHECK(!defaultResult.hasError());
        CHECK(defaultResult.images().empty());
        if (!defaultResult.hasError()) {
            const Json output = Json::parse(defaultResult.output());
            CHECK(output["has_image"] == false);
            CHECK(!output.contains("image_path"));
            CHECK(output["elements"].size() == 1);
        }
    }
    for (const std::string model : {"deepseek-flash", "glm-5.3", "local:qa"}) {
        context.model = model;
        for (const Json includeImage : {Json(true), Json(false), Json(nullptr), Json("false")}) {
            const auto unsupportedImage = tool->execute(
                Json{{"action", "observe"}, {"window_id", "101"}, {"include_image", includeImage}}.dump(), context);
            CHECK(unsupportedImage.hasError());
            CHECK(unsupportedImage.error().code() == MaiErrorCode::InvalidInput);
            CHECK(unsupportedImage.error().message().find("screenshot") != std::string::npos);
            CHECK(unsupportedImage.images().empty());
        }
    }
    CHECK(fixture.backend->captureCount == 0);
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    CHECK(!QFileInfo::exists(fixture.workspace.filePath(QStringLiteral("computer"))));
}

void verifyDispatchAndSnapshots() {
    Fixture fixture;
    auto action = fixture.observe("click");
    action.dryRun = true;
    const auto planned = fixture.controller.perform("owner", action);
    CHECK(planned);
    if (planned) {
        CHECK(planned.value().dryRun);
        CHECK(!planned.value().dispatched);
        CHECK(!planned.value().verified);
    }
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    action.dryRun = false;
    const auto clicked = fixture.controller.perform("owner", action);
    CHECK(clicked);
    if (clicked) {
        CHECK(clicked.value().state == "dispatched");
        CHECK(clicked.value().dispatched);
        CHECK(!clicked.value().verified);
        CHECK(!clicked.value().dryRun);
    }
    CHECK(fixture.backend->activationCount == 1);
    CHECK(fixture.backend->inputs.size() == 1);
    if (!fixture.backend->inputs.empty()) {
        CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::Click);
        CHECK(fixture.backend->inputs.back().expectedWindowId == 101);
        CHECK(fixture.backend->inputs.back().expectedProcessId == 5001);
        CHECK(std::abs(fixture.backend->inputs.back().x - 199.75) < 0.01);
        CHECK(std::abs(fixture.backend->inputs.back().y - 349.5) < 0.01);
    }
    const auto reused = fixture.controller.perform("owner", action);
    CHECK(!reused);
    if (!reused) CHECK(reused.error().code() == MaiErrorCode::InvalidInput);
    CHECK(fixture.backend->inputs.size() == 1);

    auto focus = fixture.observe("focus");
    const auto focused = fixture.controller.perform("owner", focus);
    CHECK(focused);
    if (focused) CHECK(!focused.value().verified);
    CHECK(fixture.backend->activationCount == 2);
    CHECK(fixture.backend->inputs.size() == 1);
    CHECK(!fixture.controller.perform("owner", focus));

    const std::pair<const char*, DesktopNativeInputKind> gestures[] = {
        {"double_click", DesktopNativeInputKind::DoubleClick},
        {"right_click", DesktopNativeInputKind::RightClick},
        {"drag", DesktopNativeInputKind::Drag}};
    for (const auto& gesture : gestures) {
        auto next = fixture.observe(gesture.first);
        next.durationMs = 100;
        const auto sent = fixture.controller.perform("owner", next);
        CHECK(sent);
        CHECK(fixture.backend->inputs.back().kind == gesture.second);
        if (gesture.second == DesktopNativeInputKind::Drag) {
            CHECK(std::abs(fixture.backend->inputs.back().toX - 399.25) < 0.01);
            CHECK(std::abs(fixture.backend->inputs.back().toY - 424.25) < 0.01);
            CHECK(fixture.backend->inputs.back().durationMs == 100);
        }
        CHECK(!fixture.controller.perform("owner", next));
    }
}

void verifyTextAndReadback() {
    Fixture fixture;
    const std::string chinese = "\xe7\xac\xac\xe4\xb8\x80\xe8\xa1\x8c\xe4\xb8\xad\xe6\x96\x87";
    auto replacement = fixture.observe("replace_text");
    replacement.text = chinese;
    const auto replaced = fixture.controller.perform("owner", replacement);
    CHECK(replaced);
    CHECK(fixture.backend->textValue == chinese);
    CHECK(fixture.backend->inputs.size() == 1);
    if (!fixture.backend->inputs.empty()) {
        CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::ReplaceFocusedText);
        CHECK(fixture.backend->inputs.back().text == chinese);
    }
    auto verify = fixture.observe("verify");
    verify.expectedText = chinese;
    const int activations = fixture.backend->activationCount;
    fixture.backend->events.clear();
    const auto matched = fixture.controller.perform("owner", verify);
    CHECK(matched);
    if (matched) {
        CHECK(matched.value().verified);
        CHECK(!matched.value().dispatched);
        CHECK(matched.value().actualText == chinese);
    }
    CHECK(fixture.backend->activationCount == activations);
    CHECK(fixture.backend->readCount == 1);
    const auto focusedEvent = std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "isFocused");
    const auto textEvent = std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "focusedText");
    CHECK(focusedEvent != fixture.backend->events.end());
    CHECK(textEvent != fixture.backend->events.end());
    CHECK(focusedEvent < textEvent);
    CHECK(!fixture.controller.perform("owner", verify));
    auto mismatch = fixture.observe("verify");
    mismatch.expectedText = "different expected text";
    const auto mismatched = fixture.controller.perform("owner", mismatch);
    CHECK(mismatched);
    if (mismatched) {
        CHECK(!mismatched.value().verified);
        CHECK(mismatched.value().actualText == chinese);
    }
    CHECK(fixture.backend->activationCount == activations);

    auto shortcut = fixture.observe("key");
    shortcut.key = "Shift+Home";
    CHECK(fixture.controller.perform("owner", shortcut));
    CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::Key);
    CHECK(fixture.backend->inputs.back().key == "Shift+Home");
    auto empty = fixture.observe("replace_text");
    empty.text.clear();
    CHECK(fixture.controller.perform("owner", empty));
    CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::ReplaceFocusedText);
    CHECK(fixture.backend->inputs.back().text.empty());
    CHECK(fixture.backend->textValue.empty());
    auto append = fixture.observe("type");
    append.text = chinese;
    CHECK(fixture.controller.perform("owner", append));
    CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::Type);
    CHECK(fixture.backend->textValue == chinese);
    auto forgotten = fixture.observe();
    CHECK(!fixture.controller.clear("owner"));
    CHECK(fixture.backend->textValue == chinese);
    CHECK(!fixture.controller.perform("owner", forgotten));
}

void verifyIdentityAndSyntaxFailures() {
    for (int changed = 0; changed < 4; ++changed) {
        Fixture fixture;
        auto action = fixture.observe();
        auto& window = fixture.backend->visible.front();
        if (changed == 0) ++window.id;
        else if (changed == 1) ++window.processId;
        else if (changed == 2) window.x += 20;
        else window.title = "A different window title";
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputs.empty());
    }
    Fixture fixture;
    auto original = fixture.observe();
    CHECK(!fixture.controller.perform("other-session", original));
    auto invalid = original;
    ++invalid.windowId;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.xRatio = 1.5;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.action = "key";
    invalid.key = "Ctrl++A";
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.action = "drag";
    invalid.durationMs = 0;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid.durationMs = 201;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.action = "type";
    invalid.text = std::string(4001, 'x');
    CHECK(!fixture.controller.perform("owner", invalid));
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    // Correcting syntax reuses the same observation because nothing crossed
    // the OS boundary in the rejected requests.
    CHECK(fixture.controller.perform("owner", original));
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyFailureAndCancellationBoundaries() {
    {
        Fixture fixture;
        auto action = fixture.observe();
        std::atomic<bool> canceled{true};
        action.isCanceled = [&canceled] { return canceled.load(); };
        const auto stopped = fixture.controller.perform("owner", action);
        CHECK(!stopped);
        if (!stopped) CHECK(stopped.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputs.empty());
        canceled = false;
        CHECK(fixture.controller.perform("owner", action));
    }
    {
        Fixture fixture;
        auto action = fixture.observe();
        std::atomic<bool> canceled{false};
        action.isCanceled = [&canceled] { return canceled.load(); };
        fixture.backend->onActivate = [&canceled] { canceled = true; };
        const auto stopped = fixture.controller.perform("owner", action);
        CHECK(!stopped);
        if (!stopped) CHECK(stopped.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.backend->activationCount == 1);
        CHECK(fixture.backend->inputs.empty());
        canceled = false;
        fixture.backend->onActivate = {};
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->activationCount == 1);
    }
    {
        Fixture fixture;
        auto action = fixture.observe();
        fixture.backend->activationError = {MaiErrorCode::Busy, "fixture activation denied"};
        const auto denied = fixture.controller.perform("owner", action);
        CHECK(!denied);
        if (!denied) CHECK(denied.error().code() == MaiErrorCode::Busy);
        CHECK(fixture.backend->inputs.empty());
        fixture.backend->activationError = {};
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->activationCount == 1);
    }
    {
        Fixture fixture;
        auto action = fixture.observe();
        fixture.backend->inputError = {MaiErrorCode::Internal, "fixture input failed after dispatch began"};
        const auto failed = fixture.controller.perform("owner", action);
        CHECK(!failed);
        if (!failed) CHECK(failed.error().code() == MaiErrorCode::Internal);
        fixture.backend->inputError = {};
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.size() == 1);
    }
    for (const char* kind : {"type", "replace_text", "verify"}) {
        Fixture fixture;
        auto action = fixture.observe(kind);
        action.text = "blocked secret";
        action.expectedText = "blocked secret";
        fixture.backend->secure = true;
        const auto secure = fixture.controller.perform("owner", action);
        CHECK(!secure);
        if (!secure) CHECK(secure.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->inputs.empty());
        CHECK(fixture.backend->readCount == 0);
    }
    {
        Fixture fixture;
        auto action = fixture.observe("verify");
        fixture.backend->focused = false;
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->readCount == 0);
        CHECK(fixture.backend->activationCount == 0);
    }
    {
        Fixture fixture;
        auto action = fixture.observe("verify");
        fixture.backend->readError = {MaiErrorCode::NotSupported, "fixture cannot safely read the focused control"};
        const auto unreadable = fixture.controller.perform("owner", action);
        CHECK(!unreadable);
        if (!unreadable) CHECK(unreadable.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->activationCount == 0);
    }
}

void verifyPublicContract() {

    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>();
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;

    const Json schema = Json::parse(tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (!schema.is_object()) return;
    const Json properties = schema.value("properties", Json::object());
    CHECK(!properties.contains("include_image"));
    const Json actions = properties.value("action", Json::object()).value("enum", Json{});
    // The public contract exposes routine desktop operations without needing a
    // native session. Native dispatch belongs to the separate fixture test.
    for (const char* action : {"list", "observe", "focus", "click", "double_click",
                               "right_click", "drag", "type", "key", "scroll",
                               "replace_text", "verify", "clear"}) {
        if (!contains(actions, action)) {
            std::fprintf(stderr, "desktop_computer schema is missing action: %s\n", action);
            ++failures;
        }
    }

    const Json key = properties.value("key", Json::object());
    CHECK(key.value("type", std::string()) == "string");
    // A fixed list of navigation keys prevents ordinary editing shortcuts such
    // as Ctrl+A and Shift+Home from reaching the native key parser.
    CHECK(!key.contains("enum"));

    const Json text = properties.value("text", Json::object());
    CHECK(text.value("type", std::string()) == "string");
    CHECK(text.value("minLength", 0) == 0);
    const std::string description = tool->description();
    CHECK(description.find("clear only forgets") != std::string::npos);
    CHECK(description.find("does not clear input text") != std::string::npos);

    MaiToolContext context;
    context.sessionId = "contract-without-observation";
    // These deliberately unknown snapshots must be rejected before any native
    // operation. NotFound proves valid empty replacement and shortcut arguments
    // reached the shared controller rather than being rejected by tool parsing.
    const auto emptyReplacement = tool->execute(
        R"({"action":"replace_text","snapshot_id":"missing","window_id":"1","text":""})",
        context);
    CHECK(emptyReplacement.error().code() == MaiErrorCode::NotFound);
    const auto shortcut = tool->execute(
        R"({"action":"key","snapshot_id":"missing","window_id":"1","key":"Ctrl+A"})",
        context);
    CHECK(shortcut.error().code() == MaiErrorCode::NotFound);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    verifyPublicContract();
    verifyKeyGrammar();
    verifySecureKeyboardInput();
    verifyWindowSelection();
    verifyScreenshotHandoff();
    verifyScreenshotDirectoryBoundary();
    verifyScreenshotDirectoryIsValidatedBeforeCapture();
    verifyScreenshotCancellationDoesNotPublishPixels();
    verifyObservationWithoutImage();
    verifyOwnedPopupInput();
    verifyDispatchAndSnapshots();
    verifyTextAndReadback();
    verifyIdentityAndSyntaxFailures();
    verifyFailureAndCancellationBoundaries();

    if (failures != 0) {
        std::fprintf(stderr, "desktop computer contract failures: %d\n", failures);
        return 1;
    }
    std::fprintf(stderr, "desktop computer public contracts verified without native input\n");
    return 0;
}

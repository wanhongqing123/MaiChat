// Native integration across two owned processes. The child only hosts a Qt
// editor; the parent uses the production tool/controller/native backend. Child
// state is an independent assertion source, never a substitute for UIA readback.
// Tool observations are metadata/UIA only. Native screenshots remain separate
// controlled fixture evidence produced by the ordinary controller API.
#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTextCursor>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <algorithm>
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

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

namespace {

using Json = nlohmann::json;

bool writeJson(const QString& path, const Json& value) {
    const QByteArray bytes = QByteArray::fromStdString(value.dump(2));
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

Json readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return Json::parse(file.readAll().toStdString(), nullptr, false);
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (condition()) return true;
        QThread::msleep(10);
    } while (elapsed.elapsed() < timeoutMs);
    return condition();
}

int unavailable(const std::string& reason) {
    std::fprintf(stderr, "native two-process computer test SKIP: %s; native workflow was not fully verified\n",
                 reason.c_str());
    return 77;
}

// The only process terminated by this test is the QProcess it starts here.
// No process names, titles or enumerated PIDs are used for process termination.
struct OwnedEditorProcess {
    QProcess process;
    ~OwnedEditorProcess() {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(2000);
        }
    }
};

int configuredFocusWaitSeconds() {
    bool valid = false;
    const int seconds = qEnvironmentVariableIntValue("MAICHAT_COMPUTER_NATIVE_WAIT_FOCUS_SECONDS", &valid);
    if (!valid || seconds <= 0) return 0;
    return seconds > 120 ? 120 : seconds;
}

#if defined(Q_OS_WIN)
// Observe only keyboard messages addressed to this owned fixture HWND. This is
// not a global keyboard hook and cannot record another application's input.
class OwnedKeyboardRecorder final : public QAbstractNativeEventFilter {
public:
    HWND window = nullptr;
    Json events = Json::array();

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr*) override {
        const auto* native = static_cast<const MSG*>(message);
        if (!window || (native->hwnd != window && !IsChild(window, native->hwnd))) return false;
        if (native->message != WM_KEYDOWN && native->message != WM_KEYUP &&
            native->message != WM_SYSKEYDOWN && native->message != WM_SYSKEYUP) return false;
        if (events.size() >= 2048) return false;
        const auto flags = std::uintptr_t(native->lParam);
        const auto scan = (flags >> 16) & 0xff;
        const bool extended = (flags & (std::uintptr_t(1) << 24)) != 0;
        UINT originalKey = UINT(native->wParam);
        if (originalKey == VK_PROCESSKEY && native->message == WM_KEYDOWN &&
            eventType == QByteArrayLiteral("windows_generic_MSG")) {
            // ImmGetVirtualKey is a read-only query valid only before this
            // message loop calls TranslateMessage. Do not replace the raw VK,
            // change the target's layout/IME, or invent a key when recovery fails.
            const HMODULE imm = GetModuleHandleW(L"imm32.dll");
            using OriginalKey = UINT(WINAPI*)(HWND);
            const auto original = imm ? reinterpret_cast<OriginalKey>(GetProcAddress(imm, "ImmGetVirtualKey")) : nullptr;
            if (original) originalKey = original(native->hwnd);
        }
        // CJK IMEs can tag right Shift as E0 36 in the window message even
        // though its native scan identity is 36 (GLFW win32_window.c). Preserve
        // the raw flag and independently record side state from this receiver's
        // message queue. No other key's extended identity is normalized.
        const bool rightShift = originalKey == VK_SHIFT && scan == 0x36;
        events.push_back({{"vk", std::uint32_t(native->wParam)},
                          {"original_vk", originalKey},
                          {"scan", scan}, {"extended", extended},
                          {"semantic_scan", rightShift ? scan : scan | (extended ? 0xe000 : 0)},
                          {"left_shift_down", (GetKeyState(VK_LSHIFT) & 0x8000) != 0},
                          {"right_shift_down", (GetKeyState(VK_RSHIFT) & 0x8000) != 0},
                          {"left_control_down", (GetKeyState(VK_LCONTROL) & 0x8000) != 0},
                          {"right_control_down", (GetKeyState(VK_RCONTROL) & 0x8000) != 0},
                          {"target_keyboard_layout", std::uint64_t(reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0)))},
                          {"up", native->message == WM_KEYUP || native->message == WM_SYSKEYUP}});
        return false;
    }
};

int receivedKey(const Json& event) {
    return event.value("original_vk", event.value("vk", 0));
}

bool allPhysicalInputReleased() {
    // The high bit is the current down state. Checking the complete virtual-key
    // range covers mouse buttons and keys without confusing Caps/NumLock toggles
    // with held input. This only reads OS state and never releases a user's key.
    for (int key = 1; key < 255; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return false;
    }
    return true;
}

int verifyWindowsMappingsWithoutInput() {
    const auto layout = reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0));
    const SHORT literalSemicolon = VkKeyScanExW(L';', reinterpret_cast<HKL>(layout));
    struct Expected { const char* name; WORD key; int extended; };
    const Expected required[] = {
        {"A", 'A', 0}, {"F24", VK_F24, 0}, {"Semicolon", VK_OEM_1, 0},
        {";", literalSemicolon == -1 ? WORD(0) : WORD(LOBYTE(literalSemicolon)), -1},
        {"Equal", VK_OEM_PLUS, 0}, {"NumpadEnter", VK_RETURN, 1},
        {"NumpadDivide", VK_DIVIDE, 1}, {"Numpad0", VK_NUMPAD0, 0},
        {"ControlRight", VK_RCONTROL, 1}, {"AltRight", VK_RMENU, 1},
        {"ShiftRight", VK_RSHIFT, 0}, {"MetaRight", VK_RWIN, 1},
        {"CapsLock", VK_CAPITAL, 0}, {"ScrollLock", VK_SCROLL, 0},
        {"ContextMenu", VK_APPS, 1}, {"PrintScreen", VK_SNAPSHOT, 1},
        {"Insert",VK_INSERT,1}, {"Delete",VK_DELETE,1}, {"Home",VK_HOME,1}, {"End",VK_END,1},
        {"PageUp",VK_PRIOR,1}, {"PageDown",VK_NEXT,1}, {"ArrowLeft",VK_LEFT,1},
        {"ArrowRight",VK_RIGHT,1}, {"ArrowUp",VK_UP,1}, {"ArrowDown",VK_DOWN,1}, {"Enter",VK_RETURN,0},
        {"Numpad1",VK_NUMPAD1,0}, {"Numpad2",VK_NUMPAD2,0}, {"Numpad3",VK_NUMPAD3,0},
        {"Numpad4",VK_NUMPAD4,0}, {"Numpad5",VK_NUMPAD5,0}, {"Numpad6",VK_NUMPAD6,0},
        {"Numpad7",VK_NUMPAD7,0}, {"Numpad8",VK_NUMPAD8,0}, {"Numpad9",VK_NUMPAD9,0},
        {"NumpadAdd",VK_ADD,0}, {"NumpadSubtract",VK_SUBTRACT,0},
        {"NumpadMultiply",VK_MULTIPLY,0}, {"NumpadDecimal",VK_DECIMAL,0},
        // Media keys have layout-provided scan identities. Their VK contract is
        // verified here without dispatching volume/playback changes to Windows.
        {"VolumeMute", VK_VOLUME_MUTE, -1}, {"MediaPlayPause", VK_MEDIA_PLAY_PAUSE, -1}};
    int failures = 0;
    Json cases = Json::array();
    for (const auto& expected : required) {
        const auto parsed = maiDesktopParseKey(expected.name);
        const auto mapped = parsed ? maiDesktopWindowsResolveKey(parsed.value(), layout)
                                   : MaiResult<DesktopWindowsKeyBinding>(parsed.error());
        const bool matched = expected.key == 0
            ? !mapped && mapped.error().code() == MaiErrorCode::NotSupported
            : mapped && mapped.value().virtualKey == expected.key &&
                (expected.extended < 0 || mapped.value().extended == bool(expected.extended));
        cases.push_back({{"name", expected.name}, {"matches", matched}});
        if (!matched) {
            ++failures;
            std::fprintf(stderr, "Windows mapping mismatch: %s\n", expected.name);
        }
    }
    struct DistinctPair { const char* navigation; const char* keypad; WORD scan; };
    const DistinctPair pairs[] = {
        {"Insert","Numpad0",0x52}, {"End","Numpad1",0x4f}, {"ArrowDown","Numpad2",0x50},
        {"PageDown","Numpad3",0x51}, {"ArrowLeft","Numpad4",0x4b}, {"ArrowRight","Numpad6",0x4d},
        {"Home","Numpad7",0x47}, {"ArrowUp","Numpad8",0x48}, {"PageUp","Numpad9",0x49},
        {"Delete","NumpadDecimal",0x53}};
    for (const auto& expected : pairs) {
        const auto navigation = maiDesktopParseKey(expected.navigation);
        const auto keypad = maiDesktopParseKey(expected.keypad);
        const auto nav = navigation ? maiDesktopWindowsResolveKey(navigation.value(), layout)
                                    : MaiResult<DesktopWindowsKeyBinding>(navigation.error());
        const auto pad = keypad ? maiDesktopWindowsResolveKey(keypad.value(), layout)
                                : MaiResult<DesktopWindowsKeyBinding>(keypad.error());
        const bool distinct = nav && pad && nav.value().virtualKey != pad.value().virtualKey &&
            nav.value().scanCode == expected.scan && pad.value().scanCode == expected.scan &&
            nav.value().extended && !pad.value().extended;
        if (!distinct) {
            ++failures;
            std::fprintf(stderr, "Windows navigation/keypad identity mismatch: %s / %s\n", expected.navigation, expected.keypad);
        }
    }
    Json catalog = Json::array();
    int supported = 0;
    int unavailableKeys = 0;
    for (const auto& name : maiDesktopKeyNames()) {
        const auto parsed = maiDesktopParseKey(name);
        if (!parsed) { ++failures; continue; }
        const auto mapped = maiDesktopWindowsResolveKey(parsed.value(), layout);
        if (mapped) {
            ++supported;
            if (!mapped.value().virtualKey || mapped.value().virtualKey > 255 || mapped.value().scanCode > 255)
                ++failures;
            catalog.push_back({{"name", name}, {"vk", mapped.value().virtualKey},
                               {"scan", mapped.value().scanCode}, {"extended", mapped.value().extended}});
        } else {
            ++unavailableKeys;
            if (mapped.error().code() != MaiErrorCode::NotSupported) ++failures;
            catalog.push_back({{"name", name}, {"error", mapped.error().message()}});
        }
    }
    for (const char* name : {"Fn", "Ctrl+Alt+Delete", "ControlRight+AltLeft+Delete"}) {
        const auto parsed = maiDesktopParseKey(name);
        if (!parsed) { ++failures; continue; }
        const auto mapped = maiDesktopWindowsResolveKey(parsed.value(), layout);
        if (mapped || mapped.error().code() != MaiErrorCode::NotSupported) ++failures;
    }
    const auto plus = maiDesktopParseKey("+");
    if (!plus) ++failures;
    else {
        const auto mapped = maiDesktopWindowsResolveKey(plus.value(), layout);
        const SHORT native = VkKeyScanExW(L'+', reinterpret_cast<HKL>(layout));
        if (native != -1) {
            if (!mapped || mapped.value().virtualKey != LOBYTE(native)) ++failures;
            else if ((HIBYTE(native) & 1) &&
                     std::find(mapped.value().impliedModifiers.begin(), mapped.value().impliedModifiers.end(),
                               DesktopKeyModifier::Shift) == mapped.value().impliedModifiers.end()) ++failures;
        }
    }
    const QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidence.isEmpty())
        if (!writeJson(QDir(evidence).filePath(QStringLiteral("windows-key-mappings.json")),
                       {{"input_dispatched", false}, {"required_cases", cases}, {"catalog", catalog},
                        {"supported", supported}, {"not_supported", unavailableKeys}, {"failures", failures}}))
            ++failures;
    std::fprintf(stderr, "Windows read-only key mappings: %d supported / %d NotSupported, %d failures; no input dispatched\n",
                 supported, unavailableKeys, failures);
    return failures ? 1 : 0;
}

int awaitOwnedForeground(QProcess& child, const DesktopNativeWindow& window, int seconds,
                         const QString& readyPath, Json& ready) {
    const HWND handle = reinterpret_cast<HWND>(std::uintptr_t(window.id));
    QElapsedTimer elapsed;
    QElapsedTimer stable;
    elapsed.start();
    while (elapsed.elapsed() < seconds * 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        DWORD currentPid = 0;
        if (child.state() != QProcess::Running ||
            !GetWindowThreadProcessId(handle, &currentPid) || currentPid != window.processId) {
            ready["focus_wait_state"] = "owned_editor_exited";
            writeJson(readyPath, ready);
            std::fprintf(stderr, "owned editor disappeared while waiting for real foreground\n");
            return 1;
        }
        if (GetForegroundWindow() == handle && allPhysicalInputReleased()) {
            if (!stable.isValid()) stable.start();
            if (stable.elapsed() >= 200) {
                ready["focus_wait_state"] = "foreground_ready";
                ready["foreground_idle_stable_ms"] = stable.elapsed();
                if (!writeJson(readyPath, ready)) return 1;
                std::fprintf(stderr, "owned editor is actually foreground with all input released for 200 ms; continuing strict native tools\n");
                return 0;
            }
        } else {
            stable.invalidate();
        }
        QThread::msleep(10);
    }
    ready["focus_wait_state"] = "timed_out";
    writeJson(readyPath, ready);
    return unavailable("timed out waiting for the owned editor to be the actual foreground with released keyboard and mouse");
}
#endif

class OwnedTextEditor final : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void()> plainEnter;
    std::function<void()> controlEnter;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
#if defined(Q_OS_WIN)
        // Unicode type's LF is text, not a physical Enter command. Native
        // key presses have VK_RETURN; VK_PACKET must retain its Unicode text.
        const bool physicalEnter = enter && event->nativeVirtualKey() == VK_RETURN;
#else
        const bool physicalEnter = enter;
#endif
        if (physicalEnter) {
#if defined(Q_OS_MAC)
            // Qt maps the physical Mac Control key to MetaModifier; its
            // ControlModifier represents Command. The tool's Ctrl is physical.
            constexpr auto controlModifier = Qt::MetaModifier;
#else
            constexpr auto controlModifier = Qt::ControlModifier;
#endif
            if (modifiers == controlModifier) {
                if (controlEnter) controlEnter();
                event->accept();
                return;
            }
            if (modifiers == Qt::NoModifier) {
                if (plainEnter) plainEnter();
                event->accept();
                return;
            }
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

// A focused ordinary Edit target consumes safe physical-key probes without
// sending messages or letting editing/navigation keys modify the chat fixture.
class OwnedKeyRecorderEdit final : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
protected:
    void keyPressEvent(QKeyEvent* event) override { event->accept(); }
    void keyReleaseEvent(QKeyEvent* event) override { event->accept(); }
};

int runEditor(const QString& title, const QString& statePath) {
    QWidget window;
    window.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&window);
    auto* heading = new QLabel(QStringLiteral("独立进程原生电脑操作验收"), &window);
    auto* recorder = new OwnedKeyRecorderEdit(&window);
    recorder->setAccessibleName(QStringLiteral("Native integration key recorder"));
    recorder->setPlaceholderText(QStringLiteral("Owned safe-key receiver"));
    auto* editor = new OwnedTextEditor(&window);
    editor->setAccessibleName(QStringLiteral("Native integration editor input"));
    editor->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* confirmed = new QLabel(QStringLiteral("等待 Enter 确认"), &window);
    auto* status = new QLabel(QStringLiteral("仅使用此测试创建的编辑器内容"), &window);
    layout->addWidget(heading);
    layout->addWidget(recorder);
    layout->addWidget(editor);
    layout->addWidget(confirmed);
    layout->addWidget(status);
    layout->addStretch();
    int enterCount = 0;
    int controlEnterCount = 0;
    int rightClickCount = 0;
#if defined(Q_OS_WIN)
    OwnedKeyboardRecorder keyboard;
    keyboard.window = reinterpret_cast<HWND>(window.winId());
    QCoreApplication::instance()->installNativeEventFilter(&keyboard);
#endif
    const auto save = [&] {
        Json state{{"ready", window.isVisible()},
                         {"pid", std::uint64_t(QCoreApplication::applicationPid())},
                         {"title", title.toUtf8().toStdString()},
                         {"text", editor->toPlainText().toUtf8().toStdString()},
                         {"selected_text", editor->textCursor().selectedText().replace(QChar(0x2029), QLatin1Char('\n')).toUtf8().toStdString()},
                         {"confirmed_text", confirmed->text().toUtf8().toStdString()},
                         {"enter_count", enterCount}, {"control_enter_count", controlEnterCount},
                         {"right_click_count", rightClickCount}};
#if defined(Q_OS_WIN)
        state["native_keys"] = keyboard.events;
#endif
        if (!writeJson(statePath, state)) {
            std::fprintf(stderr, "could not save owned editor fixture state\n");
            QCoreApplication::exit(1);
        }
    };
    QObject::connect(editor, &QPlainTextEdit::textChanged, &window, [&] { save(); });
    QObject::connect(editor, &QPlainTextEdit::selectionChanged, &window, save);
    editor->plainEnter = [&] {
        ++enterCount;
        confirmed->setText(editor->toPlainText());
        save();
    };
    editor->controlEnter = [&] {
        ++controlEnterCount;
        confirmed->setText(editor->toPlainText());
        save();
    };
    QObject::connect(editor, &QPlainTextEdit::customContextMenuRequested, &window, [&](const QPoint&) {
        ++rightClickCount;
        save();
    });
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &window, save);
    window.resize(680, 420);
    window.show();
    heartbeat.start(100);
    QTimer::singleShot(0, &window, save);
    const int result = QApplication::exec();
#if defined(Q_OS_WIN)
    QCoreApplication::instance()->removeNativeEventFilter(&keyboard);
#endif
    return result;
}

enum class InputPosition { None, Center, RecorderCenter, TextStart, SelectionDrag };

class NativeWorkflow {
public:
    NativeWorkflow(MaiTool& tool, DesktopComputerController& controller, MaiToolContext context,
                   DesktopNativeWindow window, QProcess& child, QString statePath, QString evidencePath)
        : tool_(tool),
          controller_(controller),
          context_(std::move(context)),
          window_(std::move(window)),
          child_(child),
          statePath_(std::move(statePath)),
          evidencePath_(std::move(evidencePath)) {}

    ~NativeWorkflow() {
        if (evidencePath_.isEmpty()) return;
        const Json evidence{{"backend", "production_native_two_owned_processes"},
                            {"parent_pid", std::uint64_t(QCoreApplication::applicationPid())},
                            {"child_pid", window_.processId}, {"window_id", std::to_string(window_.id)},
                            {"input_attempted", inputAttempted_}, {"exit_code", exitCode_}, {"steps", transcript_}};
        if (!writeJson(QDir(evidencePath_).filePath(QStringLiteral("native-transcript.json")), evidence))
            std::fprintf(stderr, "could not save native integration transcript\n");
        const Json state = readJson(statePath_);
        if (state.is_object()) writeJson(QDir(evidencePath_).filePath(QStringLiteral("owned-editor-state.json")), state);
    }

    int exitCode() const { return exitCode_; }

    bool expect(bool condition, const char* reason) {
        if (condition) return true;
        std::fprintf(stderr, "native two-process computer test failed: %s\n", reason);
        exitCode_ = 1;
        return false;
    }

    bool observe(const QString& stage) {
        if (!call({{"action", "observe"}, {"window_id", std::to_string(window_.id)}, {"include_elements", true}},
                  stage))
            return false;
        snapshot_ = output_;
        if (!expect(snapshot_.value("window_id", std::string()) == std::to_string(window_.id),
                    "observed a different window ID") ||
            !expect(snapshot_.value("process_id", std::uint64_t(0)) == window_.processId,
                    "observed a different process ID") ||
            !expect(!snapshot_.value("snapshot_id", std::string()).empty(), "observation omitted its snapshot token") ||
            !expect(!snapshot_.contains("image_path"), "computer metadata observation exposed an image path") ||
            !expect(!snapshot_.value("has_image", false), "computer metadata observation claimed an image") ||
            !expect(lastImages_.empty(), "computer metadata observation returned image attachments"))
            return false;

        // Preserve native screenshots through the ordinary API under a distinct
        // session so fixture evidence cannot replace the model's snapshot token
        // or become a model attachment. The exact self-owned window ID is fixed.
        const auto evidence =
            controller_.observe(context_.sessionId + "-native-evidence", "", context_.root, false, window_.id, true);
        if (!evidence) {
            transcript_.push_back({{"stage", stage.toUtf8().toStdString()},
                                   {"ordinary_fixture_capture_error", evidence.error().message()}});
            if (!inputAttempted_ && (evidence.error().code() == MaiErrorCode::Busy ||
                                     evidence.error().code() == MaiErrorCode::NotConfigured ||
                                     evidence.error().code() == MaiErrorCode::NotSupported))
                exitCode_ = unavailable(evidence.error().message());
            else
                exitCode_ = 1;
            return false;
        }
        const QString source = QString::fromStdString(evidence.value().imagePath);
        if (!expect(QFileInfo(source).size() > 100, "native window screenshot is empty")) return false;
        transcript_.push_back(
            {{"stage", stage.toUtf8().toStdString()},
             {"ordinary_fixture_capture",
              {{"window_id", std::to_string(window_.id)}, {"has_image", true}, {"model_attachment", false}}}});
        if (!evidencePath_.isEmpty()) {
            const QString destination =
                QDir(evidencePath_)
                    .filePath(QStringLiteral("%1-%2.png").arg(screenshotCount_++, 2, 10, QLatin1Char('0')).arg(stage));
            QFile::remove(destination);
            if (!expect(QFile::copy(source, destination), "could not preserve native window screenshot")) return false;
        }
        return true;
    }

    bool action(const char* name, Json fields, const QString& stage,
                InputPosition position = InputPosition::None) {
        if (!observe(stage + QStringLiteral("-before"))) return false;
        fields["action"] = name;
        fields["window_id"] = std::to_string(window_.id);
        fields["snapshot_id"] = snapshot_["snapshot_id"];
        if (position != InputPosition::None) {
            // Use the same UIA element coordinates available to a real model.
            // Child Qt coordinates can be logical pixels at 150% DPI, while
            // the production Windows observation/input boundary is physical.
            Json input;
            int matches = 0;
            if (snapshot_.contains("elements") && snapshot_["elements"].is_array()) {
                for (const auto& element : snapshot_["elements"]) {
                    const char* label = position == InputPosition::RecorderCenter ? "Native integration key recorder"
                                                                  : "Native integration editor input";
                    if (element.value("label", std::string()) == label) {
                        input = element;
                        ++matches;
                    }
                }
            }
            if (!expect(matches == 1, "UIA observation did not uniquely expose the owned editor accessible name")) return false;
            const Json& bounds = input["bounds"];
            const bool center = position == InputPosition::Center || position == InputPosition::RecorderCenter;
            fields["x_ratio"] = center ? input["center_x_ratio"].get<double>() :
                bounds["x_ratio"].get<double>() + bounds["width_ratio"].get<double>() * 0.025;
            fields["y_ratio"] = input["center_y_ratio"];
            if (position == InputPosition::TextStart || position == InputPosition::SelectionDrag)
                // The receiver is multiline: keep selection gestures on the
                // populated first line rather than the empty viewport center.
                fields["y_ratio"] = bounds["y_ratio"].get<double>() + bounds["height_ratio"].get<double>() * 0.08;
            if (position == InputPosition::SelectionDrag) {
                fields["to_x_ratio"] = bounds["x_ratio"].get<double>() + bounds["width_ratio"].get<double>() * 0.3;
                fields["to_y_ratio"] = fields["y_ratio"];
                fields["duration_ms"] = 100;
            }
        }
        if (!call(fields, stage)) return false;
        if (std::string(name) == "verify") return true;
        return expect(output_.value("dispatched", false), "native action did not report dispatched=true") &&
            expect(!output_.value("verified", true), "input dispatch falsely reported verified=true");
    }

    bool stateMatches(const std::function<bool(const Json&)>& condition, const char* reason) {
        const bool matched = waitUntil([&] {
            if (child_.state() != QProcess::Running) return false;
            const Json state = readJson(statePath_);
            return validState(state) && condition(state);
        });
        return expect(matched, reason);
    }

    bool verify(const std::string& expected, const QString& stage) {
        return action("verify", {{"expected_text", expected}}, stage) &&
            expect(output_.value("verified", false), "production focusedText did not verify the expected text") &&
            expect(!output_.value("dispatched", true), "readback unexpectedly dispatched input") &&
            expect(output_.value("actual_text", std::string()) == expected, "production UIA readback differed from expected text");
    }

private:
    bool validState(const Json& state) const {
        return state.is_object() && state.value("ready", false) &&
            state.value("pid", std::uint64_t(0)) == window_.processId &&
            state.value("title", std::string()) == window_.title &&
            state.contains("text") && state["text"].is_string();
    }

    bool call(const Json& request, const QString& stage) {
        if (!expect(child_.state() == QProcess::Running, "owned editor process exited before the operation")) return false;
        const std::string action = request.value("action", std::string());
        // Mark an input-capable call before execution: even a failed call may
        // have dispatched a partial native sequence. Only readiness work before
        // the first such attempt can report an environment skip. Focus itself
        // activates the owned window without sending keyboard/mouse input.
        if (action != "list" && action != "observe" && action != "focus" && action != "verify" && action != "clear")
            inputAttempted_ = true;
        const MaiToolResult result = tool_.execute(request.dump(), context_);
        Json record{{"stage", stage.toUtf8().toStdString()}, {"request", request}, {"input_attempted", inputAttempted_}};
        if (result.hasError()) {
            const MaiError& error = result.error();
            record["error"] = error.message();
            transcript_.push_back(record);
            std::fprintf(stderr, "native step %s (%s): %s\n", stage.toUtf8().constData(),
                         request.value("action", std::string()).c_str(), error.message().c_str());
            if (!inputAttempted_ && (error.code() == MaiErrorCode::Busy || error.code() == MaiErrorCode::NotConfigured ||
                                    error.code() == MaiErrorCode::NotSupported)) exitCode_ = unavailable(error.message());
            else exitCode_ = 1;
            return false;
        }
        output_ = Json::parse(result.output(), nullptr, false);
        lastImages_ = result.images();
        record["result"] = output_;
        record["image_attachment_count"] = lastImages_.size();
        transcript_.push_back(record);
        return expect(output_.is_object(), "native computer tool returned invalid JSON");
    }

    MaiTool& tool_;
    DesktopComputerController& controller_;
    MaiToolContext context_;
    DesktopNativeWindow window_;
    QProcess& child_;
    QString statePath_;
    QString evidencePath_;
    Json snapshot_;
    Json output_;
    Json transcript_ = Json::array();
    std::vector<MaiToolImage> lastImages_;
    int screenshotCount_ = 0;
    bool inputAttempted_ = false;
    int exitCode_ = 0;
};

int runParent() {
#if defined(Q_OS_WIN)
    if (QApplication::platformName() != QStringLiteral("windows"))
        return unavailable("a real Windows QPA session is required");
#endif
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    const QString title = QStringLiteral("MaiChat Native Editor 请单击此测试窗口 %1 %2")
        .arg(QCoreApplication::applicationPid()).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString statePath = workspace.filePath(QStringLiteral("owned-editor.json"));
    const QString evidencePath = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidencePath.isEmpty() && !QDir().mkpath(evidencePath)) return 1;
    OwnedEditorProcess editor;
    editor.process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--editor"), title, statePath});
    if (!editor.process.waitForStarted(5000)) {
        std::fprintf(stderr, "could not start owned editor: %s\n", editor.process.errorString().toUtf8().constData());
        return 1;
    }
    const std::uint64_t childPid = std::uint64_t(editor.process.processId());
    if (!waitUntil([&] {
            const Json state = readJson(statePath);
            return state.is_object() && state.value("ready", false) &&
                state.value("pid", std::uint64_t(0)) == childPid && state.value("title", std::string()) == title.toUtf8().toStdString();
        }, 5000)) {
        std::fprintf(stderr, "owned editor did not publish ready state; child status=%d\n", int(editor.process.state()));
        return 1;
    }

    DesktopNativeWindow native;
    MaiError enumerationError;
    int matches = 0;
    const bool selected = waitUntil([&] {
        const auto listed = maiDesktopWindows();
        if (!listed) {
            enumerationError = listed.error();
            return true;
        }
        matches = 0;
        for (const auto& window : listed.value()) {
            if (window.title == title.toUtf8().toStdString() && window.processId == childPid) {
                native = window;
                ++matches;
            }
        }
        return matches != 0;
    });
    if (enumerationError) return unavailable(enumerationError.message());
    if (!selected || matches != 1 || native.id == 0 || childPid == std::uint64_t(QCoreApplication::applicationPid())) {
        std::fprintf(stderr, "could not uniquely match the owned child editor PID/title\n");
        return 1;
    }
#if defined(Q_OS_WIN)
    DWORD observedPid = 0;
    const HWND childHandle = reinterpret_cast<HWND>(std::uintptr_t(native.id));
    if (!GetWindowThreadProcessId(childHandle, &observedPid) || observedPid != childPid) return 1;
#endif
    std::fprintf(stderr, "native two-process fixture: parent_pid=%llu owned_child_pid=%llu window_id=%llu\n",
                 static_cast<unsigned long long>(QCoreApplication::applicationPid()),
                 static_cast<unsigned long long>(childPid), static_cast<unsigned long long>(native.id));

    const int focusWaitSeconds = configuredFocusWaitSeconds();
    const QString readyPath = QDir(evidencePath.isEmpty() ? workspace.path() : evidencePath)
        .filePath(QStringLiteral("native-ready.json"));
    Json ready{{"parent_pid", std::uint64_t(QCoreApplication::applicationPid())},
               {"child_pid", childPid}, {"window_id", std::to_string(native.id)},
               {"title", native.title}, {"wait_focus_seconds", focusWaitSeconds},
               {"required_idle_stable_ms", 200},
               {"focus_wait_state", focusWaitSeconds == 0 ? "disabled" : "waiting"}};
    if (!writeJson(readyPath, ready)) return 1;
    std::fprintf(stderr, "owned editor ready: child_pid=%llu window_id=%llu wait_seconds=%d title=%s ready_file=%s\n",
                 static_cast<unsigned long long>(childPid), static_cast<unsigned long long>(native.id), focusWaitSeconds,
                 native.title.c_str(), readyPath.toUtf8().constData());
    std::fflush(stderr);
#if defined(Q_OS_WIN)
    // An optional human click must be confirmed from actual OS state. Waiting
    // never activates, raises, minimizes or injects into any window. Default
    // runs proceed immediately to the existing strict focus action below.
    if (focusWaitSeconds > 0) {
        const int waited = awaitOwnedForeground(editor.process, native, focusWaitSeconds, readyPath, ready);
        if (waited != 0) return waited;
    }
#endif

    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>();
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    if (!tool) return 1;
    MaiToolContext context;
    context.sessionId = "native-owned-editor-" + std::to_string(childPid);
    context.root = workspace.path().toUtf8().toStdString();
    NativeWorkflow workflow(*tool, *controller, context, native, editor.process, statePath, evidencePath);
    const std::string draft = QStringLiteral("原生中文、标点 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\n"
                                            "surrogate \U0001f600 and ZWJ \U0001f469\u200d\U0001f4bb")
                                  .toUtf8().toStdString();
    const std::string replacement = QStringLiteral("Ctrl+A 后的中文替换").toUtf8().toStdString();
    const std::string finalText = QStringLiteral("双进程原生验证：中文内容已替换。\n"
                                                "Emoji \U0001f600 / \U0001f469\u200d\U0001f4bb; punctuation +[]{}!")
                                      .toUtf8().toStdString();
#if defined(Q_OS_MAC)
    const char* selectAllKey = "Cmd+A";
#else
    const char* selectAllKey = "Ctrl+A";
#endif
    const auto textIs = [&](const std::string& expected, const char* reason) {
        return workflow.stateMatches([&](const Json& state) { return state.value("text", std::string()) == expected; }, reason);
    };

    if (!workflow.action("focus", Json::object(), QStringLiteral("focus"))) return workflow.exitCode();
    if (!workflow.action("click", Json::object(), QStringLiteral("click-key-recorder"), InputPosition::RecorderCenter)) return workflow.exitCode();
#if defined(Q_OS_WIN)
    // A real receiver must distinguish right Ctrl from the existing generic
    // left-Ctrl dispatch. F24 has no shell action in this owned editor.
    const std::size_t rightControlStart = readJson(statePath).value("native_keys", Json::array()).size();
    if (!workflow.action("key", {{"key", "ControlRight+F24"}}, QStringLiteral("right-control-native-red")) ||
        !workflow.stateMatches([&](const Json& state) {
            const Json events = state.value("native_keys", Json::array());
            bool down = false;
            bool up = false;
            for (std::size_t index = rightControlStart; index < events.size(); ++index) {
                const auto& event = events[index];
                if (receivedKey(event) != VK_CONTROL || !event.value("extended", false)) continue;
                if (event.value("up", false)) up = true;
                else down = true;
            }
            return down && up;
        }, "right Control was not delivered as an extended down/up to the owned receiver")) return workflow.exitCode();
    const HKL targetLayout = GetKeyboardLayout(GetWindowThreadProcessId(childHandle, nullptr));
    struct SafeKey { const char* name; WORD messageKey; WORD scan; bool extended; };
    const SafeKey safeKeys[] = {
        {"ControlLeft+F24",VK_CONTROL,0x1d,false}, {"ShiftLeft+F24",VK_SHIFT,0x2a,false},
        {"ShiftRight+F24",VK_SHIFT,0x36,false}, {"AltLeft+F24",VK_MENU,0x38,false},
        {"AltRight+F24",VK_MENU,0x38,true}, {"ControlRight+ShiftRight+F24",VK_SHIFT,0x36,false},
        {"ControlLeft+ControlRight+F24",VK_CONTROL,0x1d,true},
        {"A",'A',WORD(MapVirtualKeyExW('A', MAPVK_VK_TO_VSC_EX, targetLayout) & 0xff),false},
        {"9",'9',WORD(MapVirtualKeyExW('9', MAPVK_VK_TO_VSC_EX, targetLayout) & 0xff),false},
        {"Enter",VK_RETURN,0x1c,false}, {"NumpadEnter",VK_RETURN,0x1c,true},
        {"Numpad0",0,0x52,false}, {"Insert",VK_INSERT,0x52,true},
        {"Numpad1",0,0x4f,false}, {"End",VK_END,0x4f,true},
        {"Numpad2",0,0x50,false}, {"Numpad3",0,0x51,false}, {"Numpad4",0,0x4b,false},
        {"Numpad5",0,0x4c,false}, {"Numpad6",0,0x4d,false}, {"Numpad7",0,0x47,false},
        {"Numpad8",0,0x48,false}, {"Numpad9",0,0x49,false},
        {"NumpadAdd",VK_ADD,0x4e,false}, {"NumpadSubtract",VK_SUBTRACT,0x4a,false},
        {"NumpadMultiply",VK_MULTIPLY,0x37,false}, {"NumpadDecimal",0,0x53,false},
        {"NumpadDivide",VK_DIVIDE,0x35,true},
        {"F24",VK_F24,WORD(MapVirtualKeyExW(VK_F24, MAPVK_VK_TO_VSC_EX, targetLayout) & 0xff),false}};
    for (const auto& expected : safeKeys) {
        const std::size_t start = readJson(statePath).value("native_keys", Json::array()).size();
        if (!workflow.action("key", {{"key", expected.name}}, QString::fromLatin1(expected.name)) ||
            !workflow.stateMatches([&](const Json& state) {
                bool down = false;
                bool up = false;
                const Json events = state.value("native_keys", Json::array());
                for (std::size_t index = start; index < events.size(); ++index) {
                    const auto& event = events[index];
                    if ((expected.messageKey && receivedKey(event) != expected.messageKey) ||
                        event.value("scan", 0) != expected.scan) continue;
                    if (expected.messageKey == VK_SHIFT && expected.scan == 0x36) {
                        if (event.value("semantic_scan", 0) != 0x36 || event.value("left_shift_down", true) ||
                            event.value("right_shift_down", false) == event.value("up", false)) continue;
                    } else if (event.value("extended", false) != expected.extended) continue;
                    if (event.value("up", false)) up = true;
                    else down = true;
                }
                if (!down || !up) return false;
                if (std::string(expected.name) == "ShiftRight+F24") {
                    int sequence = 0;
                    for (std::size_t index = start; index < events.size(); ++index) {
                        const auto& event = events[index];
                        const bool released = event.value("up", false);
                        const bool right = event.value("right_shift_down", false);
                        if (event.value("left_shift_down", true)) return false;
                        if (sequence == 0 && receivedKey(event) == VK_SHIFT && !released && right) ++sequence;
                        else if (sequence == 1 && receivedKey(event) == VK_F24 && !released && right) ++sequence;
                        else if (sequence == 2 && receivedKey(event) == VK_F24 && released && right) ++sequence;
                        else if (sequence == 3 && receivedKey(event) == VK_SHIFT && released && !right) ++sequence;
                    }
                    if (sequence != 4) return false;
                }
                if (std::string(expected.name) == "ControlLeft+ControlRight+F24") {
                    int sequence = 0;
                    for (std::size_t index = start; index < events.size(); ++index) {
                        const auto& event = events[index];
                        const bool released = event.value("up", false);
                        const bool left = event.value("left_control_down", false);
                        const bool right = event.value("right_control_down", false);
                        const bool e0 = event.value("extended", false);
                        const int key = receivedKey(event);
                        if (sequence == 0 && key == VK_CONTROL && !released && !e0 && left && !right) ++sequence;
                        else if (sequence == 1 && key == VK_CONTROL && !released && e0 && left && right) ++sequence;
                        else if (sequence == 2 && key == VK_F24 && !released && left && right) ++sequence;
                        else if (sequence == 3 && key == VK_F24 && released && left && right) ++sequence;
                        else if (sequence == 4 && key == VK_CONTROL && released && e0 && left && !right) ++sequence;
                        else if (sequence == 5 && key == VK_CONTROL && released && !e0 && !left && !right) ++sequence;
                    }
                    if (sequence != 6) return false;
                }
                return true;
            }, "a safe physical key did not arrive with its distinct scan/extended down and up")) return workflow.exitCode();
    }
    for (const auto& physical : std::vector<std::pair<const char*, WORD>>{
             {"Semicolon",VK_OEM_1}, {"Equal",VK_OEM_PLUS}, {"Minus",VK_OEM_MINUS},
             {"Slash",VK_OEM_2}, {"Backquote",VK_OEM_3}, {"BracketLeft",VK_OEM_4},
             {"Backslash",VK_OEM_5}, {"BracketRight",VK_OEM_6}, {"Quote",VK_OEM_7},
             {"Comma",VK_OEM_COMMA}, {"Period",VK_OEM_PERIOD}}) {
        const std::size_t start = readJson(statePath).value("native_keys", Json::array()).size();
        const UINT scan = MapVirtualKeyExW(physical.second, MAPVK_VK_TO_VSC_EX, targetLayout);
        if (!workflow.action("key", {{"key", physical.first}}, QString::fromLatin1(physical.first)) ||
            !workflow.stateMatches([&](const Json& state) {
                bool down = false, up = false;
                const Json events = state.value("native_keys", Json::array());
                for (std::size_t index = start; index < events.size(); ++index) {
                    const auto& event = events[index];
                    if (receivedKey(event) != physical.second || event.value("scan", 0) != int(scan & 0xff)) continue;
                    if (event.value("up", false)) up = true;
                    else down = true;
                }
                return down && up;
            }, "a named physical punctuation key did not reach the owned receiver")) return workflow.exitCode();
    }
    const std::size_t cancelStart = readJson(statePath).value("native_keys", Json::array()).size();
    int checks = 0;
    DesktopNativeInput canceled;
    canceled.kind = DesktopNativeInputKind::Key;
    canceled.key = "ControlRight+ShiftRight+F24";
    canceled.expectedWindowId = native.id;
    canceled.expectedProcessId = native.processId;
    canceled.isCanceled = [&] { return ++checks >= 3; };
    const MaiError canceledResult = maiDesktopInput(canceled);
    if (!workflow.expect(canceledResult.code() == MaiErrorCode::Canceled, "native cancellation did not return Canceled") ||
        !workflow.stateMatches([&](const Json& state) {
            bool down = false, up = false;
            const Json events = state.value("native_keys", Json::array());
            for (std::size_t index = cancelStart; index < events.size(); ++index) {
                const auto& event = events[index];
                if (receivedKey(event) != VK_CONTROL || !event.value("extended", false)) continue;
                if (event.value("up", false)) up = true;
                else down = true;
            }
            return down && up;
        }, "cancel after modifier down did not release the injected right Control")) return workflow.exitCode();
    if (!workflow.expect(waitUntil(allPhysicalInputReleased), "native key probes left input held")) return workflow.exitCode();
#endif
    if (!workflow.action("click", Json::object(), QStringLiteral("click-editor"), InputPosition::Center)) return workflow.exitCode();
    if (!workflow.action("type", {{"text", draft}}, QStringLiteral("type-chinese")) ||
        !textIs(draft, "Unicode input did not arrive in the owned child editor")) return workflow.exitCode();
    if (!workflow.action("key", {{"key", selectAllKey}}, QStringLiteral("select-all")) ||
        !workflow.stateMatches([&](const Json& state) { return state.value("selected_text", std::string()) == draft; },
                               "Select All did not select the child editor text")) return workflow.exitCode();
    if (!workflow.action("type", {{"text", replacement}}, QStringLiteral("replace-selection")) ||
        !textIs(replacement, "typing over the selected text did not replace it")) return workflow.exitCode();
    if (!workflow.action("replace_text", {{"text", ""}}, QStringLiteral("empty-replacement")) ||
        !textIs("", "empty replacement did not clear the child editor")) return workflow.exitCode();
    if (!workflow.action("replace_text", {{"text", finalText}}, QStringLiteral("final-chinese")) ||
        !textIs(finalText, "final Chinese replacement did not arrive")) return workflow.exitCode();
    if (!workflow.action("key", {{"key", "Enter"}}, QStringLiteral("confirm-enter")) ||
        !workflow.stateMatches([&](const Json& state) {
            return state.value("enter_count", 0) == 1 && state.value("confirmed_text", std::string()) == finalText;
        }, "Enter did not confirm the actual child editor text")) return workflow.exitCode();
    if (!workflow.action("key", {{"key", "Ctrl+Enter"}}, QStringLiteral("confirm-control-enter")) ||
        !workflow.stateMatches([&](const Json& state) {
            return state.value("enter_count", 0) == 1 && state.value("control_enter_count", 0) == 1 &&
                state.value("confirmed_text", std::string()) == finalText;
        }, "Enter or Ctrl+Enter did not reach its owned counter exactly once")) return workflow.exitCode();
    if (!workflow.verify(finalText, QStringLiteral("uia-readback"))) return workflow.exitCode();

    // Verify text first, then exercise bounded mouse gestures only in the same
    // child input. The custom context handler records right-click without a menu.
    if (!workflow.action("double_click", Json::object(), QStringLiteral("double-click"), InputPosition::TextStart) ||
        !workflow.stateMatches([](const Json& state) { return !state.value("selected_text", std::string()).empty(); },
                               "native double-click did not select editor text")) return workflow.exitCode();
    if (!workflow.action("right_click", Json::object(), QStringLiteral("right-click"), InputPosition::TextStart) ||
        !workflow.stateMatches([](const Json& state) { return state.value("right_click_count", 0) == 1; },
                               "native right-click was not delivered to the child editor")) return workflow.exitCode();
    if (!workflow.action("drag", Json::object(), QStringLiteral("selection-drag"), InputPosition::SelectionDrag) ||
        !workflow.stateMatches([](const Json& state) { return !state.value("selected_text", std::string()).empty(); },
                               "native drag did not select child editor text")) return workflow.exitCode();
    if (!workflow.verify(finalText, QStringLiteral("uia-readback-after-gestures"))) return workflow.exitCode();
    if (!workflow.observe(QStringLiteral("completed-owned-editor"))) return workflow.exitCode();
    if (!textIs(finalText, "mouse gestures unexpectedly changed the child editor text")) return workflow.exitCode();
    std::fprintf(stderr, "native two-process computer workflow verified: Unicode, Ctrl+A, empty replacement, Enter, gestures and real focusedText readback\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--editor"))
        return runEditor(arguments[2], arguments[3]);
#if defined(Q_OS_WIN)
    if (arguments.size() == 2 && arguments[1] == QStringLiteral("--key-mapping"))
        return verifyWindowsMappingsWithoutInput();
#endif
    if (arguments.size() != 1) return 1;
    return runParent();
}

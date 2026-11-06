#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerKeys.h"

#include <windows.h>
#include <dwmapi.h>
#include <UIAutomation.h>
#include <UIAutomationClient.h>
#include <oleauto.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QString>

namespace {

std::string utf8(const std::wstring& value) {
    return QString::fromStdWString(value).toUtf8().toStdString();
}

std::wstring wide(const std::string& value) {
    return QString::fromUtf8(value.c_str(), int(value.size())).toStdWString();
}

HWND handle(const DesktopNativeWindow& window) {
    return reinterpret_cast<HWND>(std::uintptr_t(window.id));
}

RECT visibleBounds(HWND window) {
    RECT bounds{};
    if (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                              &bounds, sizeof(bounds)) != S_OK)
        GetWindowRect(window, &bounds);
    return bounds;
}

// UIA and DWM report physical screen coordinates. Keep GetWindowRect and pointer
// movement in that same space even when this worker inherits another DPI mode.
struct DpiScope {
    using SetContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetContext setContext = reinterpret_cast<SetContext>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    DPI_AWARENESS_CONTEXT previous = nullptr;
    DpiScope() {
        if (setContext) previous = setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~DpiScope() { if (previous) setContext(previous); }
};

bool windowMatchesProcess(const DesktopNativeWindow& window) {
    DWORD process = 0;
    return IsWindow(handle(window)) &&
        GetWindowThreadProcessId(handle(window), &process) != 0 &&
        process == window.processId;
}

// Child controls and owned menus/HUDs belong to the selected target only when
// their complete owner chain stays in that process. Same-process siblings are
// independent targets and must never receive input through stale coordinates.
bool belongsToSelected(HWND candidate, HWND selected) {
    DWORD selectedPid = 0;
    if (!candidate || !selected || !GetWindowThreadProcessId(selected, &selectedPid))
        return false;
    DWORD candidatePid = 0;
    if (!GetWindowThreadProcessId(candidate, &candidatePid) || candidatePid != selectedPid)
        return false;
    HWND current = GetAncestor(candidate, GA_ROOT);
    for (int depth = 0; current && depth < 32; ++depth) {
        DWORD currentPid = 0;
        if (!GetWindowThreadProcessId(current, &currentPid) || currentPid != selectedPid)
            return false;
        if (current == selected) return true;
        current = GetWindow(current, GW_OWNER);
        if (current) current = GetAncestor(current, GA_ROOT);
    }
    return false;
}

bool interactiveDesktop() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t inputName[256]{};
    wchar_t currentName[256]{};
    DWORD needed = 0;
    const bool matches = GetUserObjectInformationW(input, UOI_NAME,
        inputName, sizeof(inputName), &needed) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            currentName, sizeof(currentName), &needed) &&
        std::wstring(inputName) == currentName;
    CloseDesktop(input);
    return matches;
}

INPUT keyEvent(WORD key, bool up = false) {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = key;
    if (key == VK_DELETE || key == VK_INSERT || key == VK_HOME || key == VK_END ||
        key == VK_PRIOR || key == VK_NEXT || key == VK_LEFT || key == VK_RIGHT ||
        key == VK_UP || key == VK_DOWN || key == VK_LWIN)
        event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up) event.ki.dwFlags |= KEYEVENTF_KEYUP;
    return event;
}

MaiError sendInput(INPUT event) {
    const UINT accepted = SendInput(1, &event, sizeof(INPUT));
    if (accepted != 1)
        // Neither the count nor GetLastError identifies UIPI as the cause.
        return {MaiErrorCode::Internal, "Windows accepted " + std::to_string(accepted) +
            " of 1 input events; input dispatch failed"};
    return {};
}

class InputScope {
public:
    explicit InputScope(const DesktopNativeInput& input)
        : input_(input), foreground_(input.expectedWindowId ?
            reinterpret_cast<HWND>(std::uintptr_t(input.expectedWindowId)) :
            GetForegroundWindow()) {}
    ~InputScope() {
        // Cleanup deliberately ignores cancellation/focus: every successful
        // synthetic down must have its matching up even after an early return.
        for (auto event = releases_.rbegin(); event != releases_.rend(); ++event)
            sendInput(*event);
    }
    MaiError validate() const {
        if (input_.isCanceled && input_.isCanceled())
            return {MaiErrorCode::Canceled, "computer input canceled"};
        if (!foreground_ || !belongsToSelected(GetForegroundWindow(), foreground_))
            return {MaiErrorCode::Busy, "foreground window changed during computer input"};
        DWORD process = 0;
        if (input_.expectedProcessId &&
            (!GetWindowThreadProcessId(foreground_, &process) ||
             process != input_.expectedProcessId))
            return {MaiErrorCode::Busy, "foreground window no longer belongs to the selected process"};
        return {};
    }
    MaiError hold(INPUT down, INPUT up) {
        const MaiError ready = validate();
        if (ready) return ready;
        if (down.type == INPUT_MOUSE) {
            POINT point{};
            if (!GetCursorPos(&point))
                return {MaiErrorCode::Internal, "Windows could not inspect the pointer"};
            const MaiError target = validatePoint(point);
            if (target) return target;
        }
        const MaiError sent = sendInput(down);
        if (!sent) releases_.push_back(up);
        return sent;
    }
    MaiError release() {
        if (releases_.empty()) return {};
        const MaiError sent = sendInput(releases_.back());
        if (!sent) releases_.pop_back();
        return sent;
    }
    MaiError tap(INPUT down, INPUT up) {
        const MaiError held = hold(down, up);
        if (held) return held;
        const MaiError released = release();
        if (released) return released;
        return validate();
    }
    MaiError move(double x, double y) const {
        const POINT point{int(std::lround(x)), int(std::lround(y))};
        const MaiError ready = validatePoint(point);
        if (ready) return ready;
        if (!SetCursorPos(point.x, point.y))
            return {MaiErrorCode::Internal, "Windows could not move the pointer"};
        return validate();
    }
private:
    MaiError validatePoint(POINT point) const {
        const MaiError ready = validate();
        if (ready) return ready;
        if (!belongsToSelected(WindowFromPoint(point), foreground_))
            return {MaiErrorCode::Busy, "pointer location no longer belongs to the selected window"};
        return {};
    }
    const DesktopNativeInput& input_;
    HWND foreground_ = nullptr;
    std::vector<INPUT> releases_;
};

WORD nativeKey(const DesktopKeyChord& key) {
    switch (key.key) {
    case DesktopKeyCode::Character: return WORD(key.character);
    case DesktopKeyCode::Enter: return VK_RETURN;
    case DesktopKeyCode::Tab: return VK_TAB;
    case DesktopKeyCode::Escape: return VK_ESCAPE;
    case DesktopKeyCode::Backspace: return VK_BACK;
    case DesktopKeyCode::Delete: return VK_DELETE;
    case DesktopKeyCode::ArrowLeft: return VK_LEFT;
    case DesktopKeyCode::ArrowRight: return VK_RIGHT;
    case DesktopKeyCode::ArrowUp: return VK_UP;
    case DesktopKeyCode::ArrowDown: return VK_DOWN;
    case DesktopKeyCode::Home: return VK_HOME;
    case DesktopKeyCode::End: return VK_END;
    case DesktopKeyCode::PageUp: return VK_PRIOR;
    case DesktopKeyCode::PageDown: return VK_NEXT;
    case DesktopKeyCode::Space: return VK_SPACE;
    case DesktopKeyCode::Insert: return VK_INSERT;
    case DesktopKeyCode::Function: return WORD(VK_F1 + key.function - 1);
    }
    return 0;
}

MaiError sendChord(InputScope& scope, const DesktopKeyChord& chord) {
    int held = 0;
    for (const auto modifier : chord.modifiers) {
        WORD key = VK_CONTROL;
        switch (modifier) {
        case DesktopKeyModifier::Control: key = VK_CONTROL; break;
        case DesktopKeyModifier::Shift: key = VK_SHIFT; break;
        case DesktopKeyModifier::Alt: key = VK_MENU; break;
        case DesktopKeyModifier::Meta: key = VK_LWIN; break;
        }
        // Preserve modifiers held by the user; only our downs enter cleanup.
        const bool pressed = (GetAsyncKeyState(key) & 0x8000) ||
            (modifier == DesktopKeyModifier::Meta &&
             (GetAsyncKeyState(VK_RWIN) & 0x8000));
        if (pressed) continue;
        const MaiError sent = scope.hold(keyEvent(key), keyEvent(key, true));
        if (sent) return sent;
        ++held;
    }
    const WORD key = nativeKey(chord);
    if (GetAsyncKeyState(key) & 0x8000)
        return {MaiErrorCode::Busy, "the requested key is already held by the user"};
    const MaiError tapped = scope.tap(keyEvent(key), keyEvent(key, true));
    if (tapped) return tapped;
    while (held-- > 0) {
        const MaiError released = scope.release();
        if (released) return released;
    }
    return scope.validate();
}

MaiError sendText(InputScope& scope, const std::string& text) {
    const QString decoded = QString::fromUtf8(text.c_str(), int(text.size()));
    for (QChar unit : decoded) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = unit.unicode();
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        const MaiError sent = scope.tap(down, up);
        if (sent) return sent;
    }
    return scope.validate();
}

bool validPoint(double x, double y) {
    constexpr double limit = double((std::numeric_limits<int>::max)()) - 1;
    return std::isfinite(x) && std::isfinite(y) &&
        std::abs(x) <= limit && std::abs(y) <= limit;
}

bool hasUnexpectedModifiers(const DesktopKeyChord* chord) {
    const std::pair<DesktopKeyModifier, bool> pressed[] = {
        {DesktopKeyModifier::Control, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0},
        {DesktopKeyModifier::Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0},
        {DesktopKeyModifier::Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0},
        {DesktopKeyModifier::Meta, ((GetAsyncKeyState(VK_LWIN) |
                                   GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0}};
    for (const auto& item : pressed) {
        if (item.second && (!chord || std::find(chord->modifiers.begin(),
                chord->modifiers.end(), item.first) == chord->modifiers.end())) return true;
    }
    return false;
}

struct ComScope {
    bool uninitialize = false;
    ComScope() {
        const HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize = SUCCEEDED(status);
    }
    ~ComScope() { if (uninitialize) CoUninitialize(); }
};

MaiResult<IUIAutomation*> automation() {
    IUIAutomation* value = nullptr;
    const HRESULT status = CoCreateInstance(CLSID_CUIAutomation, nullptr,
        CLSCTX_INPROC_SERVER, IID_IUIAutomation, reinterpret_cast<void**>(&value));
    if (FAILED(status) || !value)
        return {MaiErrorCode::NotConfigured, "Windows UI Automation is unavailable"};
    return value;
}

std::string controlRole(CONTROLTYPEID type) {
    switch (type) {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_EditControlTypeId: return "edit";
    case UIA_TextControlTypeId: return "text";
    case UIA_HyperlinkControlTypeId: return "link";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_ComboBoxControlTypeId: return "combobox";
    case UIA_ListItemControlTypeId: return "listitem";
    case UIA_MenuItemControlTypeId: return "menuitem";
    case UIA_TabItemControlTypeId: return "tabitem";
    case UIA_WindowControlTypeId: return "window";
    default: return "control:" + std::to_string(type);
    }
}

MaiResult<IUIAutomationElement*> focusedElementForWindow(
    IUIAutomation* ui, const DesktopNativeWindow& window) {
    const auto active = maiDesktopWindowIsFocused(window);
    if (!active) return active.error();
    if (!active.value())
        return {MaiErrorCode::Busy, "selected window does not have foreground focus"};
    IUIAutomationElement* focused = nullptr;
    if (FAILED(ui->GetFocusedElement(&focused)) || !focused)
        return {MaiErrorCode::NotConfigured, "focused input is unavailable to UI Automation"};
    int process = 0;
    if (FAILED(focused->get_CurrentProcessId(&process)) ||
        std::uint64_t(process) != window.processId) {
        focused->Release();
        return {MaiErrorCode::Busy, "focused input is outside the selected process"};
    }
    IUIAutomationElement* root = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(ui->ElementFromHandle(handle(window), &root)) || !root ||
        FAILED(ui->get_RawViewWalker(&walker)) || !walker) {
        if (root) root->Release();
        if (walker) walker->Release();
        focused->Release();
        return {MaiErrorCode::NotConfigured, "could not verify the focused input window"};
    }
    IUIAutomationElement* current = focused;
    current->AddRef();
    bool belongs = false;
    for (int depth = 0; current && depth < 128; ++depth) {
        BOOL same = FALSE;
        if (SUCCEEDED(ui->CompareElements(current, root, &same)) && same) {
            belongs = true;
            break;
        }
        IUIAutomationElement* parent = nullptr;
        walker->GetParentElement(current, &parent);
        current->Release();
        current = parent;
    }
    if (current) current->Release();
    walker->Release();
    root->Release();
    if (!belongs) {
        focused->Release();
        return {MaiErrorCode::Busy, "focused input is outside the selected window"};
    }
    return focused;
}

} // namespace

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows() {
    DpiScope dpi;
    std::vector<DesktopNativeWindow> windows;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* output = reinterpret_cast<std::vector<DesktopNativeWindow>*>(parameter);
        if (output->size() >= 200) return FALSE;
        if (!IsWindowVisible(window) || IsIconic(window) ||
            GetWindowTextLengthW(window) <= 0) return TRUE;
        const int length = GetWindowTextLengthW(window);
        std::vector<wchar_t> title(std::size_t(length + 1));
        if (GetWindowTextW(window, title.data(), length + 1) <= 0) return TRUE;
        RECT rectangle = visibleBounds(window);
        const int width = rectangle.right - rectangle.left;
        const int height = rectangle.bottom - rectangle.top;
        if (width < 80 || height < 50) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0) return TRUE;
        DesktopNativeWindow candidate;
        candidate.id = std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
        candidate.processId = pid;
        candidate.title = utf8(title.data());
        candidate.x = rectangle.left;
        candidate.y = rectangle.top;
        candidate.width = width;
        candidate.height = height;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            wchar_t path[32768]{};
            DWORD count = DWORD(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &count))
                candidate.application = utf8(std::wstring(path, count));
            CloseHandle(process);
        }
        output->push_back(std::move(candidate));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(
    const DesktopNativeWindow& window) {
    DpiScope dpi;
    if (!windowMatchesProcess(window))
        return {MaiErrorCode::NotFound, "selected window no longer belongs to the observed process"};
    ComScope com;
    const auto opened = automation();
    if (!opened) return opened.error();
    IUIAutomation* ui = opened.value();
    IUIAutomationElement* root = nullptr;
    const HRESULT found = ui->ElementFromHandle(handle(window), &root);
    if (FAILED(found) || !root) {
        ui->Release();
        return {MaiErrorCode::NotFound, "selected window is unavailable to UI Automation"};
    }
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(ui->get_ControlViewWalker(&walker)) || !walker) {
        root->Release();
        ui->Release();
        return {MaiErrorCode::NotConfigured, "UI Automation walker is unavailable"};
    }
    std::vector<DesktopNativeElement> output;
    std::vector<IUIAutomationElement*> pending{root};
    for (std::size_t index = 0; index < pending.size() && index < 250; ++index) {
        IUIAutomationElement* current = pending[index];
        DesktopNativeElement item;
        BSTR name = nullptr;
        if (SUCCEEDED(current->get_CurrentName(&name)) && name) {
            if (SysStringLen(name) <= 4096)
                item.label = utf8(std::wstring(name, SysStringLen(name)));
            SysFreeString(name);
        }
        CONTROLTYPEID type = 0;
        current->get_CurrentControlType(&type);
        item.role = controlRole(type);
        RECT frame{};
        if (!item.label.empty() &&
            SUCCEEDED(current->get_CurrentBoundingRectangle(&frame)) &&
            frame.right > frame.left && frame.bottom > frame.top &&
            output.size() < 80) {
            item.x = frame.left;
            item.y = frame.top;
            item.width = frame.right - frame.left;
            item.height = frame.bottom - frame.top;
            output.push_back(std::move(item));
        }
        IUIAutomationElement* child = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(current, &child)) && child) {
            while (child && pending.size() < 250) {
                pending.push_back(child);
                IUIAutomationElement* next = nullptr;
                walker->GetNextSiblingElement(child, &next);
                child = next;
            }
            if (child) child->Release();
        }
        current->Release();
    }
    walker->Release();
    ui->Release();
    return output;
}

MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window) {
    DpiScope dpi;
    HWND selected = handle(window);
    if (!windowMatchesProcess(window) || !IsWindowVisible(selected) || IsIconic(selected))
        return {MaiErrorCode::NotFound, "selected Windows window is no longer visible"};
    SetForegroundWindow(selected);
    if (GetForegroundWindow() != selected)
        return {MaiErrorCode::Busy, "Windows did not allow the selected window to become foreground"};
    return {};
}

MaiError maiDesktopClick(double x, double y) {
    DesktopNativeInput input;
    input.x = x;
    input.y = y;
    return maiDesktopInput(input);
}

MaiError maiDesktopType(const std::string& text) {
    DesktopNativeInput input;
    input.kind = DesktopNativeInputKind::Type;
    input.text = text;
    return maiDesktopInput(input);
}

MaiError maiDesktopKey(const std::string& key) {
    DesktopNativeInput input;
    input.kind = DesktopNativeInputKind::Key;
    input.key = key;
    return maiDesktopInput(input);
}

MaiError maiDesktopScroll(double x, double y, int lines) {
    DesktopNativeInput input;
    input.kind = DesktopNativeInputKind::Scroll;
    input.x = x;
    input.y = y;
    input.lines = lines;
    return maiDesktopInput(input);
}

MaiResult<bool> maiDesktopWindowIsFocused(const DesktopNativeWindow& window) {
    if (!windowMatchesProcess(window) || !IsWindowVisible(handle(window)) ||
        IsIconic(handle(window)))
        return {MaiErrorCode::NotFound, "selected window is no longer visible in the observed process"};
    return interactiveDesktop() && GetForegroundWindow() == handle(window);
}

MaiResult<bool> maiDesktopWindowAcceptsInput(const DesktopNativeWindow& window) {
    if (!windowMatchesProcess(window) || !IsWindowVisible(handle(window)) ||
        IsIconic(handle(window)))
        return {MaiErrorCode::NotFound, "selected window is no longer visible in the observed process"};
    // Keep owned popup menus active between observations and subsequent keys.
    // Unrelated same-process windows and every cross-process window fail here.
    return interactiveDesktop() && belongsToSelected(GetForegroundWindow(), handle(window));
}

MaiError maiDesktopInput(const DesktopNativeInput& input) {
    DpiScope dpi;
    if (input.kind == DesktopNativeInputKind::Type ||
        input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
        if (input.text.size() > 4000 ||
            (input.kind == DesktopNativeInputKind::Type && input.text.empty()))
            return {MaiErrorCode::InvalidInput, "text must contain at most 4000 UTF-8 bytes"};
    }
    const bool positioned = input.kind == DesktopNativeInputKind::Click ||
        input.kind == DesktopNativeInputKind::DoubleClick ||
        input.kind == DesktopNativeInputKind::RightClick ||
        input.kind == DesktopNativeInputKind::Drag ||
        input.kind == DesktopNativeInputKind::Scroll;
    if (positioned && !validPoint(input.x, input.y))
        return {MaiErrorCode::InvalidInput, "pointer coordinates are invalid"};
    if (input.kind == DesktopNativeInputKind::Drag &&
        (!validPoint(input.toX, input.toY) || input.durationMs < 1 || input.durationMs > 200))
        return {MaiErrorCode::InvalidInput, "drag needs a valid destination and 1 to 200 ms duration"};
    if (input.kind == DesktopNativeInputKind::Scroll &&
        (input.lines == 0 || input.lines < -10 || input.lines > 10))
        return {MaiErrorCode::InvalidInput, "scroll needs -10 to 10 lines, excluding zero"};
    if ((input.expectedWindowId == 0) != (input.expectedProcessId == 0))
        return {MaiErrorCode::InvalidInput, "expected window and process identities must be provided together"};
    DesktopKeyChord chord;
    if (input.kind == DesktopNativeInputKind::Key) {
        const auto parsed = maiDesktopParseKey(input.key);
        if (!parsed) return parsed.error();
        chord = parsed.value();
    }
    // Extra physical modifiers change the requested action (for example a
    // Ctrl-click or a different shortcut). Preserve those user keys and wait.
    if (hasUnexpectedModifiers(input.kind == DesktopNativeInputKind::Key ? &chord : nullptr))
        return {MaiErrorCode::Busy, "release modifiers that are not part of the requested action"};
    if (!interactiveDesktop())
        return {MaiErrorCode::NotConfigured, "Windows input requires the active interactive desktop"};
    InputScope scope(input);
    const MaiError ready = scope.validate();
    if (ready) return ready;
    if (input.kind == DesktopNativeInputKind::Key) return sendChord(scope, chord);
    if (input.kind == DesktopNativeInputKind::Type)
        return sendText(scope, input.text);
    if (input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
        const auto select = maiDesktopParseKey("Ctrl+A");
        const MaiError selected = sendChord(scope, select.value());
        if (selected) return selected;
        // Replacing with an empty value must delete the selection explicitly.
        if (input.text.empty())
            return scope.tap(keyEvent(VK_BACK), keyEvent(VK_BACK, true));
        return sendText(scope, input.text);
    }
    // Moving a pointer while the user holds another button can drag their
    // content before our own down occurs. Check before any cursor movement.
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000))
        return {MaiErrorCode::Busy, "a mouse button is already held by the user"};
    const MaiError moved = scope.move(input.x, input.y);
    if (moved) return moved;
    if (input.kind == DesktopNativeInputKind::Scroll) {
        INPUT event{};
        event.type = INPUT_MOUSE;
        event.mi.dwFlags = MOUSEEVENTF_WHEEL;
        event.mi.mouseData = DWORD(input.lines * WHEEL_DELTA);
        const MaiError sent = sendInput(event);
        return sent ? sent : scope.validate();
    }
    const bool right = input.kind == DesktopNativeInputKind::RightClick;
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    INPUT up = down;
    up.mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    if (input.kind == DesktopNativeInputKind::Drag) {
        const MaiError held = scope.hold(down, up);
        if (held) return held;
        const int steps = (std::max)(1, input.durationMs / 10);
        const auto started = std::chrono::steady_clock::now();
        for (int step = 1; step <= steps; ++step) {
            std::this_thread::sleep_until(started +
                std::chrono::milliseconds(input.durationMs * step / steps));
            const double fraction = double(step) / steps;
            const MaiError dragged = scope.move(input.x + (input.toX - input.x) * fraction,
                input.y + (input.toY - input.y) * fraction);
            if (dragged) return dragged;
        }
        const MaiError released = scope.release();
        return released ? released : scope.validate();
    }
    const MaiError clicked = scope.tap(down, up);
    if (clicked || input.kind != DesktopNativeInputKind::DoubleClick) return clicked;
    // Windows derives WM_*DBLCLK from two native clicks within its own time and
    // distance thresholds. Keep the pointer fixed and use that system interval.
    std::this_thread::sleep_for(std::chrono::milliseconds(
        (std::min)(DWORD(40), (std::max)(DWORD(1), DWORD(GetDoubleClickTime() / 4)))));
    return scope.tap(down, up);
}

MaiResult<std::string> maiDesktopFocusedText(const DesktopNativeWindow& window) {
    DpiScope dpi;
    ComScope com;
    const auto opened = automation();
    if (!opened) return opened.error();
    IUIAutomation* ui = opened.value();
    const auto found = focusedElementForWindow(ui, window);
    if (!found) {
        ui->Release();
        return found.error();
    }
    IUIAutomationElement* focused = found.value();
    BOOL secure = FALSE;
    if (FAILED(focused->get_CurrentIsPassword(&secure)) || secure) {
        focused->Release();
        ui->Release();
        return {MaiErrorCode::NotSupported, "secure or unverified focused input cannot be read"};
    }
    BSTR value = nullptr;
    HRESULT read = E_FAIL;
    IUIAutomationValuePattern* valuePattern = nullptr;
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_ValuePatternId,
            IID_IUIAutomationValuePattern, reinterpret_cast<void**>(&valuePattern))) && valuePattern) {
        read = valuePattern->get_CurrentValue(&value);
        valuePattern->Release();
    } else {
        IUIAutomationTextPattern* textPattern = nullptr;
        if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId,
                IID_IUIAutomationTextPattern, reinterpret_cast<void**>(&textPattern))) && textPattern) {
            IUIAutomationTextRange* range = nullptr;
            if (SUCCEEDED(textPattern->get_DocumentRange(&range)) && range) {
                read = range->GetText(65537, &value);
                range->Release();
            }
            textPattern->Release();
        }
    }
    focused->Release();
    ui->Release();
    if (FAILED(read)) {
        if (value) SysFreeString(value);
        return {MaiErrorCode::NotSupported, "focused input does not expose a readable text value"};
    }
    if (value && SysStringLen(value) > 65536) {
        SysFreeString(value);
        return {MaiErrorCode::NotSupported, "focused input text exceeds the readback limit"};
    }
    const std::string result = value ? utf8(std::wstring(value, SysStringLen(value))) : std::string();
    if (value) SysFreeString(value);
    const auto stillFocused = maiDesktopWindowIsFocused(window);
    if (!stillFocused) return stillFocused.error();
    if (!stillFocused.value())
        return {MaiErrorCode::Busy, "selected window lost focus while reading its text"};
    return result;
}

MaiResult<bool> maiDesktopFocusedInputIsSecure() {
    ComScope com;
    const auto opened = automation();
    if (!opened) return opened.error();
    IUIAutomation* ui = opened.value();
    IUIAutomationElement* focused = nullptr;
    const HRESULT status = ui->GetFocusedElement(&focused);
    ui->Release();
    if (FAILED(status) || !focused)
        return {MaiErrorCode::NotConfigured, "focused input is unavailable to UI Automation"};
    BOOL protectedInput = FALSE;
    const HRESULT secure = focused->get_CurrentIsPassword(&protectedInput);
    focused->Release();
    if (FAILED(secure))
        return {MaiErrorCode::NotConfigured, "could not verify whether focused input is secure"};
    return protectedInput != FALSE;
}

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerWindowsInputInternal.h"
#include "MaiChatTools/DesktopComputerWindowsTouch.h"

#include <windows.h>
#include <dwmapi.h>
#include <UIAutomation.h>
#include <UIAutomationClient.h>
#include <oleauto.h>

#include <cmath>
#include <atomic>
#include <optional>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QString>
#include <QScopeGuard>

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
    if (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)) != S_OK)
        GetWindowRect(window, &bounds);
    return bounds;
}

// UIA and DWM report physical screen coordinates. Keep GetWindowRect and pointer
// movement in that same space even when this worker inherits another DPI mode.
struct DpiScope {
    using SetContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetContext setContext =
        reinterpret_cast<SetContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    DPI_AWARENESS_CONTEXT previous = nullptr;
    DpiScope() {
        if (setContext) previous = setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~DpiScope() {
        if (previous) setContext(previous);
    }
};

bool windowMatchesProcess(const DesktopNativeWindow& window) {
    DWORD process = 0;
    return IsWindow(handle(window)) && GetWindowThreadProcessId(handle(window), &process) != 0 &&
           process == window.processId;
}

// Child controls and owned menus/HUDs belong to the selected target only when
// their complete owner chain stays in that process. Same-process siblings are
// independent targets and must never receive input through stale coordinates.
bool belongsToSelected(HWND candidate, HWND selected) {
    DWORD selectedPid = 0;
    if (!candidate || !selected || !GetWindowThreadProcessId(selected, &selectedPid)) return false;
    DWORD candidatePid = 0;
    if (!GetWindowThreadProcessId(candidate, &candidatePid) || candidatePid != selectedPid) return false;
    HWND current = GetAncestor(candidate, GA_ROOT);
    for (int depth = 0; current && depth < 32; ++depth) {
        DWORD currentPid = 0;
        if (!GetWindowThreadProcessId(current, &currentPid) || currentPid != selectedPid) return false;
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
    const bool matches = GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) &&
                         GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, currentName,
                                                   sizeof(currentName), &needed) &&
                         std::wstring(inputName) == currentName;
    CloseDesktop(input);
    return matches;
}

// HMONITOR is the native identity shared with WGC. Resolve it through the
// active monitor list instead of accepting a nearest/primary fallback.
bool monitorInformation(std::uint64_t id, MONITORINFO& information) {
    if (!id || id > std::numeric_limits<std::uintptr_t>::max()) return false;
    struct Search {
        HMONITOR selected = nullptr;
        MONITORINFO information{};
        bool found = false;
    } search;
    search.selected = reinterpret_cast<HMONITOR>(std::uintptr_t(id));
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(parameter);
            if (monitor != search.selected) return TRUE;
            search.information.cbSize = sizeof(MONITORINFO);
            search.found = GetMonitorInfoW(monitor, &search.information) != FALSE;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found) information = search.information;
    return search.found;
}

DesktopNativeDisplay nativeDisplay(HMONITOR monitor, const MONITORINFO& information) {
    DesktopNativeDisplay display;
    display.id = std::uint64_t(reinterpret_cast<std::uintptr_t>(monitor));
    display.x = information.rcMonitor.left;
    display.y = information.rcMonitor.top;
    display.width = information.rcMonitor.right - information.rcMonitor.left;
    display.height = information.rcMonitor.bottom - information.rcMonitor.top;
    display.primary = (information.dwFlags & MONITORINFOF_PRIMARY) != 0;
    return display;
}

INPUT keyEvent(const DesktopWindowsKeyBinding& key, bool up = false) {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = key.virtualKey;
    event.ki.wScan = key.scanCode;
    if (key.extended) event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up) event.ki.dwFlags |= KEYEVENTF_KEYUP;
    return event;
}

DesktopWindowsKeyBinding virtualKeyBinding(WORD key, HKL layout) {
    const UINT scan = MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, layout);
    DesktopWindowsKeyBinding binding;
    binding.virtualKey = key;
    binding.scanCode = WORD(scan & 0xff);
    binding.extended = (scan & 0xff00) == 0xe000;
    // Some CJK layouts return only the low scan byte even with VK_TO_VSC_EX.
    // Preserve the independent navigation cluster and other documented E0
    // identities explicitly; keypad digits keep their separate non-E0 VKs.
    switch (key) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_DIVIDE: binding.extended = true; break;
        default: break;
    }
    // NumLock and Pause are distinct VK sequences. Pause is E1, not E0; neither
    // may inherit KEYEVENTF_EXTENDEDKEY merely because their scan bytes overlap.
    if (key == VK_NUMLOCK || key == VK_PAUSE) binding.extended = false;
    if (key == VK_SNAPSHOT) {
        binding.scanCode = 0x37;
        binding.extended = true;
    }
    return binding;
}

HKL foregroundLayout() {
    const DWORD thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    return thread ? GetKeyboardLayout(thread) : nullptr;
}

MaiResult<DesktopWindowsKeyBinding> modifierBinding(DesktopKeyModifier modifier, HKL layout) {
    WORD key = 0;
    switch (modifier) {
        case DesktopKeyModifier::Control: key = VK_CONTROL; break;
        case DesktopKeyModifier::Shift: key = VK_SHIFT; break;
        case DesktopKeyModifier::Alt: key = VK_MENU; break;
        case DesktopKeyModifier::Meta: key = VK_LWIN; break;
        case DesktopKeyModifier::ControlLeft: key = VK_LCONTROL; break;
        case DesktopKeyModifier::ControlRight: key = VK_RCONTROL; break;
        case DesktopKeyModifier::ShiftLeft: key = VK_LSHIFT; break;
        case DesktopKeyModifier::ShiftRight: key = VK_RSHIFT; break;
        case DesktopKeyModifier::AltLeft: key = VK_LMENU; break;
        case DesktopKeyModifier::AltRight: key = VK_RMENU; break;
        case DesktopKeyModifier::MetaLeft: key = VK_LWIN; break;
        case DesktopKeyModifier::MetaRight: key = VK_RWIN; break;
    }
    if (!key) return {MaiErrorCode::NotSupported, "Windows has no native mapping for this modifier"};
    return virtualKeyBinding(key, layout);
}

bool modifierPressed(DesktopKeyModifier modifier) {
    const auto binding = modifierBinding(modifier, GetKeyboardLayout(0));
    if (!binding) return false;
    if (modifier == DesktopKeyModifier::Meta)
        return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    return (GetAsyncKeyState(binding.value().virtualKey) & 0x8000) != 0;
}

bool hasModifierFamily(const std::vector<DesktopKeyModifier>& modifiers, DesktopKeyModifier family) {
    return std::any_of(modifiers.begin(), modifiers.end(),
                       [family](DesktopKeyModifier modifier) { return maiDesktopModifierFamily(modifier) == family; });
}

DesktopKeyChord withImpliedModifiers(DesktopKeyChord chord, const DesktopWindowsKeyBinding& binding) {
    for (const auto modifier : binding.impliedModifiers)
        if (!hasModifierFamily(chord.modifiers, maiDesktopModifierFamily(modifier)))
            chord.modifiers.push_back(modifier);
    return chord;
}

#include "MaiChatTools/DesktopComputerWindowsScope.h"

MaiError sendChord(InputScope& scope, const DesktopKeyChord& chord, const DesktopWindowsKeyBinding& key, HKL layout) {
    DesktopKeyModifier terminal;
    const bool isModifier = maiDesktopKeyModifier(chord.key, terminal);
    if (!isModifier && (scope.ownsBinding(key) || (scope.keyState(key.virtualKey) & 0x8000)))
        return {MaiErrorCode::Busy, "the requested terminal key is already held"};
    int held = 0;
    for (const auto modifier : chord.modifiers) {
        const auto binding = modifierBinding(modifier, layout);
        if (!binding) return binding.error();
        // Generic families borrow either user-held side. Explicit modifiers only
        // borrow that exact side, and every injected side owns its matching up.
        if (scope.modifierPressed(modifier)) continue;
        const MaiError sent = scope.hold(keyEvent(binding.value()), keyEvent(binding.value(), true));
        if (sent) return sent;
        ++held;
    }
    if (!isModifier || !scope.modifierPressed(terminal)) {
        const MaiError tapped = scope.tap(keyEvent(key), keyEvent(key, true));
        if (tapped) return tapped;
    }
    while (held-- > 0) {
        const MaiError released = scope.release();
        if (released) return released;
    }
    return scope.validate();
}

MaiError sendChord(InputScope& scope, const DesktopKeyChord& chord) {
    const HKL layout = scope.layout();
    const auto binding = maiDesktopWindowsResolveKey(chord, reinterpret_cast<std::uintptr_t>(layout));
    if (!binding) return binding.error();
    return sendChord(scope, withImpliedModifiers(chord, binding.value()), binding.value(), layout);
}

MaiError InputScope::unicode(std::string_view text) {
    DesktopInputEvent request;
    request.kind = DesktopInputEventKind::UnicodeText;
    request.text = std::string(text);
    if (const auto error = maiDesktopValidateInputEvents({request})) return error;
    if (text.empty()) return {};
    if (const auto secure = guardLiteralText()) return secure;
    const QString decoded = QString::fromUtf8(text.data(), int(text.size()));
    for (QChar unit : decoded) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = unit.unicode();
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        if (const auto sent = tap(down, up)) return sent;
    }
    return validate();
}

bool validPoint(double x, double y) {
    constexpr double limit = double((std::numeric_limits<int>::max)()) - 1;
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= limit && std::abs(y) <= limit;
}

bool hasUnexpectedModifiers(const DesktopKeyChord* chord, const InputScope* owner = nullptr) {
    std::vector<DesktopKeyModifier> requested = chord ? chord->modifiers : std::vector<DesktopKeyModifier>{};
    DesktopKeyModifier terminal;
    if (chord && maiDesktopKeyModifier(chord->key, terminal)) requested.push_back(terminal);
    const std::pair<DesktopKeyModifier, std::pair<DesktopKeyModifier, DesktopKeyModifier>> sides[] = {
        {DesktopKeyModifier::Control, {DesktopKeyModifier::ControlLeft, DesktopKeyModifier::ControlRight}},
        {DesktopKeyModifier::Shift, {DesktopKeyModifier::ShiftLeft, DesktopKeyModifier::ShiftRight}},
        {DesktopKeyModifier::Alt, {DesktopKeyModifier::AltLeft, DesktopKeyModifier::AltRight}},
        {DesktopKeyModifier::Meta, {DesktopKeyModifier::MetaLeft, DesktopKeyModifier::MetaRight}}};
    const auto contains = [&](DesktopKeyModifier modifier) {
        return std::find(requested.begin(), requested.end(), modifier) != requested.end();
    };
    for (const auto& family : sides) {
        if (contains(family.first)) continue;
        for (const auto side : {family.second.first, family.second.second})
            if ((owner ? owner->modifierPressed(side) : modifierPressed(side)) && !contains(side) && !(owner && owner->ownsModifier(side))) return true;
    }
    return false;
}

struct ComScope {
    bool uninitialize = false;
    ComScope() {
        const HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize = SUCCEEDED(status);
    }
    ~ComScope() {
        if (uninitialize) CoUninitialize();
    }
};

MaiResult<IUIAutomation*> automation() {
    IUIAutomation* value = nullptr;
    const HRESULT status = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                            reinterpret_cast<void**>(&value));
    if (FAILED(status) || !value) return {MaiErrorCode::NotConfigured, "Windows UI Automation is unavailable"};
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

MaiResult<IUIAutomationElement*> focusedElementForWindow(IUIAutomation* ui, const DesktopNativeWindow& window) {
    const auto active = maiDesktopWindowIsFocused(window);
    if (!active) return active.error();
    if (!active.value()) return {MaiErrorCode::Busy, "selected window does not have foreground focus"};
    IUIAutomationElement* focused = nullptr;
    if (FAILED(ui->GetFocusedElement(&focused)) || !focused)
        return {MaiErrorCode::NotConfigured, "focused input is unavailable to UI Automation"};
    int process = 0;
    if (FAILED(focused->get_CurrentProcessId(&process)) || std::uint64_t(process) != window.processId) {
        focused->Release();
        return {MaiErrorCode::Busy, "focused input is outside the selected process"};
    }
    IUIAutomationElement* root = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(ui->ElementFromHandle(handle(window), &root)) || !root || FAILED(ui->get_RawViewWalker(&walker)) ||
        !walker) {
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

}  // namespace

MaiResult<DesktopWindowsKeyBinding> maiDesktopWindowsResolveKey(const DesktopKeyChord& chord,
                                                                std::uintptr_t keyboardLayout) {
    if (chord.key == DesktopKeyCode::Delete && hasModifierFamily(chord.modifiers, DesktopKeyModifier::Control) &&
        hasModifierFamily(chord.modifiers, DesktopKeyModifier::Alt))
        return {MaiErrorCode::NotSupported,
                "Ctrl+Alt+Delete is a system secure-attention sequence, not injectable keyboard input"};
    if (chord.key == DesktopKeyCode::Fn || chord.key == DesktopKeyCode::Eisu || chord.key == DesktopKeyCode::IntlRo ||
        chord.key == DesktopKeyCode::IntlYen || chord.key == DesktopKeyCode::NumpadEqual ||
        chord.key == DesktopKeyCode::NumpadComma)
        return {MaiErrorCode::NotSupported, "Windows has no reliable distinct native mapping for this hardware key"};
    if (!keyboardLayout) return {MaiErrorCode::NotConfigured, "the target Windows keyboard layout is unavailable"};
    const HKL layout = reinterpret_cast<HKL>(keyboardLayout);
    if (chord.key == DesktopKeyCode::Character) {
        const unsigned char character = static_cast<unsigned char>(chord.character);
        if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))
            return virtualKeyBinding(character, layout);
        if (character < '!' || character > '~')
            return {MaiErrorCode::InvalidInput, "a keyboard character must be printable ASCII"};
        const SHORT translated = VkKeyScanExW(wchar_t(character), layout);
        if (translated == -1 || (HIBYTE(translated) & ~7))
            return {MaiErrorCode::NotSupported,
                    "this character has no native key in the target keyboard layout; use Unicode type for text"};
        auto binding = virtualKeyBinding(LOBYTE(translated), layout);
        if (!binding.virtualKey)
            return {MaiErrorCode::NotSupported, "the target keyboard layout returned an empty virtual key"};
        if (HIBYTE(translated) & 2) binding.impliedModifiers.push_back(DesktopKeyModifier::Control);
        if (HIBYTE(translated) & 1) binding.impliedModifiers.push_back(DesktopKeyModifier::Shift);
        if (HIBYTE(translated) & 4) binding.impliedModifiers.push_back(DesktopKeyModifier::Alt);
        return binding;
    }
    DesktopKeyModifier modifier;
    if (maiDesktopKeyModifier(chord.key, modifier)) return modifierBinding(modifier, layout);
    if (chord.key == DesktopKeyCode::Function) {
        if (chord.function < 1 || chord.function > 24)
            return {MaiErrorCode::InvalidInput, "Windows function keys are F1 through F24"};
        return virtualKeyBinding(WORD(VK_F1 + chord.function - 1), layout);
    }
    const std::pair<DesktopKeyCode, WORD> keys[] = {{DesktopKeyCode::Enter, VK_RETURN},
                                                    {DesktopKeyCode::Tab, VK_TAB},
                                                    {DesktopKeyCode::Escape, VK_ESCAPE},
                                                    {DesktopKeyCode::Backspace, VK_BACK},
                                                    {DesktopKeyCode::Delete, VK_DELETE},
                                                    {DesktopKeyCode::ArrowLeft, VK_LEFT},
                                                    {DesktopKeyCode::ArrowRight, VK_RIGHT},
                                                    {DesktopKeyCode::ArrowUp, VK_UP},
                                                    {DesktopKeyCode::ArrowDown, VK_DOWN},
                                                    {DesktopKeyCode::Home, VK_HOME},
                                                    {DesktopKeyCode::End, VK_END},
                                                    {DesktopKeyCode::PageUp, VK_PRIOR},
                                                    {DesktopKeyCode::PageDown, VK_NEXT},
                                                    {DesktopKeyCode::Space, VK_SPACE},
                                                    {DesktopKeyCode::Insert, VK_INSERT},
                                                    {DesktopKeyCode::Backquote, VK_OEM_3},
                                                    {DesktopKeyCode::Minus, VK_OEM_MINUS},
                                                    {DesktopKeyCode::Equal, VK_OEM_PLUS},
                                                    {DesktopKeyCode::BracketLeft, VK_OEM_4},
                                                    {DesktopKeyCode::BracketRight, VK_OEM_6},
                                                    {DesktopKeyCode::Backslash, VK_OEM_5},
                                                    {DesktopKeyCode::IntlBackslash, VK_OEM_102},
                                                    {DesktopKeyCode::Semicolon, VK_OEM_1},
                                                    {DesktopKeyCode::Quote, VK_OEM_7},
                                                    {DesktopKeyCode::Comma, VK_OEM_COMMA},
                                                    {DesktopKeyCode::Period, VK_OEM_PERIOD},
                                                    {DesktopKeyCode::Slash, VK_OEM_2},
                                                    {DesktopKeyCode::CapsLock, VK_CAPITAL},
                                                    {DesktopKeyCode::NumLock, VK_NUMLOCK},
                                                    {DesktopKeyCode::ScrollLock, VK_SCROLL},
                                                    {DesktopKeyCode::PrintScreen, VK_SNAPSHOT},
                                                    {DesktopKeyCode::Pause, VK_PAUSE},
                                                    {DesktopKeyCode::ContextMenu, VK_APPS},
                                                    {DesktopKeyCode::Numpad0, VK_NUMPAD0},
                                                    {DesktopKeyCode::Numpad1, VK_NUMPAD1},
                                                    {DesktopKeyCode::Numpad2, VK_NUMPAD2},
                                                    {DesktopKeyCode::Numpad3, VK_NUMPAD3},
                                                    {DesktopKeyCode::Numpad4, VK_NUMPAD4},
                                                    {DesktopKeyCode::Numpad5, VK_NUMPAD5},
                                                    {DesktopKeyCode::Numpad6, VK_NUMPAD6},
                                                    {DesktopKeyCode::Numpad7, VK_NUMPAD7},
                                                    {DesktopKeyCode::Numpad8, VK_NUMPAD8},
                                                    {DesktopKeyCode::Numpad9, VK_NUMPAD9},
                                                    {DesktopKeyCode::NumpadAdd, VK_ADD},
                                                    {DesktopKeyCode::NumpadSubtract, VK_SUBTRACT},
                                                    {DesktopKeyCode::NumpadMultiply, VK_MULTIPLY},
                                                    {DesktopKeyCode::NumpadDivide, VK_DIVIDE},
                                                    {DesktopKeyCode::NumpadDecimal, VK_DECIMAL},
                                                    {DesktopKeyCode::NumpadEnter, VK_RETURN},
                                                    {DesktopKeyCode::Clear, VK_CLEAR},
                                                    {DesktopKeyCode::Help, VK_HELP},
                                                    {DesktopKeyCode::Convert, VK_CONVERT},
                                                    {DesktopKeyCode::NonConvert, VK_NONCONVERT},
                                                    {DesktopKeyCode::KanaMode, VK_KANA},
                                                    {DesktopKeyCode::VolumeMute, VK_VOLUME_MUTE},
                                                    {DesktopKeyCode::VolumeDown, VK_VOLUME_DOWN},
                                                    {DesktopKeyCode::VolumeUp, VK_VOLUME_UP},
                                                    {DesktopKeyCode::MediaPlayPause, VK_MEDIA_PLAY_PAUSE},
                                                    {DesktopKeyCode::MediaStop, VK_MEDIA_STOP},
                                                    {DesktopKeyCode::MediaNext, VK_MEDIA_NEXT_TRACK},
                                                    {DesktopKeyCode::MediaPrevious, VK_MEDIA_PREV_TRACK},
                                                    {DesktopKeyCode::BrowserBack, VK_BROWSER_BACK},
                                                    {DesktopKeyCode::BrowserForward, VK_BROWSER_FORWARD},
                                                    {DesktopKeyCode::BrowserRefresh, VK_BROWSER_REFRESH},
                                                    {DesktopKeyCode::BrowserStop, VK_BROWSER_STOP},
                                                    {DesktopKeyCode::BrowserSearch, VK_BROWSER_SEARCH},
                                                    {DesktopKeyCode::BrowserFavorites, VK_BROWSER_FAVORITES},
                                                    {DesktopKeyCode::BrowserHome, VK_BROWSER_HOME},
                                                    {DesktopKeyCode::LaunchMail, VK_LAUNCH_MAIL},
                                                    {DesktopKeyCode::LaunchMedia, VK_LAUNCH_MEDIA_SELECT},
                                                    {DesktopKeyCode::LaunchApp1, VK_LAUNCH_APP1},
                                                    {DesktopKeyCode::LaunchApp2, VK_LAUNCH_APP2}};
    for (const auto& item : keys) {
        if (item.first != chord.key) continue;
        auto binding = virtualKeyBinding(item.second, layout);
        if (chord.key == DesktopKeyCode::NumpadEnter) {
            binding.scanCode = 0x1c;
            binding.extended = true;
        }
        return binding;
    }
    return {MaiErrorCode::NotSupported, "Windows has no native mapping for this key"};
}

MaiResult<std::vector<DesktopNativeDisplay>> maiDesktopDisplays() {
    DpiScope dpi;
    struct Enumeration {
        std::vector<DesktopNativeDisplay> displays;
        bool failed = false;
    } enumeration;
    const BOOL enumerated = EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
            auto& enumeration = *reinterpret_cast<Enumeration*>(parameter);
            MONITORINFO information{};
            information.cbSize = sizeof(information);
            if (!GetMonitorInfoW(monitor, &information) || information.rcMonitor.right <= information.rcMonitor.left ||
                information.rcMonitor.bottom <= information.rcMonitor.top) {
                enumeration.failed = true;
                return FALSE;
            }
            enumeration.displays.push_back(nativeDisplay(monitor, information));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&enumeration));
    if (!enumerated || enumeration.failed)
        return {MaiErrorCode::Internal, "Windows could not enumerate active displays"};
    return enumeration.displays;
}

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows() {
    DpiScope dpi;
    std::vector<DesktopNativeWindow> windows;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* output = reinterpret_cast<std::vector<DesktopNativeWindow>*>(parameter);
            if (output->size() >= 200) return FALSE;
            if (!IsWindowVisible(window) || IsIconic(window) || GetWindowTextLengthW(window) <= 0) return TRUE;
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
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(const DesktopNativeWindow& window) {
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
            if (SysStringLen(name) <= 4096) item.label = utf8(std::wstring(name, SysStringLen(name)));
            SysFreeString(name);
        }
        CONTROLTYPEID type = 0;
        current->get_CurrentControlType(&type);
        item.role = controlRole(type);
        RECT frame{};
        if (!item.label.empty() && SUCCEEDED(current->get_CurrentBoundingRectangle(&frame)) &&
            frame.right > frame.left && frame.bottom > frame.top && output.size() < 80) {
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
    return maiDesktopActivateWindow(window, {});
}

MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window, std::function<bool()> isCanceled) {
    DpiScope dpi;
    if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
    HWND selected = handle(window);
    if (!windowMatchesProcess(window) || !IsWindowVisible(selected) || IsIconic(selected))
        return {MaiErrorCode::NotFound, "selected Windows window is no longer visible"};
    SetForegroundWindow(selected);
    // Cross-input-queue activation is asynchronous. Wait only for the real
    // foreground transition after this one normal request; a message round trip
    // alone can still precede it. Do not grant rights or attach input queues.
    // Same-thread activation is already synchronous.
    if (GetWindowThreadProcessId(selected, nullptr) != GetCurrentThreadId()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (GetForegroundWindow() != selected && std::chrono::steady_clock::now() < deadline) {
            if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
            if (!windowMatchesProcess(window) || !IsWindowVisible(selected) || IsIconic(selected))
                return {MaiErrorCode::NotFound, "selected Windows window changed while becoming foreground"};
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
    if (!windowMatchesProcess(window) || !IsWindowVisible(selected) || IsIconic(selected))
        return {MaiErrorCode::NotFound, "selected Windows window changed while becoming foreground"};
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
    if (!windowMatchesProcess(window) || !IsWindowVisible(handle(window)) || IsIconic(handle(window)))
        return {MaiErrorCode::NotFound, "selected window is no longer visible in the observed process"};
    return interactiveDesktop() && GetForegroundWindow() == handle(window);
}

MaiResult<bool> maiDesktopWindowAcceptsInput(const DesktopNativeWindow& window) {
    if (!windowMatchesProcess(window) || !IsWindowVisible(handle(window)) || IsIconic(handle(window)))
        return {MaiErrorCode::NotFound, "selected window is no longer visible in the observed process"};
    // Keep owned popup menus active between observations and subsequent keys.
    // Unrelated same-process windows and every cross-process window fail here.
    return interactiveDesktop() && belongsToSelected(GetForegroundWindow(), handle(window));
}

namespace {
std::string independentKeyTag(const DesktopKeyChord& key) {
    return "key:" + std::to_string(int(key.key)) + ":" + std::to_string(int(key.character)) + ":" +
           std::to_string(key.function);
}

std::string independentKeyName(const DesktopKeyChord& key, const std::string& requested) {
    for (const auto& name : maiDesktopKeyNames()) {
        const auto candidate = maiDesktopParseKey(name);
        if (candidate && candidate.value().modifiers.empty() && candidate.value().key == key.key &&
            candidate.value().character == key.character && candidate.value().function == key.function)
            return name;
    }
    return requested;
}

struct MouseBinding {
    const char* name;
    int key;
    DWORD down;
    DWORD up;
    DWORD data;
};

MouseBinding mouseBinding(DesktopMouseButton button) {
    switch (button) {
        case DesktopMouseButton::Left: return {"left", VK_LBUTTON, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0};
        case DesktopMouseButton::Right: return {"right", VK_RBUTTON, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0};
        case DesktopMouseButton::Middle: return {"middle", VK_MBUTTON, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0};
        case DesktopMouseButton::Back: return {"back", VK_XBUTTON1, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1};
        case DesktopMouseButton::Forward: return {"forward", VK_XBUTTON2, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2};
    }
    return {};
}


MaiError InputScope::dispatchTyped(const DesktopInputEvent& event, const DesktopInputPort& port,
                                  const DesktopInputOptions& options) {
    using Kind = DesktopInputEventKind;
    if ((event.kind == Kind::RelativeMove && !event.deltaX && !event.deltaY) ||
        (event.kind == Kind::Wheel && !event.wheelDelta120) || (event.kind == Kind::UnicodeText && event.text.empty())) return {};
    if (event.kind == Kind::RawKey) {
        if (!event.key.release && !options.keyboardEnabled)
            return {MaiErrorCode::NotConfigured, "keyboard input is disabled"};
        return rawKey(event.key, options);
    }
    if (event.kind == Kind::MouseButton) {
        const auto button = event.button == 1 ? DesktopMouseButton::Left : event.button == 2 ? DesktopMouseButton::Middle :
            event.button == 3 ? DesktopMouseButton::Right : event.button == 4 ? DesktopMouseButton::Back : DesktopMouseButton::Forward;
        const auto binding = mouseBinding(button);
        const auto tag = std::string("mouse:") + binding.name;
        if (event.release) return releaseTag(tag);
        if (!options.mouseEnabled) return {MaiErrorCode::NotConfigured, "mouse input is disabled"};
        if (ownsTag(tag)) return {};
        if (const auto gate = validatePointerTarget()) return gate;
        if ((keyState(binding.key) & 0x8000) && !ownsButton(binding.key))
            return {MaiErrorCode::Busy, "Mouse button is already held outside this input owner"};
        INPUT down{};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = binding.down;
        down.mi.mouseData = binding.data;
        auto up = down;
        up.mi.dwFlags = binding.up;
        return hold(down, up, tag, binding.name);
    }
    if (event.kind == Kind::UnicodeText) {
        if (!options.keyboardEnabled) return {MaiErrorCode::NotConfigured, "keyboard input is disabled"};
        return unicode(event.text);
    }
    if (event.kind == Kind::Touch) {
        if (!options.mouseEnabled || !options.nativePenTouchEnabled)
            return {MaiErrorCode::NotConfigured, "native touch input is disabled"};
        if (hooks_.emitInput) return {MaiErrorCode::NotSupported, "pointer devices require their explicit native test transport"};
        if (!touch_) touch_ = makeDesktopComputerWindowsTouch();
        const auto result = touch_->dispatch(event.contacts, port);
        publish();
        return result;
    }
    if (event.kind == Kind::Pen)
        return {MaiErrorCode::NotSupported, "native pen system API adapter is a pending candidate"};
    if (event.kind != Kind::RelativeMove && event.kind != Kind::AbsoluteMove && event.kind != Kind::Wheel)
        return {MaiErrorCode::NotSupported, "virtual devices are deferred in this native input session"};
    if (!options.mouseEnabled) return {MaiErrorCode::NotConfigured, "mouse input is disabled"};
    if (event.kind == Kind::RelativeMove) return moveRelative(event.deltaX, event.deltaY);
    if (event.kind == Kind::Wheel) return wheel(event.wheelDelta120, event.horizontal, options.highResolutionScroll);
    if (!std::isfinite(port.x) || !std::isfinite(port.y) || !std::isfinite(port.width) || !std::isfinite(port.height) ||
        port.width < 1 || port.height < 1)
        return {MaiErrorCode::InvalidInput, "absolute movement requires a valid observed viewport"};
    const auto x = port.x + event.x * (port.width - 1), y = port.y + event.y * (port.height - 1);
    if (!validPoint(x, y)) return {MaiErrorCode::InvalidInput, "absolute pointer coordinate is outside Windows range"};
    if (const auto foreign = noForeignMouseButtons()) return foreign;
    return move(x, y);
}

MaiError dispatchNativeInput(const DesktopNativeInput& input, InputScope& scope) {
    DpiScope dpi;
    if (int(input.kind) < int(DesktopNativeInputKind::Click) || int(input.kind) > int(DesktopNativeInputKind::KeyUp))
        return {MaiErrorCode::InvalidInput, "invalid native input kind"};
    if ((input.kind == DesktopNativeInputKind::Click || input.kind == DesktopNativeInputKind::DoubleClick ||
         input.kind == DesktopNativeInputKind::Drag || input.kind == DesktopNativeInputKind::MouseDown ||
         input.kind == DesktopNativeInputKind::MouseUp) &&
        !mouseBinding(input.button).name)
        return {MaiErrorCode::InvalidInput, "invalid mouse button"};
    if (input.kind == DesktopNativeInputKind::Scroll && input.scrollAxis != DesktopScrollAxis::Vertical &&
        input.scrollAxis != DesktopScrollAxis::Horizontal)
        return {MaiErrorCode::InvalidInput, "invalid scroll axis"};
    // An up refers to an original recorded native event. Do not re-resolve its
    // layout, require focus, or reject cancellation before pairing that event.
    if (input.kind == DesktopNativeInputKind::MouseUp) {
        const auto binding = mouseBinding(input.button);
        if (!binding.name) return {MaiErrorCode::InvalidInput, "invalid mouse button"};
        return scope.releaseTag(std::string("mouse:") + binding.name);
    }
    if (input.kind == DesktopNativeInputKind::KeyUp) {
        const auto parsed = maiDesktopParseKey(input.key);
        if (!parsed) return parsed.error();
        if (!parsed.value().modifiers.empty()) return {MaiErrorCode::InvalidInput, "key_up needs one key, not a chord"};
        return scope.releaseTag(independentKeyTag(parsed.value()));
    }
    if (input.kind == DesktopNativeInputKind::MouseDown) {
        const auto binding = mouseBinding(input.button);
        if (!binding.name) return {MaiErrorCode::InvalidInput, "invalid mouse button"};
        if (scope.ownsTag(std::string("mouse:") + binding.name)) return scope.validate();
    }
    if (input.kind == DesktopNativeInputKind::KeyDown) {
        const auto parsed = maiDesktopParseKey(input.key);
        if (!parsed) return parsed.error();
        if (!parsed.value().modifiers.empty())
            return {MaiErrorCode::InvalidInput, "key_down needs one key, not a chord"};
        if (scope.ownsTag(independentKeyTag(parsed.value()))) return scope.validate();
    }
    if (input.kind == DesktopNativeInputKind::Type || input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
        if (input.text.size() > 4000 || (input.kind == DesktopNativeInputKind::Type && input.text.empty()))
            return {MaiErrorCode::InvalidInput, "text must contain at most 4000 UTF-8 bytes"};
        DesktopInputEvent text;
        text.kind = DesktopInputEventKind::UnicodeText;
        text.text = input.text;
        if (const auto error = maiDesktopValidateInputEvents({text})) return error;
    }
    const bool positioned =
        input.kind == DesktopNativeInputKind::Click || input.kind == DesktopNativeInputKind::DoubleClick ||
        input.kind == DesktopNativeInputKind::RightClick || input.kind == DesktopNativeInputKind::Drag ||
        input.kind == DesktopNativeInputKind::Scroll || input.kind == DesktopNativeInputKind::Move ||
        input.kind == DesktopNativeInputKind::MouseDown;
    if (positioned && !validPoint(input.x, input.y))
        return {MaiErrorCode::InvalidInput, "pointer coordinates are invalid"};
    const int maximumDragMs = input.desktopScope ? 5000 : 200;
    if (input.kind == DesktopNativeInputKind::Drag &&
        (!validPoint(input.toX, input.toY) || input.durationMs < 1 || input.durationMs > maximumDragMs))
        return {MaiErrorCode::InvalidInput,
                "drag needs a valid destination and 1 to " + std::to_string(maximumDragMs) + " ms duration"};
    if (input.kind == DesktopNativeInputKind::Scroll && (input.lines == 0 || input.lines < -10 || input.lines > 10))
        return {MaiErrorCode::InvalidInput, "scroll needs -10 to 10 lines, excluding zero"};
    if ((input.expectedWindowId == 0) != (input.expectedProcessId == 0))
        return {MaiErrorCode::InvalidInput, "expected window and process identities must be provided together"};
    if (input.desktopScope && (!input.display.id || input.display.id > std::numeric_limits<std::uintptr_t>::max()))
        return {MaiErrorCode::InvalidInput, "display identity must be a non-zero native monitor identifier"};
    DesktopKeyChord chord;
    DesktopWindowsKeyBinding keyBinding;
    HKL keyLayout = nullptr;
    if (input.kind == DesktopNativeInputKind::Key || input.kind == DesktopNativeInputKind::KeyDown) {
        const auto parsed = maiDesktopParseKey(input.key);
        if (!parsed) return parsed.error();
        if (input.kind == DesktopNativeInputKind::KeyDown && !parsed.value().modifiers.empty())
            return {MaiErrorCode::InvalidInput, "key_down needs one key, not a chord"};
        keyLayout = scope.layout();
        auto effective = parsed.value();
        for (const auto modifier : {DesktopKeyModifier::ControlLeft, DesktopKeyModifier::ControlRight,
                                    DesktopKeyModifier::AltLeft, DesktopKeyModifier::AltRight})
            if (scope.ownsModifier(modifier)) effective.modifiers.push_back(modifier);
        const auto resolved = maiDesktopWindowsResolveKey(effective, reinterpret_cast<std::uintptr_t>(keyLayout));
        if (!resolved) return resolved.error();
        if (input.kind == DesktopNativeInputKind::KeyDown && !resolved.value().impliedModifiers.empty())
            return {MaiErrorCode::InvalidInput,
                    "hold named physical keys and modifiers independently; use type for text"};
        keyBinding = resolved.value();
        chord = withImpliedModifiers(parsed.value(), keyBinding);
    }
    // Physical pointer intent is checked by the shared Scope primitives.
    // A named keyboard chord retains its explicitly requested modifier policy.
    if ((input.kind == DesktopNativeInputKind::Key || input.kind == DesktopNativeInputKind::KeyDown) &&
        hasUnexpectedModifiers(input.kind == DesktopNativeInputKind::Key ? &chord : nullptr, &scope))
        return {MaiErrorCode::Busy, "release modifiers that are not part of the requested action"};
    if (!scope.isInteractive())
        return {MaiErrorCode::NotConfigured, "Windows input requires the active interactive desktop"};
    const MaiError ready = scope.validate();
    if (ready) return ready;
    if (input.kind == DesktopNativeInputKind::Key || input.kind == DesktopNativeInputKind::KeyDown) {
        std::uint8_t modifiers = 0;
        for (const auto modifier : chord.modifiers) {
            switch (maiDesktopModifierFamily(modifier)) {
            case DesktopKeyModifier::Shift: modifiers |= 1; break;
            case DesktopKeyModifier::Control: modifiers |= 2; break;
            case DesktopKeyModifier::Alt: modifiers |= 4; break;
            case DesktopKeyModifier::Meta: modifiers |= 8; break;
            default: break;
            }
        }
        if (const auto secure = scope.guardKey(keyBinding.virtualKey, modifiers)) return secure;
    }
    if (input.kind == DesktopNativeInputKind::ReplaceFocusedText)
        if (const auto secure = scope.guardLiteralText()) return secure;
    if (input.kind == DesktopNativeInputKind::KeyDown) {
        if ((scope.keyState(keyBinding.virtualKey) & 0x8000) && !scope.ownsBinding(keyBinding))
            return {MaiErrorCode::Busy, "the requested key is already down"};
        return scope.hold(keyEvent(keyBinding), keyEvent(keyBinding, true), independentKeyTag(chord),
                          independentKeyName(chord, input.key));
    }
    if (input.kind == DesktopNativeInputKind::Key) return sendChord(scope, chord, keyBinding, keyLayout);
    if (input.kind == DesktopNativeInputKind::Type) return scope.unicode(input.text);
    if (input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
        const auto select = maiDesktopParseKey("Ctrl+A");
        const MaiError selected = sendChord(scope, select.value());
        if (selected) return selected;
        // Replacing with an empty value must delete the selection explicitly.
        if (input.text.empty()) {
            const auto backspace = virtualKeyBinding(VK_BACK, scope.layout());
            return scope.tap(keyEvent(backspace), keyEvent(backspace, true));
        }
        return scope.unicode(input.text);
    }
    // Moving a pointer while the user holds another button can drag their
    // content before our own down occurs. Check before any cursor movement.
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        if ((scope.keyState(button) & 0x8000) && !scope.ownsButton(button))
            return {MaiErrorCode::Busy, "a mouse button is already held outside this input session"};
    const MaiError moved = scope.move(input.x, input.y);
    if (moved) return moved;
    if (input.kind == DesktopNativeInputKind::Move) return {};
    if (input.kind == DesktopNativeInputKind::Scroll)
        return scope.wheel(input.lines * WHEEL_DELTA, input.scrollAxis == DesktopScrollAxis::Horizontal, true);

    const auto binding =
        mouseBinding(input.kind == DesktopNativeInputKind::RightClick ? DesktopMouseButton::Right : input.button);
    if (!binding.name) return {MaiErrorCode::InvalidInput, "invalid mouse button"};
    if ((scope.keyState(binding.key) & 0x8000) && !(input.kind == DesktopNativeInputKind::MouseDown && scope.ownsButton(binding.key)))
        return {MaiErrorCode::Busy, "the requested mouse button is already down; use move or mouse_up"};
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = binding.down;
    down.mi.mouseData = binding.data;
    INPUT up = down;
    up.mi.dwFlags = binding.up;
    if (input.kind == DesktopNativeInputKind::MouseDown)
        return scope.hold(down, up, std::string("mouse:") + binding.name, binding.name);
    if (input.kind == DesktopNativeInputKind::Drag) {
        const MaiError held = scope.hold(down, up);
        if (held) return held;
        const int steps = (std::max)(1, input.durationMs / 10);
        const auto started = std::chrono::steady_clock::now();
        for (int step = 1; step <= steps; ++step) {
            std::this_thread::sleep_until(started + std::chrono::milliseconds(input.durationMs * step / steps));
            const double fraction = double(step) / steps;
            const MaiError dragged =
                scope.move(input.x + (input.toX - input.x) * fraction, input.y + (input.toY - input.y) * fraction);
            if (dragged) return dragged;
        }
        const MaiError released = scope.release();
        return released ? released : scope.validate();
    }
    const MaiError clicked = scope.tap(down, up);
    if (clicked || input.kind != DesktopNativeInputKind::DoubleClick) return clicked;
    // Windows derives WM_*DBLCLK from two native clicks within its own time and
    // distance thresholds. Keep the pointer fixed and use that system interval.
    std::this_thread::sleep_for(
        std::chrono::milliseconds((std::min)(DWORD(40), (std::max)(DWORD(1), DWORD(GetDoubleClickTime() / 4)))));
    return scope.tap(down, up);
}


MaiError InputScope::dispatchLegacy(const DesktopNativeInput& input) { return dispatchNativeInput(input, *this); }

class WindowsInputSession final : public DesktopNativeInputSession {
public:
    explicit WindowsInputSession(DesktopWindowsInputHooks hooks = {}) : hooks_(std::move(hooks)) {}
    ~WindowsInputSession() override { releaseAll(); }
    MaiError input(const DesktopNativeInput& input) override {
        if (activeOperation_.load()) return inFlight();
        return perform(input, input.kind == DesktopNativeInputKind::KeyUp || input.kind == DesktopNativeInputKind::MouseUp,
            [&](InputScope& scope) { return scope.dispatchLegacy(input); });
    }
    MaiError inputEvents(const std::vector<DesktopInputEvent>& events, const DesktopNativeInput& input,
                         const DesktopInputPort& port, const DesktopInputOptions& options) override {
        if (activeOperation_.load()) return inFlight();
        if (const auto error = maiDesktopValidateInputEvents(events)) return error;
        if (const auto error = maiDesktopValidateInputOptions(options)) return error;
        if (std::any_of(events.begin(), events.end(), [](const auto& event) { return event.delayMs != 0; }))
            return {MaiErrorCode::InvalidInput, "native input requires a prepared event with consumed delay"};
        if (events.empty()) return {};
        const auto neutral = [](const DesktopInputEvent& event) {
            return (event.kind == DesktopInputEventKind::RelativeMove && !event.deltaX && !event.deltaY) ||
                (event.kind == DesktopInputEventKind::Wheel && !event.wheelDelta120) ||
                (event.kind == DesktopInputEventKind::UnicodeText && event.text.empty());
        };
        // This is only a no-effect native fast path, not the host's broader
        // authorization/only-release classification or a renewed scope.
        if (std::all_of(events.begin(), events.end(), neutral)) return {};
        auto scope = input;
        scope.kind = std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == DesktopInputEventKind::RawKey || event.kind == DesktopInputEventKind::UnicodeText;
        }) ? DesktopNativeInputKind::Key : DesktopNativeInputKind::Move;
        return perform(scope, maiDesktopInputEventsOnlyRelease(events), [&](InputScope& owner) {
            for (const auto& event : events) {
                // Common consumes batch delays/repeat timing. This native seam
                // receives prepared events and never silently drops a delay.
                if (event.delayMs) return MaiError{MaiErrorCode::InvalidInput, "native input requires a prepared event with consumed delay"};
                if (!maiDesktopInputEventsOnlyRelease({event}))
                    if (const auto ready = owner.validate()) return ready;
                if (const auto error = owner.dispatchTyped(event, port, options)) return error;
            }
            return MaiError{};
        });
    }
    DesktopNativeInputState state() const override { return *std::atomic_load(&snapshot_->state); }
    MaiError validate() const override {
        if (state().empty()) return {};
        const auto scope = std::atomic_load(&snapshot_->scope);
        return hooks_.validate ? hooks_.validate(*scope) : maiDesktopWindowsValidateInputScope(*scope);
    }
    MaiError resetControls() override {
        if (activeOperation_.load()) return inFlight();
        std::unique_lock<std::mutex> operation(operationMutex_, std::try_to_lock);
        if (!operation.owns_lock()) return {MaiErrorCode::Busy, "native input is in flight"};
        ActiveOperation active(activeOperation_);
        ++epoch_;
        const auto result = scope_ ? scope_->releaseAll() : MaiError{};
        if (result) cleanupRequested_ = true;
        return result;
    }
    MaiError releaseAll() override {
        ++epoch_; // Revoke immediately, before any worker can need GUI progress.
        cleanupRequested_ = true;
        if (activeOperation_.load()) return {MaiErrorCode::Busy, "native input cancellation requested; cleanup is pending"};
        std::unique_lock<std::mutex> operation(operationMutex_, std::try_to_lock);
        if (!operation.owns_lock()) return {MaiErrorCode::Busy, "native input cancellation requested; cleanup is pending"};
        ActiveOperation active(activeOperation_);
        cleanupRequested_ = false;
        const auto result = scope_ ? scope_->releaseAll() : MaiError{};
        if (result) cleanupRequested_ = true;
        return result;
    }
    DesktopInputCapabilities capabilities() const override {
        DesktopInputCapabilities result;
        result.baseline = "Windows native SendInput / User32";
        for (const auto* name : {"absolute_move", "relative_move", "mouse_button", "wheel", "raw_key", "unicode_text"})
            result.features.push_back({name, true, true, "win32_native", "System native input; application acknowledgement is separate", {}});
        result.features.push_back({"touch", true, false, "win32_synthetic_touch_frame", "System API candidate",
            "Continuous-touch native acceptance has not passed on this host"});
        result.features.push_back({"pen", false, false, "win32_synthetic_pen", "System API candidate", "Native pen adapter is pending"});
        return result;
    }
private:
    class ActiveOperation {
    public:
        explicit ActiveOperation(std::atomic<bool>& active) : active_(active) { active_.store(true); }
        ~ActiveOperation() { active_.store(false); }
    private:
        std::atomic<bool>& active_;
    };
    static MaiError inFlight() { return {MaiErrorCode::Busy, "another native input operation is in flight"}; }
    template<class Function>
    MaiError perform(const DesktopNativeInput& input, bool cleanupOnly, Function&& function) {
        // A caller cancellation callback may re-enter this object on the same
        // thread. Never call mutex.try_lock() when that thread already owns it.
        if (activeOperation_.load()) return inFlight();
        DpiScope dpi; // Both entry points resolve/read/move in physical coordinates, then restore the caller.
        std::unique_lock<std::mutex> operation(operationMutex_, std::try_to_lock);
        if (!operation.owns_lock()) return {MaiErrorCode::Busy, "another native input operation is in flight"};
        ActiveOperation active(activeOperation_);
        const auto generation = epoch_.load(); // Fixed at admission, before any pending native UP can block.
        if (cleanupRequested_.exchange(false) && scope_) {
            if (const auto pending = scope_->releaseAll()) {
                cleanupRequested_ = true;
                return pending;
            }
        }
        if (epoch_.load() != generation)
            return {MaiErrorCode::Canceled, "native input canceled during pending cleanup"};
        auto scoped = input;
        scoped.isCanceled = [this, generation, &input] {
            if (epoch_.load() != generation) return true;
            const bool requested = input.isCanceled && input.isCanceled();
            return requested || epoch_.load() != generation;
        };
        if (!scope_) scope_ = std::make_unique<InputScope>(scoped, hooks_, snapshot_);
        if (const auto prepared = scope_->begin(scoped, cleanupOnly)) return prepared;
        auto result = function(*scope_);
        const auto temporary = scope_->finishCall();
        if (!result && temporary) result = temporary;
        const bool revoked = generation != epoch_.load();
        if (!result && revoked) result = {MaiErrorCode::Canceled, "native input canceled"};
        const bool requestedCleanup = cleanupRequested_.exchange(false);
        if (result || revoked || requestedCleanup) {
            const auto cleanup = scope_->releaseAll();
            if (cleanup) {
                cleanupRequested_ = true;
                result = result ? MaiError{result.code(), result.message() + "; cleanup failed: " + cleanup.message()} : cleanup;
            }
        }
        return result;
    }
    DesktopWindowsInputHooks hooks_;
    std::shared_ptr<WindowsInputSnapshot> snapshot_ = std::make_shared<WindowsInputSnapshot>();
    std::mutex operationMutex_;
    std::atomic<bool> activeOperation_{false};
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<bool> cleanupRequested_{false};
    std::unique_ptr<InputScope> scope_;
};

}  // namespace

std::shared_ptr<DesktopNativeInputSession> maiDesktopCreateInputSession() {
    return std::make_shared<WindowsInputSession>();
}

std::shared_ptr<DesktopNativeInputSession> makeDesktopWindowsInputSessionForTesting(DesktopWindowsInputHooks hooks) {
    return std::make_shared<WindowsInputSession>(std::move(hooks));
}

MaiError maiDesktopWindowsValidateInputScope(const DesktopNativeInput& input) {
    DpiScope dpi;
    if ((input.expectedWindowId == 0) != (input.expectedProcessId == 0))
        return {MaiErrorCode::InvalidInput, "expected window and process identities must be provided together"};
    if (!interactiveDesktop())
        return {MaiErrorCode::NotConfigured, "Windows input requires the active interactive desktop"};
    InputScope scope(input);
    return scope.validate();
}

MaiError maiDesktopInput(const DesktopNativeInput& input) {
    if (input.kind == DesktopNativeInputKind::MouseDown || input.kind == DesktopNativeInputKind::MouseUp ||
        input.kind == DesktopNativeInputKind::KeyDown || input.kind == DesktopNativeInputKind::KeyUp)
        return {MaiErrorCode::InvalidInput, "independent down/up requires a DesktopNativeInputSession"};
    InputScope scope(input);
    return dispatchNativeInput(input, scope);
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
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                               reinterpret_cast<void**>(&valuePattern))) &&
        valuePattern) {
        read = valuePattern->get_CurrentValue(&value);
        valuePattern->Release();
    } else {
        IUIAutomationTextPattern* textPattern = nullptr;
        if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                                   reinterpret_cast<void**>(&textPattern))) &&
            textPattern) {
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
    if (!stillFocused.value()) return {MaiErrorCode::Busy, "selected window lost focus while reading its text"};
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
    if (FAILED(secure)) return {MaiErrorCode::NotConfigured, "could not verify whether focused input is secure"};
    return protectedInput != FALSE;
}

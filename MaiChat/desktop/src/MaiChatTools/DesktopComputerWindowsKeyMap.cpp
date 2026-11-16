#include "MaiChatTools/DesktopComputerWindowsInputInternal.h"

#include <array>

namespace {
// Normalized protocol keys describe US physical positions. These PC set-1
// identities are independent of the user's active layout; resolving them must
// not load or activate another layout. Unlisted Windows system keys use VK.
// Scan identities: Microsoft Keyboard Input Overview, scan-code table:
// https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input
WORD normalizedScan(WORD key) {
    constexpr std::array<WORD, 26> letters{
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
    if (key >= '1' && key <= '9') return WORD(2 + key - '1');
    if (key >= VK_F1 && key <= VK_F10) return WORD(0x3b + key - VK_F1);
    if (key >= VK_F13 && key <= VK_F23) return WORD(0x64 + key - VK_F13);
    constexpr std::array<WORD, 10> keypad{0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49};
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return keypad[key - VK_NUMPAD0];
    switch (key) {
    case '0': return 0x0b;
    case VK_BACK: return 0x0e;
    case VK_TAB: return 0x0f;
    case VK_RETURN: return 0x1c;
    case VK_ESCAPE: return 1;
    case VK_SPACE: return 0x39;
    case VK_SHIFT: case VK_LSHIFT: return 0x2a;
    case VK_RSHIFT: return 0x36;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 0x1d;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return 0x38;
    case VK_CAPITAL: return 0x3a;
    case VK_NUMLOCK: return 0x45;
    case VK_SCROLL: return 0x46;
    case VK_F11: return 0x57;
    case VK_F12: return 0x58;
    case VK_F24: return 0x76;
    case VK_SNAPSHOT: return 0x37;
    case VK_INSERT: return 0x52;
    case VK_DELETE: return 0x53;
    case VK_HOME: return 0x47;
    case VK_END: return 0x4f;
    case VK_PRIOR: return 0x49;
    case VK_NEXT: return 0x51;
    case VK_LEFT: return 0x4b;
    case VK_RIGHT: return 0x4d;
    case VK_UP: return 0x48;
    case VK_DOWN: return 0x50;
    case VK_MULTIPLY: return 0x37;
    case VK_ADD: return 0x4e;
    case VK_SUBTRACT: return 0x4a;
    case VK_DECIMAL: return 0x53;
    case VK_DIVIDE: return 0x35;
    case VK_OEM_1: return 0x27;
    case VK_OEM_PLUS: return 0x0d;
    case VK_OEM_COMMA: return 0x33;
    case VK_OEM_MINUS: return 0x0c;
    case VK_OEM_PERIOD: return 0x34;
    case VK_OEM_2: return 0x35;
    case VK_OEM_3: return 0x29;
    case VK_OEM_4: return 0x1a;
    case VK_OEM_5: return 0x2b;
    case VK_OEM_6: return 0x1b;
    case VK_OEM_7: return 0x28;
    case VK_OEM_102: return 0x56;
    default: return 0;
    }
}
bool extendedKey(WORD key) {
    switch (key) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_SNAPSHOT: return true;
    default: return false;
    }
}
}

MaiResult<DesktopWindowsRawBinding> maiDesktopWindowsResolveRawKey(
    const DesktopInputRawKey& key, const DesktopInputOptions& options, std::uintptr_t layout) {
    const auto code = key.code & 0x8000 ? key.code & 0xff : key.code;
    if (!code || code >= 0xff)
        return {MaiErrorCode::NotSupported, "raw key has no Windows virtual-key mapping"};
    const auto vk = WORD(code);
    const auto mapped = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, reinterpret_cast<HKL>(layout));
    const bool normalized = (key.flags & 1) == 0;
    const auto scan = normalized ? normalizedScan(vk) : WORD(mapped & 0xff);
    bool extended = (key.modifiers & 0x10) || extendedKey(vk) || (mapped & 0xff00) == 0xe000;
    if (vk == VK_PAUSE || vk == VK_NUMLOCK) extended = false;
    const bool useScan = scan && (normalized || options.alwaysSendScanCodes || extended) &&
                         vk != VK_LWIN && vk != VK_RWIN && vk != VK_PAUSE;
    DesktopWindowsRawBinding result;
    result.virtualKey = vk;
    result.physicalScan = scan ? scan : WORD(mapped & 0xff);
    result.extended = extended;
    result.down.type = INPUT_KEYBOARD;
    result.down.ki.wVk = useScan ? 0 : vk;
    result.down.ki.wScan = useScan ? scan : 0;
    if (useScan) result.down.ki.dwFlags |= KEYEVENTF_SCANCODE;
    if (extended) result.down.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    result.up = result.down;
    result.up.ki.dwFlags |= KEYEVENTF_KEYUP;
    return result;
}

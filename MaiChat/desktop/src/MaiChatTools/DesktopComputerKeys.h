#pragma once

#include <string>
#include <vector>

#include "MaiError.h"

enum class DesktopKeyModifier {
    Control, Shift, Alt, Meta,
    ControlLeft, ControlRight, ShiftLeft, ShiftRight,
    AltLeft, AltRight, MetaLeft, MetaRight
};

enum class DesktopKeyCode {
    Character,
    Enter,
    Tab,
    Escape,
    Backspace,
    Delete,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    Home,
    End,
    PageUp,
    PageDown,
    Space,
    Insert,
    Function,
    Control,
    Shift,
    Alt,
    Meta,
    Backquote, Minus, Equal, BracketLeft, BracketRight, Backslash, IntlBackslash,
    Semicolon, Quote, Comma, Period, Slash,
    CapsLock, NumLock, ScrollLock, PrintScreen, Pause, ContextMenu,
    Numpad0, Numpad1, Numpad2, Numpad3, Numpad4,
    Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
    NumpadAdd, NumpadSubtract, NumpadMultiply, NumpadDivide, NumpadDecimal,
    NumpadEnter, NumpadEqual, NumpadComma, Clear, Help, Convert, NonConvert,
    KanaMode, Eisu, IntlRo, IntlYen,
    ControlLeft, ControlRight, ShiftLeft, ShiftRight, AltLeft, AltRight, MetaLeft, MetaRight,
    Fn, VolumeMute, VolumeDown, VolumeUp, MediaPlayPause, MediaStop, MediaNext, MediaPrevious,
    BrowserBack, BrowserForward, BrowserRefresh, BrowserStop, BrowserSearch, BrowserFavorites,
    BrowserHome, LaunchMail, LaunchMedia, LaunchApp1, LaunchApp2
};

struct DesktopKeyChord {
    std::vector<DesktopKeyModifier> modifiers;
    DesktopKeyCode key = DesktopKeyCode::Character;
    char character = 0;
    int function = 0;
};

// Shared grammar prevents Windows and macOS from accepting different shortcuts.
// Modifier order is preserved for native down / reverse up dispatch. Aliases and
// ASCII key names are case insensitive. Literal ASCII punctuation is a character
// resolved through the native layout, distinct from a named physical key. A final
// '+' is literal (including Ctrl++); other empty components fail. Same physical
// modifiers and generic/sided combinations of the same family are ambiguous and
// fail, while opposite explicit sides and modifier-only chords are valid.
MaiResult<DesktopKeyChord> maiDesktopParseKey(const std::string& key);

// Sided modifiers retain their identity for native dispatch; family checks use
// one of the original four values. A modifier key may also end a chord.
DesktopKeyModifier maiDesktopModifierFamily(DesktopKeyModifier modifier);
bool maiDesktopKeyModifier(DesktopKeyCode key, DesktopKeyModifier& modifier);

// Canonical shared grammar names, not a promise of native platform availability.
// No arbitrary native keycodes are accepted; unsupported native mappings fail.
const std::vector<std::string>& maiDesktopKeyNames();

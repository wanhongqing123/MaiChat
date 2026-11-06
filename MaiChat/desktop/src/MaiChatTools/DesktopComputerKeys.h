#pragma once

#include <string>
#include <vector>

#include "MaiError.h"

enum class DesktopKeyModifier { Control, Shift, Alt, Meta };

enum class DesktopKeyCode {
    Character, Enter, Tab, Escape, Backspace, Delete,
    ArrowLeft, ArrowRight, ArrowUp, ArrowDown,
    Home, End, PageUp, PageDown, Space, Insert, Function
};

struct DesktopKeyChord {
    std::vector<DesktopKeyModifier> modifiers;
    DesktopKeyCode key = DesktopKeyCode::Character;
    char character = 0;
    int function = 0;
};

// Shared grammar prevents Windows and macOS from accepting different shortcuts.
// Modifier order is preserved for native down / reverse up dispatch. Aliases and
// ASCII key names are case insensitive; duplicate or empty components fail.
MaiResult<DesktopKeyChord> maiDesktopParseKey(const std::string& key);

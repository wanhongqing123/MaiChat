#include "MaiChatTools/DesktopComputerKeys.h"

#include <algorithm>
#include <utility>

namespace {

std::string normalized(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    for (char& byte : value) {
        if (byte >= 'a' && byte <= 'z') byte = char(byte - 'a' + 'A');
    }
    return value;
}

bool modifier(const std::string& token, DesktopKeyModifier& value) {
    if (token == "CTRL" || token == "CONTROL") value = DesktopKeyModifier::Control;
    else if (token == "SHIFT") value = DesktopKeyModifier::Shift;
    else if (token == "ALT") value = DesktopKeyModifier::Alt;
    else if (token == "META" || token == "CMD" || token == "SUPER")
        value = DesktopKeyModifier::Meta;
    else return false;
    return true;
}

} // namespace

MaiResult<DesktopKeyChord> maiDesktopParseKey(const std::string& key) {
    if (key.empty() || key.size() > 128)
        return {MaiErrorCode::InvalidInput, "key needs one named key with optional modifiers"};
    DesktopKeyChord chord;
    std::size_t start = 0;
    std::string token;
    for (;;) {
        const auto separator = key.find('+', start);
        token = normalized(key.substr(start, separator == std::string::npos ?
            std::string::npos : separator - start));
        if (token.empty())
            return {MaiErrorCode::InvalidInput, "key contains an empty component"};
        if (separator == std::string::npos) break;
        DesktopKeyModifier value = DesktopKeyModifier::Control;
        if (!modifier(token, value) ||
            std::find(chord.modifiers.begin(), chord.modifiers.end(), value) !=
                chord.modifiers.end())
            return {MaiErrorCode::InvalidInput, "key has an unknown or repeated modifier"};
        chord.modifiers.push_back(value);
        start = separator + 1;
    }
    if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') ||
                             (token[0] >= '0' && token[0] <= '9'))) {
        chord.character = token[0];
        return chord;
    }
    if (chord.modifiers.empty()) {
        DesktopKeyModifier single;
        if (modifier(token, single)) {
            chord.key = single == DesktopKeyModifier::Control ? DesktopKeyCode::Control
                        : single == DesktopKeyModifier::Shift ? DesktopKeyCode::Shift
                        : single == DesktopKeyModifier::Alt   ? DesktopKeyCode::Alt
                                                              : DesktopKeyCode::Meta;
            return chord;
        }
    }
    const std::pair<const char*, DesktopKeyCode> named[] = {
        {"ENTER", DesktopKeyCode::Enter}, {"TAB", DesktopKeyCode::Tab},
        {"ESCAPE", DesktopKeyCode::Escape}, {"BACKSPACE", DesktopKeyCode::Backspace},
        {"DELETE", DesktopKeyCode::Delete}, {"ARROWLEFT", DesktopKeyCode::ArrowLeft},
        {"ARROWRIGHT", DesktopKeyCode::ArrowRight}, {"ARROWUP", DesktopKeyCode::ArrowUp},
        {"ARROWDOWN", DesktopKeyCode::ArrowDown}, {"HOME", DesktopKeyCode::Home},
        {"END", DesktopKeyCode::End}, {"PAGEUP", DesktopKeyCode::PageUp},
        {"PAGEDOWN", DesktopKeyCode::PageDown}, {"SPACE", DesktopKeyCode::Space},
        {"INSERT", DesktopKeyCode::Insert}};
    for (const auto& item : named) {
        if (token != item.first) continue;
        chord.key = item.second;
        return chord;
    }
    if (token.size() >= 2 && token.size() <= 3 && token[0] == 'F' &&
        token[1] >= '1' && token[1] <= '9' &&
        (token.size() == 2 || (token[2] >= '0' && token[2] <= '9'))) {
        chord.function = token[1] - '0';
        if (token.size() == 3) chord.function = chord.function * 10 + token[2] - '0';
        if (chord.function <= 24) {
            chord.key = DesktopKeyCode::Function;
            return chord;
        }
    }
    return {MaiErrorCode::InvalidInput, "unsupported key; use A-Z, 0-9, F1-F24 or a named editing key"};
}

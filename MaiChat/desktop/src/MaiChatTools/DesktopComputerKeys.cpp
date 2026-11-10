#include "MaiChatTools/DesktopComputerKeys.h"

#include <algorithm>
#include <utility>

namespace {

struct NamedKey {
    const char* name;
    DesktopKeyCode code;
};

// Shared names describe keys, including keys that a particular native backend
// cannot dispatch. The backend must reject those explicitly instead of choosing
// an unrelated key or posting text under the guise of a hardware event.
const NamedKey kNamedKeys[] = {
    {"Enter", DesktopKeyCode::Enter}, {"Tab", DesktopKeyCode::Tab},
    {"Escape", DesktopKeyCode::Escape}, {"Backspace", DesktopKeyCode::Backspace},
    {"Delete", DesktopKeyCode::Delete}, {"ArrowLeft", DesktopKeyCode::ArrowLeft},
    {"ArrowRight", DesktopKeyCode::ArrowRight}, {"ArrowUp", DesktopKeyCode::ArrowUp},
    {"ArrowDown", DesktopKeyCode::ArrowDown}, {"Home", DesktopKeyCode::Home},
    {"End", DesktopKeyCode::End}, {"PageUp", DesktopKeyCode::PageUp},
    {"PageDown", DesktopKeyCode::PageDown}, {"Space", DesktopKeyCode::Space},
    {"Insert", DesktopKeyCode::Insert}, {"Control", DesktopKeyCode::Control},
    {"Shift", DesktopKeyCode::Shift}, {"Alt", DesktopKeyCode::Alt}, {"Meta", DesktopKeyCode::Meta},
    {"Backquote", DesktopKeyCode::Backquote}, {"Minus", DesktopKeyCode::Minus},
    {"Equal", DesktopKeyCode::Equal}, {"BracketLeft", DesktopKeyCode::BracketLeft},
    {"BracketRight", DesktopKeyCode::BracketRight}, {"Backslash", DesktopKeyCode::Backslash},
    {"IntlBackslash", DesktopKeyCode::IntlBackslash}, {"Semicolon", DesktopKeyCode::Semicolon},
    {"Quote", DesktopKeyCode::Quote}, {"Comma", DesktopKeyCode::Comma},
    {"Period", DesktopKeyCode::Period}, {"Slash", DesktopKeyCode::Slash},
    {"CapsLock", DesktopKeyCode::CapsLock}, {"NumLock", DesktopKeyCode::NumLock},
    {"ScrollLock", DesktopKeyCode::ScrollLock}, {"PrintScreen", DesktopKeyCode::PrintScreen},
    {"Pause", DesktopKeyCode::Pause}, {"ContextMenu", DesktopKeyCode::ContextMenu},
    {"Numpad0", DesktopKeyCode::Numpad0}, {"Numpad1", DesktopKeyCode::Numpad1},
    {"Numpad2", DesktopKeyCode::Numpad2}, {"Numpad3", DesktopKeyCode::Numpad3},
    {"Numpad4", DesktopKeyCode::Numpad4}, {"Numpad5", DesktopKeyCode::Numpad5},
    {"Numpad6", DesktopKeyCode::Numpad6}, {"Numpad7", DesktopKeyCode::Numpad7},
    {"Numpad8", DesktopKeyCode::Numpad8}, {"Numpad9", DesktopKeyCode::Numpad9},
    {"NumpadAdd", DesktopKeyCode::NumpadAdd}, {"NumpadSubtract", DesktopKeyCode::NumpadSubtract},
    {"NumpadMultiply", DesktopKeyCode::NumpadMultiply}, {"NumpadDivide", DesktopKeyCode::NumpadDivide},
    {"NumpadDecimal", DesktopKeyCode::NumpadDecimal}, {"NumpadEnter", DesktopKeyCode::NumpadEnter},
    {"NumpadEqual", DesktopKeyCode::NumpadEqual}, {"NumpadComma", DesktopKeyCode::NumpadComma},
    {"Clear", DesktopKeyCode::Clear}, {"Help", DesktopKeyCode::Help},
    {"Convert", DesktopKeyCode::Convert}, {"NonConvert", DesktopKeyCode::NonConvert},
    {"KanaMode", DesktopKeyCode::KanaMode}, {"Eisu", DesktopKeyCode::Eisu},
    {"IntlRo", DesktopKeyCode::IntlRo}, {"IntlYen", DesktopKeyCode::IntlYen},
    {"ControlLeft", DesktopKeyCode::ControlLeft}, {"ControlRight", DesktopKeyCode::ControlRight},
    {"ShiftLeft", DesktopKeyCode::ShiftLeft}, {"ShiftRight", DesktopKeyCode::ShiftRight},
    {"AltLeft", DesktopKeyCode::AltLeft}, {"AltRight", DesktopKeyCode::AltRight},
    {"MetaLeft", DesktopKeyCode::MetaLeft}, {"MetaRight", DesktopKeyCode::MetaRight},
    {"Fn", DesktopKeyCode::Fn}, {"VolumeMute", DesktopKeyCode::VolumeMute},
    {"VolumeDown", DesktopKeyCode::VolumeDown}, {"VolumeUp", DesktopKeyCode::VolumeUp},
    {"MediaPlayPause", DesktopKeyCode::MediaPlayPause}, {"MediaStop", DesktopKeyCode::MediaStop},
    {"MediaNext", DesktopKeyCode::MediaNext}, {"MediaPrevious", DesktopKeyCode::MediaPrevious},
    {"BrowserBack", DesktopKeyCode::BrowserBack}, {"BrowserForward", DesktopKeyCode::BrowserForward},
    {"BrowserRefresh", DesktopKeyCode::BrowserRefresh}, {"BrowserStop", DesktopKeyCode::BrowserStop},
    {"BrowserSearch", DesktopKeyCode::BrowserSearch}, {"BrowserFavorites", DesktopKeyCode::BrowserFavorites},
    {"BrowserHome", DesktopKeyCode::BrowserHome}, {"LaunchMail", DesktopKeyCode::LaunchMail},
    {"LaunchMedia", DesktopKeyCode::LaunchMedia}, {"LaunchApp1", DesktopKeyCode::LaunchApp1},
    {"LaunchApp2", DesktopKeyCode::LaunchApp2}
};

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
    else if (token == "ALT" || token == "OPTION") value = DesktopKeyModifier::Alt;
    else if (token == "META" || token == "CMD" || token == "SUPER" || token == "WIN" || token == "WINDOWS")
        value = DesktopKeyModifier::Meta;
    else if (token == "CONTROLLEFT" || token == "CTRLLEFT" || token == "LEFTCONTROL" || token == "LEFTCTRL")
        value = DesktopKeyModifier::ControlLeft;
    else if (token == "CONTROLRIGHT" || token == "CTRLRIGHT" || token == "RIGHTCONTROL" || token == "RIGHTCTRL")
        value = DesktopKeyModifier::ControlRight;
    else if (token == "SHIFTLEFT" || token == "LEFTSHIFT") value = DesktopKeyModifier::ShiftLeft;
    else if (token == "SHIFTRIGHT" || token == "RIGHTSHIFT") value = DesktopKeyModifier::ShiftRight;
    else if (token == "ALTLEFT" || token == "LEFTALT" || token == "OPTIONLEFT" || token == "LEFTOPTION")
        value = DesktopKeyModifier::AltLeft;
    else if (token == "ALTRIGHT" || token == "RIGHTALT" || token == "OPTIONRIGHT" || token == "RIGHTOPTION")
        value = DesktopKeyModifier::AltRight;
    else if (token == "METALEFT" || token == "LEFTMETA" || token == "CMDLEFT" || token == "LEFTCMD" ||
             token == "WINLEFT" || token == "LEFTWIN") value = DesktopKeyModifier::MetaLeft;
    else if (token == "METARIGHT" || token == "RIGHTMETA" || token == "CMDRIGHT" || token == "RIGHTCMD" ||
             token == "WINRIGHT" || token == "RIGHTWIN") value = DesktopKeyModifier::MetaRight;
    else return false;
    return true;
}

DesktopKeyCode modifierKey(DesktopKeyModifier modifier) {
    switch (modifier) {
    case DesktopKeyModifier::Control: return DesktopKeyCode::Control;
    case DesktopKeyModifier::Shift: return DesktopKeyCode::Shift;
    case DesktopKeyModifier::Alt: return DesktopKeyCode::Alt;
    case DesktopKeyModifier::Meta: return DesktopKeyCode::Meta;
    case DesktopKeyModifier::ControlLeft: return DesktopKeyCode::ControlLeft;
    case DesktopKeyModifier::ControlRight: return DesktopKeyCode::ControlRight;
    case DesktopKeyModifier::ShiftLeft: return DesktopKeyCode::ShiftLeft;
    case DesktopKeyModifier::ShiftRight: return DesktopKeyCode::ShiftRight;
    case DesktopKeyModifier::AltLeft: return DesktopKeyCode::AltLeft;
    case DesktopKeyModifier::AltRight: return DesktopKeyCode::AltRight;
    case DesktopKeyModifier::MetaLeft: return DesktopKeyCode::MetaLeft;
    case DesktopKeyModifier::MetaRight: return DesktopKeyCode::MetaRight;
    }
    return DesktopKeyCode::Character;
}

bool ambiguousModifier(const std::vector<DesktopKeyModifier>& held, DesktopKeyModifier next) {
    const auto family = maiDesktopModifierFamily(next);
    for (const auto previous : held) {
        if (previous == next) return true;
        if (maiDesktopModifierFamily(previous) == family && (previous == family || next == family)) return true;
    }
    return false;
}

} // namespace

DesktopKeyModifier maiDesktopModifierFamily(DesktopKeyModifier modifier) {
    switch (modifier) {
    case DesktopKeyModifier::ControlLeft:
    case DesktopKeyModifier::ControlRight: return DesktopKeyModifier::Control;
    case DesktopKeyModifier::ShiftLeft:
    case DesktopKeyModifier::ShiftRight: return DesktopKeyModifier::Shift;
    case DesktopKeyModifier::AltLeft:
    case DesktopKeyModifier::AltRight: return DesktopKeyModifier::Alt;
    case DesktopKeyModifier::MetaLeft:
    case DesktopKeyModifier::MetaRight: return DesktopKeyModifier::Meta;
    default: return modifier;
    }
}

bool maiDesktopKeyModifier(DesktopKeyCode key, DesktopKeyModifier& modifier) {
    for (const auto candidate : {DesktopKeyModifier::Control, DesktopKeyModifier::Shift,
            DesktopKeyModifier::Alt, DesktopKeyModifier::Meta, DesktopKeyModifier::ControlLeft,
            DesktopKeyModifier::ControlRight, DesktopKeyModifier::ShiftLeft, DesktopKeyModifier::ShiftRight,
            DesktopKeyModifier::AltLeft, DesktopKeyModifier::AltRight, DesktopKeyModifier::MetaLeft,
            DesktopKeyModifier::MetaRight}) {
        if (key == modifierKey(candidate)) {
            modifier = candidate;
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& maiDesktopKeyNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> result;
        for (char character = '!'; character <= '~'; ++character) {
            if (character < 'a' || character > 'z') result.emplace_back(1, character);
        }
        for (int function = 1; function <= 24; ++function) result.push_back("F" + std::to_string(function));
        // A readable name is useful when '+' would otherwise be mistaken for
        // the chord delimiter; both spellings select the same character key.
        result.emplace_back("Plus");
        for (const auto& key : kNamedKeys) result.emplace_back(key.name);
        return result;
    }();
    return names;
}

MaiResult<DesktopKeyChord> maiDesktopParseKey(const std::string& key) {
    if (key.empty() || key.size() > 128)
        return {MaiErrorCode::InvalidInput, "key needs one named key with optional modifiers"};
    DesktopKeyChord chord;
    std::size_t start = 0;
    std::string token;
    for (;;) {
        // '+' is the only printable key that is also grammar punctuation. Only
        // a complete remaining terminal may mean plus; Ctrl++A stays malformed.
        if (normalized(key.substr(start)) == "+") {
            token = "+";
            break;
        }
        const auto separator = key.find('+', start);
        token = normalized(key.substr(start, separator == std::string::npos ?
            std::string::npos : separator - start));
        if (token.empty())
            return {MaiErrorCode::InvalidInput, "key contains an empty component"};
        if (separator == std::string::npos) break;
        DesktopKeyModifier value = DesktopKeyModifier::Control;
        if (!modifier(token, value) || ambiguousModifier(chord.modifiers, value))
            return {MaiErrorCode::InvalidInput, "key has an unknown, repeated or ambiguous modifier"};
        chord.modifiers.push_back(value);
        start = separator + 1;
    }
    if (token == "PLUS") token = "+";
    if (token.size() == 1 && token[0] >= '!' && token[0] <= '~') {
        chord.character = token[0];
        return chord;
    }
    DesktopKeyModifier terminal;
    if (modifier(token, terminal)) {
        if (ambiguousModifier(chord.modifiers, terminal))
            return {MaiErrorCode::InvalidInput, "key repeats or ambiguously selects the terminal modifier"};
        chord.key = modifierKey(terminal);
        return chord;
    }
    if (token == "RETURN") token = "ENTER";
    else if (token == "ESC") token = "ESCAPE";
    else if (token == "DEL") token = "DELETE";
    else if (token == "PGUP") token = "PAGEUP";
    else if (token == "PGDN") token = "PAGEDOWN";
    for (const auto& item : kNamedKeys) {
        if (token != normalized(item.name)) continue;
        chord.key = item.code;
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
    return {MaiErrorCode::InvalidInput, "unsupported key; use a canonical key name or printable ASCII key"};
}

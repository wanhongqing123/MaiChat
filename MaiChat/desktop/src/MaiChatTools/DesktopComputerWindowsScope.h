#pragma once

// Included in DesktopComputerWindows.cpp's private namespace after its Win32
// helpers. One physical binding owns all legacy/raw/temporary logical leases.
struct WindowsInputSnapshot {
    std::shared_ptr<const DesktopNativeInputState> state = std::make_shared<const DesktopNativeInputState>();
    std::shared_ptr<const DesktopNativeInput> scope = std::make_shared<const DesktopNativeInput>();
    std::shared_ptr<DesktopComputerWindowsPen> pen;
    std::shared_ptr<DesktopComputerWindowsTouch> touch;
};

class InputScope {
public:
    InputScope(const DesktopNativeInput& input, DesktopWindowsInputHooks hooks = {},
               std::shared_ptr<WindowsInputSnapshot> snapshot = std::make_shared<WindowsInputSnapshot>())
        : hooks_(std::move(hooks)), snapshot_(std::move(snapshot)) { begin(input); }
    ~InputScope() { releaseAll(); }

    MaiError begin(const DesktopNativeInput& input, bool cleanupOnly = false) {
        if (cleanupOnly || input.kind == DesktopNativeInputKind::MouseUp || input.kind == DesktopNativeInputKind::KeyUp) {
            input_.isCanceled = {};
            return {};
        }
        if (input.executionWindow && !input.desktopScope)
            return {MaiErrorCode::InvalidInput, "execution window restriction requires display coordinates"};
        const bool owned = !std::atomic_load(&snapshot_->state)->empty() || (pen_ && !pen_->state().empty()) ||
                           (touch_ && !touch_->state().empty());
        auto selected = input.desktopScope ? nullptr : input.expectedWindowId
            ? reinterpret_cast<HWND>(std::uintptr_t(input.expectedWindowId)) : GetForegroundWindow();
        if (owned) {
            bool changed = input.desktopScope != input_.desktopScope;
            // A display may retain its identity while its permitted receiver
            // changes. Never move held native controls across that boundary.
            changed = changed || input.executionWindow.has_value() != input_.executionWindow.has_value();
            if (input.executionWindow && input_.executionWindow) {
                const auto& a = *input.executionWindow;
                const auto& b = *input_.executionWindow;
                changed = changed || a.id != b.id || a.processId != b.processId || a.x != b.x || a.y != b.y ||
                    a.width != b.width || a.height != b.height;
            }
            if (input.desktopScope) {
                const auto& a = input.display;
                const auto& b = input_.display;
                changed = changed || a.id != b.id || a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height;
            } else {
                changed = changed || (input.expectedWindowId && selected != foreground_) ||
                    (input.expectedProcessId && boundProcess_ && input.expectedProcessId != boundProcess_);
                selected = foreground_; // An omitted target never rebinds a held default session.
            }
            if (changed) return {MaiErrorCode::Busy, "release owned inputs before changing the native input target"};
        } else {
            boundProcess_ = DWORD(input.expectedProcessId);
            if (!input.desktopScope && !boundProcess_ && selected && !hooks_.validate)
                GetWindowThreadProcessId(selected, &boundProcess_);
        }
        input_ = input;
        foreground_ = selected;
        if (!input_.desktopScope && !input_.expectedProcessId) input_.expectedProcessId = boundProcess_;
        auto watched = input_;
        watched.isCanceled = {};
        watched.key.clear();
        watched.text.clear();
        if (!watched.desktopScope && !watched.expectedWindowId && foreground_) {
            watched.expectedWindowId = std::uint64_t(reinterpret_cast<std::uintptr_t>(foreground_));
            watched.expectedProcessId = boundProcess_;
        }
        std::atomic_store(&snapshot_->scope, std::make_shared<const DesktopNativeInput>(std::move(watched)));
        return {};
    }
    MaiError finishCall() {
        MaiError first;
        for (;;) {
            const auto token = latestTemporary();
            if (!token) break;
            first = releaseToken(*token);
            if (first) break; // Retain the exact failed native up for releaseAll retry.
        }
        input_.isCanceled = {};
        input_.text.clear();
        input_.key.clear();
        publish();
        return first;
    }
    MaiError releaseAll() {
        MaiError first;
        for (std::size_t index = held_.size(); index-- > 0;) {
            const auto error = emitInput(held_[index].up);
            if (!error) held_.erase(held_.begin() + std::ptrdiff_t(index));
            else if (!first) first = error;
            publish();
        }
        verticalRemainder_ = horizontalRemainder_ = 0;
        if (touch_) {
            const auto error = touch_->releaseAll();
            if (!first && error) first = error;
            if (!error && touch_->state().empty()) {
                touch_.reset();
                std::atomic_store(&snapshot_->touch, touch_);
            }
        }
        if (pen_) {
            const auto error = pen_->releaseAll();
            if (!first && error) first = error;
            if (!error && pen_->state().empty()) {
                pen_.reset();
                std::atomic_store(&snapshot_->pen, pen_);
            }
        }
        publish();
        return first;
    }
    bool ownsTag(const std::string& tag) const { return tokenForTag(tag).has_value(); }
    MaiError releaseTag(const std::string& tag) {
        const auto token = tokenForTag(tag);
        return token ? releaseToken(*token) : MaiError{};
    }
    bool ownsButton(int key) const {
        for (const auto& entry : held_) if (entry.vk == key && entry.up.type == INPUT_MOUSE) return true;
        return false;
    }
    bool ownsModifier(DesktopKeyModifier modifier) const {
        switch (modifier) {
        case DesktopKeyModifier::Control: return ownsModifier(DesktopKeyModifier::ControlLeft) || ownsModifier(DesktopKeyModifier::ControlRight);
        case DesktopKeyModifier::Shift: return ownsModifier(DesktopKeyModifier::ShiftLeft) || ownsModifier(DesktopKeyModifier::ShiftRight);
        case DesktopKeyModifier::Alt: return ownsModifier(DesktopKeyModifier::AltLeft) || ownsModifier(DesktopKeyModifier::AltRight);
        case DesktopKeyModifier::Meta: return ownsModifier(DesktopKeyModifier::MetaLeft) || ownsModifier(DesktopKeyModifier::MetaRight);
        default: break;
        }
        const auto binding = modifierBinding(modifier, layout());
        if (!binding) return false;
        const auto candidate = keyEvent(binding.value(), true);
        for (const auto& entry : held_)
            if (sameBinding(entry, candidate, binding.value().virtualKey, binding.value().scanCode)) return true;
        return false;
    }
    bool ownsBinding(const DesktopWindowsKeyBinding& binding) const {
        const auto up = keyEvent(binding, true);
        for (const auto& entry : held_)
            if (sameBinding(entry, up, binding.virtualKey, binding.scanCode)) return true;
        return false;
    }
    bool isInteractive() const { return hooks_.validate ? true : interactiveDesktop(); }
    SHORT keyState(int key) const { return hooks_.keyState ? hooks_.keyState(key) : GetAsyncKeyState(key); }
    HKL layout() const {
        return hooks_.layout ? reinterpret_cast<HKL>(hooks_.layout()) : foregroundLayout();
    }
    bool modifierPressed(DesktopKeyModifier modifier) const {
        if (ownsModifier(modifier)) return true;
        const auto binding = modifierBinding(modifier, layout());
        if (!binding) return false;
        if (modifier == DesktopKeyModifier::Meta) return ((keyState(VK_LWIN) | keyState(VK_RWIN)) & 0x8000) != 0;
        return (keyState(binding.value().virtualKey) & 0x8000) != 0;
    }
    MaiError validate() const {
        if (input_.isCanceled && input_.isCanceled()) return {MaiErrorCode::Canceled, "computer input canceled"};
        if (hooks_.validate) {
            if (const auto error = hooks_.validate(input_)) return error;
            // Test seams may replace OS state, but a receiver still requires
            // its actual native identity unless the test supplies no receiver.
            if (!input_.executionWindow) return {};
        }
        if (input_.desktopScope) {
            if (!interactiveDesktop()) return {MaiErrorCode::NotConfigured, "Windows input requires the active interactive desktop"};
            MONITORINFO information{};
            if (!monitorInformation(input_.display.id, information)) return {MaiErrorCode::NotFound, "observed display is no longer active"};
            const auto& bounds = information.rcMonitor;
            if (input_.display.x != bounds.left || input_.display.y != bounds.top ||
                input_.display.width != bounds.right - bounds.left || input_.display.height != bounds.bottom - bounds.top)
                return {MaiErrorCode::Busy, "observed display geometry changed before input"};
            if (input_.executionWindow) {
                const auto& receiver = *input_.executionWindow;
                if (!receiver.id || !receiver.processId || receiver.minimized || receiver.cloaked ||
                    !receiver.boundsAvailable || !windowMatchesProcess(receiver) || !IsWindowVisible(handle(receiver)) ||
                    IsIconic(handle(receiver)))
                    return {MaiErrorCode::Busy, "execution window identity or visibility changed before input"};
                DWORD cloaked = 0;
                if (FAILED(DwmGetWindowAttribute(handle(receiver), DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
                    return {MaiErrorCode::Busy, "execution window state is unavailable or cloaked"};
                const auto actual = visibleBounds(handle(receiver));
                if (receiver.x != actual.left || receiver.y != actual.top ||
                    receiver.width != actual.right - actual.left || receiver.height != actual.bottom - actual.top)
                    return {MaiErrorCode::Busy, "execution window geometry changed before input"};
                if (!belongsToSelected(GetForegroundWindow(), handle(receiver)))
                    return {MaiErrorCode::Busy, "foreground no longer belongs to the execution window"};
            }
            return {};
        }
        if (!foreground_ || !belongsToSelected(GetForegroundWindow(), foreground_))
            return {MaiErrorCode::Busy, "foreground window changed during computer input"};
        DWORD process = 0;
        if (input_.expectedProcessId && (!GetWindowThreadProcessId(foreground_, &process) || process != input_.expectedProcessId))
            return {MaiErrorCode::Busy, "foreground window no longer belongs to the selected process"};
        return {};
    }
    MaiError validatePoint(POINT point) const {
        if (const auto error = validate()) return error;
        if (input_.executionWindow) {
            const auto& receiver = *input_.executionWindow;
            if (const auto outside = maiDesktopValidateExecutionPoint(receiver, point.x, point.y)) return outside;
            if (!belongsToSelected(WindowFromPhysicalPoint(point), handle(receiver)))
                return {MaiErrorCode::Busy, "pointer hit does not belong to the execution window"};
        }
        if (hooks_.hitTest) return hooks_.hitTest(input_, point);
        if (input_.desktopScope) {
            const auto& bounds = input_.display;
            if (point.x < bounds.x || point.y < bounds.y || point.x >= bounds.x + bounds.width || point.y >= bounds.y + bounds.height)
                return {MaiErrorCode::InvalidInput, "pointer location is outside the observed display"};
            return {};
        }
        if (!belongsToSelected(WindowFromPhysicalPoint(point), foreground_))
            return {MaiErrorCode::Busy, "pointer location no longer belongs to the selected window"};
        return {};
    }
    MaiResult<POINT> cursor() const {
        if (hooks_.cursor) return hooks_.cursor();
        POINT result{};
        if (!GetCursorPos(&result)) return {MaiErrorCode::Internal, "Windows could not inspect the pointer"};
        return result;
    }
    MaiError validatePointerTarget() const {
        if (const auto ready = validate()) return ready;
        if (const auto modifiers = pointerModifiers()) return modifiers;
        if (input_.desktopScope && !input_.executionWindow) return {}; // Unrestricted display input may cross windows.
        const auto point = cursor();
        return point ? validatePoint(point.value()) : point.error();
    }
    MaiError noForeignMouseButtons() const {
        for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
            if ((keyState(key) & 0x8000) && !ownsButton(key))
                return {MaiErrorCode::Busy, "a mouse button is already held outside this input session"};
        return {};
    }
    MaiError pointerModifiers() const {
        const struct { WORD generic, left, right; DesktopKeyModifier family, leftName, rightName; } families[]{
            {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, DesktopKeyModifier::Shift, DesktopKeyModifier::ShiftLeft, DesktopKeyModifier::ShiftRight},
            {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, DesktopKeyModifier::Control, DesktopKeyModifier::ControlLeft, DesktopKeyModifier::ControlRight},
            {VK_MENU, VK_LMENU, VK_RMENU, DesktopKeyModifier::Alt, DesktopKeyModifier::AltLeft, DesktopKeyModifier::AltRight}};
        for (const auto& family : families)
            if (((keyState(family.generic) & 0x8000) && !ownsModifier(family.family)) ||
                ((keyState(family.left) & 0x8000) && !ownsModifier(family.leftName)) ||
                ((keyState(family.right) & 0x8000) && !ownsModifier(family.rightName)))
                return {MaiErrorCode::Busy, "release modifiers that are not part of the requested pointer action"};
        if (((keyState(VK_LWIN) & 0x8000) && !ownsModifier(DesktopKeyModifier::MetaLeft)) ||
            ((keyState(VK_RWIN) & 0x8000) && !ownsModifier(DesktopKeyModifier::MetaRight)))
            return {MaiErrorCode::Busy, "release modifiers that are not part of the requested pointer action"};
        return {};
    }
    MaiError emitInput(const INPUT& event) const {
        const bool releasing = event.type == INPUT_KEYBOARD ? (event.ki.dwFlags & KEYEVENTF_KEYUP) != 0 :
            (event.mi.dwFlags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_MIDDLEUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_XUP)) != 0;
        if (!releasing && input_.isCanceled && input_.isCanceled())
            return {MaiErrorCode::Canceled, "computer input canceled before native emission"};
        const UINT accepted = hooks_.emitInput ? hooks_.emitInput(event) : SendInput(1, const_cast<INPUT*>(&event), sizeof(INPUT));
        if (accepted != 1) return {MaiErrorCode::Internal, "Windows accepted " + std::to_string(accepted) + " of 1 input events; input dispatch failed"};
        return {};
    }
    MaiError hold(INPUT down, INPUT up, std::string tag = {}, std::string label = {},
                  std::optional<DesktopInputRawKey> raw = {}, WORD vk = 0, WORD scan = 0,
                  std::vector<DesktopWindowsRawBinding> synthetic = {}) {
        if (!tag.empty() && ownsTag(tag)) return {};
        if (const auto ready = validate()) return ready;
        if (down.type == INPUT_MOUSE)
            if (const auto target = validatePointerTarget()) return target;
        if (!vk) vk = nativeVk(up);
        if (!scan && up.type == INPUT_KEYBOARD) scan = up.ki.wScan;
        Alias alias{++sequence_, std::move(tag), std::move(label), std::move(raw), std::move(synthetic)};
        for (auto& entry : held_) if (sameBinding(entry, up, vk, scan)) {
            entry.aliases.push_back(std::move(alias));
            publish();
            return {};
        }
        held_.push_back(Held{up, vk, scan, {std::move(alias)}});
        publish(); // The paired up exists before a down can escape.
        const auto sent = emitInput(down);
        if (sent) held_.pop_back(); // SendInput's single-event count zero means no down was accepted.
        publish();
        return sent;
    }
    MaiError release() {
        const auto token = latestTemporary();
        return token ? releaseToken(*token) : MaiError{};
    }
    MaiError tap(INPUT down, INPUT up) {
        const auto held = hold(down, up);
        if (held) return held;
        const auto released = release();
        return released ? released : validate();
    }
    MaiError move(double x, double y) const {
        const POINT point{int(std::lround(x)), int(std::lround(y))};
        if (const auto ready = validatePoint(point)) return ready;
        if (const auto modifiers = pointerModifiers()) return modifiers;
        if (input_.isCanceled && input_.isCanceled()) return {MaiErrorCode::Canceled, "computer input canceled before pointer movement"};
        if (hooks_.move) {
            if (const auto error = hooks_.move(point)) return error;
        } else if (!SetCursorPos(point.x, point.y)) return {MaiErrorCode::Internal, "Windows could not move the pointer"};
        return validate();
    }
    MaiError moveRelative(std::int32_t x, std::int32_t y) const {
        if (const auto ready = validate()) return ready;
        if (input_.executionWindow && (x || y))
            return {MaiErrorCode::NotSupported,
                    "window-restricted input requires absolute motion; relative motion is subject to OS acceleration"};
        if (const auto modifiers = pointerModifiers()) return modifiers;
        if (const auto foreign = noForeignMouseButtons()) return foreign;
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = x;
        input.mi.dy = y;
        const auto sent = emitInput(input);
        return sent ? sent : validate();
    }
    MaiError wheel(std::int32_t delta, bool horizontal, bool highResolution) {
        if (!delta) return {};
        if (const auto target = validatePointerTarget()) return target;
        auto amount = std::int64_t(delta);
        auto& remainder = horizontal ? horizontalRemainder_ : verticalRemainder_;
        if (!highResolution) { amount += remainder; remainder = amount % 120; amount -= remainder; }
        while (amount) {
            if (const auto ready = validatePointerTarget()) return ready;
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            const auto limit = highResolution ? 32767 : 32760;
            const auto lower = highResolution ? -32768 : -32760;
            const auto part = std::int32_t(std::clamp<std::int64_t>(amount, lower, limit));
            input.mi.mouseData = DWORD(part);
            if (const auto sent = emitInput(input)) return sent;
            amount -= part;
        }
        return validate();
    }
    MaiError rawKey(const DesktopInputRawKey& source, const DesktopInputOptions& options) {
        const auto tag = std::string("raw:") + std::to_string(maiDesktopInputKeyIdentity(source));
        if (source.release) return releaseTag(tag);
        const auto token = tokenForTag(tag);
        if (source.repeat) {
            if (!token) return {};
            for (const auto& entry : held_) for (const auto& alias : entry.aliases) if (alias.id == *token) {
                auto down = entry.up;
                down.ki.dwFlags &= ~KEYEVENTF_KEYUP;
                const auto vk = entry.vk;
                const auto synthetic = alias.synthetic;
                if (const auto secure = guardRawPlan(vk, synthetic)) return secure;
                std::vector<std::uint64_t> temporary;
                auto result = introduceModifiers(synthetic, temporary);
                if (!result) result = guardKey(vk);
                if (!result) result = emitInput(down);
                return retireModifiers(temporary, result);
            }
            return {};
        }
        if (token) return {};
        const auto effective = maiDesktopRemapInputKey(source, options);
        const auto binding = maiDesktopWindowsResolveRawKey(effective, options, std::uintptr_t(layout()));
        if (!binding) return binding.error();
        if ((keyState(binding.value().virtualKey) & 0x8000) && !ownsNative(binding.value()))
            return {MaiErrorCode::Busy, "Keyboard key is already held outside this input owner"};
        // Resolve the missing source-side modifiers before any input escapes.
        // Their native bindings, like the terminal key, freeze at first down.
        const auto prepared = prepareModifiers(source, options);
        if (!prepared) return prepared.error();
        auto original = source;
        original.release = original.repeat = false;
        // Acquiring an alias is metadata only. A modifier tap with no new key
        // down would itself be an unrelated observable input.
        if (ownsNative(binding.value()))
            return hold(binding.value().down, binding.value().up, tag,
                        "raw:" + std::to_string(source.code) + ":" + std::to_string(source.flags), original,
                        binding.value().virtualKey, binding.value().physicalScan, prepared.value());
        if (const auto secure = guardRawPlan(binding.value().virtualKey, prepared.value())) return secure;
        std::vector<std::uint64_t> temporary;
        MaiError first = introduceModifiers(prepared.value(), temporary);
        // A permitted prefix such as Tab may change the focused control. Check
        // this actual down again with current native modifiers and secure focus.
        if (!first) first = guardKey(binding.value().virtualKey);
        if (!first) {
            first = hold(binding.value().down, binding.value().up, tag,
                         "raw:" + std::to_string(source.code) + ":" + std::to_string(source.flags), original,
                         binding.value().virtualKey, binding.value().physicalScan, prepared.value());
        }
        return retireModifiers(temporary, first);
    }
    MaiError dispatchTyped(const DesktopInputEvent&, const DesktopInputPort&, const DesktopInputOptions&);
    MaiError dispatchLegacy(const DesktopNativeInput&);
    MaiError unicode(std::string_view);
    MaiError guardKey(WORD key, std::uint8_t requested = 0) const {
        if (const auto ready = validate()) return ready;
        if (key == VK_TAB || key == VK_ESCAPE || isModifierKey(key)) return {};
        const auto modifiers = std::uint8_t(requested | actualModifiers());
        if (key == VK_DELETE && (modifiers & 6) == 6)
            return {MaiErrorCode::NotSupported, "Ctrl+Alt+Delete is a system secure-attention sequence, not injectable keyboard input"};
        if (input_.desktopScope && modifiers == 8 && (key == 'R' || key == 'E' || key == 'D' || key == 'S')) return {};
        return guardText();
    }
    MaiError guardText() const {
        if (const auto ready = validate()) return ready;
        const auto secure = hooks_.secureInput ? hooks_.secureInput() : maiDesktopFocusedInputIsSecure();
        if (!secure) return secure.error();
        return secure.value() ? MaiError{MaiErrorCode::NotSupported, "secure inputs need manual user entry"} : MaiError{};
    }
    MaiError guardLiteralText() const {
        if (ownedModifier() || externalModifier()) return {MaiErrorCode::Busy, "literal text needs released modifiers"};
        return guardText();
    }
    bool externalModifier() const {
        for (const int key : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
                              VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
            if (keyState(key) & 0x8000) return true;
        return false;
    }
    bool ownedModifier() const {
        return std::any_of(held_.begin(), held_.end(), [](const auto& value) { return isModifierKey(value.vk); });
    }

private:
    struct Alias {
        std::uint64_t id;
        std::string tag, label;
        std::optional<DesktopInputRawKey> raw;
        // Binding metadata for repeat, not another ownership ledger. Every
        // actual temporary down still acquires a lease in held_.aliases.
        std::vector<DesktopWindowsRawBinding> synthetic;
    };
    struct Held { INPUT up; WORD vk, scan; std::vector<Alias> aliases; };
    static bool isModifierKey(WORD key) {
        return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_CONTROL || key == VK_LCONTROL ||
               key == VK_RCONTROL || key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN;
    }
    static std::uint8_t modifierMask(WORD key) {
        if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) return 1;
        if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL) return 2;
        if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU) return 4;
        if (key == VK_LWIN || key == VK_RWIN) return 8;
        return 0;
    }
    std::uint8_t sourceModifiers() const {
        std::uint8_t result = 0;
        // Packet bits refer to canonical source families, even when a source
        // Alt currently owns A or right Alt owns a cached native Meta binding.
        for (const auto& entry : held_)
            for (const auto& alias : entry.aliases)
                if (alias.raw) result |= modifierMask(WORD(maiDesktopInputKeyIdentity(*alias.raw) >> 8));
        return result;
    }
    std::uint8_t actualModifiers() const {
        std::uint8_t result = 0;
        for (const auto& entry : held_)
            if (entry.up.type == INPUT_KEYBOARD) result |= modifierMask(entry.vk);
        for (const WORD key : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
                               VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
            if (keyState(key) & 0x8000) result |= modifierMask(key);
        return result;
    }
    MaiResult<std::vector<DesktopWindowsRawBinding>> prepareModifiers(const DesktopInputRawKey& source,
                                                                      const DesktopInputOptions& options) const {
        std::vector<DesktopWindowsRawBinding> prepared;
        if (isModifierKey(WORD(maiDesktopInputKeyIdentity(source) >> 8))) return prepared;
        const auto mask = std::uint8_t(source.modifiers & 15 & ~sourceModifiers());
        const WORD modifiers[]{VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN};
        for (unsigned index = 0; index < 4; ++index) {
            if (!(mask & (1u << index))) continue;
            DesktopInputRawKey modifier;
            modifier.code = modifiers[index];
            const auto effective = maiDesktopRemapInputKey(modifier, options);
            const auto binding = maiDesktopWindowsResolveRawKey(effective, options, std::uintptr_t(layout()));
            if (!binding) return binding.error();
            prepared.push_back(binding.value());
        }
        return prepared;
    }
    bool borrowsBinding(const DesktopWindowsRawBinding& binding) const {
        if (ownsNative(binding) || (keyState(binding.virtualKey) & 0x8000)) return true;
        // A generic temporary modifier can borrow either native side. Sided
        // remappings retain their exact binding and never release another side.
        if (binding.virtualKey == VK_SHIFT || binding.virtualKey == VK_CONTROL || binding.virtualKey == VK_MENU)
            return (actualModifiers() & modifierMask(binding.virtualKey)) != 0;
        return false;
    }
    MaiError guardRawPlan(WORD target, const std::vector<DesktopWindowsRawBinding>& synthetic) const {
        if (const auto ready = validate()) return ready;
        auto finalModifiers = std::uint8_t(actualModifiers() | modifierMask(target));
        bool hasDelete = target == VK_DELETE;
        for (const auto& binding : synthetic) {
            finalModifiers |= modifierMask(binding.virtualKey);
            hasDelete = hasDelete || binding.virtualKey == VK_DELETE;
        }
        // Check the complete mapped chord before any prefix can escape. Source
        // modifier bits cannot predict the host's secure-attention combination.
        if (hasDelete && (finalModifiers & 6) == 6)
            return {MaiErrorCode::NotSupported,
                    "Ctrl+Alt+Delete is a system secure-attention sequence, not injectable keyboard input"};
        auto effects = actualModifiers();
        for (const auto& binding : synthetic) {
            if (borrowsBinding(binding)) continue;
            if (const auto secure = guardKey(binding.virtualKey, effects)) return secure;
            effects |= modifierMask(binding.virtualKey);
        }
        // Use only modifiers that really precede this event. A mapped ordinary
        // key cannot borrow a future Win press to bypass secure text handling.
        return guardKey(target, effects);
    }
    MaiError introduceModifiers(const std::vector<DesktopWindowsRawBinding>& synthetic,
                                std::vector<std::uint64_t>& temporary) {
        for (const auto& binding : synthetic) {
            if (borrowsBinding(binding)) continue;
            // Earlier prefixes can navigate inside the selected window. The
            // prepared plan cannot authorize this down after focus has changed.
            if (const auto secure = guardKey(binding.virtualKey)) return secure;
            const auto result = hold(binding.down, binding.up, {}, {}, {}, binding.virtualKey, binding.physicalScan);
            if (result) return result;
            temporary.push_back(sequence_);
        }
        return {};
    }
    MaiError retireModifiers(const std::vector<std::uint64_t>& temporary, MaiError first) {
        for (auto it = temporary.rbegin(); it != temporary.rend(); ++it) {
            const auto error = releaseToken(*it);
            if (error && !first) first = error;
        }
        return first;
    }
    static WORD nativeVk(const INPUT& input) {
        if (input.type == INPUT_KEYBOARD) return input.ki.wVk;
        const auto flags = input.mi.dwFlags;
        if (flags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP)) return VK_LBUTTON;
        if (flags & (MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_MIDDLEUP)) return VK_MBUTTON;
        if (flags & (MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP)) return VK_RBUTTON;
        if (flags & (MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP)) return input.mi.mouseData == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        return 0;
    }
    static bool sameBinding(const Held& held, const INPUT& up, WORD vk, WORD scan) {
        if (held.up.type != up.type) return false;
        if (up.type == INPUT_MOUSE) return held.vk == vk;
        const auto unicode = (up.ki.dwFlags & KEYEVENTF_UNICODE) != 0;
        if (unicode || (held.up.ki.dwFlags & KEYEVENTF_UNICODE))
            return unicode && (held.up.ki.dwFlags & KEYEVENTF_UNICODE) && held.up.ki.wScan == up.ki.wScan;
        const bool special = vk == VK_PAUSE || vk == VK_NUMLOCK || held.vk == VK_PAUSE || held.vk == VK_NUMLOCK;
        if (special || !scan || !held.scan) return vk && held.vk == vk;
        return held.scan == scan && ((held.up.ki.dwFlags ^ up.ki.dwFlags) & KEYEVENTF_EXTENDEDKEY) == 0;
    }
    bool ownsNative(const DesktopWindowsRawBinding& binding) const {
        for (const auto& entry : held_) if (sameBinding(entry, binding.up, binding.virtualKey, binding.physicalScan)) return true;
        return false;
    }
    std::optional<std::uint64_t> tokenForTag(const std::string& tag) const {
        for (const auto& entry : held_) for (const auto& alias : entry.aliases) if (alias.tag == tag) return alias.id;
        return {};
    }
    std::optional<std::uint64_t> latestTemporary() const {
        std::uint64_t result = 0;
        for (const auto& entry : held_) for (const auto& alias : entry.aliases)
            if (alias.tag.empty()) result = std::max(result, alias.id);
        return result ? std::optional<std::uint64_t>{result} : std::nullopt;
    }
    MaiError releaseToken(std::uint64_t token) {
        for (std::size_t index = 0; index < held_.size(); ++index) {
            auto& entry = held_[index];
            for (std::size_t alias = 0; alias < entry.aliases.size(); ++alias) if (entry.aliases[alias].id == token) {
                if (entry.aliases.size() > 1) {
                    entry.aliases.erase(entry.aliases.begin() + std::ptrdiff_t(alias));
                    publish();
                    return {};
                }
                const auto sent = emitInput(entry.up);
                if (!sent) held_.erase(held_.begin() + std::ptrdiff_t(index));
                publish();
                return sent;
            }
        }
        return {};
    }
    void publish() const {
        DesktopNativeInputState state;
        for (const auto& entry : held_) {
            if (entry.up.type == INPUT_MOUSE) {
                const char* name = entry.vk == VK_LBUTTON ? "left" : entry.vk == VK_MBUTTON ? "middle" :
                    entry.vk == VK_RBUTTON ? "right" : entry.vk == VK_XBUTTON1 ? "back" : "forward";
                state.buttonsDown.push_back(name);
            } else {
                std::string label = "pending_key";
                for (const auto& alias : entry.aliases) if (!alias.label.empty()) { label = alias.label; break; }
                state.keysDown.push_back(std::move(label));
            }
            for (const auto& alias : entry.aliases) if (alias.raw) state.rawKeys.push_back(*alias.raw);
        }
        std::atomic_store(&snapshot_->state, std::make_shared<const DesktopNativeInputState>(std::move(state)));
    }
    DesktopNativeInput input_;
    HWND foreground_ = nullptr;
    DWORD boundProcess_ = 0;
    DesktopWindowsInputHooks hooks_;
    std::shared_ptr<WindowsInputSnapshot> snapshot_;
    std::vector<Held> held_;
    std::uint64_t sequence_ = 0;
    std::int64_t verticalRemainder_ = 0, horizontalRemainder_ = 0;
    std::shared_ptr<DesktopComputerWindowsTouch> touch_;
    std::shared_ptr<DesktopComputerWindowsPen> pen_;
};

#include "MaiChatTools/DesktopComputerWindowsInputInternal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <set>
#include <thread>

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", name); }
}
DesktopInputEvent raw(std::uint16_t code, bool release = false, std::uint8_t flags = 0, std::uint8_t modifiers = 0) {
    DesktopInputEvent event;
    event.kind = DesktopInputEventKind::RawKey;
    event.key.code = code;
    event.key.flags = flags;
    event.key.modifiers = modifiers;
    event.key.release = release;
    return event;
}
struct Fake {
    std::vector<INPUT> events;
    std::vector<POINT> moves;
    std::set<int> foreignKeys;
    POINT point{20, 30};
    bool hit = true, valid = true, failUps = false, secure = false, secureError = false;
    int emits = 0, failAt = -1, layoutCalls = 0;
    std::function<void(const INPUT&)> onEmit;
    std::function<void()> onMove, onCursor;
    std::shared_ptr<DesktopNativeInputSession> session;
    Fake() {
        DesktopWindowsInputHooks hooks;
        hooks.emitInput = [this](const INPUT& input) {
            ++emits;
            if (emits == failAt || (failUps && input.type == INPUT_KEYBOARD && (input.ki.dwFlags & KEYEVENTF_KEYUP))) return UINT(0);
            events.push_back(input);
            if (onEmit) onEmit(input);
            return UINT(1);
        };
        hooks.move = [this](POINT value) { point = value; moves.push_back(value); if (onMove) onMove(); return MaiError{}; };
        hooks.cursor = [this] { if (onCursor) onCursor(); return MaiResult<POINT>{point}; };
        hooks.validate = [this](const DesktopNativeInput&) { return valid ? MaiError{} : MaiError{MaiErrorCode::Busy, "fake focus changed"}; };
        hooks.hitTest = [this](const DesktopNativeInput& scope, POINT) {
            return hit || scope.desktopScope ? MaiError{} : MaiError{MaiErrorCode::Busy, "fake foreign sibling"};
        };
        hooks.keyState = [this](int key) { return foreignKeys.count(key) ? SHORT(-32768) : SHORT(0); };
        hooks.layout = [this] { ++layoutCalls; return std::uintptr_t(GetKeyboardLayout(0)); };
        hooks.secureInput = [this]() -> MaiResult<bool> {
            if (secureError) return {MaiErrorCode::NotConfigured, "fake secure state unavailable"};
            return secure;
        };
        session = makeDesktopWindowsInputSessionForTesting(std::move(hooks));
    }
    ~Fake() { onEmit = {}; session->releaseAll(); session.reset(); }
    MaiError send(const DesktopInputEvent& event, const DesktopInputOptions& options = {}, const DesktopNativeInput& scope = {}) {
        return session->inputEvents({event}, scope, {}, options);
    }
    MaiError named(DesktopNativeInputKind kind, const char* key) {
        DesktopNativeInput input;
        input.kind = kind;
        input.key = key;
        return session->input(input);
    }
};

void rawIdentityAndBinding() {
    Fake fake;
    check(!fake.send(raw(0x8041, true)), "unknown raw up succeeds without effect");
    check(fake.events.empty(), "unknown raw up never emits");
    DesktopInputOptions options;
    options.keyBindings = {{0x41, 0x42}};
    check(!fake.send(raw(0x8041), options), "raw down remaps inside native");
    auto state = fake.session->state();
    check(state.rawKeys.size() == 1 && state.rawKeys[0].code == 0x8041 && state.rawKeys[0].flags == 0,
          "raw projection retains complete original source");
    check(!state.rawKeys[0].release && !state.rawKeys[0].repeat, "raw projection is held down metadata");
    check(fake.events.size() == 1 && fake.events[0].ki.wScan == 0x30, "effective B uses physical US scan");
    const auto first = fake.events[0];
    const auto resolutions = fake.layoutCalls;
    options.keyBindings = {{0x41, 0x43}};
    check(!fake.send(raw(0x8041, false, 0, 0x10), options), "duplicate does not reinterpret extended bit");
    check(fake.events.size() == 1 && fake.layoutCalls == resolutions, "duplicate never resolves or emits again");
    auto repeated = raw(0x8041, false, 0, 0x10);
    repeated.key.repeat = true;
    check(!fake.send(repeated, options), "repeat reuses original source lease");
    check(fake.events.size() == 2 && fake.events.back().ki.wScan == first.ki.wScan &&
          fake.events.back().ki.dwFlags == first.ki.dwFlags, "repeat uses cached effective binding");
    check(!fake.send(raw(0x8041, true, 0, 0x10), options), "up uses cached original binding");
    check(fake.events.size() == 3 && fake.events.back().ki.wScan == first.ki.wScan &&
          fake.events.back().ki.dwFlags == (first.ki.dwFlags | KEYEVENTF_KEYUP), "up ignores changed options and modifiers");
    check(fake.session->state().empty(), "raw up empties native ledger");
    check(!fake.send(repeated) && fake.events.size() == 3, "late repeat never resurrects a key");
}
void aliasesAndMixedEntry() {
    Fake fake;
    check(!fake.named(DesktopNativeInputKind::KeyDown, "Enter"), "legacy independent enter down");
    check(!fake.send(raw(0x800d)), "raw alias acquires same native binding");
    check(fake.events.size() == 1 && fake.session->state().keysDown.size() == 1, "legacy and raw share one physical down");
    check(fake.session->state().rawKeys.size() == 1, "raw alias is projected from common ledger");
    check(!fake.named(DesktopNativeInputKind::KeyUp, "Enter") && fake.events.size() == 1, "legacy up preserves raw alias");
    check(!fake.send(raw(0x800d, true)) && fake.events.size() == 2 && fake.session->state().empty(), "final alias up releases once");
    check(!fake.send(raw(0x8011)), "typed control hold");
    const auto start = fake.events.size();
    check(!fake.named(DesktopNativeInputKind::Key, "Ctrl+A"), "legacy chord borrows same-ledger control");
    bool additionalControl = false;
    for (std::size_t index = start; index < fake.events.size(); ++index)
        additionalControl = additionalControl || fake.events[index].ki.wScan == 0x1d;
    check(!additionalControl && fake.session->state().rawKeys.size() == 1, "temporary chord never represses or releases persistent control");
    check(!fake.send(raw(0x8011, true)), "typed control final up");
    check(!fake.send(raw(0x8041)) && !fake.send(raw(0x0041)), "full16 source identities both acquire aliases");
    check(fake.session->state().rawKeys.size() == 2, "source high prefix is not stripped from identity");
    const auto count = fake.events.size();
    check(!fake.send(raw(0x0041, true)) && fake.events.size() == count, "first alias up has no native up");
    check(!fake.send(raw(0x8041, true)) && fake.session->state().empty(), "second alias final up");
}
void mouseWheelAndUnicode() {
    Fake fake;
    DesktopInputEvent button;
    button.kind = DesktopInputEventKind::MouseButton;
    button.button = 1;
    fake.hit = false;
    check(bool(fake.send(button)) && fake.events.empty() && fake.moves.empty(), "foreign stationary button blocked before emitInput or warp");
    DesktopInputEvent wheel;
    wheel.kind = DesktopInputEventKind::Wheel;
    wheel.wheelDelta120 = 15;
    check(bool(fake.send(wheel)) && fake.events.empty(), "foreign wheel blocked before emission");
    DesktopNativeInput display;
    display.desktopScope = true;
    check(!fake.send(wheel, {}, display) && fake.events.back().mi.mouseData == 15, "display scope permits stationary wheel across windows");
    fake.hit = true;
    check(!fake.send(button), "typed stationary mouse down");
    fake.hit = false;
    fake.valid = false;
    button.release = true;
    check(!fake.send(button) && fake.session->state().empty(), "targeted mouse up ignores focus and hittest");
    fake.hit = fake.valid = true;
    const auto moved = fake.moves.size();
    DesktopInputEvent relative;
    relative.kind = DesktopInputEventKind::RelativeMove;
    relative.deltaX = -17;
    relative.deltaY = 11;
    check(!fake.send(relative), "relative movement native dispatch");
    check(fake.moves.size() == moved && fake.events.back().mi.dwFlags == MOUSEEVENTF_MOVE &&
          fake.events.back().mi.dx == -17 && fake.events.back().mi.dy == 11, "relative never warps cursor");
    DesktopInputOptions options;
    options.highResolutionScroll = false;
    wheel.wheelDelta120 = 60;
    const auto before = fake.events.size();
    check(!fake.send(wheel, options) && fake.events.size() == before, "coarse wheel retains sub-line remainder");
    check(!fake.send(wheel, options) && fake.events.size() == before + 1 && fake.events.back().mi.mouseData == 120,
          "coarse wheel emits exactly the completed line");
    wheel.horizontal = true;
    wheel.wheelDelta120 = -30;
    check(!fake.send(wheel) && fake.events.back().mi.dwFlags == MOUSEEVENTF_HWHEEL &&
          std::int32_t(fake.events.back().mi.mouseData) == -30, "fine horizontal signed delta preserved");
    DesktopInputEvent text;
    text.kind = DesktopInputEventKind::UnicodeText;
    text.text = "A\xf0\x9f\x98\x80" "B";
    const auto textStart = fake.events.size();
    check(!fake.send(text), "typed unicode uses native core");
    check(fake.events.size() == textStart + 8 && fake.events[textStart + 2].ki.wScan == 0xd83d &&
          fake.events[textStart + 4].ki.wScan == 0xde00 && fake.session->state().empty(), "unicode surrogate pairs and paired ups");
}
void failureSnapshotAndCancellation() {
    Fake fake;
    bool snapshotSeen = false;
    fake.onEmit = [&](const INPUT&) { snapshotSeen = !fake.session->state().rawKeys.empty(); };
    check(!fake.send(raw(0x8041)) && snapshotSeen, "state callback reads immutable snapshot during native emission");
    fake.onEmit = {};
    fake.failAt = fake.emits + 1;
    check(bool(fake.send(raw(0x8041, true))), "failed up reports failure");
    // The per-call failure cleanup retries; force subsequent attempts to fail
    // in the dedicated case below instead of assuming a single failed attempt
    // implies that the final cleanup must fail too.
    fake.failAt = -1;
    check(!fake.session->releaseAll() && fake.session->state().empty(), "cleanup can retry original up");
    Fake chunks;
    std::atomic<bool> canceled{false};
    DesktopNativeInput scope;
    scope.isCanceled = [&] { return canceled.load(); };
    chunks.onEmit = [&](const INPUT&) { if (chunks.events.size() == 3) canceled = true; };
    DesktopInputEvent wheel;
    wheel.kind = DesktopInputEventKind::Wheel;
    wheel.wheelDelta120 = 2147483647;
    check(bool(chunks.send(wheel, {}, scope)), "large wheel observes cancellation between native chunks");
    check(chunks.events.size() == 3, "large wheel stops without extra chunks");
    for (const auto& input : chunks.events)
        check(std::int32_t(input.mi.mouseData) <= 32767, "native wheel chunk fits signed message delta");
}
void modifiersAndRetainedFailure() {
    Fake fake;
    check(!fake.send(raw(0x8041, false, 0, 2)), "raw shortcut first down");
    auto repeated = raw(0x8041);
    repeated.key.repeat = true;
    const auto before = fake.events.size();
    check(!fake.send(repeated), "raw shortcut repeats");
    check(fake.events.size() == before + 3 && fake.events[before].ki.wScan == 0x1d &&
          (fake.events[before + 2].ki.dwFlags & KEYEVENTF_KEYUP), "repeat preserves first-down temporary modifier mask");
    fake.session->releaseAll();
    DesktopInputOptions options;
    options.rightAltToMeta = true;
    check(!fake.send(raw(0x80a5), options), "right alt source maps to meta");
    options.rightAltToMeta = false; // The held source retains its original effective binding.
    const auto start = fake.events.size();
    check(!fake.send(raw(0x8042, false, 0, 4), options), "next client alt shortcut follows cached right-alt binding");
    bool altEmitted = false;
    for (std::size_t index = start; index < fake.events.size(); ++index)
        altEmitted = altEmitted || fake.events[index].ki.wScan == 0x38;
    check(!altEmitted, "remapped held right-alt does not manufacture left-alt");
    const auto held = fake.session->state().rawKeys;
    check(held.size() == 2 && held.back().modifiers == 4, "raw projection keeps original client modifier bits");
    fake.session->releaseAll();
    check(!fake.send(raw(0x80a4)) && !fake.send(raw(0x8043, false, 0, 4)), "left alt remains real alt");
    check(!fake.send(raw(0x80a4, true)), "left alt independent up");
    repeated = raw(0x8043);
    repeated.key.repeat = true;
    const auto leftStart = fake.events.size();
    check(!fake.send(repeated) && fake.events.size() == leftStart + 3 && fake.events[leftStart].ki.wScan == 0x38,
          "left-alt modifier intent is not suppressed by right-alt policy");
    fake.session->releaseAll();
    check(!fake.send(raw(0x8041)), "retain-failure setup");
    fake.failUps = true;
    fake.valid = false;
    check(bool(fake.send(raw(0x8041, true))), "failed own up remains an error despite focus loss");
    check(fake.session->state().rawKeys.size() == 1, "failed up and failed cleanup retain original raw source");
    fake.failUps = false;
    check(!fake.session->releaseAll() && fake.session->state().empty(), "retry clears retained binding without focus");
    Fake alias;
    check(!alias.named(DesktopNativeInputKind::KeyDown, "Enter"), "modifier-alias setup");
    options = {};
    options.keyBindings = {{0x41, 0x0d}};
    const auto aliases = alias.events.size();
    check(!alias.send(raw(0x8041, false, 0, 2), options) && alias.events.size() == aliases,
          "acquiring physical alias emits no temporary modifier taps");
    Fake text;
    DesktopNativeInput invalid;
    invalid.kind = DesktopNativeInputKind::Type;
    invalid.text = "\xed\xa0\x80";
    check(bool(text.session->input(invalid)) && text.events.empty(), "legacy text rejects invalid UTF-8 before emission");
}
void nonblockingSnapshotAndRelease() {
    Fake fake;
    std::atomic<bool> entered{false}, allow{false};
    fake.onEmit = [&](const INPUT& input) {
        if (input.type != INPUT_KEYBOARD || (input.ki.dwFlags & KEYEVENTF_KEYUP)) return;
        entered = true;
        while (!allow.load()) std::this_thread::yield();
    };
    MaiError sent;
    std::thread worker([&] { sent = fake.send(raw(0x8041)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    check(entered.load(), "native fake is blocked inside the send call");
    // Rescue bounds a regression that accidentally waits for operationMutex.
    std::thread rescue([&] {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!allow.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        allow = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const auto snapshot = fake.session->state();
    const auto cleanup = fake.session->releaseAll();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(snapshot.rawKeys.size() == 1 && elapsed < std::chrono::milliseconds(100), "state and cancellation never wait for native/GUI progress");
    check(cleanup.code() == MaiErrorCode::Busy, "in-flight release reports pending cleanup truthfully");
    allow = true;
    worker.join();
    rescue.join();
    check(bool(sent) && fake.session->state().empty(), "epoch revocation cleans owned down when native call completes");
    fake.onEmit = {};
    std::weak_ptr<int> released;
    {
        auto token = std::make_shared<int>(1);
        released = token;
        DesktopNativeInput scope;
        scope.isCanceled = [token] { return false; };
        check(!fake.send(raw(0x8042), {}, scope), "per-call callback setup");
        scope.isCanceled = {};
        token.reset();
    }
    check(released.expired(), "native state does not retain per-call cancellation context");
    check(fake.session->state().rawKeys.size() == 1, "completed cancellation cannot retire a later fresh hold");
}
void neutralAndSecureIntent() {
    Fake neutral;
    neutral.valid = neutral.hit = false;
    DesktopInputOptions disabled;
    disabled.keyboardEnabled = disabled.mouseEnabled = false;
    DesktopInputEvent relative;
    relative.kind = DesktopInputEventKind::RelativeMove;
    DesktopInputEvent wheel;
    wheel.kind = DesktopInputEventKind::Wheel;
    DesktopInputEvent empty;
    empty.kind = DesktopInputEventKind::UnicodeText;
    check(!neutral.send(relative, disabled) && !neutral.send(wheel, disabled) && !neutral.send(empty, disabled) &&
          neutral.events.empty() && neutral.moves.empty(), "neutral events neither require focus nor enabled devices");
    Fake fake;
    fake.secure = true;
    DesktopNativeInput text;
    text.kind = DesktopNativeInputKind::Type;
    text.text = "text";
    check(bool(fake.session->input(text)) && fake.events.empty(), "legacy text secure check belongs to native core");
    check(bool(fake.named(DesktopNativeInputKind::Key, "Ctrl+A")) && fake.events.empty(), "legacy editing shortcut cannot bypass protected input");
    DesktopInputEvent unicode;
    unicode.kind = DesktopInputEventKind::UnicodeText;
    unicode.text = "text";
    check(bool(fake.send(unicode)) && fake.events.empty(), "typed unicode uses same protected-input rule");
    DesktopInputOptions remap;
    remap.keyBindings = {{VK_TAB, 'A'}};
    check(bool(fake.send(raw(VK_TAB), remap)) && fake.events.empty(), "source navigation remapped to character is protected");
    remap.keyBindings = {{'A', VK_TAB}};
    check(!fake.send(raw('A'), remap) && fake.events.size() == 1, "effective navigation retains its exemption");
    check(!fake.send(raw('A', true), remap), "protected target still permits cached targeted up");
    fake.secureError = true;
    check(!fake.named(DesktopNativeInputKind::Key, "Tab"), "Tab escape navigation needs no secure-control introspection");
    check(!fake.named(DesktopNativeInputKind::Key, "Escape"), "Escape navigation survives missing secure metadata");
    check(!fake.named(DesktopNativeInputKind::KeyDown, "MetaLeft"), "standalone meta can leave protected control");
    check(!fake.named(DesktopNativeInputKind::KeyUp, "MetaLeft"), "meta targeted up remains usable");
    DesktopNativeInput desktop;
    desktop.desktopScope = true;
    check(!fake.send(raw(VK_LWIN), {}, desktop), "desktop launcher meta setup");
    check(!fake.send(raw('E'), {}, desktop), "exact desktop Meta E uses actual held modifier exemption");
    check(!fake.send(raw('E', true), {}, desktop), "desktop launcher key up");
    check(!fake.send(raw(VK_LWIN, true), {}, desktop), "desktop launcher modifier up");
    check(bool(fake.send(raw('Q'), {}, desktop)), "arbitrary desktop character still needs verifiable secure state");
}
void mouseAliasesMappingAndPreflight() {
    Fake fake;
    DesktopNativeInput down;
    down.kind = DesktopNativeInputKind::MouseDown;
    down.x = 20;
    down.y = 30;
    check(!fake.session->input(down), "legacy mouse hold shares native core");
    DesktopInputEvent button;
    button.kind = DesktopInputEventKind::MouseButton;
    button.button = 1;
    button.release = true;
    check(!fake.send(button) && fake.events.size() == 2 && fake.session->state().empty(),
          "typed up pairs the ordinary hold of the same physical mouse button");
    button.release = false;
    check(!fake.send(button), "typed mouse down after cross-entry release");
    check(!fake.session->input(down) && fake.events.size() == 3 && fake.session->state().buttonsDown.size() == 1,
          "duplicate ordinary down after typed down is the same idempotent mouse identity");
    down.kind = DesktopNativeInputKind::MouseUp;
    fake.valid = fake.hit = false;
    check(!fake.session->input(down) && fake.events.size() == 4 && fake.session->state().empty(),
          "ordinary mouse up pairs the typed hold without focus");
    button.release = true;
    check(!fake.send(button) && fake.events.size() == 4 && fake.session->state().empty(), "repeated up across entry points emits no extra release");
    Fake position;
    DesktopInputEvent absolute;
    absolute.kind = DesktopInputEventKind::AbsoluteMove;
    absolute.x = absolute.y = 1;
    DesktopInputPort port;
    port.x = -1920;
    port.y = -100;
    port.width = 100;
    port.height = 200;
    check(!position.session->inputEvents({absolute}, {}, port, {}), "typed absolute maps negative-origin viewport");
    DesktopNativeInput move;
    move.kind = DesktopNativeInputKind::Move;
    move.x = -1821;
    move.y = 99;
    check(!position.session->input(move) && position.moves.size() == 2 && position.moves[0].x == -1821 &&
          position.moves[0].y == 99 && position.moves[1].x == position.moves[0].x && position.moves[1].y == position.moves[0].y,
          "legacy and typed absolute use identical inclusive physical primitive");
    Fake preflight;
    auto first = raw(0x8011), delayed = raw(0x8041);
    delayed.delayMs = 1;
    check(bool(preflight.session->inputEvents({first, delayed}, {}, {}, {})) && preflight.events.empty(),
          "unconsumed native batch delay fails before any input");
    DesktopInputOptions options;
    options.alwaysSendScanCodes = false;
    auto native = raw(VK_F11);
    native.key.flags = 1;
    const auto f11 = maiDesktopWindowsResolveRawKey(native.key, options, std::uintptr_t(GetKeyboardLayout(0)));
    check(f11 && f11.value().down.ki.wVk == VK_F11, "non-normalized F11 remains a Windows VK without SDK portable remapping");
    native.key = {};
    native.key.code = VK_F24;
    const auto f24 = maiDesktopWindowsResolveRawKey(native.key, {}, std::uintptr_t(GetKeyboardLayout(0)));
    check(f24 && f24.value().physicalScan == 0x76, "normalized F24 retains standard physical scan");
    Fake attention;
    check(bool(attention.send(raw(VK_DELETE, false, 0, 6))) && attention.events.empty(),
          "raw Ctrl Alt Delete cannot bypass legacy secure-attention rejection");
}
void targetAndNativeEndpoints() {
    Fake target;
    DesktopNativeInput first, other;
    first.expectedWindowId = 111;
    first.expectedProcessId = 7;
    other.expectedWindowId = 222;
    other.expectedProcessId = 7;
    check(!target.send(raw(VK_CONTROL), {}, first), "target binding initial hold");
    const auto emitted = target.events.size();
    check(bool(target.send(raw('A'), {}, other)) && target.events.size() == emitted && target.session->state().rawKeys.size() == 1,
          "held native controls cannot silently rebind to another target");
    check(!target.send(raw(VK_CONTROL, true), {}, other), "targeted up still uses original native binding");
    Fake mouse;
    const DWORD downs[]{MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
    const DWORD ups[]{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_XUP, MOUSEEVENTF_XUP};
    for (std::uint8_t number = 1; number <= 5; ++number) {
        DesktopInputEvent event;
        event.kind = DesktopInputEventKind::MouseButton;
        event.button = number;
        check(!mouse.send(event) && mouse.events.back().mi.dwFlags == downs[number - 1], "five-button protocol down maps to exact Win32 flag");
        if (number >= 4) check(mouse.events.back().mi.mouseData == DWORD(number == 4 ? XBUTTON1 : XBUTTON2), "extended mouse identity preserved");
        event.release = true;
        check(!mouse.send(event) && mouse.events.back().mi.dwFlags == ups[number - 1], "five-button cached native up");
        DesktopNativeInput legacy;
        legacy.kind = DesktopNativeInputKind::MouseDown;
        const DesktopMouseButton names[]{DesktopMouseButton::Left, DesktopMouseButton::Middle, DesktopMouseButton::Right,
                                        DesktopMouseButton::Back, DesktopMouseButton::Forward};
        legacy.button = names[number - 1];
        legacy.x = 20;
        legacy.y = 30;
        check(!mouse.session->input(legacy), "five-button ordinary independent down");
        const auto count = mouse.events.size();
        event.release = false;
        check(!mouse.send(event) && mouse.events.size() == count, "five-button cross-entry duplicate down is idempotent");
        event.release = true;
        check(!mouse.send(event) && mouse.events.size() == count + 1 && mouse.session->state().empty(),
              "five-button typed up ends ordinary down without crossing another identity");
        event.release = false;
        check(!mouse.send(event), "five-button typed independent down");
        legacy.kind = DesktopNativeInputKind::MouseUp;
        check(!mouse.session->input(legacy) && mouse.session->state().empty() && mouse.events.back().mi.dwFlags == ups[number - 1],
              "five-button ordinary up ends typed down with cached up flags");
    }
    check(mouse.moves.size() == 5 && mouse.session->state().empty(), "only ordinary positioned downs move; typed five-button edges stay stationary");
    Fake wheel;
    DesktopInputEvent event;
    event.kind = DesktopInputEventKind::Wheel;
    event.wheelDelta120 = (-2147483647 - 1);
    check(!wheel.send(event), "full int32 negative wheel accepted without narrowing");
    std::int64_t sum = 0;
    bool bounded = true;
    for (const auto& input : wheel.events) {
        const auto delta = std::int32_t(input.mi.mouseData);
        sum += delta;
        bounded = bounded && delta >= -32768 && delta <= 32767;
    }
    check(sum == (-2147483647LL - 1) && bounded, "all native SHORT chunks sum to exact int32 endpoint");
    DesktopInputOptions coarse;
    coarse.highResolutionScroll = false;
    event.wheelDelta120 = 60;
    check(!wheel.send(event, coarse), "coarse remainder before reset");
    const auto count = wheel.events.size();
    check(!wheel.session->resetControls() && !wheel.send(event, coarse) && wheel.events.size() == count,
          "reset clears only owned coarse remainder");
}
void wheelRechecksTargetAndThreadDpi() {
    Fake crossing;
    crossing.onEmit = [&](const INPUT&) { crossing.hit = false; };
    DesktopInputEvent wheel;
    wheel.kind = DesktopInputEventKind::Wheel;
    wheel.wheelDelta120 = 65536;
    check(bool(crossing.send(wheel)) && crossing.events.size() == 1,
          "every wheel chunk rechecks physical target before emission");
    using SetContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetContext = DPI_AWARENESS_CONTEXT(WINAPI*)();
    using EqualContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    const auto module = GetModuleHandleW(L"user32.dll");
    const auto set = reinterpret_cast<SetContext>(GetProcAddress(module, "SetThreadDpiAwarenessContext"));
    const auto get = reinterpret_cast<GetContext>(GetProcAddress(module, "GetThreadDpiAwarenessContext"));
    const auto equal = reinterpret_cast<EqualContext>(GetProcAddress(module, "AreDpiAwarenessContextsEqual"));
    check(set && get && equal, "actual Windows DPI observation APIs available");
    if (!set || !get || !equal) return;
    const auto previous = set(DPI_AWARENESS_CONTEXT_UNAWARE);
    check(previous != nullptr, "test enters thread-local unaware context without changing process settings");
    Fake dpi;
    std::vector<bool> moves, cursors;
    dpi.onMove = [&] { moves.push_back(equal(get(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE); };
    dpi.onCursor = [&] { cursors.push_back(equal(get(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE); };
    DesktopInputEvent absolute;
    absolute.kind = DesktopInputEventKind::AbsoluteMove;
    absolute.x = absolute.y = .5;
    DesktopInputPort port;
    port.width = port.height = 100;
    check(!dpi.session->inputEvents({absolute}, {}, port, {}), "typed absolute fake dispatch under actual unaware caller");
    check(equal(get(), DPI_AWARENESS_CONTEXT_UNAWARE), "typed dispatch restores caller thread DPI");
    DesktopNativeInput legacy;
    legacy.kind = DesktopNativeInputKind::Move;
    legacy.x = legacy.y = 50;
    check(!dpi.session->input(legacy), "legacy absolute fake dispatch under same caller");
    check(equal(get(), DPI_AWARENESS_CONTEXT_UNAWARE), "legacy dispatch restores caller thread DPI");
    wheel.wheelDelta120 = 15;
    check(!dpi.send(wheel), "stationary fake wheel observes native cursor under same DPI scope");
    check(moves.size() == 2 && moves[0] && moves[1] && !cursors.empty() && cursors[0],
          "both entry points and cursor preflight use physical per-monitor coordinates");
    check(equal(get(), DPI_AWARENESS_CONTEXT_UNAWARE), "cursor and wheel dispatch restore outer DPI");
    set(previous);
}
void atomicHeldKeyAndPointerModifiers() {
    Fake held;
    check(!held.send(raw('A')), "atomic terminal held setup");
    const auto count = held.events.size();
    const auto attempted = held.named(DesktopNativeInputKind::Key, "Ctrl+A");
    bool newDown = false;
    for (std::size_t index = count; index < held.events.size(); ++index)
        newDown = newDown || (held.events[index].type == INPUT_KEYBOARD && !(held.events[index].ki.dwFlags & KEYEVENTF_KEYUP));
    check(attempted.code() == MaiErrorCode::Busy && !newDown && held.session->state().empty(),
          "atomic terminal already held is deterministic Busy before temporary presses and applies own error cleanup");
    Fake modifier;
    check(!modifier.send(raw(VK_RCONTROL)), "right-side modifier held setup");
    const auto before = modifier.events.size();
    check(!modifier.named(DesktopNativeInputKind::Key, "Ctrl") && modifier.events.size() == before && modifier.session->state().rawKeys.size() == 1,
          "generic single modifier tap borrows either held side without a second press");
    Fake pointer;
    pointer.foreignKeys = {VK_CONTROL, VK_LCONTROL};
    DesktopInputEvent button;
    button.kind = DesktopInputEventKind::MouseButton;
    button.button = 1;
    check(pointer.send(button).code() == MaiErrorCode::Busy && pointer.events.empty(), "typed button rejects foreign modifier before emission");
    DesktopInputEvent wheel;
    wheel.kind = DesktopInputEventKind::Wheel;
    wheel.wheelDelta120 = 15;
    check(pointer.send(wheel).code() == MaiErrorCode::Busy && pointer.events.empty(), "typed wheel rejects foreign modifier before emission");
    DesktopInputEvent relative;
    relative.kind = DesktopInputEventKind::RelativeMove;
    relative.deltaX = 5;
    check(pointer.send(relative).code() == MaiErrorCode::Busy && pointer.events.empty() && pointer.moves.empty(),
          "relative preserves unknown modifier safety without introducing a warp");
    DesktopInputEvent absolute;
    absolute.kind = DesktopInputEventKind::AbsoluteMove;
    absolute.x = absolute.y = .5;
    DesktopInputPort port;
    port.width = port.height = 100;
    check(pointer.session->inputEvents({absolute}, {}, port, {}).code() == MaiErrorCode::Busy && pointer.moves.empty(),
          "absolute and legacy move share foreign modifier safety");
    pointer.foreignKeys.clear();
    check(!pointer.send(raw(VK_CONTROL)), "same-ledger modifier setup for pointer combination");
    pointer.foreignKeys = {VK_CONTROL, VK_LCONTROL}; // Real GetAsyncKeyState also observes our owned injected hold.
    check(!pointer.send(button) && !pointer.send(wheel) && !pointer.send(relative), "owned modifier permits explicit same-owner pointer combination");
    pointer.valid = pointer.hit = false;
    button.release = true;
    check(!pointer.send(button), "owned pointer up ignores focus hittest and modifier gate");
    check(!pointer.session->releaseAll() && pointer.session->state().empty(), "own modifier cleanup never releases a foreign source");
}
void cancellationCallbackCannotEmitAfterRevocation() {
    // Sweep every cancellation observation, including the last preflight and
    // post-dispatch check. The canceling thread is different here, so the old
    // implementation's self-owned mutex precondition is never violated.
    for (int at = 1; at <= 12; ++at) {
        Fake fake;
        int calls = 0;
        bool revoked = false;
        std::size_t eventsAtRevocation = 0;
        DesktopNativeInput scope;
        scope.isCanceled = [&] {
            if (++calls == at) {
                revoked = true;
                eventsAtRevocation = fake.events.size();
                std::thread cancel([&] { fake.session->releaseAll(); });
                cancel.join();
            }
            return false; // A callback may cancel through the session API.
        };
        DesktopInputEvent wheel;
        wheel.kind = DesktopInputEventKind::Wheel;
        wheel.wheelDelta120 = 15;
        fake.send(wheel, {}, scope);
        if (revoked) check(fake.events.size() == eventsAtRevocation,
                           "caller callback revocation cannot allow a later native effect");
    }
}
void sameThreadCallerReentry() {
    Fake fake;
    check(!fake.send(raw('C')), "reentry existing native hold setup");
    bool entered = false, readable = false;
    MaiError legacy, typed, reset, release;
    DesktopNativeInput scope;
    scope.isCanceled = [&] {
        if (!entered) {
            entered = true;
            readable = fake.session->state().rawKeys.size() == 1 && !fake.session->validate() &&
                !fake.session->capabilities().features.empty() && fake.session->feedback(0, 1).events.empty();
            DesktopNativeInput move;
            move.kind = DesktopNativeInputKind::Move;
            legacy = fake.session->input(move);
            typed = fake.send(raw('B'));
            reset = fake.session->resetControls();
            release = fake.session->releaseAll();
        }
        return false;
    };
    const auto before = fake.events.size();
    const auto outer = fake.send(raw('A'), {}, scope);
    check(entered && readable, "same-thread caller can read native snapshots without an operation lock");
    check(legacy.code() == MaiErrorCode::Busy && typed.code() == MaiErrorCode::Busy && reset.code() == MaiErrorCode::Busy,
          "reentrant input and reset are Busy before attempting the owned mutex");
    check(release.code() == MaiErrorCode::Busy && outer.code() == MaiErrorCode::Canceled,
          "reentrant release revokes the outer call even when caller returns false");
    check(fake.moves.empty() && fake.events.size() == before + 1 &&
          (fake.events.back().ki.dwFlags & KEYEVENTF_KEYUP) && fake.session->state().empty(),
          "outer cleanup emits only its previous owned up and no late requested down");
    check(!fake.send(raw('D')) && fake.session->state().rawKeys.size() == 1,
          "fresh input after reentrant cancellation is not consumed by an old request");
}
void revocationDuringPendingCleanup() {
    Fake fake;
    check(!fake.send(raw('A')) && !fake.send(raw('B')), "pending cleanup two-binding setup");
    bool requestedAgain = false;
    fake.onEmit = [&](const INPUT& input) {
        if (!requestedAgain && input.type == INPUT_KEYBOARD && (input.ki.dwFlags & KEYEVENTF_KEYUP)) {
            requestedAgain = true;
            fake.session->releaseAll(); // Queue one more cleanup while this successful UP is in flight.
            fake.failAt = fake.emits + 1; // The other binding must remain available to the next drain.
        }
    };
    check(bool(fake.session->releaseAll()) && fake.session->state().rawKeys.size() == 1,
          "failed cleanup preserves a pending original binding");
    fake.failAt = -1;
    bool canceledDrain = false;
    MaiError concurrent;
    fake.onEmit = [&](const INPUT& input) {
        if (!canceledDrain && input.type == INPUT_KEYBOARD && (input.ki.dwFlags & KEYEVENTF_KEYUP)) {
            canceledDrain = true;
            std::thread cancel([&] { concurrent = fake.session->releaseAll(); });
            cancel.join();
        }
    };
    const auto before = fake.events.size();
    const auto result = fake.send(raw('C'));
    bool newDown = false;
    for (std::size_t index = before; index < fake.events.size(); ++index)
        newDown = newDown || !(fake.events[index].ki.dwFlags & KEYEVENTF_KEYUP);
    check(canceledDrain && concurrent.code() == MaiErrorCode::Busy && result.code() == MaiErrorCode::Canceled && !newDown,
          "revocation during admitted pending-UP drain cannot be adopted as a new input generation");
    fake.onEmit = {};
    check(fake.session->state().empty() && !fake.send(raw('D')) && fake.session->state().rawKeys.size() == 1,
          "drained binding is empty and the next fresh admission can hold normally");
}
}
int main() {
    rawIdentityAndBinding();
    aliasesAndMixedEntry();
    mouseWheelAndUnicode();
    failureSnapshotAndCancellation();
    modifiersAndRetainedFailure();
    nonblockingSnapshotAndRelease();
    neutralAndSecureIntent();
    mouseAliasesMappingAndPreflight();
    targetAndNativeEndpoints();
    wheelRechecksTargetAndThreadDpi();
    atomicHeldKeyAndPointerModifiers();
    cancellationCallbackCannotEmitAfterRevocation();
    sameThreadCallerReentry();
    revocationDuringPendingCleanup();
    std::printf("DesktopComputerWindowsNativeSessionTest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

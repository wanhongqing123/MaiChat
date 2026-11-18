#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiChatTools/DesktopComputerInputTypes.h"

struct DesktopNativeWindow {
    std::uint64_t id = 0;
    std::uint64_t processId = 0;
    std::string title;
    std::string application;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    // Discovery includes real top-level windows that cannot currently accept
    // pointer input. In particular, iconic coordinates are not screen bounds.
    bool minimized = false;
    bool cloaked = false;
    bool boundsAvailable = true;
    bool toolWindow = false;
    std::uint64_t ownerId = 0;
    std::string className;
};

enum class DesktopWindowStateAction { Minimize, Restore };

// Snapshot-owned native identity. Platform references retain only the selected
// object and are never serialized or accepted from model arguments.
class DesktopNativeElementReference {
public:
    virtual ~DesktopNativeElementReference() = default;
};

struct DesktopNativeElement {
    std::string role;
    std::string label;
    // Screen bounds use Windows physical pixels or macOS native screen points,
    // independently of PNG dimensions and the model's selected-target ratios.
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    // Internal provider identity; an absent identity/reference never receives a
    // fabricated actionable element ID merely from its label and rectangle.
    std::string nativeId;
    std::string automationId;
    bool enabled = true;
    bool offscreen = false;
    std::shared_ptr<const DesktopNativeElementReference> reference;
};

struct DesktopNativeAccessibilitySnapshot {
    std::vector<DesktopNativeElement> elements;
    std::string provider = "backend";
    std::string status = "available";
    std::size_t visitedNodes = 0;
    // Legacy elements() backends cannot claim complete traversal counts.
    bool countsKnown = false;
    bool truncated = false;
    std::string limitReason;
};

// Bounds use native pointer coordinates; the observation reports PNG pixels
// separately and supplies the normalized mapping between these spaces.
struct DesktopNativeDisplay {
    std::uint64_t id = 0;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool primary = false;
};

enum class DesktopNativeInputKind {
    Click,
    DoubleClick,
    RightClick,
    Drag,
    Type,
    Key,
    ReplaceFocusedText,
    Scroll,
    Move,
    MouseDown,
    MouseUp,
    KeyDown,
    KeyUp
};

enum class DesktopMouseButton { Left, Right, Middle, Back, Forward };
enum class DesktopScrollAxis { Vertical, Horizontal };

// The ledger describes this session's synthetic holds, not physical hardware
// state or acknowledgement from the receiving application.
struct DesktopNativeInputState {
    std::vector<std::string> buttonsDown;
    std::vector<std::string> keysDown;
    // Read-only projection of persistent raw source tokens from the native
    // ledger. These are the original source codes, not a second held-key map.
    // The first down freezes each token's resolved binding; up/repeat never
    // reinterpret it after a layout or remapping change. release/repeat are false.
    std::vector<DesktopInputRawKey> rawKeys;
    std::vector<std::uint32_t> touchPointers;
    bool penActive = false;
    // Neutral devices still own native resources. They participate in the
    // owner's timeout/turn cleanup even when no control is currently pressed.
    std::vector<std::string> devices;
    std::vector<DesktopInputDevice> controllers;
    std::int64_t idleRemainingMs = 0;
    bool empty() const {
        return buttonsDown.empty() && keysDown.empty() && rawKeys.empty() && touchPointers.empty() && !penActive && devices.empty() &&
               controllers.empty();
    }
};

// The controller resolves observed window coordinates before native dispatch.
// Cancellation is checked throughout gestures; native code always releases its
// own pressed buttons and keys when dispatch fails or the foreground changes.
struct DesktopNativeInput {
    DesktopNativeInputKind kind = DesktopNativeInputKind::Click;
    double x = 0;
    double y = 0;
    double toX = 0;
    double toY = 0;
    std::string text;
    std::string key;
    int lines = 0;
    int durationMs = 200;
    DesktopMouseButton button = DesktopMouseButton::Left;
    DesktopScrollAxis scrollAxis = DesktopScrollAxis::Vertical;
    // New controller calls bind dispatch to the observed native identity. Zero
    // keeps the older free function wrappers scoped to their initial foreground.
    std::uint64_t expectedWindowId = 0;
    std::uint64_t expectedProcessId = 0;
    // Explicit display scope permits intentional foreground changes; the legacy
    // window scope continues checking the selected window identity.
    bool desktopScope = false;
    DesktopNativeDisplay display;
    std::function<bool()> isCanceled;
};

// Ordinary native sessions pair only their own downs with ups. Implementations
// serialize these methods and preserve native source/held state across input()
// calls. Per-call cancellation callbacks MUST be cleared before returning.
// releaseAll and targeted ups remain usable after focus loss/cancellation;
// destructor cleanup is independent of the caller's observation token.
class DesktopNativeInputSession {
public:
    virtual ~DesktopNativeInputSession() = default;
    virtual MaiError input(const DesktopNativeInput& input) = 0;
    virtual DesktopNativeInputState state() const = 0;
    // Read-only target/permission validation for the owner's watchdog; no
    // per-call cancellation callback is retained or consulted here.
    virtual MaiError validate() const = 0;
    // Reset owned controls while retaining typed controller identities. The
    // legacy default owns no gamepads and delegates to releaseAll(). Like
    // targeted ups, this cleanup does not need a current observation/focus.
    // Composite reset returns Busy without effects while public input is in
    // flight; releaseAll remains the asynchronous cancellation/teardown entry.
    virtual MaiError resetControls();
    virtual MaiError releaseAll() = 0;
    virtual MaiError inputEvents(const std::vector<DesktopInputEvent>& events, const DesktopNativeInput& scope,
                                 const DesktopInputPort& port, const DesktopInputOptions& options);
    virtual DesktopInputCapabilities capabilities() const;
    virtual DesktopInputFeedbackBatch feedback(std::uint64_t afterSequence, std::uint32_t limit) const;
};

std::shared_ptr<DesktopNativeInputSession> maiDesktopCreateInputSession();

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows();
MaiResult<std::vector<DesktopNativeDisplay>> maiDesktopDisplays();
MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(const DesktopNativeWindow& window);
MaiResult<DesktopNativeAccessibilitySnapshot> maiDesktopInspectElements(
    const DesktopNativeWindow& window, const std::function<bool()>& isCanceled = {});
MaiResult<DesktopNativeElement> maiDesktopResolveElement(
    const DesktopNativeWindow& window, const DesktopNativeElement& observedElement,
    const std::function<bool()>& isCanceled = {});
MaiError maiDesktopSetWindowState(const DesktopNativeWindow& window, DesktopWindowStateAction action,
                                   const std::function<bool()>& isCanceled = {});
MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window);
// Callback is valid only during this synchronous call. Queued platform work
// must abandon activation and clear its callback before the waiter returns.
MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window, std::function<bool()> isCanceled);
MaiError maiDesktopClick(double x, double y);
MaiError maiDesktopType(const std::string& text);
MaiError maiDesktopKey(const std::string& key);
MaiError maiDesktopScroll(double x, double y, int lines);
MaiResult<bool> maiDesktopFocusedInputIsSecure();
MaiError maiDesktopInput(const DesktopNativeInput& input);
MaiResult<std::string> maiDesktopFocusedText(const DesktopNativeWindow& window);
MaiResult<bool> maiDesktopWindowIsFocused(const DesktopNativeWindow& window);
// Input may continue in a menu/HUD owned by the selected window. Text readback
// and explicit focus verification still require the selected window itself.
MaiResult<bool> maiDesktopWindowAcceptsInput(const DesktopNativeWindow& window);

#if defined(_WIN32)
#include "MaiChatTools/DesktopComputerKeys.h"

// No injection or cursor movement. Reuses the native scope's interactive
// desktop, monitor geometry, foreground and process checks for all event kinds.
MaiError maiDesktopWindowsValidateInputScope(const DesktopNativeInput& scope);

// Read-only native key resolution shared by dispatch and host diagnostics. The
// supplied layout is the target thread's HKL; resolving never activates a window
// or injects input. A valid VK and its scan/extended identity are returned, or a
// typed error for unsupported hardware/system keys. impliedModifiers only apply
// to literal characters; named punctuation keys retain their physical identity.
struct DesktopWindowsKeyBinding {
    std::uint16_t virtualKey = 0;
    std::uint16_t scanCode = 0;
    bool extended = false;
    std::vector<DesktopKeyModifier> impliedModifiers;
};

MaiResult<DesktopWindowsKeyBinding> maiDesktopWindowsResolveKey(const DesktopKeyChord& chord,
                                                                std::uintptr_t keyboardLayout);
#endif

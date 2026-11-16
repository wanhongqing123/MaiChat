// Atomic Windows touchscreen frames, independent of the HID SDK. Only this
// source's synthetic handle/slots are refreshed or canceled during cleanup.
#if defined(_WIN32)
#if !defined(WINVER) || WINVER < 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#if !defined(NTDDI_VERSION) || NTDDI_VERSION < 0x0A000006
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "MaiChatTools/DesktopComputerWindowsTouchInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace {
using Phase = DesktopInputPhase;
constexpr std::size_t maxContacts = 10;

struct Environment {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
};

struct HeldContact {
    std::uint32_t slot = 0;
    std::int32_t screenX = 0, screenY = 0;
    std::int64_t left = 0, top = 0, right = 0, bottom = 0;
    std::uint32_t pressure = 0, orientation = 0;
    bool touching = false, hasPressure = false, hasOrientation = false, hasArea = false;
};
using Contacts = std::map<std::uint32_t, HeldContact>;

MaiError invalid(const char* message) { return {MaiErrorCode::InvalidInput, message}; }

bool nativeCoordinate(double value) {
    return std::isfinite(value) && value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

MaiResult<Environment> environment(const DesktopInputPort& port) {
    if (!nativeCoordinate(port.environmentX) || !nativeCoordinate(port.environmentY) ||
        !nativeCoordinate(port.environmentWidth) || !nativeCoordinate(port.environmentHeight) ||
        port.environmentWidth < 1 || port.environmentHeight < 1 ||
        std::floor(port.environmentX) != port.environmentX || std::floor(port.environmentY) != port.environmentY ||
        std::floor(port.environmentWidth) != port.environmentWidth || std::floor(port.environmentHeight) != port.environmentHeight)
        return invalid("touch virtual desktop needs finite integral native bounds");
    const auto right = port.environmentX + port.environmentWidth - 1;
    const auto bottom = port.environmentY + port.environmentHeight - 1;
    if (!nativeCoordinate(right) || !nativeCoordinate(bottom)) return invalid("touch virtual desktop endpoints overflow native coordinates");
    return Environment{static_cast<std::int32_t>(port.environmentX), static_cast<std::int32_t>(port.environmentY),
        static_cast<std::int32_t>(port.environmentWidth), static_cast<std::int32_t>(port.environmentHeight)};
}

MaiResult<HeldContact> project(const DesktopInputTouch& input, const DesktopInputPort& port,
                               const Environment& desktop, std::uint32_t slot) {
    if (!nativeCoordinate(port.x) || !nativeCoordinate(port.y) || !nativeCoordinate(port.width) ||
        !nativeCoordinate(port.height) || port.width < 1 || port.height < 1 ||
        !nativeCoordinate(port.x + port.width - 1) || !nativeCoordinate(port.y + port.height - 1))
        return invalid("touch viewport needs finite native bounds of at least one pixel");
    const auto x = std::llround(port.x + input.x * (port.width - 1));
    const auto y = std::llround(port.y + input.y * (port.height - 1));
    if (x < desktop.x || y < desktop.y || x >= std::int64_t(desktop.x) + desktop.width ||
        y >= std::int64_t(desktop.y) + desktop.height) return invalid("touch point is outside the observed virtual desktop");
    HeldContact result;
    result.slot = slot;
    result.screenX = static_cast<std::int32_t>(x);
    result.screenY = static_cast<std::int32_t>(y);
    result.touching = input.phase != Phase::Hover;
    result.hasPressure = !result.touching || input.pressureOrDistance > 0;
    result.pressure = !result.touching ? 0 : input.pressureOrDistance > 0 ?
        static_cast<std::uint32_t>(std::lround(input.pressureOrDistance * 1024.0)) : 512;
    result.hasOrientation = input.rotation != 0xffff;
    // The protocol measures clockwise from vertical; Win32 measures from +X.
    // Ellipse axes are symmetric, but retaining the full native angle avoids
    // turning an unknown orientation into a reported zero-degree measurement.
    result.orientation = result.hasOrientation ? (input.rotation % 360 + 270) % 360 : 0;
    result.left = result.right = x;
    result.top = result.bottom = y;
    const double major = input.contactAreaMajor > 0 ? input.contactAreaMajor : input.contactAreaMinor;
    const double minor = input.contactAreaMinor > 0 ? input.contactAreaMinor : major;
    result.hasArea = major > 0 && minor > 0;
    if (result.hasArea) {
        constexpr double pi = 3.14159265358979323846;
        const auto angle = result.hasOrientation ? (input.rotation % 360) * pi / 180 : pi / 4;
        const auto width = std::hypot(major * port.width * std::sin(angle), minor * port.width * std::cos(angle));
        const auto height = std::hypot(major * port.height * std::cos(angle), minor * port.height * std::sin(angle));
        result.left = (std::max)(std::int64_t(desktop.x), static_cast<std::int64_t>(std::floor(x - width / 2)));
        result.top = (std::max)(std::int64_t(desktop.y), static_cast<std::int64_t>(std::floor(y - height / 2)));
        result.right = (std::min)(std::int64_t(desktop.x) + desktop.width,
            (std::max)(result.left + 1, static_cast<std::int64_t>(std::ceil(x + width / 2))));
        result.bottom = (std::min)(std::int64_t(desktop.y) + desktop.height,
            (std::max)(result.top + 1, static_cast<std::int64_t>(std::ceil(y + height / 2))));
    }
    return result;
}

DesktopWindowsTouchPoint nativePoint(const HeldContact& contact, const Environment& desktop, std::uint32_t flags) {
    DesktopWindowsTouchPoint result;
    result.pointerId = contact.slot;
    result.flags = flags;
    result.screenX = contact.screenX;
    result.screenY = contact.screenY;
    result.x = static_cast<std::int32_t>(std::int64_t(contact.screenX) - desktop.x);
    result.y = static_cast<std::int32_t>(std::int64_t(contact.screenY) - desktop.y);
    // A valid center can survive a virtual-desktop rebase while the cached
    // contact box crosses its new edge. Clip the box before native narrowing.
    result.left = static_cast<std::int32_t>((std::max)(std::int64_t(desktop.x), contact.left) - desktop.x);
    result.top = static_cast<std::int32_t>((std::max)(std::int64_t(desktop.y), contact.top) - desktop.y);
    result.right = static_cast<std::int32_t>((std::min)(std::int64_t(desktop.x) + desktop.width, contact.right) - desktop.x);
    result.bottom = static_cast<std::int32_t>((std::min)(std::int64_t(desktop.y) + desktop.height, contact.bottom) - desktop.y);
    result.pressure = contact.pressure;
    result.orientation = contact.orientation;
    result.hasPressure = contact.hasPressure;
    result.hasOrientation = contact.hasOrientation;
    result.hasArea = contact.hasArea;
    return result;
}

DesktopWindowsTouchFrame frame(const Environment& desktop) {
    DesktopWindowsTouchFrame result;
    result.virtualX = desktop.x;
    result.virtualY = desktop.y;
    result.virtualWidth = desktop.width;
    result.virtualHeight = desktop.height;
    return result;
}

std::uint32_t restingFlags(const HeldContact& contact) {
    return kDesktopTouchUpdate | kDesktopTouchInRange | (contact.touching ? kDesktopTouchInContact : 0);
}

class TouchFrames final : public DesktopComputerWindowsTouch {
public:
    TouchFrames(std::shared_ptr<DesktopWindowsTouchTransport> transport, std::chrono::milliseconds interval)
        : transport_(std::move(transport)), interval_((std::max)(std::chrono::milliseconds(0), interval)) {
        static std::atomic<std::uint64_t> sequence{0};
        sourceName_ = "windows-touch-" + std::to_string(++sequence);
    }
    ~TouchFrames() override { releaseAll(); }

    MaiError dispatch(const std::vector<DesktopInputTouch>& inputs, const DesktopInputPort& port) override {
        DesktopInputEvent request;
        request.kind = DesktopInputEventKind::Touch;
        request.contacts = inputs;
        if (auto error = maiDesktopValidateInputEvents({request})) return error;
        std::unique_lock<std::mutex> operation(operationMutex_);
        Contacts previous;
        Environment desktop;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (faulted_) return {MaiErrorCode::Busy, "touch source needs releaseAll after a failed native submission"};
            previous = contacts_;
            desktop = desktop_;
        }
        auto next = previous;
        std::map<std::uint32_t, bool> ending;
        bool changed = false;
        bool haveDesktop = false;
        for (const auto& input : inputs) {
            if (input.phase == Phase::CancelAll) {
                for (const auto& held : previous) ending[held.first] = true;
                next.clear();
                changed = changed || !previous.empty();
                continue;
            }
            if (input.phase == Phase::Up || input.phase == Phase::Cancel || input.phase == Phase::HoverLeave) {
                const auto held = next.find(input.pointerId);
                if (held == next.end()) continue; // Unknown ups never create/inject a source.
                if (previous.find(input.pointerId) != previous.end())
                    ending[input.pointerId] = input.phase == Phase::Cancel;
                next.erase(held);
                changed = true;
                continue;
            }
            if (!haveDesktop) {
                const auto bounds = environment(port);
                if (!bounds) return bounds.error();
                desktop = bounds.value();
                haveDesktop = true;
            }
            if (ending.find(input.pointerId) != ending.end())
                return invalid("an ended touch ID cannot restart within the same native frame");
            auto held = next.find(input.pointerId);
            std::uint32_t slot = held == next.end() ? 0 : held->second.slot;
            if (held == next.end()) {
                std::array<bool, maxContacts> used{};
                // Ended slots remain reserved through acceptance of their UP.
                for (const auto& value : previous) used[value.second.slot] = true;
                for (const auto& value : next) used[value.second.slot] = true;
                while (slot < maxContacts && used[slot]) ++slot;
                if (slot == maxContacts) return {MaiErrorCode::Busy, "all ten native touch slots remain owned in this frame"};
            }
            const auto projected = project(input, port, desktop, slot);
            if (!projected) return projected.error();
            next[input.pointerId] = projected.value();
            changed = true;
        }
        if (!changed) return {};
        auto pending = frame(desktop);
        for (const auto& held : previous) {
            const auto end = ending.find(held.first);
            if (end != ending.end()) {
                const auto flags = (held.second.touching ? kDesktopTouchUp : kDesktopTouchUpdate) |
                    (end->second ? kDesktopTouchCanceled : 0);
                pending.points.push_back(nativePoint(held.second, desktop, flags));
            }
        }
        for (const auto& held : next) {
            const auto old = previous.find(held.first);
            auto flags = restingFlags(held.second);
            if (old == previous.end() || (!old->second.touching && held.second.touching))
                flags = kDesktopTouchInRange | (held.second.touching ? kDesktopTouchDown | kDesktopTouchInContact : kDesktopTouchUpdate);
            else if (old->second.touching && !held.second.touching)
                flags = kDesktopTouchUp | kDesktopTouchInRange;
            pending.points.push_back(nativePoint(held.second, desktop, flags));
        }
        if (pending.points.empty()) return {};
        for (const auto& point : pending.points) {
            if (point.x < 0 || point.y < 0 || point.x >= pending.virtualWidth || point.y >= pending.virtualHeight)
                return invalid("an owned touch lies outside the new virtual desktop; release it before rebinding bounds");
        }
        std::sort(pending.points.begin(), pending.points.end(), [](const auto& a, const auto& b) { return a.pointerId < b.pointerId; });
        bool opened = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            opened = deviceOpen_;
        }
        if (!opened) {
            if (auto error = transport_->create(maxContacts)) return error;
            std::lock_guard<std::mutex> lock(stateMutex_);
            deviceOpen_ = true; // Own the resource before a down can escape.
        }
        if (auto error = submit(pending)) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            faulted_ = true;
            return error;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            contacts_ = std::move(next); // Remove ended contacts only after accepted frame.
            desktop_ = desktop;
        }
        if (emptyContacts()) return finishSource();
        if (interval_.count() > 0 && !worker_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                stopping_ = false;
            }
            worker_ = std::thread([this] { refresh(); });
        }
        return {};
    }

    DesktopNativeInputState state() const override {
        std::lock_guard<std::mutex> lock(stateMutex_);
        DesktopNativeInputState result;
        for (const auto& contact : contacts_) result.touchPointers.push_back(contact.first);
        if (deviceOpen_) result.devices.push_back(sourceName_);
        return result;
    }

    MaiError releaseAll() override {
        std::unique_lock<std::mutex> operation(operationMutex_);
        stopWorker();
        Contacts held;
        Environment desktop;
        bool opened = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            held = contacts_;
            desktop = desktop_;
            opened = deviceOpen_;
        }
        if (!opened) return {};
        MaiError canceled;
        if (!held.empty()) {
            auto cancel = frame(desktop);
            for (const auto& contact : held)
                cancel.points.push_back(nativePoint(contact.second, desktop,
                    kDesktopTouchCanceled | (contact.second.touching ? kDesktopTouchUp : kDesktopTouchUpdate)));
            canceled = submit(cancel);
        }
        // Destroy is attempted even if cancellation failed. A destroyed owned
        // source ends its contacts; it does not prove the app rolled back work.
        const auto destroyed = transport_->destroy();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!canceled || !destroyed) contacts_.clear();
            if (!destroyed) { deviceOpen_ = false; faulted_ = false; desktop_ = {}; }
            else faulted_ = true;
        }
        return destroyed ? destroyed : canceled;
    }

private:
    bool emptyContacts() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return contacts_.empty();
    }
    MaiError submit(const DesktopWindowsTouchFrame& frameInput) {
        auto value = frameInput;
        // Keep slot order stable across caller deltas, stationary refresh and
        // cancellation even when opaque caller IDs have a different order.
        std::sort(value.points.begin(), value.points.end(), [](const auto& a, const auto& b) { return a.pointerId < b.pointerId; });
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto result = transport_->inject(value);
            switch (result.kind) {
            case DesktopWindowsTouchSubmitKind::Accepted: return {};
            case DesktopWindowsTouchSubmitKind::Failed:
                return result.error ? result.error : MaiError{MaiErrorCode::Internal, "native touch frame was rejected"};
            case DesktopWindowsTouchSubmitKind::Retry:
                if (attempt != 7) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                break;
            default: return {MaiErrorCode::Internal, "unknown native touch submission status"};
            }
        }
        return {MaiErrorCode::Busy, "native touch frame remained not ready after eight identical attempts"};
    }
    void stopWorker() {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        // Refresh never blocks on operationMutex_, so public cleanup may hold
        // that mutex while joining without a reverse-lock deadlock.
        if (worker_.joinable()) worker_.join();
    }
    MaiError finishSource() {
        stopWorker();
        const auto result = transport_->destroy();
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!result) { deviceOpen_ = false; faulted_ = false; desktop_ = {}; }
        else faulted_ = true;
        return result;
    }
    void refresh() {
        std::unique_lock<std::mutex> state(stateMutex_);
        while (!stopping_) {
            changed_.wait_for(state, interval_, [&] { return stopping_; });
            if (stopping_) break;
            state.unlock();
            {
                std::unique_lock<std::mutex> operation(operationMutex_, std::try_to_lock);
                if (operation.owns_lock()) {
                    DesktopWindowsTouchFrame update;
                    bool active = false;
                    {
                        std::lock_guard<std::mutex> lock(stateMutex_);
                        active = !stopping_ && !faulted_ && deviceOpen_ && !contacts_.empty();
                        if (active) {
                            update = frame(desktop_);
                            for (const auto& held : contacts_)
                                update.points.push_back(nativePoint(held.second, desktop_, restingFlags(held.second)));
                        }
                    }
                    if (active) if (auto error = submit(update)) {
                        std::lock_guard<std::mutex> lock(stateMutex_);
                        faulted_ = true;
                    }
                }
            }
            state.lock();
        }
    }
    std::shared_ptr<DesktopWindowsTouchTransport> transport_;
    const std::chrono::milliseconds interval_;
    std::string sourceName_;
    std::mutex operationMutex_;
    mutable std::mutex stateMutex_;
    std::condition_variable changed_;
    std::thread worker_;
    Contacts contacts_;
    Environment desktop_;
    bool deviceOpen_ = false, stopping_ = false, faulted_ = false;
};

#if defined(_WIN32)
class NativeTransport final : public DesktopWindowsTouchTransport {
public:
    explicit NativeTransport(std::shared_ptr<DesktopWindowsTouchNativeTrace> trace = {}, bool feedback = true, bool explicitQpc = false)
        : trace_(std::move(trace)), feedback_(feedback), explicitQpc_(explicitQpc) {
        module_ = LoadLibraryW(L"user32.dll");
        if (module_) {
            create_ = reinterpret_cast<Create>(GetProcAddress(module_, "CreateSyntheticPointerDevice"));
            inject_ = reinterpret_cast<Inject>(GetProcAddress(module_, "InjectSyntheticPointerInput"));
            destroy_ = reinterpret_cast<Destroy>(GetProcAddress(module_, "DestroySyntheticPointerDevice"));
            dpi_ = reinterpret_cast<Dpi>(GetProcAddress(module_, "SetThreadDpiAwarenessContext"));
            getDpi_ = reinterpret_cast<GetDpi>(GetProcAddress(module_, "GetThreadDpiAwarenessContext"));
            awareness_ = reinterpret_cast<Awareness>(GetProcAddress(module_, "GetAwarenessFromDpiAwarenessContext"));
            equalDpi_ = reinterpret_cast<EqualDpi>(GetProcAddress(module_, "AreDpiAwarenessContextsEqual"));
        }
    }
    ~NativeTransport() override { destroy(); if (module_) FreeLibrary(module_); }
    MaiError create(std::uint32_t capacity) override {
        if (!create_ || !inject_ || !destroy_)
            return {MaiErrorCode::NotSupported, "native touchscreen needs Windows 10 1809 synthetic pointer APIs"};
        if (device_) return {MaiErrorCode::Busy, "this native touchscreen source is already open"};
        lastAcceptedQpc_ = pendingQpc_ = 0;
        pendingBytes_.clear();
        if (trace_) { creator_ = currentDpi(); creatorThread_ = GetCurrentThreadId(); capacity_ = capacity; }
        device_ = create_(PT_TOUCH, capacity, feedback_ ? POINTER_FEEDBACK_DEFAULT : POINTER_FEEDBACK_NONE);
        return device_ ? MaiError{} : MaiError{MaiErrorCode::Internal,
            "create owned touchscreen failed (Win32 " + std::to_string(GetLastError()) + ")"};
    }
    DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame& value) override {
        if (!device_ || value.points.empty() || value.points.size() > maxContacts)
            return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::InvalidInput, "invalid owned native touchscreen frame"}};
        const auto before = trace_ ? currentDpi() : DpiSample{};
        const auto previousDpi = dpi_ ? dpi_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
        const auto effective = trace_ ? currentDpi() : DpiSample{};
        const bool sameDesktop = GetSystemMetrics(SM_XVIRTUALSCREEN) == value.virtualX &&
            GetSystemMetrics(SM_YVIRTUALSCREEN) == value.virtualY && GetSystemMetrics(SM_CXVIRTUALSCREEN) == value.virtualWidth &&
            GetSystemMetrics(SM_CYVIRTUALSCREEN) == value.virtualHeight;
        if (!sameDesktop) {
            if (dpi_ && previousDpi) dpi_(previousDpi);
            return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Busy, "virtual desktop changed before touchscreen submission"}};
        }
        std::array<POINTER_TYPE_INFO, maxContacts> frame{};
        for (std::size_t index = 0; index < value.points.size(); ++index) {
            const auto& source = value.points[index];
            frame[index].type = PT_TOUCH;
            auto& native = frame[index].touchInfo;
            native.pointerInfo.pointerType = PT_TOUCH;
            native.pointerInfo.pointerId = source.pointerId;
            native.pointerInfo.pointerFlags = source.flags;
            native.pointerInfo.ptPixelLocation = {source.x, source.y};
            native.pressure = source.pressure;
            native.orientation = source.orientation;
            native.rcContact = {source.left, source.top, source.right, source.bottom};
            if (source.hasPressure) native.touchMask |= TOUCH_MASK_PRESSURE;
            if (source.hasOrientation) native.touchMask |= TOUCH_MASK_ORIENTATION;
            if (source.hasArea) native.touchMask |= TOUCH_MASK_CONTACTAREA;
        }
        if (explicitQpc_) {
            // Diagnostic only: POINTER_INFO documents optional injection
            // timestamps. Synthetic's own page does not restate those rules.
            // Keep the first element's timestamp stable across native retries.
            const auto size = sizeof(POINTER_TYPE_INFO) * value.points.size();
            const auto* begin = reinterpret_cast<const std::uint8_t*>(frame.data());
            const std::vector<std::uint8_t> plain(begin, begin + size);
            if (!pendingQpc_ || pendingBytes_ != plain) {
                LARGE_INTEGER now{}, frequency{};
                if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
                    if (dpi_ && previousDpi) dpi_(previousDpi);
                    return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "test injection timestamp counter unavailable"}};
                }
                const auto sample = std::uint64_t(now.QuadPart);
                const auto minimumStep = std::uint64_t(frequency.QuadPart) / 10000 + 1;
                if (sample <= lastAcceptedQpc_ || sample - lastAcceptedQpc_ < minimumStep) {
                    if (dpi_ && previousDpi) dpi_(previousDpi);
                    return {DesktopWindowsTouchSubmitKind::Retry, {MaiErrorCode::Busy, "test injection timestamp needs a later 0.1-ms window"}};
                }
                pendingQpc_ = sample; // Sampled now, never a fabricated future tick.
                pendingBytes_ = plain;
            }
            frame[0].touchInfo.pointerInfo.PerformanceCount = pendingQpc_;
        }
        DesktopWindowsTouchNativePacket packet;
        if (trace_) {
            packet.frame = value;
            packet.creatorThread = creatorThread_;
            packet.injectorThread = GetCurrentThreadId();
            packet.maxContacts = capacity_;
            packet.visualFeedback = feedback_;
            packet.creatorDpi = creator_.context;
            packet.beforeDpi = before.context;
            packet.effectiveDpi = effective.context;
            packet.creatorAwareness = creator_.awareness;
            packet.beforeAwareness = before.awareness;
            packet.effectiveAwareness = effective.awareness;
            packet.creatorV2 = creator_.v2;
            packet.beforeV2 = before.v2;
            packet.effectiveV2 = effective.v2;
            packet.stride = sizeof(POINTER_TYPE_INFO);
            packet.sourceHandle = std::uint64_t(reinterpret_cast<std::uintptr_t>(device_));
            packet.bytes.resize(sizeof(POINTER_TYPE_INFO) * value.points.size());
            std::memcpy(packet.bytes.data(), frame.data(), packet.bytes.size());
        }
        if (trace_) packet.callStartedMs = GetTickCount64();
        const auto accepted = inject_(device_, frame.data(), static_cast<UINT32>(value.points.size()));
        const auto error = accepted ? ERROR_SUCCESS : GetLastError();
        if (trace_) packet.callFinishedMs = GetTickCount64();
        if (dpi_ && previousDpi) dpi_(previousDpi);
        if (trace_) {
            packet.restoredDpi = currentDpi().context;
            packet.accepted = accepted != FALSE;
            packet.error = error;
            trace_->append(std::move(packet));
        }
        if (accepted) {
            if (explicitQpc_) { lastAcceptedQpc_ = pendingQpc_; pendingQpc_ = 0; pendingBytes_.clear(); }
            return {};
        }
        if (error != ERROR_NOT_READY) { pendingQpc_ = 0; pendingBytes_.clear(); }
        return {error == ERROR_NOT_READY ? DesktopWindowsTouchSubmitKind::Retry : DesktopWindowsTouchSubmitKind::Failed,
            {MaiErrorCode::Internal, "owned touchscreen frame failed (Win32 " + std::to_string(error) + ")"}};
    }
    MaiError destroy() override {
        if (device_) { destroy_(device_); device_ = nullptr; }
        return {};
    }
private:
    struct DpiSample { std::int64_t context = 0; int awareness = -1; bool v2 = false; };
    DpiSample currentDpi() const {
        const auto context = getDpi_ ? getDpi_() : nullptr;
        return {static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(context)),
            context && awareness_ ? int(awareness_(context)) : -1,
            context && equalDpi_ && equalDpi_(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    }
    using Create = HSYNTHETICPOINTERDEVICE (WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
    using Inject = BOOL (WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
    using Destroy = VOID (WINAPI*)(HSYNTHETICPOINTERDEVICE);
    using Dpi = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetDpi = DPI_AWARENESS_CONTEXT (WINAPI*)();
    using Awareness = DPI_AWARENESS (WINAPI*)(DPI_AWARENESS_CONTEXT);
    using EqualDpi = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    HMODULE module_ = nullptr;
    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    Create create_ = nullptr;
    Inject inject_ = nullptr;
    Destroy destroy_ = nullptr;
    Dpi dpi_ = nullptr;
    GetDpi getDpi_ = nullptr;
    Awareness awareness_ = nullptr;
    EqualDpi equalDpi_ = nullptr;
    DpiSample creator_;
    DWORD creatorThread_ = 0;
    std::uint32_t capacity_ = 0;
    std::shared_ptr<DesktopWindowsTouchNativeTrace> trace_;
    bool feedback_ = true;
    bool explicitQpc_ = false;
    std::uint64_t lastAcceptedQpc_ = 0, pendingQpc_ = 0;
    std::vector<std::uint8_t> pendingBytes_;
};
#else
class NativeTransport final : public DesktopWindowsTouchTransport {
public:
    explicit NativeTransport(std::shared_ptr<DesktopWindowsTouchNativeTrace> = {}, bool = true, bool = false) {}
    MaiError create(std::uint32_t) override { return {MaiErrorCode::NotSupported, "Windows native touchscreen is unavailable on this platform"}; }
    DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame&) override {
        return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::NotSupported, "Windows native touchscreen is unavailable"}};
    }
    MaiError destroy() override { return {}; }
};
#endif
} // namespace

std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouchWithTransport(
    std::shared_ptr<DesktopWindowsTouchTransport> transport, std::chrono::milliseconds refreshInterval) {
    return transport ? std::make_shared<TouchFrames>(std::move(transport), refreshInterval) : nullptr;
}

std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouch() {
    return makeDesktopComputerWindowsTouchWithTransport(std::make_shared<NativeTransport>());
}

std::shared_ptr<DesktopWindowsTouchTransport> makeDesktopWindowsTouchNativeTransportForTesting(
    std::shared_ptr<DesktopWindowsTouchNativeTrace> trace, bool visualFeedback, bool explicitQpc) {
    return std::make_shared<NativeTransport>(std::move(trace), visualFeedback, explicitQpc);
}

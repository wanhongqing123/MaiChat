// 文件作用：用自有原生窗口验证常驻采集线程、连续帧和会话生命周期；不采集显示器或用户窗口。
// 窗口在线程内绘制纯色，测试线程独立解码 PNG 中心像素，避免把内部计数当作图像正确性的证据。
#include "MaiScreenshot.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "MaiThread.h"

#define REQUIRE(condition)                                                           \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                            \
        }                                                                            \
    } while (false)

// 全局期限覆盖普通同步 API 意外不返回的回归；超时只退出本测试进程，系统会回收其自有窗口。
class MaiCaptureTestDeadline {
public:
    MaiCaptureTestDeadline()
        : mThread([this] {
              MaiThread::setCurrentName("mai-cap-limit");
              std::unique_lock<std::mutex> lock(mMutex);
              if (!mCondition.wait_for(lock, std::chrono::seconds(90),
                                       [this] { return mFinished; })) {
                  std::fprintf(stderr,
                               "FAIL screenshot lifecycle exceeded its 90 second deadline\n");
                  std::fflush(stderr);
                  std::_Exit(1);
              }
          }) {}

    ~MaiCaptureTestDeadline() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFinished = true;
        }
        mCondition.notify_one();
        mThread.join();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mFinished = false;
    std::thread mThread;
};

struct MaiCaptureWindowPaint {
    COLORREF color;
};

class MaiOwnedCaptureWindows {
public:
    enum : UINT { SetColor = WM_APP + 1, Resize, Hide, Minimize, Restore };

    MaiOwnedCaptureWindows() {
        mReady = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!mReady) return;
        mThread = std::thread([this] { run(); });
        mStarted = ::WaitForSingleObject(mReady, 5000) == WAIT_OBJECT_0 && mFirst && mSecond;
    }

    ~MaiOwnedCaptureWindows() {
        if (mThread.joinable()) {
            ::PostThreadMessageW(mThreadId, WM_QUIT, 0, 0);
            if (::WaitForSingleObject(mThread.native_handle(), 5000) != WAIT_OBJECT_0) {
                std::fprintf(stderr, "FAIL owned window thread did not stop within five seconds\n");
                std::fflush(stderr);
                std::_Exit(1);
            }
            mThread.join();
        }
        if (mReady) ::CloseHandle(mReady);
    }

    bool isStarted() const {
        return mStarted;
    }
    HWND first() const {
        return mFirst;
    }
    HWND second() const {
        return mSecond;
    }

    bool command(HWND window, UINT message, WPARAM first = 0, LPARAM second = 0) {
        DWORD_PTR result = 0;
        return ::SendMessageTimeoutW(window, message, first, second, SMTO_ABORTIFHUNG, 2000,
                                     &result) != 0 &&
               result == 1;
    }

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM first,
                                            LPARAM second) {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(second);
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* paint =
            reinterpret_cast<MaiCaptureWindowPaint*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PAINT && paint) {
            PAINTSTRUCT state{};
            HDC context = ::BeginPaint(window, &state);
            RECT client{};
            ::GetClientRect(window, &client);
            HBRUSH brush = ::CreateSolidBrush(paint->color);
            ::FillRect(context, &client, brush);
            ::DeleteObject(brush);
            ::EndPaint(window, &state);
            return 0;
        }
        if (message == SetColor && paint) {
            paint->color = static_cast<COLORREF>(first);
            ::InvalidateRect(window, nullptr, FALSE);
            ::UpdateWindow(window);
            return 1;
        }
        if (message == Resize)
            return ::SetWindowPos(window, nullptr, 0, 0, static_cast<int>(first),
                                  static_cast<int>(second),
                                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        if (message == Hide || message == Minimize || message == Restore) {
            ::ShowWindow(window, message == Hide       ? SW_HIDE
                                 : message == Minimize ? SW_SHOWMINNOACTIVE
                                                       : SW_SHOWNOACTIVATE);
            ::UpdateWindow(window);
            return 1;
        }
        if (message == WM_CLOSE) return ::DestroyWindow(window) != FALSE;
        return ::DefWindowProcW(window, message, first, second);
    }

    void destroyOwnedWindow(HWND window) {
        DWORD process = 0;
        if (::GetWindowThreadProcessId(window, &process) == mThreadId &&
            process == ::GetCurrentProcessId())
            ::DestroyWindow(window);
    }

    void run() {
        MaiThread::setCurrentName("mai-cap-window");
        mThreadId = ::GetCurrentThreadId();
        // WS_POPUP 无边框；物理像素尺寸不依赖主线程的 DPI 虚拟化或标题栏主题。
        ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const wchar_t* className = L"MaiScreenshotSessionTestsOwnedWindow";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProcedure;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        if (!::RegisterClassExW(&windowClass)) {
            ::SetEvent(mReady);
            return;
        }
        RECT workArea{0, 0, 800, 600};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        mFirst = ::CreateWindowExW(WS_EX_NOACTIVATE, className, L"Mai owned red capture fixture",
                                   WS_POPUP, workArea.left + 20, workArea.top + 20, 320, 220,
                                   nullptr, nullptr, windowClass.hInstance, &mFirstPaint);
        mSecond = ::CreateWindowExW(WS_EX_NOACTIVATE, className, L"Mai owned blue capture fixture",
                                    WS_POPUP, workArea.left + 360, workArea.top + 20, 240, 180,
                                    nullptr, nullptr, windowClass.hInstance, &mSecondPaint);
        if (mFirst && mSecond) {
            ::ShowWindow(mFirst, SW_SHOWNOACTIVATE);
            ::ShowWindow(mSecond, SW_SHOWNOACTIVATE);
            ::UpdateWindow(mFirst);
            ::UpdateWindow(mSecond);
        }
        ::SetEvent(mReady);
        if (mFirst && mSecond) {
            MSG message{};
            while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }
        destroyOwnedWindow(mSecond);
        destroyOwnedWindow(mFirst);
        ::UnregisterClassW(className, windowClass.hInstance);
    }

    HANDLE mReady = nullptr;
    std::thread mThread;
    std::atomic<DWORD> mThreadId{0};
    HWND mFirst = nullptr;
    HWND mSecond = nullptr;
    MaiCaptureWindowPaint mFirstPaint{RGB(220, 30, 40)};
    MaiCaptureWindowPaint mSecondPaint{RGB(20, 50, 220)};
    bool mStarted = false;
};

struct MaiDecodedCapture {
    UINT width = 0;
    UINT height = 0;
    std::array<BYTE, 4> center{};
    std::uint64_t frameSequence = 0;
    std::int64_t frameReceivedAtMs = 0;
};

static bool decodeCapture(const MaiScreenshot& screenshot, MaiDecodedCapture& decoded) {
    using Microsoft::WRL::ComPtr;
    REQUIRE(!screenshot.pngBytes.empty());
    REQUIRE(screenshot.pngBytes.size() <= std::numeric_limits<DWORD>::max());
    ComPtr<IWICImagingFactory> factory;
    REQUIRE(SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(factory.GetAddressOf()))));
    ComPtr<IWICStream> stream;
    REQUIRE(SUCCEEDED(factory->CreateStream(stream.GetAddressOf())));
    REQUIRE(
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(screenshot.pngBytes.data()),
                                               static_cast<DWORD>(screenshot.pngBytes.size()))));
    ComPtr<IWICBitmapDecoder> decoder;
    REQUIRE(SUCCEEDED(factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())));
    GUID container{};
    REQUIRE(SUCCEEDED(decoder->GetContainerFormat(&container)));
    REQUIRE(::IsEqualGUID(container, GUID_ContainerFormatPng));
    ComPtr<IWICBitmapFrameDecode> frame;
    REQUIRE(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())));
    REQUIRE(SUCCEEDED(frame->GetSize(&decoded.width, &decoded.height)));
    REQUIRE(decoded.width > 0 && decoded.height > 0);
    REQUIRE(screenshot.width == static_cast<int>(decoded.width));
    REQUIRE(screenshot.height == static_cast<int>(decoded.height));
    ComPtr<IWICFormatConverter> converter;
    REQUIRE(SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())));
    REQUIRE(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom)));
    const WICRect center{static_cast<INT>(decoded.width / 2), static_cast<INT>(decoded.height / 2),
                         1, 1};
    REQUIRE(SUCCEEDED(converter->CopyPixels(&center, 4, 4, decoded.center.data())));
    decoded.frameSequence = screenshot.frameSequence;
    decoded.frameReceivedAtMs = screenshot.frameReceivedAtMs;
    return true;
}

static bool hasColorAndSize(const MaiDecodedCapture& decoded, COLORREF color, UINT width,
                            UINT height) {
    return decoded.width == width && decoded.height == height &&
           std::abs(int(decoded.center[0]) - int(GetBValue(color))) <= 8 &&
           std::abs(int(decoded.center[1]) - int(GetGValue(color))) <= 8 &&
           std::abs(int(decoded.center[2]) - int(GetRValue(color))) <= 8;
}

static MaiScreenshotRequest ownedRequest(HWND window) {
    MaiScreenshotRequest request;
    request.target = MaiScreenshotTarget::WindowById;
    request.windowId = reinterpret_cast<std::uintptr_t>(window);
    return request;
}

static bool waitUntil(const std::function<bool()>& condition, unsigned timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        if (condition()) return true;
        ::Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static bool captureMatches(HWND window, COLORREF color, UINT width, UINT height,
                           bool awaitChangedFrame = false, MaiDecodedCapture* observed = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        MaiScreenshot screenshot;
        const MaiError error = maiCaptureScreenshot(ownedRequest(window), screenshot);
        if (error) std::fprintf(stderr, "capture failed: %s\n", error.message().c_str());
        REQUIRE(!error);
        MaiDecodedCapture decoded;
        REQUIRE(decodeCapture(screenshot, decoded));
        REQUIRE(decoded.frameSequence > 0 && decoded.frameReceivedAtMs > 0);
        if (hasColorAndSize(decoded, color, width, height)) {
            if (observed) *observed = decoded;
            return true;
        }
        if (!awaitChangedFrame) break;
        ::Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "FAIL native PNG did not contain expected owner size and center color\n");
    return false;
}

static bool rejectedTargetClearsOutput(HWND window) {
    MaiScreenshot screenshot;
    screenshot.pngBytes = {1, 2, 3};
    screenshot.width = 11;
    screenshot.height = 12;
    screenshot.frameSequence = 13;
    screenshot.frameReceivedAtMs = 14;
    const MaiError error = maiCaptureScreenshot(ownedRequest(window), screenshot);
    REQUIRE(error.code() == MaiErrorCode::NotFound);
    REQUIRE(screenshot.pngBytes.empty() && screenshot.width == 0 && screenshot.height == 0);
    REQUIRE(screenshot.frameSequence == 0 && screenshot.frameReceivedAtMs == 0);
    return true;
}

static bool canceledCallsClearOutput(HWND window) {
    auto request = ownedRequest(window);
    request.isCanceled = [] { return true; };
    MaiScreenshot screenshot;
    screenshot.pngBytes = {1, 2, 3};
    screenshot.width = screenshot.height = 7;
    screenshot.frameSequence = 8;
    screenshot.frameReceivedAtMs = 9;
    REQUIRE(maiCaptureScreenshot(request, screenshot).code() == MaiErrorCode::Canceled);
    REQUIRE(screenshot.pngBytes.empty() && screenshot.width == 0 && screenshot.height == 0);
    REQUIRE(screenshot.frameSequence == 0 && screenshot.frameReceivedAtMs == 0);

    struct MaiCancellationState {
        std::atomic<bool> entered{false};
        std::atomic<bool> canceled{false};
        std::atomic<unsigned> checks{0};
    };
    auto state = std::make_shared<MaiCancellationState>();
    request.isCanceled = [state] {
        ++state->checks;
        state->entered = true;
        // 在真实调用已经进入取消检查后，由另一线程改变原子标记；静态缓存无需人为等待新帧。
        waitUntil([&] { return state->canceled.load(); }, 2000);
        return state->canceled.load();
    };
    std::thread canceler([state] {
        MaiThread::setCurrentName("mai-cap-cancel");
        waitUntil([&] { return state->entered.load(); }, 2000);
        state->canceled = true;
    });
    const MaiError canceled = maiCaptureScreenshot(request, screenshot);
    canceler.join();
    REQUIRE(state->entered.load());
    REQUIRE(canceled.code() == MaiErrorCode::Canceled);
    REQUIRE(screenshot.pngBytes.empty() && screenshot.width == 0 && screenshot.height == 0);
    REQUIRE(screenshot.frameSequence == 0 && screenshot.frameReceivedAtMs == 0);
    const unsigned checksAtReturn = state->checks.load();
    ::Sleep(30);
    REQUIRE(state->checks.load() == checksAtReturn);
    return true;
}

static bool sameResources(const MaiScreenshotCaptureState& first,
                          const MaiScreenshotCaptureState& second) {
    return second.workerRunning && second.capturing && first.workerStarts == second.workerStarts &&
           first.deviceCreations == second.deviceCreations &&
           first.sessionStarts == second.sessionStarts;
}

static bool concurrentCapturesReuseSession(HWND window) {
    struct MaiCallerResult {
        MaiError error;
        MaiScreenshot screenshot;
    };
    std::array<MaiCallerResult, 4> results;
    std::array<std::thread, 4> callers;
    HANDLE start = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(start != nullptr);
    const auto before = maiScreenshotCaptureState();
    std::atomic<unsigned> ready{0};
    for (std::size_t index = 0; index < callers.size(); ++index) {
        callers[index] = std::thread([&, index] {
            MaiThread::setCurrentName("mai-cap-caller");
            ++ready;
            if (::WaitForSingleObject(start, 5000) != WAIT_OBJECT_0) {
                results[index].error = {MaiErrorCode::Internal, "caller start deadline expired"};
                return;
            }
            results[index].error =
                maiCaptureScreenshot(ownedRequest(window), results[index].screenshot);
        });
    }
    const bool allReady = waitUntil([&] { return ready == callers.size(); });
    ::SetEvent(start);
    for (auto& caller : callers) {
        if (::WaitForSingleObject(caller.native_handle(), 8000) != WAIT_OBJECT_0) {
            std::fprintf(stderr,
                         "FAIL concurrent native capture did not finish within eight seconds\n");
            std::fflush(stderr);
            std::_Exit(1);
        }
        caller.join();
    }
    ::CloseHandle(start);
    REQUIRE(allReady);
    for (const auto& result : results) {
        REQUIRE(!result.error);
        MaiDecodedCapture decoded;
        REQUIRE(decodeCapture(result.screenshot, decoded));
        REQUIRE(decoded.frameSequence > 0 && decoded.frameReceivedAtMs > 0);
        REQUIRE(hasColorAndSize(decoded, RGB(20, 200, 60), 320, 220));
    }
    REQUIRE(sameResources(before, maiScreenshotCaptureState()));
    return true;
}

static bool exerciseLifecycle(MaiOwnedCaptureWindows& windows,
                              const MaiScreenshotCaptureState& initial) {
    const auto first = maiScreenshotCaptureState();
    REQUIRE(first.workerRunning && first.capturing);
    REQUIRE(first.workerStarts == initial.workerStarts + 1);
    REQUIRE(first.deviceCreations == initial.deviceCreations + 1);
    REQUIRE(first.sessionStarts == initial.sessionStarts + 1);
    REQUIRE(first.framesReceived > initial.framesReceived);
    // 静态画面可复用最近完整帧，不要求每次命令之后都出现新帧，避免制造静态超时。
    MaiDecodedCapture redFrame;
    for (int repeat = 0; repeat < 3; ++repeat)
        REQUIRE(captureMatches(windows.first(), RGB(220, 30, 40), 320, 220, false, &redFrame));
    REQUIRE(sameResources(first, maiScreenshotCaptureState()));

    const auto beforeRepaint = maiScreenshotCaptureState();
    REQUIRE(windows.command(windows.first(), MaiOwnedCaptureWindows::SetColor, RGB(20, 200, 60)));
    // 等待独立采集线程自行接收帧；仅在读取时重新抓一帧的实现不能满足这个断言。
    REQUIRE(waitUntil(
        [&] { return maiScreenshotCaptureState().framesReceived > beforeRepaint.framesReceived; }));
    MaiDecodedCapture greenFrame;
    REQUIRE(captureMatches(windows.first(), RGB(20, 200, 60), 320, 220, true, &greenFrame));
    REQUIRE(greenFrame.frameSequence > redFrame.frameSequence);
    REQUIRE(greenFrame.frameReceivedAtMs >= redFrame.frameReceivedAtMs);
    REQUIRE(sameResources(first, maiScreenshotCaptureState()));
    REQUIRE(concurrentCapturesReuseSession(windows.first()));
    REQUIRE(canceledCallsClearOutput(windows.first()));

    auto beforeSwitch = maiScreenshotCaptureState();
    REQUIRE(captureMatches(windows.second(), RGB(20, 50, 220), 240, 180));
    auto afterSwitch = maiScreenshotCaptureState();
    REQUIRE(afterSwitch.workerStarts == beforeSwitch.workerStarts);
    REQUIRE(afterSwitch.deviceCreations == beforeSwitch.deviceCreations);
    REQUIRE(afterSwitch.sessionStarts == beforeSwitch.sessionStarts + 1);
    REQUIRE(captureMatches(windows.first(), RGB(20, 200, 60), 320, 220));
    const auto returned = maiScreenshotCaptureState();
    REQUIRE(returned.workerStarts == first.workerStarts);
    REQUIRE(returned.deviceCreations == first.deviceCreations);
    REQUIRE(returned.sessionStarts == afterSwitch.sessionStarts + 1);

    REQUIRE(windows.command(windows.first(), MaiOwnedCaptureWindows::Resize, 360, 260));
    REQUIRE(captureMatches(windows.first(), RGB(20, 200, 60), 360, 260, true));
    REQUIRE(maiScreenshotCaptureState().deviceCreations == first.deviceCreations);
    REQUIRE(windows.command(windows.first(), MaiOwnedCaptureWindows::Minimize));
    REQUIRE(::IsIconic(windows.first()));
    REQUIRE(rejectedTargetClearsOutput(windows.first()));
    REQUIRE(windows.command(windows.first(), MaiOwnedCaptureWindows::Restore));
    REQUIRE(captureMatches(windows.first(), RGB(20, 200, 60), 360, 260, true));
    REQUIRE(windows.command(windows.first(), MaiOwnedCaptureWindows::Hide));
    REQUIRE(!::IsWindowVisible(windows.first()));
    REQUIRE(rejectedTargetClearsOutput(windows.first()));
    REQUIRE(windows.command(windows.first(), MaiOwnedCaptureWindows::Restore));
    REQUIRE(captureMatches(windows.first(), RGB(20, 200, 60), 360, 260, true));
    REQUIRE(windows.command(windows.first(), WM_CLOSE));
    REQUIRE(!::IsWindow(windows.first()));
    REQUIRE(rejectedTargetClearsOutput(windows.first()));

    REQUIRE(!maiStopScreenshotCapture());
    const auto stopped = maiScreenshotCaptureState();
    REQUIRE(!stopped.workerRunning && !stopped.capturing);
    REQUIRE(!maiStopScreenshotCapture());
    const auto stoppedAgain = maiScreenshotCaptureState();
    REQUIRE(!stoppedAgain.workerRunning && !stoppedAgain.capturing);
    REQUIRE(stoppedAgain.workerStarts == stopped.workerStarts);
    REQUIRE(stoppedAgain.deviceCreations == stopped.deviceCreations);
    REQUIRE(stoppedAgain.sessionStarts == stopped.sessionStarts);
    REQUIRE(stoppedAgain.framesReceived == stopped.framesReceived);
    REQUIRE(captureMatches(windows.second(), RGB(20, 50, 220), 240, 180));
    const auto restarted = maiScreenshotCaptureState();
    REQUIRE(restarted.workerRunning && restarted.capturing);
    REQUIRE(restarted.workerStarts == stopped.workerStarts + 1);
    REQUIRE(restarted.sessionStarts == stopped.sessionStarts + 1);
    REQUIRE(restarted.framesReceived > stopped.framesReceived);
    REQUIRE(!maiStopScreenshotCapture());
    REQUIRE(!maiScreenshotCaptureState().workerRunning && !maiScreenshotCaptureState().capturing);
    return true;
}

static bool userObjectName(HANDLE object, std::wstring& name) {
    DWORD bytes = 0;
    ::GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
    if (!bytes || bytes > 32768 * sizeof(wchar_t)) return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!::GetUserObjectInformationW(object, UOI_NAME, buffer.data(), bytes, &bytes)) return false;
    name = buffer.data();
    return !name.empty();
}

static bool nonInputDesktopChild(const std::wstring& nonce) {
    REQUIRE(nonce.find(L"MaiCapturePrivate-") == 0 && nonce.size() < 100);
    std::wstring desktopName;
    REQUIRE(userObjectName(::GetThreadDesktop(::GetCurrentThreadId()), desktopName));
    REQUIRE(desktopName == nonce);
    HDESK input = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    REQUIRE(input != nullptr);
    std::wstring inputName;
    const bool namedInput = userObjectName(input, inputName);
    const bool closedInput = ::CloseDesktop(input) != FALSE;
    REQUIRE(namedInput && closedInput && inputName != nonce);
    REQUIRE(!maiStopScreenshotCapture());
    const auto before = maiScreenshotCaptureState();
    MaiOwnedCaptureWindows windows;
    REQUIRE(windows.isStarted());
    // nonce 事件只确认本次自有 child 已在指定私有桌面创建窗口，不改变任何输入桌面。
    const std::wstring readyName = L"Local\\" + nonce + L"-ready";
    HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    REQUIRE(ready != nullptr);
    const bool signaled = ::SetEvent(ready) != FALSE;
    const bool closedReady = ::CloseHandle(ready) != FALSE;
    REQUIRE(signaled && closedReady);
    MaiScreenshot screenshot;
    screenshot.pngBytes = {1, 2, 3};
    screenshot.width = screenshot.height = 7;
    screenshot.frameSequence = 8;
    screenshot.frameReceivedAtMs = 9;
    const MaiError rejected = maiCaptureScreenshot(ownedRequest(windows.first()), screenshot);
    std::fprintf(stderr, "private desktop capture result: code=%d message=%s\n",
                 static_cast<int>(rejected.code()), rejected.message().c_str());
    REQUIRE(rejected.code() == MaiErrorCode::NotConfigured);
    REQUIRE(rejected.message().find("interactive desktop") != std::string::npos);
    REQUIRE(screenshot.pngBytes.empty() && screenshot.width == 0 && screenshot.height == 0);
    REQUIRE(screenshot.frameSequence == 0 && screenshot.frameReceivedAtMs == 0);
    const auto rejectedState = maiScreenshotCaptureState();
    REQUIRE(rejectedState.deviceCreations == before.deviceCreations);
    REQUIRE(rejectedState.sessionStarts == before.sessionStarts);
    REQUIRE(!rejectedState.capturing);
    REQUIRE(!maiStopScreenshotCapture());
    REQUIRE(!maiScreenshotCaptureState().workerRunning && !maiScreenshotCaptureState().capturing);
    return true;
}

struct MaiPrivateDesktopProcess {
    HDESK desktop = nullptr;
    HANDLE ready = nullptr;
    PROCESS_INFORMATION child{};
    std::wstring executable;
    std::wstring nonce;
    std::wstring launchedArguments;

    bool isOwnedChild() const {
        if (!child.hProcess || ::GetProcessId(child.hProcess) != child.dwProcessId ||
            nonce.find(L"MaiCapturePrivate-") != 0 ||
            launchedArguments != L"\"" + executable + L"\" --non-input-desktop-probe " + nonce)
            return false;
        std::vector<wchar_t> image(32768, L'\0');
        DWORD length = static_cast<DWORD>(image.size());
        return ::QueryFullProcessImageNameW(child.hProcess, 0, image.data(), &length) &&
               ::CompareStringOrdinal(image.data(), static_cast<int>(length), executable.c_str(),
                                      static_cast<int>(executable.size()), TRUE) == CSTR_EQUAL;
    }

    ~MaiPrivateDesktopProcess() {
        if (child.hProcess && ::WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
            // 仅终止本次 CreateProcess 返回的对象；先核对 exe、PID 以及实际传入的 nonce 参数。
            // ready 事件尚未发出时仍可清理启动失败的自有 child，不按进程名或枚举 PID 终止程序。
            if (!isOwnedChild() || !::TerminateProcess(child.hProcess, 1) ||
                ::WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) {
                std::fprintf(stderr, "FAIL owned private desktop child cleanup failed\n");
                std::fflush(stderr);
                std::_Exit(1);
            }
        }
        if (child.hThread) ::CloseHandle(child.hThread);
        if (child.hProcess) ::CloseHandle(child.hProcess);
        if (ready) ::CloseHandle(ready);
        if (desktop) ::CloseDesktop(desktop);
    }
};

static bool privateDesktopRejectsCapture(bool& skipped) {
    MaiPrivateDesktopProcess owned;
    std::wstring originalThreadDesktop;
    REQUIRE(userObjectName(::GetThreadDesktop(::GetCurrentThreadId()), originalThreadDesktop));
    GUID identifier{};
    REQUIRE(SUCCEEDED(::CoCreateGuid(&identifier)));
    wchar_t identifierText[40]{};
    REQUIRE(::StringFromGUID2(identifier, identifierText, 40) > 0);
    owned.nonce = L"MaiCapturePrivate-" + std::wstring(identifierText);
    // CreateDesktop 会打开同名现存对象；先查不存在，再以不可预测的 GUID 名创建并核对句柄名称。
    HDESK existing = ::OpenDesktopW(owned.nonce.c_str(), 0, FALSE, DESKTOP_READOBJECTS);
    if (existing) {
        ::CloseDesktop(existing);
        REQUIRE(false);
    }
    owned.desktop = ::CreateDesktopW(
        owned.nonce.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE,
        nullptr);
    if (!owned.desktop) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
            std::printf("SKIP private desktop probe: CreateDesktop permission unavailable (%lu)\n",
                        error);
            skipped = true;
            return true;
        }
        std::fprintf(stderr, "FAIL CreateDesktop for owned probe: %lu\n", error);
        return false;
    }
    std::wstring desktopName, stationName;
    REQUIRE(userObjectName(owned.desktop, desktopName) && desktopName == owned.nonce);
    std::wstring currentThreadDesktop;
    REQUIRE(userObjectName(::GetThreadDesktop(::GetCurrentThreadId()), currentThreadDesktop));
    REQUIRE(currentThreadDesktop == originalThreadDesktop);
    REQUIRE(userObjectName(::GetProcessWindowStation(), stationName));
    std::vector<wchar_t> executable(32768, L'\0');
    const DWORD length =
        ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0 && length < executable.size());
    owned.executable.assign(executable.data(), length);
    const std::wstring readyName = L"Local\\" + owned.nonce + L"-ready";
    ::SetLastError(ERROR_SUCCESS);
    owned.ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    REQUIRE(owned.ready != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS);
    owned.launchedArguments =
        L"\"" + owned.executable + L"\" --non-input-desktop-probe " + owned.nonce;
    std::vector<wchar_t> arguments(owned.launchedArguments.begin(), owned.launchedArguments.end());
    arguments.push_back(L'\0');
    std::wstring childDesktop = stationName + L"\\" + owned.nonce;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = childDesktop.data();
    REQUIRE(::CreateProcessW(owned.executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED, nullptr, nullptr, &startup, &owned.child));
    REQUIRE(owned.isOwnedChild());
    REQUIRE(::ResumeThread(owned.child.hThread) != static_cast<DWORD>(-1));
    const DWORD waited = ::WaitForSingleObject(owned.child.hProcess, 15000);
    REQUIRE(waited == WAIT_OBJECT_0);
    REQUIRE(::WaitForSingleObject(owned.ready, 0) == WAIT_OBJECT_0);
    DWORD exitCode = 1;
    REQUIRE(::GetExitCodeProcess(owned.child.hProcess, &exitCode));
    if (exitCode != 0)
        std::fprintf(stderr, "FAIL owned private desktop child exit: %lu\n", exitCode);
    REQUIRE(exitCode == 0);
    REQUIRE(userObjectName(owned.desktop, desktopName) && desktopName == owned.nonce);
    REQUIRE(::CloseDesktop(owned.desktop));
    owned.desktop = nullptr;
    std::printf("private non-input desktop capture guard test passed\n");
    return true;
}

static bool decodeDisplayDimensions(const MaiScreenshot& screenshot) {
    using Microsoft::WRL::ComPtr;
    REQUIRE(!screenshot.pngBytes.empty());
    REQUIRE(screenshot.pngBytes.size() <= std::numeric_limits<DWORD>::max());
    ComPtr<IWICImagingFactory> factory;
    REQUIRE(SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(factory.GetAddressOf()))));
    ComPtr<IWICStream> stream;
    REQUIRE(SUCCEEDED(factory->CreateStream(stream.GetAddressOf())));
    REQUIRE(
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(screenshot.pngBytes.data()),
                                               static_cast<DWORD>(screenshot.pngBytes.size()))));
    ComPtr<IWICBitmapDecoder> decoder;
    REQUIRE(SUCCEEDED(factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())));
    GUID container{};
    REQUIRE(SUCCEEDED(decoder->GetContainerFormat(&container)));
    REQUIRE(::IsEqualGUID(container, GUID_ContainerFormatPng));
    ComPtr<IWICBitmapFrameDecode> frame;
    REQUIRE(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())));
    UINT width = 0, height = 0;
    REQUIRE(SUCCEEDED(frame->GetSize(&width, &height)));
    REQUIRE(width > 0 && height > 0);
    REQUIRE(screenshot.width == static_cast<int>(width));
    REQUIRE(screenshot.height == static_cast<int>(height));
    // 此独立入口只读解码尺寸，不复制任何像素、采样颜色或生成可离开进程的图像证据。
    return true;
}

static bool pointerMonitor(HMONITOR& monitor) {
    POINT pointer{};
    REQUIRE(::GetPhysicalCursorPos(&pointer));
    monitor = ::MonitorFromPoint(pointer, MONITOR_DEFAULTTONULL);
    REQUIRE(monitor != nullptr);
    return true;
}

static void printDisplayMetadata(unsigned index, const MaiScreenshot& screenshot,
                                 const MaiScreenshotCaptureState& state) {
    std::printf(
        "display metadata capture %u: width=%d height=%d frameSequence=%llu "
        "frameReceivedAtMs=%lld workerRunning=%d capturing=%d workerStarts=%llu "
        "deviceCreations=%llu sessionStarts=%llu framesReceived=%llu\n",
        index, screenshot.width, screenshot.height,
        static_cast<unsigned long long>(screenshot.frameSequence),
        static_cast<long long>(screenshot.frameReceivedAtMs), state.workerRunning, state.capturing,
        static_cast<unsigned long long>(state.workerStarts),
        static_cast<unsigned long long>(state.deviceCreations),
        static_cast<unsigned long long>(state.sessionStarts),
        static_cast<unsigned long long>(state.framesReceived));
}

static bool captureDisplayMetadata(bool& sourceChanged) {
    MaiScreenshotRequest request;
    request.target = MaiScreenshotTarget::DisplayUnderPointer;
    std::array<HMONITOR, 4> monitors{};
    MaiScreenshotCaptureState firstState;
    MaiScreenshotCaptureState secondState;
    for (unsigned index = 0; index < 2; ++index) {
        REQUIRE(pointerMonitor(monitors[index * 2]));
        MaiScreenshot screenshot;
        const MaiError captured = maiCaptureScreenshot(request, screenshot);
        const bool monitorObserved = pointerMonitor(monitors[index * 2 + 1]);
        if (captured)
            std::fprintf(stderr, "FAIL actual display capture: %s\n", captured.message().c_str());
        REQUIRE(!captured);
        REQUIRE(monitorObserved);
        REQUIRE(decodeDisplayDimensions(screenshot));
        REQUIRE(screenshot.frameSequence > 0 && screenshot.frameReceivedAtMs > 0);
        const auto state = maiScreenshotCaptureState();
        printDisplayMetadata(index + 1, screenshot, state);
        REQUIRE(state.workerRunning && state.capturing);
        if (index == 0)
            firstState = state;
        else
            secondState = state;
    }
    for (HMONITOR monitor : monitors) sourceChanged |= monitor != monitors.front();
    if (sourceChanged) {
        std::printf(
            "source changed: pointer monitor differed during verification; "
            "same-monitor resource reuse was not verified\n");
    } else {
        REQUIRE(sameResources(firstState, secondState));
    }
    return true;
}

static bool displayMetadataOnly(bool& sourceChanged) {
    std::printf(
        "metadata-only actual display verification; PNG stays in process memory; "
        "no image delivery\n");
    REQUIRE(!maiStopScreenshotCapture());
    const auto initial = maiScreenshotCaptureState();
    REQUIRE(!initial.workerRunning && !initial.capturing);
    const bool verified = captureDisplayMetadata(sourceChanged);
    // 成功和失败都显式停止；静态帧可重复读取，不为取得新序号等待下一帧。
    const MaiError stopped = maiStopScreenshotCapture();
    const auto finalState = maiScreenshotCaptureState();
    REQUIRE(!stopped);
    REQUIRE(!finalState.workerRunning && !finalState.capturing);
    REQUIRE(verified);
    if (!sourceChanged)
        std::printf(
            "actual display metadata and same-monitor resource reuse verified; "
            "capture stopped and inactive; no image delivery\n");
    return true;
}

int main(int argc, char** argv) {
    MaiThread::setCurrentName("mai-cap-test");
    MaiCaptureTestDeadline deadline;
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        std::fprintf(stderr, "FAIL independent WIC decoder COM initialization failed\n");
        return 1;
    }
    struct MaiCaptureTestCleanup {
        ~MaiCaptureTestCleanup() {
            maiStopScreenshotCapture();
            ::CoUninitialize();
        }
    } cleanup;
    if (argc == 3 && std::string(argv[1]) == "--non-input-desktop-probe") {
        const std::string nonce(argv[2]);
        return nonInputDesktopChild(std::wstring(nonce.begin(), nonce.end())) ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--display-metadata-only") {
        if (!maiIsScreenshotSupported()) {
            std::printf("SKIP actual display metadata verification: native backend unavailable\n");
            return 77;
        }
        bool sourceChanged = false;
        if (!displayMetadataOnly(sourceChanged)) return 1;
        return sourceChanged ? 77 : 0;
    }
    if (argc != 1) {
        std::fprintf(stderr,
                     "usage: MaiScreenshotSessionTests "
                     "[--non-input-desktop-probe nonce|--display-metadata-only]\n");
        return 2;
    }
    if (!maiIsScreenshotSupported()) {
        std::printf("SKIP native screenshot backend is unavailable\n");
        return 77;
    }
    if (maiStopScreenshotCapture()) return 1;
    const auto initial = maiScreenshotCaptureState();
    if (initial.workerRunning || initial.capturing) {
        std::fprintf(stderr, "FAIL stop did not leave an inactive backend\n");
        return 1;
    }
    MaiOwnedCaptureWindows windows;
    if (!windows.isStarted()) {
        std::fprintf(stderr, "FAIL owned nonactivating GUI fixture was not ready\n");
        return 1;
    }
    MaiScreenshot screenshot;
    const MaiError captured = maiCaptureScreenshot(ownedRequest(windows.first()), screenshot);
    if (captured) {
        if (captured.code() == MaiErrorCode::NotSupported &&
            captured.message() == "Windows Graphics Capture is not supported on this system") {
            std::printf("SKIP %s\n", captured.message().c_str());
            return 77;
        }
        std::fprintf(stderr, "FAIL initial owned capture: %s\n", captured.message().c_str());
        return 1;
    }
    MaiDecodedCapture firstImage;
    if (!decodeCapture(screenshot, firstImage) ||
        !hasColorAndSize(firstImage, RGB(220, 30, 40), 320, 220)) {
        std::fprintf(stderr, "FAIL first PNG did not decode to the owned red window\n");
        return 1;
    }
    if (!exerciseLifecycle(windows, initial)) return 1;
    std::printf("screenshot session lifecycle tests passed\n");
    bool privateProbeSkipped = false;
    if (!privateDesktopRejectsCapture(privateProbeSkipped)) return 1;
    if (privateProbeSkipped) {
        std::printf("private desktop guard remains unverified; complete suite is skipped\n");
        return 77;
    }
    return 0;
}

#else

int main() {
    std::printf("SKIP native screenshot session lifecycle test requires Windows\n");
    return 77;
}

#endif

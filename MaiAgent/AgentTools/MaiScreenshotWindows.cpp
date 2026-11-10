// 文件作用：由常驻工作线程持有 Windows 原生采集会话，按命令生成当前完整画面的 PNG。
// 直接依赖：MaiScreenshot.h、MaiThread.h；WGC 回调只唤醒线程，D3D 和 COM 的使用及释放均留在线程内。
#include "MaiScreenshot.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <objidl.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <wtsapi32.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiThread.h"
#include "MaiTime.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kMaximumWidth = 2560;
constexpr UINT kMaximumHeight = 1600;
constexpr DWORD kCaptureTimeoutMs = 2000;
constexpr DWORD kCaptureSettleMs = 100;
constexpr DWORD kCancelPollMs = 20;
constexpr DWORD kEnvironmentCheckMs = 250;
constexpr std::size_t kMaximumPendingCaptures = 64;

// 帧序号和接收时间是独立屏障：调用方同时提供两者时必须同时严格递增。
// 不能用本次编码时间替代原始帧时间，也不能把相等的缓存帧作为操作后的新画面。
bool framePassesBarrier(const MaiScreenshotRequest& request, std::uint64_t sequence,
                        std::int64_t receivedAtMs, std::int64_t sourceProducedAtMs) {
    return (!request.afterFrameSequence || sequence > request.afterFrameSequence) &&
           (!request.afterFrameReceivedAtMs ||
            (receivedAtMs > request.afterFrameReceivedAtMs && sourceProducedAtMs > 0 &&
             sourceProducedAtMs > request.afterFrameReceivedAtMs));
}

// QPC 频率不是固定的 10 MHz。先分离整秒再缩放余数，并向上取整后采样，
// 换算源帧时间时得到保守下界；异常频率或无法安全表示的值不给出时间证明。
bool qpcIn100nsCeil(std::int64_t counter, std::int64_t frequency, std::int64_t& value) {
    constexpr std::int64_t ticksPerSecond = 10000000;
    if (counter < 0 || frequency <= 0) return false;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto seconds = counter / frequency;
    const auto remainder = counter % frequency;
    if (seconds > maximum / ticksPerSecond || remainder > maximum / ticksPerSecond) return false;
    const auto scaledRemainder = remainder * ticksPerSecond;
    const auto fraction = scaledRemainder / frequency + (scaledRemainder % frequency != 0);
    const auto whole = seconds * ticksPerSecond;
    if (whole > maximum - fraction) return false;
    value = whole + fraction;
    return true;
}

std::int64_t sourceProducedUnixMs(std::int64_t sourceQpc100ns, std::int64_t unix100ns,
                                  std::int64_t qpcAfter, std::int64_t frequency) {
    std::int64_t sampledQpc100ns = 0;
    if (sourceQpc100ns <= 0 || unix100ns <= 0 ||
        !qpcIn100nsCeil(qpcAfter, frequency, sampledQpc100ns) || sourceQpc100ns > sampledQpc100ns)
        return 0;
    const auto age100ns = sampledQpc100ns - sourceQpc100ns;
    if (age100ns >= unix100ns) return 0;
    return (unix100ns - age100ns) / 10000;
}

// WGC 的 SystemRelativeTime 是 compositor 产出帧时的 QPC TimeSpan（100ns），
// 不是 TryGetNextFrame/drain 的时间。以 QPC 夹取精确 UTC，再减去后采样得到的帧龄，
// 防止输入前已排队、输入后才取出的旧像素被标记为操作后的观察。
// 官方契约：learn.microsoft.com/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime
// TimeSpan 单位：learn.microsoft.com/uwp/api/windows.foundation.timespan.duration
std::int64_t frameSourceProducedAtMs(std::int64_t sourceQpc100ns) {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER before{};
    LARGE_INTEGER after{};
    if (!::QueryPerformanceFrequency(&frequency) || !::QueryPerformanceCounter(&before)) return 0;
    FILETIME utc{};
    ::GetSystemTimePreciseAsFileTime(&utc);
    if (!::QueryPerformanceCounter(&after) || before.QuadPart < 0 ||
        after.QuadPart < before.QuadPart)
        return 0;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = utc.dwLowDateTime;
    ticks.HighPart = utc.dwHighDateTime;
    constexpr std::uint64_t unixEpoch100ns = 116444736000000000ULL;
    if (ticks.QuadPart <= unixEpoch100ns) return 0;
    const auto unix100ns = ticks.QuadPart - unixEpoch100ns;
    if (unix100ns > std::uint64_t(std::numeric_limits<std::int64_t>::max())) return 0;
    return sourceProducedUnixMs(sourceQpc100ns, std::int64_t(unix100ns), after.QuadPart,
                                frequency.QuadPart);
}

MaiError hresultError(const char* operation, HRESULT result) {
    char code[16] = {};
    std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
    return MaiError::make(MaiErrorCode::Internal,
                          std::string(operation) + " failed (0x" + code + ")");
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), length);
    return wide;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                          length, nullptr, nullptr);
    return utf8;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return text;
}

struct WindowSearch {
    std::wstring expected;
    std::vector<HWND> exact;
    std::vector<HWND> partial;
};

bool visibleWindowTitle(HWND window, std::wstring& title) {
    if (!::IsWindowVisible(window) || ::IsIconic(window)) return false;
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0) return false;
    title.assign(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) return false;
    title.resize(static_cast<std::size_t>(copied));
    return true;
}

BOOL CALLBACK collectMatchingWindows(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    std::wstring title;
    if (!visibleWindowTitle(window, title)) return TRUE;
    const std::wstring folded = lower(title);
    if (folded == search.expected) {
        search.exact.push_back(window);
    } else if (folded.find(search.expected) != std::wstring::npos) {
        search.partial.push_back(window);
    }
    return TRUE;
}

BOOL CALLBACK collectVisibleWindowTitles(HWND window, LPARAM parameter) {
    auto& titles = *reinterpret_cast<std::vector<std::string>*>(parameter);
    std::wstring title;
    if (visibleWindowTitle(window, title)) {
        std::string utf8 = narrow(title);
        if (!utf8.empty()) titles.push_back(std::move(utf8));
    }
    return TRUE;
}

MaiError findWindow(const std::string& titleUtf8, HWND& window) {
    const std::wstring title = widen(titleUtf8);
    if (title.empty()) {
        return MaiError::make(MaiErrorCode::InvalidInput,
                              "windowTitle is empty or is not valid UTF-8");
    }
    WindowSearch search;
    search.expected = lower(title);
    ::EnumWindows(collectMatchingWindows, reinterpret_cast<LPARAM>(&search));
    const std::vector<HWND>& matches = search.exact.empty() ? search.partial : search.exact;
    if (matches.empty()) {
        return MaiError::make(MaiErrorCode::NotFound,
                              "no visible top-level window matches windowTitle");
    }
    if (matches.size() != 1) {
        return MaiError::make(
            MaiErrorCode::InvalidInput,
            "windowTitle matches more than one visible window; use a more exact title");
    }
    window = matches.front();
    return {};
}

MaiError createDefaultDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
    const HRESULT result = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.GetAddressOf(), &selected,
        context.GetAddressOf());
    if (FAILED(result)) return hresultError("D3D11CreateDevice", result);
    return {};
}

MaiError copyTextureToMemory(ID3D11Device* device, ID3D11DeviceContext* context,
                             ID3D11Texture2D* texture, ComPtr<ID3D11Texture2D>& staging,
                             std::vector<std::uint8_t>& pixels, UINT& width, UINT& height) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return MaiError::make(MaiErrorCode::NotSupported,
                              "DXGI returned unsupported desktop texture format " +
                                  std::to_string(static_cast<int>(description.Format)));
    }

    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    D3D11_TEXTURE2D_DESC existing{};
    if (staging) staging->GetDesc(&existing);
    HRESULT result = S_OK;
    if (!staging || existing.Width != description.Width || existing.Height != description.Height ||
        existing.Format != description.Format) {
        staging.Reset();
        result = device->CreateTexture2D(&stagingDescription, nullptr, staging.GetAddressOf());
        if (FAILED(result)) return hresultError("ID3D11Device::CreateTexture2D", result);
    }

    width = description.Width;
    height = description.Height;
    const std::size_t destinationRowBytes = static_cast<std::size_t>(width) * 4;
    // 分配放在 Map 前；映射成功后的路径只复制字节，异常也不会把 staging 留在映射状态。
    pixels.resize(destinationRowBytes * height);
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) return hresultError("ID3D11DeviceContext::Map", result);

    for (UINT row = 0; row < height; ++row) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch * row;
        auto* destination = pixels.data() + destinationRowBytes * row;
        std::memcpy(destination, source, destinationRowBytes);
        for (UINT column = 0; column < width; ++column) destination[column * 4 + 3] = 0xFF;
    }
    context->Unmap(staging.Get(), 0);
    return {};
}

// 采集总在独立线程建立 MTA，不借用 Qt/OLE 的 STA；退出前先释放全部原生对象，
// 再清理 WinRT 工厂缓存和 apartment，避免重启采集时命中已失效的 COM 对象。
class MaiWinRtApartment {
public:
    MaiWinRtApartment() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }

    ~MaiWinRtApartment() {
        winrt::clear_factory_cache();
        winrt::uninit_apartment();
    }

    MaiWinRtApartment(const MaiWinRtApartment&) = delete;
    MaiWinRtApartment& operator=(const MaiWinRtApartment&) = delete;
};

// 通过 Windows Imaging Component 将捕获像素编码为 PNG，并填写真实图像尺寸。
// COM、像素布局或编码失败时返回错误，不输出不完整的截图字节。
MaiError encodePng(IWICImagingFactory* factory, const std::vector<std::uint8_t>& pixels,
                   UINT sourceWidth, UINT sourceHeight, MaiScreenshot& screenshot) {
    if (pixels.size() > static_cast<std::size_t>(UINT_MAX)) {
        return MaiError::make(MaiErrorCode::Internal, "captured screen is too large to encode");
    }
    ComPtr<IWICBitmap> bitmap;
    HRESULT result = factory->CreateBitmapFromMemory(
        sourceWidth, sourceHeight, GUID_WICPixelFormat32bppBGRA, sourceWidth * 4,
        static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()), bitmap.GetAddressOf());
    if (FAILED(result)) return hresultError("IWICImagingFactory::CreateBitmapFromMemory", result);

    ComPtr<IWICBitmapSource> source;
    result = bitmap.As(&source);
    if (FAILED(result)) return hresultError("IWICBitmap::QueryInterface", result);

    UINT width = 0;
    UINT height = 0;
    result = source->GetSize(&width, &height);
    if (FAILED(result)) return hresultError("IWICBitmapSource::GetSize", result);
    if (width > kMaximumWidth || height > kMaximumHeight) {
        const double scale = std::min(static_cast<double>(kMaximumWidth) / width,
                                      static_cast<double>(kMaximumHeight) / height);
        const UINT scaledWidth = std::max(1u, static_cast<UINT>(width * scale));
        const UINT scaledHeight = std::max(1u, static_cast<UINT>(height * scale));
        ComPtr<IWICBitmapScaler> scaler;
        result = factory->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(result)) return hresultError("IWICImagingFactory::CreateBitmapScaler", result);
        result = scaler->Initialize(source.Get(), scaledWidth, scaledHeight,
                                    WICBitmapInterpolationModeFant);
        if (FAILED(result)) return hresultError("IWICBitmapScaler::Initialize", result);
        result = scaler.As(&source);
        if (FAILED(result)) return hresultError("IWICBitmapScaler::QueryInterface", result);
        width = scaledWidth;
        height = scaledHeight;
    }

    ComPtr<IStream> stream;
    result = ::CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(result)) return hresultError("CreateStreamOnHGlobal", result);
    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(result)) return hresultError("IWICImagingFactory::CreateEncoder", result);
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) return hresultError("IWICBitmapEncoder::Initialize", result);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (FAILED(result)) return hresultError("IWICBitmapEncoder::CreateNewFrame", result);
    result = frame->Initialize(properties.Get());
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::Initialize", result);
    result = frame->SetSize(width, height);
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::SetSize", result);
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    result = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::SetPixelFormat", result);
    result = frame->WriteSource(source.Get(), nullptr);
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::WriteSource", result);
    result = frame->Commit();
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::Commit", result);
    result = encoder->Commit();
    if (FAILED(result)) return hresultError("IWICBitmapEncoder::Commit", result);

    HGLOBAL storage = nullptr;
    result = ::GetHGlobalFromStream(stream.Get(), &storage);
    if (FAILED(result)) return hresultError("GetHGlobalFromStream", result);
    const SIZE_T size = ::GlobalSize(storage);
    const void* data = ::GlobalLock(storage);
    if (data == nullptr || size == 0) {
        if (data != nullptr) ::GlobalUnlock(storage);
        return MaiError::make(MaiErrorCode::Internal, "WIC produced an empty PNG");
    }
    const auto* first = static_cast<const std::uint8_t*>(data);
    screenshot.pngBytes.assign(first, first + size);
    ::GlobalUnlock(storage);
    screenshot.width = static_cast<int>(width);
    screenshot.height = static_cast<int>(height);
    return {};
}

bool isCaptureWindow(HWND window) {
    return ::IsWindow(window) && ::IsWindowVisible(window) && !::IsIconic(window) &&
           ::GetAncestor(window, GA_ROOT) == window;
}

// 只接受本次枚举仍存在的 HMONITOR，不能把失效标识重新解释为主屏或鼠标所在屏。
bool isCaptureMonitor(HMONITOR monitor) {
    if (!monitor) return false;
    struct Search {
        HMONITOR selected = nullptr;
        bool found = false;
    } search;
    search.selected = monitor;
    ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR candidate, HDC, LPRECT, LPARAM parameter) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(parameter);
            if (candidate != search.selected) return TRUE;
            MONITORINFO information{};
            information.cbSize = sizeof(information);
            search.found = ::GetMonitorInfoW(candidate, &information) &&
                           information.rcMonitor.right > information.rcMonitor.left &&
                           information.rcMonitor.bottom > information.rcMonitor.top;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

MaiError closedTargetError() {
    return MaiError::make(MaiErrorCode::NotFound,
                          "capture target is closed, hidden, minimized, or disconnected");
}

MaiError canceledCaptureError() {
    return MaiError::make(MaiErrorCode::Canceled, "screen capture was canceled");
}

// 静态帧只能在原交互环境仍有效时复用。WGC 的 Closed 只承诺目标关闭，不能用未收到事件
// 推断会话仍已解锁或仍是输入桌面；这里只读查询这些状态，不切换桌面，也不声称验证实时采集许可。
MaiError verifyCaptureEnvironment() {
    DWORD sessionId = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId)) {
        return MaiError::make(MaiErrorCode::NotConfigured,
                              "cannot verify the interactive desktop session for screen capture");
    }
    LPWSTR raw = nullptr;
    DWORD bytes = 0;
    const BOOL queried = ::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                                       WTSSessionInfoEx, &raw, &bytes);
    bool activeUnlocked = false;
    if (queried && raw && bytes >= sizeof(WTSINFOEXW)) {
        const auto* information = reinterpret_cast<const WTSINFOEXW*>(raw);
        if (information->Level == 1) {
            const auto& session = information->Data.WTSInfoExLevel1;
            activeUnlocked = session.SessionId == sessionId && session.SessionState == WTSActive &&
                             session.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
        }
    }
    // 缓冲还包含用户和域名。只读上述状态字段后立即释放，不把结构或身份字段写入日志。
    if (raw) ::WTSFreeMemory(raw);
    if (!activeUnlocked) {
        return MaiError::make(
            MaiErrorCode::NotConfigured,
            "screen capture requires an active unlocked interactive desktop session");
    }
    const HDESK input = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    const HDESK current = ::GetThreadDesktop(::GetCurrentThreadId());
    wchar_t inputName[256] = {};
    wchar_t currentName[256] = {};
    DWORD needed = 0;
    const bool matches =
        input && current &&
        ::GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) &&
        ::GetUserObjectInformationW(current, UOI_NAME, currentName, sizeof(currentName), &needed) &&
        ::lstrcmpiW(inputName, currentName) == 0;
    if (input) ::CloseDesktop(input);
    if (!matches) {
        return MaiError::make(MaiErrorCode::NotConfigured,
                              "screen capture requires the current interactive desktop");
    }
    return {};
}

// 回调可能在撤销注册后才退栈，因此事件句柄必须由回调和 worker 共同持有，不能引用栈变量。
// WGC 的内部线程只设置事件和关闭标记，不触碰 D3D immediate context 或调用方的取消函数。
struct MaiScreenshotWakeState {
    winrt::handle event{::CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<bool> stopping{false};
};

struct MaiScreenshotTargetState {
    std::atomic<bool> closed{false};
};

struct MaiScreenshotCommand {
    MaiScreenshotRequest request;
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    MaiError error;
    MaiScreenshot screenshot;

    void complete(MaiError completionError, MaiScreenshot completionScreenshot = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = std::move(completionError);
            screenshot = error.hasError() ? MaiScreenshot{} : std::move(completionScreenshot);
            done = true;
        }
        finished.notify_one();
    }
};

// 此队列只承载截图命令。生命周期参考 Chromium base::Thread / SequenceBound：
// 创建、调用、清理原生对象都在同一线程；stop 先唤醒等待，再清理并 join，不能 detach 后继续引用宿主。
class MaiScreenshotWindowsWorker {
public:
    ~MaiScreenshotWindowsWorker() {
        stop();
    }

    MaiScreenshotCaptureState state() {
        std::lock_guard<std::mutex> lock(mMutex);
        return mState;
    }

    MaiError capture(const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
        auto command = std::make_shared<MaiScreenshotCommand>();
        command->request = request;
        if (mStopsInProgress.load() != 0) return canceledCaptureError();
        {
            std::lock_guard<std::mutex> lifecycle(mLifecycleMutex);
            if (mStopsInProgress.load() != 0) return canceledCaptureError();
            const MaiError startError = start();
            if (startError.hasError()) return startError;
            std::lock_guard<std::mutex> lock(mMutex);
            if (mStopsInProgress.load() != 0) return canceledCaptureError();
            if (!mState.workerRunning) return mWorkerError;
            if (mCommands.size() >= kMaximumPendingCaptures) {
                return MaiError::make(MaiErrorCode::Busy, "screen capture queue is full");
            }
            mCommands.push_back(command);
            ::SetEvent(mWake->event.get());
        }
        // 调用方必须等 worker 完成命令后才能返回，否则 request.isCanceled 可能悬挂引用调用上下文。
        std::unique_lock<std::mutex> lock(command->mutex);
        command->finished.wait(lock, [&] { return command->done; });
        if (!command->error.hasError()) screenshot = std::move(command->screenshot);
        return command->error;
    }

    MaiError stop() {
        // 多个 stop 可以同时到达；前一个完成时不能重新开放后一个仍在等待收回的后端。
        mStopsInProgress.fetch_add(1);
        std::lock_guard<std::mutex> lifecycle(mLifecycleMutex);
        if (mWorker.joinable()) {
            mWake->stopping.store(true);
            ::SetEvent(mWake->event.get());
            mWorker.join();
        }
        mWake.reset();
        mStopsInProgress.fetch_sub(1);
        return {};
    }

private:
    // mLifecycleMutex 序列化线程启动和收回；mMutex 仅保护队列和诊断，不跨原生调用持有。
    MaiError start() {
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            running = mState.workerRunning;
        }
        if (mWorker.joinable() && running) return {};
        if (mWorker.joinable()) mWorker.join();
        mWake = std::make_shared<MaiScreenshotWakeState>();
        if (!mWake->event) {
            return MaiError::make(MaiErrorCode::Internal,
                                  "CreateEvent failed while starting native capture");
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mWorkerError = {};
            mState.workerRunning = true;
        }
        try {
            mWorker = std::thread([this, wake = mWake] { run(wake); });
            std::lock_guard<std::mutex> lock(mMutex);
            ++mState.workerStarts;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mMutex);
            mState.workerRunning = false;
            return MaiError::make(MaiErrorCode::Internal, "cannot start screen capture worker");
        }
        return {};
    }

    void setCapturing(bool capturing) {
        std::lock_guard<std::mutex> lock(mMutex);
        mState.capturing = capturing;
    }

    MaiError checkEnvironment(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (force || mEnvironmentCheckedAt == std::chrono::steady_clock::time_point{} ||
            now - mEnvironmentCheckedAt >= std::chrono::milliseconds(kEnvironmentCheckMs)) {
            mEnvironmentCheckedAt = now;
            mEnvironmentError = verifyCaptureEnvironment();
            if (mEnvironmentError.hasError()) closeCapture();
        }
        return mEnvironmentError;
    }

    DWORD environmentWaitMs() const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - mEnvironmentCheckedAt)
                                 .count();
        if (elapsed >= kEnvironmentCheckMs) return 0;
        return kEnvironmentCheckMs - static_cast<DWORD>(elapsed);
    }

    void clearLatestFrame() {
        mLatestTexture.Reset();
        mStagingTexture.Reset();
        mFrameSequence = 0;
        mFrameReceivedAtMs = 0;
        mFrameSourceProducedAtMs = 0;
        mFrameSourceQpc100ns = 0;
    }

    void closeCapture() noexcept {
        clearLatestFrame();
        if (mTargetState) mTargetState->closed.store(true);
        // 清理中的单项 COM 失败不能阻止其余对象释放，也不能逃出 worker 的退出路径。
        try {
            if (mPool && mHasFrameToken) mPool.FrameArrived(mFrameToken);
        } catch (...) {
        }
        try {
            if (mItem && mHasClosedToken) mItem.Closed(mClosedToken);
        } catch (...) {
        }
        mHasFrameToken = false;
        mHasClosedToken = false;
        try {
            if (mSession) mSession.Close();
        } catch (...) {
        }
        try {
            if (mPool) mPool.Close();
        } catch (...) {
        }
        mSession = nullptr;
        mPool = nullptr;
        mItem = nullptr;
        mTargetState.reset();
        mWindow = nullptr;
        mMonitor = nullptr;
        mWindowProcess = 0;
        mPoolSize = {};
        setCapturing(false);
    }

    void releaseDevice() noexcept {
        closeCapture();
        mImagingFactory.Reset();
        mCaptureDevice = nullptr;
        mContext.Reset();
        mDevice.Reset();
    }

    void discardFailedCapture() noexcept {
        // CopyResource 没有 HRESULT；Map 或取帧报错后还要查询设备状态，下一次命令才重新建设备。
        if (mDevice && FAILED(mDevice->GetDeviceRemovedReason())) {
            releaseDevice();
        } else {
            closeCapture();
        }
    }

    MaiError ensureDevice() {
        if (mDevice && mCaptureDevice && mImagingFactory) return {};
        releaseDevice();
        MaiError error = createDefaultDevice(mDevice, mContext);
        if (error.hasError()) return error;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mState.deviceCreations;
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT result = mDevice.As(&dxgiDevice);
        if (FAILED(result)) return hresultError("ID3D11Device::QueryInterface", result);
        result = ::CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(mCaptureDevice)));
        if (FAILED(result)) return hresultError("CreateDirect3D11DeviceFromDXGIDevice", result);
        result = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(mImagingFactory.GetAddressOf()));
        if (FAILED(result)) return hresultError("CoCreateInstance(WICImagingFactory)", result);
        return {};
    }

    MaiError selectTarget(const MaiScreenshotRequest& request,
                          const std::shared_ptr<MaiScreenshotWakeState>& wake) {
        // 必须先验证交互环境，再解释窗口句柄或建立新设备，避免非输入桌面被误报为普通目标丢失。
        const MaiError environmentError = checkEnvironment(true);
        if (environmentError.hasError()) return environmentError;
        HWND window = nullptr;
        HMONITOR monitor = nullptr;
        DWORD windowProcess = 0;
        if (request.target == MaiScreenshotTarget::WindowByTitle) {
            const MaiError error = findWindow(request.windowTitle, window);
            if (error.hasError()) {
                closeCapture();
                return error;
            }
        } else if (request.target == MaiScreenshotTarget::WindowById) {
            if (request.windowId == 0 ||
                request.windowId > std::numeric_limits<std::uintptr_t>::max()) {
                closeCapture();
                return MaiError::make(MaiErrorCode::InvalidInput,
                                      "windowId must be a non-zero native window identifier");
            }
            window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.windowId));
        } else if (request.target == MaiScreenshotTarget::DisplayById) {
            if (request.displayId == 0 ||
                request.displayId > std::numeric_limits<std::uintptr_t>::max()) {
                closeCapture();
                return MaiError::make(MaiErrorCode::InvalidInput,
                                      "displayId must be a non-zero native monitor identifier");
            }
            monitor = reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(request.displayId));
            if (!isCaptureMonitor(monitor)) {
                closeCapture();
                return closedTargetError();
            }
        } else if (request.target == MaiScreenshotTarget::DisplayUnderPointer) {
            POINT pointer{};
            if (!::GetCursorPos(&pointer)) {
                closeCapture();
                return MaiError::make(MaiErrorCode::Internal, "GetCursorPos failed");
            }
            monitor = ::MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY);
        } else {
            closeCapture();
            return MaiError::make(MaiErrorCode::InvalidInput, "unknown screen capture target");
        }
        if (window && !isCaptureWindow(window)) {
            closeCapture();
            return closedTargetError();
        }
        if (window) ::GetWindowThreadProcessId(window, &windowProcess);
        if (mSession && mWindow == window && mMonitor == monitor &&
            mWindowProcess == windowProcess && mTargetState && !mTargetState->closed.load()) {
            return {};
        }
        closeCapture();
        using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
        using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
        if (!GraphicsCaptureSession::IsSupported()) {
            return MaiError::make(MaiErrorCode::NotSupported,
                                  "Windows Graphics Capture is not supported on this system");
        }
        const MaiError deviceError = ensureDevice();
        if (deviceError.hasError()) return deviceError;
        const auto interop =
            winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (window) {
            winrt::check_hresult(interop->CreateForWindow(
                window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(mItem)));
        } else {
            winrt::check_hresult(interop->CreateForMonitor(
                monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(mItem)));
        }
        mPoolSize = mItem.Size();
        if (mPoolSize.Width <= 0 || mPoolSize.Height <= 0) return closedTargetError();
        using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
        using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
        mPool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            mCaptureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, mPoolSize);
        mTargetState = std::make_shared<MaiScreenshotTargetState>();
        mFrameToken =
            mPool.FrameArrived([wake](const auto&, const auto&) { ::SetEvent(wake->event.get()); });
        mHasFrameToken = true;
        mClosedToken = mItem.Closed([wake, target = mTargetState](const auto&, const auto&) {
            target->closed.store(true);
            ::SetEvent(wake->event.get());
        });
        mHasClosedToken = true;
        mSession = mPool.CreateCaptureSession(mItem);
        mWindow = window;
        mMonitor = monitor;
        mWindowProcess = windowProcess;
        mSession.StartCapture();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mState.sessionStarts;
            mState.capturing = true;
        }
        return {};
    }

    bool targetIsAvailable() const {
        if (!mTargetState || mTargetState->closed.load()) return false;
        if (mWindow) {
            DWORD process = 0;
            ::GetWindowThreadProcessId(mWindow, &process);
            return isCaptureWindow(mWindow) && process == mWindowProcess;
        }
        return isCaptureMonitor(mMonitor);
    }

    MaiError drainFrames(const std::shared_ptr<MaiScreenshotWakeState>& wake) {
        if (!mPool) return {};
        if (!targetIsAvailable()) return closedTargetError();
        // 有限批次避免高刷新率画面饿死命令队列；通常两个池缓冲读完后立即得到空帧。
        for (unsigned received = 0; received < 16 && !wake->stopping.load(); ++received) {
            auto frame = mPool.TryGetNextFrame();
            if (!frame) break;
            const auto contentSize = frame.ContentSize();
            if (contentSize.Width <= 0 || contentSize.Height <= 0) {
                frame.Close();
                return closedTargetError();
            }
            if (contentSize.Width != mPoolSize.Width || contentSize.Height != mPoolSize.Height) {
                // 帧表面仍是旧池大小。先交还帧、丢弃旧完整图，再按明确的新尺寸重建池和会话。
                // 实测关闭会话后在旧池 CreateCaptureSession 返回 E_UNEXPECTED，因此必须整对更换。
                // 只有尺寸变化才重配，保留 item 和设备；普通静态截图始终沿用原会话。
                const char* resizeStage = "WGC resize frame.Close";
                try {
                    frame.Close();
                    frame = nullptr;
                    clearLatestFrame();
                    resizeStage = "WGC resize session.Close";
                    mSession.Close();
                    mSession = nullptr;
                    setCapturing(false);
                    resizeStage = "WGC resize pool.FrameArrived/remove";
                    mPool.FrameArrived(mFrameToken);
                    mHasFrameToken = false;
                    resizeStage = "WGC resize pool.Close";
                    mPool.Close();
                    mPool = nullptr;
                    using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
                    using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
                    resizeStage = "WGC resize pool.CreateFreeThreaded";
                    mPool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                        mCaptureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
                    // 旧回调即使仍在退栈也只设置事件；新一代像素只能从新池读取，不能沿用旧图。
                    resizeStage = "WGC resize pool.FrameArrived/add";
                    mFrameToken = mPool.FrameArrived(
                        [wake](const auto&, const auto&) { ::SetEvent(wake->event.get()); });
                    mHasFrameToken = true;
                    mPoolSize = contentSize;
                    resizeStage = "WGC resize pool.CreateCaptureSession";
                    mSession = mPool.CreateCaptureSession(mItem);
                    resizeStage = "WGC resize session.StartCapture";
                    mSession.StartCapture();
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                        ++mState.sessionStarts;
                        mState.capturing = true;
                    }
                } catch (const winrt::hresult_error& exception) {
                    const MaiError error = hresultError(resizeStage, exception.code());
                    // 空闲取帧也会经历 resize；保留固定阶段和 HRESULT，避免后台错误丢失定位信息。
                    std::fprintf(stderr, "%s\n", error.message().c_str());
                    return error;
                }
                continue;
            }
            std::int64_t sourceProducedAtMs = 0;
            std::int64_t sourceQpc100ns = 0;
            try {
                sourceQpc100ns = frame.SystemRelativeTime().count();
                sourceProducedAtMs = frameSourceProducedAtMs(sourceQpc100ns);
            } catch (const winrt::hresult_error&) {
                // 普通截图保留既有行为；需要新帧的命令无法证明源时间时继续等待，不能回退缓存。
            }
            using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
            const auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())));
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Width < static_cast<UINT>(contentSize.Width) ||
                description.Height < static_cast<UINT>(contentSize.Height)) {
                frame.Close();
                return MaiError::make(MaiErrorCode::Internal,
                                      "capture frame surface is smaller than its content");
            }
            if (!mLatestTexture) {
                description.Width = static_cast<UINT>(contentSize.Width);
                description.Height = static_cast<UINT>(contentSize.Height);
                description.BindFlags = 0;
                description.CPUAccessFlags = 0;
                description.MiscFlags = 0;
                description.Usage = D3D11_USAGE_DEFAULT;
                winrt::check_hresult(
                    mDevice->CreateTexture2D(&description, nullptr, mLatestTexture.GetAddressOf()));
            }
            const D3D11_BOX region{0,
                                   0,
                                   0,
                                   static_cast<UINT>(contentSize.Width),
                                   static_cast<UINT>(contentSize.Height),
                                   1};
            mContext->CopySubresourceRegion(mLatestTexture.Get(), 0, 0, 0, 0, texture.Get(), 0,
                                            &region);
            frame.Close();
            const HRESULT deviceStatus = mDevice->GetDeviceRemovedReason();
            if (FAILED(deviceStatus)) return hresultError("capture D3D11 device", deviceStatus);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mFrameSequence = ++mState.framesReceived;
            }
            mFrameReceivedAtMs = MaiTime::getCurrentTime();
            mFrameSourceProducedAtMs = sourceProducedAtMs;
            mFrameSourceQpc100ns = sourceQpc100ns;
        }
        return {};
    }

    bool isCanceled(const MaiScreenshotRequest& request,
                    const std::shared_ptr<MaiScreenshotWakeState>& wake) const {
        return mStopsInProgress.load() != 0 || wake->stopping.load() ||
               (request.isCanceled && request.isCanceled());
    }

    MaiError captureCurrent(const MaiScreenshotRequest& request, MaiScreenshot& screenshot,
                            const std::shared_ptr<MaiScreenshotWakeState>& wake,
                            bool& freshnessTimedOut) {
        freshnessTimedOut = false;
        if (isCanceled(request, wake)) return canceledCaptureError();
        MaiError error = selectTarget(request, wake);
        if (error.hasError()) return error;
        const auto started = std::chrono::steady_clock::now();
        const bool hadFrame = mLatestTexture != nullptr;
        const auto previousSequence = mFrameSequence;
        const bool hasBarrier = request.afterFrameSequence || request.afterFrameReceivedAtMs;
        auto timeout = std::chrono::milliseconds(hasBarrier || !hadFrame ? kCaptureTimeoutMs
                                                                         : kCaptureSettleMs);
        for (;;) {
            if (isCanceled(request, wake)) return canceledCaptureError();
            error = checkEnvironment(false);
            if (error.hasError()) return error;
            // 原生 WGC 实测会把 compositor 时间戳提前数毫秒交付。保留原始
            // QPC，只在时钟确实赶上后重新换算；不把稍晚的 drain/编码时间
            // 当成源时间。先检查这张候选再 drain，避免高帧率不断覆盖候选。
            if (hasBarrier && mLatestTexture) {
                if (!mFrameSourceProducedAtMs && mFrameSourceQpc100ns > 0)
                    mFrameSourceProducedAtMs = frameSourceProducedAtMs(mFrameSourceQpc100ns);
                if (framePassesBarrier(request, mFrameSequence, mFrameReceivedAtMs, mFrameSourceProducedAtMs))
                    break;
            }
            error = drainFrames(wake);
            if (error.hasError()) return error;
            if (mLatestTexture &&
                (hasBarrier ? framePassesBarrier(request, mFrameSequence, mFrameReceivedAtMs,
                                                 mFrameSourceProducedAtMs)
                            : (!hadFrame || mFrameSequence > previousSequence)))
                break;
            // WGC 在 Windows 11 24H2 可能只发送变化的画面。已有完整图时短暂等候更新，
            // 无屏障请求仍可沿用同一图及真实元数据；要求新帧的请求只能等待原会话后续投帧。
            // 达到截止时明确失败，不能靠重启会话或重写接收时间制造“新帧”。
            if (!mLatestTexture) timeout = std::chrono::milliseconds(kCaptureTimeoutMs);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed >= timeout) {
                if (hasBarrier) {
                    freshnessTimedOut = true;
                    return MaiError::make(MaiErrorCode::Busy,
                                          "Windows Graphics Capture timed out waiting for a frame "
                                          "newer than the requested barrier");
                }
                if (mLatestTexture) break;
                return MaiError::make(MaiErrorCode::Internal,
                                      "Windows Graphics Capture timed out waiting for a frame");
            }
            const DWORD wait = ::WaitForSingleObject(wake->event.get(),
                                                     std::min(kCancelPollMs, environmentWaitMs()));
            if (wait == WAIT_FAILED) {
                return MaiError::make(MaiErrorCode::Internal, "capture event wait failed");
            }
        }
        if (isCanceled(request, wake)) return canceledCaptureError();
        error = checkEnvironment(true);
        if (error.hasError()) return error;
        if (!targetIsAvailable()) return closedTargetError();
        std::vector<std::uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        error = copyTextureToMemory(mDevice.Get(), mContext.Get(), mLatestTexture.Get(),
                                    mStagingTexture, pixels, width, height);
        if (error.hasError()) return error;
        error = checkEnvironment(true);
        if (error.hasError()) return error;
        error = encodePng(mImagingFactory.Get(), pixels, width, height, screenshot);
        if (error.hasError()) return error;
        if (isCanceled(request, wake)) return canceledCaptureError();
        error = checkEnvironment(true);
        if (error.hasError()) return error;
        if (!targetIsAvailable()) return closedTargetError();
        screenshot.frameSequence = mFrameSequence;
        screenshot.frameReceivedAtMs = mFrameReceivedAtMs;
        return {};
    }

    void execute(const std::shared_ptr<MaiScreenshotCommand>& command,
                 const std::shared_ptr<MaiScreenshotWakeState>& wake) {
        MaiScreenshot screenshot;
        MaiError error;
        bool freshnessTimedOut = false;
        try {
            error = captureCurrent(command->request, screenshot, wake, freshnessTimedOut);
        } catch (const winrt::hresult_error& exception) {
            error = hresultError("Windows Graphics Capture", exception.code());
        } catch (...) {
            error = MaiError::make(MaiErrorCode::Internal,
                                   "Windows Graphics Capture failed unexpectedly");
        }
        // 新帧等待超时不表示设备或原生会话损坏。保持常驻会话，下一次请求继续等待真实帧；
        // 命令完成仍通过 complete 清空失败输出，不泄露上一张缓存图。
        if (error.hasError() && error.code() != MaiErrorCode::Canceled && !freshnessTimedOut)
            discardFailedCapture();
        command->complete(std::move(error), std::move(screenshot));
    }

    void run(const std::shared_ptr<MaiScreenshotWakeState>& wake) {
        MaiThread::setCurrentName("mai-capture");
        MaiError terminalError = canceledCaptureError();
        try {
            MaiWinRtApartment apartment;
            try {
                while (!wake->stopping.load()) {
                    std::shared_ptr<MaiScreenshotCommand> command;
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                        if (!mCommands.empty()) {
                            command = mCommands.front();
                            mCommands.pop_front();
                        }
                    }
                    if (command) {
                        execute(command, wake);
                        continue;
                    }
                    try {
                        MaiError error = checkEnvironment(false);
                        if (!error.hasError()) error = drainFrames(wake);
                        if (error.hasError()) discardFailedCapture();
                    } catch (...) {
                        discardFailedCapture();
                    }
                    // 即使 WGC 不再投帧，也按环境检查的剩余期限唤醒，失效后立即丢弃缓存并关目标。
                    const DWORD wait =
                        ::WaitForSingleObject(wake->event.get(), environmentWaitMs());
                    if (wait == WAIT_FAILED) {
                        terminalError = MaiError::make(MaiErrorCode::Internal,
                                                       "capture worker event wait failed");
                        break;
                    }
                }
            } catch (...) {
                terminalError = MaiError::make(MaiErrorCode::Internal,
                                               "screen capture worker failed unexpectedly");
            }
            // Apartment 的析构必须晚于所有 WinRT、WIC、D3D 对象的释放。
            releaseDevice();
        } catch (const winrt::hresult_error& exception) {
            terminalError = hresultError("screen capture apartment", exception.code());
        } catch (...) {
            terminalError =
                MaiError::make(MaiErrorCode::Internal, "cannot initialize screen capture worker");
        }
        std::deque<std::shared_ptr<MaiScreenshotCommand>> pending;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mState.workerRunning = false;
            mState.capturing = false;
            mWorkerError = terminalError;
            pending.swap(mCommands);
        }
        for (const auto& command : pending) command->complete(terminalError);
    }

    std::mutex mLifecycleMutex;
    std::mutex mMutex;
    std::atomic<unsigned> mStopsInProgress{0};
    std::thread mWorker;
    std::shared_ptr<MaiScreenshotWakeState> mWake;
    std::deque<std::shared_ptr<MaiScreenshotCommand>> mCommands;
    MaiScreenshotCaptureState mState;
    MaiError mWorkerError;

    // 下列状态只由 mai-capture 访问；stop 在 join 后才允许其他线程销毁 worker 门面。
    std::chrono::steady_clock::time_point mEnvironmentCheckedAt{};
    MaiError mEnvironmentError;
    ComPtr<ID3D11Device> mDevice;
    ComPtr<ID3D11DeviceContext> mContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice mCaptureDevice{nullptr};
    ComPtr<IWICImagingFactory> mImagingFactory;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem mItem{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool mPool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession mSession{nullptr};
    winrt::event_token mFrameToken{};
    winrt::event_token mClosedToken{};
    bool mHasFrameToken = false;
    bool mHasClosedToken = false;
    std::shared_ptr<MaiScreenshotTargetState> mTargetState;
    HWND mWindow = nullptr;
    HMONITOR mMonitor = nullptr;
    DWORD mWindowProcess = 0;
    winrt::Windows::Graphics::SizeInt32 mPoolSize{};
    ComPtr<ID3D11Texture2D> mLatestTexture;
    ComPtr<ID3D11Texture2D> mStagingTexture;
    std::uint64_t mFrameSequence = 0;
    std::int64_t mFrameReceivedAtMs = 0;
    // 只用于新帧屏障的保守源生产时间；0 表示无法证明，公开接收时间口径保持不变。
    std::int64_t mFrameSourceProducedAtMs = 0;
    std::int64_t mFrameSourceQpc100ns = 0;
};

MaiScreenshotWindowsWorker& screenshotWorker() {
    static MaiScreenshotWindowsWorker worker;
    return worker;
}

}  // namespace

bool maiIsScreenshotSupported() {
    return true;
}

MaiScreenshotCaptureState maiScreenshotCaptureState() {
    return screenshotWorker().state();
}

MaiError maiStopScreenshotCapture() {
    maiAssertBlockingAllowed("maiStopScreenshotCapture");
    return screenshotWorker().stop();
}

MaiError maiCaptureScreenshot(const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
    maiAssertBlockingAllowed("maiCaptureScreenshot");
    screenshot = MaiScreenshot{};
    return screenshotWorker().capture(request, screenshot);
}

MaiError maiListCaptureWindows(std::vector<std::string>& titles) {
    titles.clear();
    if (!::EnumWindows(collectVisibleWindowTitles, reinterpret_cast<LPARAM>(&titles))) {
        return MaiError::make(MaiErrorCode::Internal, "EnumWindows failed");
    }
    std::sort(titles.begin(), titles.end());
    return {};
}

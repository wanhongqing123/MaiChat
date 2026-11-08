// 文件作用：Windows 原生相机的设备无关状态和 Media Foundation 适配。
// 专用 MTA 线程持有 CaptureEngine，预览和照片分别走原生 PreviewSink 和 PhotoSink。
#include "MaiCameraCapture.h"

#include <windows.h>

#include <mfapi.h>
#include <mfcaptureengine.h>
#include <mferror.h>
#include <mfidl.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::size_t kMaximumCameraPhotoBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaximumCameraFrameBytes = 64 * 1024 * 1024;
constexpr std::uint32_t kMaximumCameraDimension = 16384;

std::string cameraError(const char* operation, HRESULT result) {
    if (result == E_ACCESSDENIED) return "Camera access was denied. Check Windows camera privacy settings.";
    char code[16]{};
    std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (0x" + code + ")";
}

struct MaiCameraWindowsNotice {
    enum class Kind { State, Photo };
    Kind kind = Kind::State;
    MaiCameraCapture::State state = MaiCameraCapture::State::Stopped;
    std::string error;
    MaiCameraCapture::Photo photo;
};

// 原生回调仅更新本代状态和有界待交付数据；用户回调只由拥有者线程 drain。
// deliveryMutex 为 stop 提供正在交付回调的屏障，不持有它调用任何 MF 引擎方法。
struct MaiCameraWindowsSession {
    explicit MaiCameraWindowsSession(MaiCameraCapture::Callbacks value) : callbacks(std::move(value)) {}
    std::mutex mutex;
    std::mutex deliveryMutex;
    std::condition_variable changed;
    MaiCameraCapture::Callbacks callbacks;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    MaiCameraCapture::State state = MaiCameraCapture::State::Stopped;
    bool closing = false;
    bool terminal = false;
    bool initialized = false;
    bool previewStarted = false;
    std::shared_ptr<MaiCameraCapture::Frame> preview;
    std::deque<MaiCameraWindowsNotice> notices;
    std::uint64_t photoRequest = 0;
    std::uint64_t photoSerial = 0;
    bool photoTaken = false;
    bool photoSample = false;
    bool photoIssued = false;
    std::vector<std::uint8_t> photoBytes;
    std::string photoError;
};

bool cameraCopyPreviewSample(IMFSample* sample, std::uint32_t width, std::uint32_t height, LONG defaultStride,
                             MaiCameraCapture::Frame& output, std::string& error) {
    output = {};
    error.clear();
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    if (!sample || !width || !height || width > kMaximumCameraDimension || height > kMaximumCameraDimension ||
        rowBytes * height > kMaximumCameraFrameBytes) {
        error = "Camera preview sample has invalid dimensions or no pixels";
        return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) {
        error = cameraError("Camera preview buffer", result);
        return false;
    }
    std::vector<std::uint8_t> pixels(rowBytes * height);
    const auto copyRows = [&](const BYTE* top, LONG pitch) {
        if (!top || static_cast<std::uint64_t>(pitch < 0 ? -static_cast<std::int64_t>(pitch) : pitch) < rowBytes)
            return false;
        for (std::uint32_t row = 0; row < height; ++row) {
            auto* destination = pixels.data() + rowBytes * row;
            std::memcpy(destination, top + static_cast<std::ptrdiff_t>(pitch) * row, rowBytes);
            for (std::uint32_t column = 0; column < width; ++column) destination[column * 4 + 3] = 255;
        }
        return true;
    };
    bool copied = false;
    ComPtr<IMF2DBuffer2> surface;
    if (SUCCEEDED(buffer.As(&surface))) {
        BYTE* top = nullptr;
        BYTE* beginning = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        result = surface->Lock2DSize(MF2DBuffer_LockFlags_Read, &top, &pitch, &beginning, &length);
        if (SUCCEEDED(result)) {
            const auto first = reinterpret_cast<std::uintptr_t>(beginning);
            const auto topAddress = reinterpret_cast<std::uintptr_t>(top);
            const auto lastOffset = static_cast<std::int64_t>(pitch) * (height - 1);
            const auto low = static_cast<std::int64_t>(topAddress - first) + std::min<std::int64_t>(0, lastOffset);
            const auto high = static_cast<std::int64_t>(topAddress - first) + std::max<std::int64_t>(0, lastOffset) +
                              static_cast<std::int64_t>(rowBytes);
            if (topAddress >= first && low >= 0 && high <= length) copied = copyRows(top, pitch);
            surface->Unlock2D();
        }
    } else {
        BYTE* beginning = nullptr;
        DWORD length = 0;
        result = buffer->Lock(&beginning, nullptr, &length);
        if (SUCCEEDED(result)) {
            const std::int64_t pitch = defaultStride ? defaultStride : static_cast<LONG>(rowBytes);
            const auto magnitude = static_cast<std::uint64_t>(pitch < 0 ? -pitch : pitch);
            if (magnitude >= rowBytes && magnitude * (height - 1) + rowBytes <= length) {
                const BYTE* top = pitch < 0 ? beginning + magnitude * (height - 1) : beginning;
                copied = copyRows(top, static_cast<LONG>(pitch));
            }
            buffer->Unlock();
        }
    }
    if (!copied) {
        error = FAILED(result) ? cameraError("Camera preview lock", result)
                               : "Camera preview buffer is smaller than its frame";
        return false;
    }
    output.width = width;
    output.height = height;
    output.stride = static_cast<std::uint32_t>(rowBytes);
    output.pixels = std::move(pixels);
    return true;
}

bool cameraCopyPhotoSample(IMFSample* sample, std::vector<std::uint8_t>& bytes, std::string& error) {
    bytes.clear();
    error.clear();
    DWORD total = 0;
    if (!sample || FAILED(sample->GetTotalLength(&total)) || !total || total > kMaximumCameraPhotoBytes) {
        error = "Native camera photo is empty or exceeds the byte limit";
        return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) {
        error = cameraError("Native camera photo buffer", result);
        return false;
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    result = buffer->Lock(&data, nullptr, &length);
    if (FAILED(result)) {
        error = cameraError("Native camera photo lock", result);
        return false;
    }
    try {
        if (data && length && length <= kMaximumCameraPhotoBytes) bytes.assign(data, data + length);
    } catch (...) {
        buffer->Unlock();
        throw;
    }
    buffer->Unlock();
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> image;
    ComPtr<IWICFormatConverter> pixels;
    GUID container{};
    UINT width = 0, height = 0;
    bool valid =
        !bytes.empty() &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetContainerFormat(&container)) && container == GUID_ContainerFormatJpeg &&
        SUCCEEDED(decoder->GetFrame(0, &image)) && SUCCEEDED(image->GetSize(&width, &height)) && width && height &&
        width <= kMaximumCameraDimension && height <= kMaximumCameraDimension;
    if (valid) {
        valid = SUCCEEDED(factory->CreateFormatConverter(&pixels)) &&
                SUCCEEDED(pixels->Initialize(image.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                             nullptr, 0, WICBitmapPaletteTypeCustom));
    }
    if (valid) {
        // WIC 读到尺寸不代表熵数据可解码；逐带覆盖整帧，只保留至多 1 MiB 校验像素，不重编码原始 JPEG。
        constexpr UINT maximumBandBytes = 1024 * 1024;
        const UINT stride = width * 4;
        const UINT bandRows = std::min(height, maximumBandBytes / stride);
        std::vector<BYTE> band(static_cast<std::size_t>(stride) * bandRows);
        for (UINT top = 0; top < height && valid; top += bandRows) {
            const WICRect rectangle{0, static_cast<INT>(top), static_cast<INT>(width),
                                    static_cast<INT>(std::min(bandRows, height - top))};
            valid = SUCCEEDED(pixels->CopyPixels(&rectangle, stride, static_cast<UINT>(band.size()), band.data()));
        }
    }
    if (!valid) {
        bytes.clear();
        error = "Native camera photo did not contain a valid JPEG image";
    }
    return valid;
}

bool cameraIsCurrentLocked(const MaiCameraWindowsSession& session, std::uint64_t generation) {
    return generation == session.generation && !session.closing && !session.terminal;
}

void cameraNoticeStateLocked(MaiCameraWindowsSession& session, MaiCameraCapture::State state,
                             const std::string& error = {}) {
    session.state = state;
    MaiCameraWindowsNotice notice;
    notice.state = state;
    notice.error = error;
    session.notices.push_back(std::move(notice));
}

void cameraFinishPhotoLocked(MaiCameraWindowsSession& session, const std::string& error = {}) {
    if (!session.photoRequest) return;
    MaiCameraWindowsNotice notice;
    notice.kind = MaiCameraWindowsNotice::Kind::Photo;
    notice.photo.requestId = session.photoRequest;
    notice.photo.error = error;
    if (error.empty()) notice.photo.jpeg = std::move(session.photoBytes);
    session.notices.push_back(std::move(notice));
    session.photoRequest = 0;
    session.photoBytes.clear();
    session.photoError.clear();
    session.photoSample = false;
    session.photoTaken = false;
    session.photoIssued = false;
}

std::uint64_t cameraBeginSession(const std::shared_ptr<MaiCameraWindowsSession>& session) {
    std::lock_guard<std::mutex> lock(session->mutex);
    ++session->generation;
    session->closing = false;
    session->terminal = false;
    session->initialized = false;
    session->previewStarted = false;
    session->preview.reset();
    session->notices.clear();
    session->photoRequest = 0;
    session->photoBytes.clear();
    session->photoError.clear();
    session->photoSample = session->photoTaken = session->photoIssued = false;
    cameraNoticeStateLocked(*session, MaiCameraCapture::State::Starting);
    session->changed.notify_all();
    return session->generation;
}

void cameraNativeFailure(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                         const std::string& error) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!cameraIsCurrentLocked(*session, generation)) return;
    session->terminal = true;
    session->preview.reset();
    cameraFinishPhotoLocked(*session, error);
    cameraNoticeStateLocked(*session, MaiCameraCapture::State::Error, error);
    session->changed.notify_all();
}

void cameraNativeInitialized(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                             HRESULT result) {
    if (FAILED(result)) {
        cameraNativeFailure(session, generation, cameraError("Camera initialization", result));
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    if (cameraIsCurrentLocked(*session, generation)) session->initialized = true;
}

void cameraNativePreviewStarted(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                                HRESULT result) {
    if (FAILED(result)) {
        cameraNativeFailure(session, generation, cameraError("Camera preview start", result));
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    if (cameraIsCurrentLocked(*session, generation)) session->previewStarted = true;
}

void cameraReceivePreview(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                          IMFSample* sample, std::uint32_t width, std::uint32_t height, LONG stride) {
    if (!sample) return;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!cameraIsCurrentLocked(*session, generation)) return;
    }
    auto frame = std::make_shared<MaiCameraCapture::Frame>();
    std::string error;
    if (!cameraCopyPreviewSample(sample, width, height, stride, *frame, error)) {
        cameraNativeFailure(session, generation, error);
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!cameraIsCurrentLocked(*session, generation)) return;
    frame->sequence = ++session->sequence;
    if (session->state != MaiCameraCapture::State::Running)
        cameraNoticeStateLocked(*session, MaiCameraCapture::State::Running);
    session->preview = std::move(frame);
    session->changed.notify_all();
}

bool cameraBeginPhoto(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                      std::uint64_t requestId) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!requestId || !cameraIsCurrentLocked(*session, generation) ||
        session->state != MaiCameraCapture::State::Running || session->photoRequest)
        return false;
    session->photoRequest = requestId;
    ++session->photoSerial;
    session->photoBytes.clear();
    session->photoError.clear();
    session->photoSample = session->photoTaken = session->photoIssued = false;
    session->changed.notify_all();
    return true;
}

void cameraReceivePhoto(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                        std::uint64_t requestId, IMFSample* sample) {
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!requestId || !cameraIsCurrentLocked(*session, generation) || session->photoRequest != requestId ||
            session->photoSample)
            return;
    }
    std::vector<std::uint8_t> bytes;
    std::string error;
    cameraCopyPhotoSample(sample, bytes, error);
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!cameraIsCurrentLocked(*session, generation) || session->photoRequest != requestId || session->photoSample)
        return;
    session->photoSample = true;
    session->photoBytes = std::move(bytes);
    session->photoError = error;
    if (!error.empty() || session->photoTaken) cameraFinishPhotoLocked(*session, error);
    session->changed.notify_all();
}

void cameraNativePhotoTaken(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation,
                            std::uint64_t requestId, HRESULT result) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!requestId || !cameraIsCurrentLocked(*session, generation) || session->photoRequest != requestId) return;
    if (FAILED(result)) {
        cameraFinishPhotoLocked(*session, cameraError("Native camera photo", result));
    } else {
        session->photoTaken = true;
        if (session->photoSample) cameraFinishPhotoLocked(*session, session->photoError);
    }
    session->changed.notify_all();
}

bool cameraDrainOne(const std::shared_ptr<MaiCameraWindowsSession>& session) {
    std::lock_guard<std::mutex> delivery(session->deliveryMutex);
    MaiCameraWindowsNotice notice;
    std::shared_ptr<MaiCameraCapture::Frame> frame;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing) return false;
        if (!session->notices.empty()) {
            notice = std::move(session->notices.front());
            session->notices.pop_front();
        } else if (session->preview) {
            frame.swap(session->preview);
        } else {
            return false;
        }
    }
    try {
        if (frame) {
            if (session->callbacks.frame) session->callbacks.frame(*frame);
        } else if (notice.kind == MaiCameraWindowsNotice::Kind::Photo) {
            if (session->callbacks.photo) session->callbacks.photo(notice.photo);
        } else if (session->callbacks.state) {
            session->callbacks.state(notice.state, notice.error);
        }
    } catch (...) {
        // 用户回调不应抛异常；隔离它，避免异常越过系统/交付线程边界。
    }
    return true;
}

void cameraCloseSession(const std::shared_ptr<MaiCameraWindowsSession>& session) {
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->closing) ++session->generation;
        session->closing = true;
        session->terminal = true;
        session->state = MaiCameraCapture::State::Stopped;
        session->preview.reset();
        session->notices.clear();
        session->photoRequest = 0;
        session->photoBytes.clear();
        session->photoError.clear();
        session->photoSample = session->photoTaken = session->photoIssued = false;
    }
    session->changed.notify_all();
    std::lock_guard<std::mutex> delivery(session->deliveryMutex);
}

// 只装载 MF 平台，不激活设备。枚举和真正采集各自配对 startup/shutdown，均不在 MF 工作队列中执行。
class MaiCameraMfApartment {
public:
    MaiCameraMfApartment() {
        mCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(mCom)) mMf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    }
    ~MaiCameraMfApartment() {
        if (SUCCEEDED(mMf)) MFShutdown();
        if (SUCCEEDED(mCom)) CoUninitialize();
    }
    HRESULT result() const {
        return FAILED(mCom) ? mCom : mMf;
    }

private:
    HRESULT mCom = E_FAIL;
    HRESULT mMf = E_FAIL;
};

HRESULT cameraEnumerate(ComPtr<IMFActivate>* selected, bool& found) {
    found = false;
    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (FAILED(result)) return result;
    result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(result)) return result;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    result = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    if (SUCCEEDED(result)) {
        found = count != 0;
        if (selected && count) *selected = devices[0];
    }
    for (UINT32 index = 0; index < count; ++index) devices[index]->Release();
    CoTaskMemFree(devices);
    return result;
}

struct MaiCameraNativeEvent {
    GUID type{};
    HRESULT result = S_OK;
    std::uint64_t request = 0;
    std::uint64_t serial = 0;
};

struct MaiCameraNativeSample {
    ComPtr<IMFSample> sample;
    std::uint64_t request = 0;
    std::uint64_t serial = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    LONG stride = 0;
};

struct MaiCameraNativeMailbox {
    std::mutex mutex;
    std::weak_ptr<MaiCameraWindowsSession> session;
    std::uint64_t generation = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> overflow{false};
    std::atomic<std::uint64_t> photoRequest{0};
    std::atomic<std::uint64_t> photoSerial{0};
    std::deque<MaiCameraNativeEvent> events;
    MaiCameraNativeSample preview;
    MaiCameraNativeSample photo;
    bool photoWaiting = false;

    void wake() {
        if (const auto state = session.lock()) state->changed.notify_all();
    }
};

class MaiCameraEngineEvents final : public IMFCaptureEngineOnEventCallback {
public:
    explicit MaiCameraEngineEvents(std::weak_ptr<MaiCameraNativeMailbox> mailbox) : mMailbox(std::move(mailbox)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFCaptureEngineOnEventCallback)) return E_NOINTERFACE;
        *output = static_cast<IMFCaptureEngineOnEventCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++mReferences;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --mReferences;
        if (!references) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(IMFMediaEvent* event) override {
        const auto mailbox = mMailbox.lock();
        if (!mailbox || mailbox->closed.load() || !event) return S_OK;
        try {
            MaiCameraNativeEvent value;
            HRESULT result = event->GetStatus(&value.result);
            if (FAILED(result)) value.result = result;
            result = event->GetExtendedType(&value.type);
            if (FAILED(result)) {
                value.type = MF_CAPTURE_ENGINE_ERROR;
                value.result = result;
            }
            value.request = mailbox->photoRequest.load();
            value.serial = mailbox->photoSerial.load();
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                if (mailbox->events.size() < 64)
                    mailbox->events.push_back(value);
                else
                    mailbox->overflow = true;
            }
            mailbox->wake();
        } catch (...) {
            mailbox->overflow = true;
            mailbox->wake();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> mReferences{1};
    std::weak_ptr<MaiCameraNativeMailbox> mMailbox;
};

#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TESTS)
// 仅独立白盒 target 使用，精确暂停两次身份检查之间的系统回调，不改变产品路径。
std::function<void()> cameraSampleValidationTestBarrier;
#endif

class MaiCameraEngineSamples final : public IMFCaptureEngineOnSampleCallback {
public:
    MaiCameraEngineSamples(std::weak_ptr<MaiCameraNativeMailbox> mailbox, MaiCameraNativeSample format)
        : mMailbox(std::move(mailbox)), mFormat(std::move(format)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFCaptureEngineOnSampleCallback)) return E_NOINTERFACE;
        *output = static_cast<IMFCaptureEngineOnSampleCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++mReferences;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --mReferences;
        if (!references) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE OnSample(IMFSample* sample) override {
        const auto mailbox = mMailbox.lock();
        if (!mailbox || mailbox->closed.load() || mailbox->stopping.load()) return S_OK;
        if (!sample && !mFormat.request) return S_OK;
        try {
            const auto session = mailbox->session.lock();
            if (!session) return S_OK;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (!cameraIsCurrentLocked(*session, mailbox->generation) ||
                    (mFormat.request &&
                     (session->photoRequest != mFormat.request || session->photoSerial != mFormat.serial)))
                    return S_OK;
            }
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TESTS)
            if (cameraSampleValidationTestBarrier) cameraSampleValidationTestBarrier();
#endif
            MaiCameraNativeSample value = mFormat;
            value.sample = sample;
            MaiCameraNativeSample replaced;
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                if (mailbox->stopping.load() || mailbox->closed.load()) return S_OK;
                // 入队与身份检查共用 mailbox -> session 锁序；旧回调不能在新请求建立后抢占单槽。
                std::lock_guard<std::mutex> sessionLock(session->mutex);
                if (!cameraIsCurrentLocked(*session, mailbox->generation) ||
                    (value.request && (session->photoRequest != value.request || session->photoSerial != value.serial)))
                    return S_OK;
                if (value.request) {
                    if (mailbox->photoWaiting && mailbox->photo.request == value.request &&
                        mailbox->photo.serial == value.serial)
                        return S_OK;
                    replaced = std::move(mailbox->photo);
                    mailbox->photo = std::move(value);
                    mailbox->photoWaiting = true;
                } else {
                    replaced = std::move(mailbox->preview);
                    mailbox->preview = std::move(value);
                }
            }
            mailbox->wake();
        } catch (...) {
            mailbox->overflow = true;
            mailbox->wake();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> mReferences{1};
    std::weak_ptr<MaiCameraNativeMailbox> mMailbox;
    const MaiCameraNativeSample mFormat;
};

class MaiCameraWindowsNative {
public:
    explicit MaiCameraWindowsNative(std::shared_ptr<MaiCameraNativeMailbox> mailbox) : mMailbox(std::move(mailbox)) {}
    ~MaiCameraWindowsNative() {
        close();
    }

    HRESULT initialize() {
        bool found = false;
        HRESULT result = cameraEnumerate(&mActivate, found);
        if (FAILED(result)) return result;
        if (!found) return MF_E_NO_CAPTURE_DEVICES_AVAILABLE;
        ComPtr<IMFCaptureEngineClassFactory> factory;
        result =
            CoCreateInstance(CLSID_MFCaptureEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(result)) return result;
        result = factory->CreateInstance(CLSID_MFCaptureEngine, IID_PPV_ARGS(&mEngine));
        if (FAILED(result)) return result;
        ComPtr<IMFAttributes> attributes;
        result = MFCreateAttributes(&attributes, 1);
        if (FAILED(result)) return result;
        // nullptr audio 仍会默认选麦克风；必须显式只用视频设备。
        result = attributes->SetUINT32(MF_CAPTURE_ENGINE_USE_VIDEO_DEVICE_ONLY, TRUE);
        if (FAILED(result)) return result;
        mEvents.Attach(new MaiCameraEngineEvents(mMailbox));
        return mEngine->Initialize(mEvents.Get(), attributes.Get(), nullptr, mActivate.Get());
    }

    HRESULT startPreview() {
        HRESULT result = mEngine->GetSource(&mSource);
        if (FAILED(result)) return result;
        DWORD count = 0;
        result = mSource->GetDeviceStreamCount(&count);
        if (FAILED(result)) return result;
        DWORD stream = MAXDWORD;
        for (DWORD index = 0; index < count; ++index) {
            MF_CAPTURE_ENGINE_STREAM_CATEGORY category{};
            if (SUCCEEDED(mSource->GetDeviceStreamCategory(index, &category))) {
                if (category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_VIDEO_PREVIEW) {
                    stream = index;
                    break;
                }
                if (category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_VIDEO_CAPTURE && stream == MAXDWORD) stream = index;
            }
        }
        if (stream == MAXDWORD) return MF_E_INVALIDSTREAMNUMBER;
        ComPtr<IMFMediaType> native;
        result = mSource->GetCurrentDeviceMediaType(stream, &native);
        if (FAILED(result)) return result;
        UINT32 width = 0, height = 0;
        result = MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(result) || !width || !height) return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
        const double scale = std::min(1.0, std::min(1280.0 / width, 720.0 / height));
        width = std::max<UINT32>(1, static_cast<UINT32>(width * scale));
        height = std::max<UINT32>(1, static_cast<UINT32>(height * scale));
        ComPtr<IMFMediaType> output;
        result = MFCreateMediaType(&output);
        if (FAILED(result)) return result;
        if (FAILED(result = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(result = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
            FAILED(result = MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height)) ||
            FAILED(result = MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 15, 1)) ||
            FAILED(result = MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(result = output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(result = output->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
            FAILED(result = output->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4)))
            return result;
        ComPtr<IMFCaptureSink> sink;
        result = mEngine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW, &sink);
        if (FAILED(result)) return result;
        result = sink.As(&mPreview);
        if (FAILED(result)) return result;
        result = mPreview->AddStream(stream, output.Get(), nullptr, &mPreviewIndex);
        if (FAILED(result)) return result;
        // 不做前摄镜像，预览与原生照片同方向；忽略不支持此可选控制的驱动。
        mPreview->SetMirrorState(FALSE);
        MaiCameraNativeSample format;
        format.width = width;
        format.height = height;
        format.stride = static_cast<LONG>(width * 4);
        ComPtr<IMFMediaType> actual;
        if (SUCCEEDED(mPreview->GetOutputMediaType(mPreviewIndex, &actual))) {
            MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &format.width, &format.height);
            UINT32 stride = 0;
            if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) format.stride = static_cast<LONG>(stride);
        }
        mPreviewSamples.Attach(new MaiCameraEngineSamples(mMailbox, format));
        result = mPreview->SetSampleCallback(mPreviewIndex, mPreviewSamples.Get());
        if (FAILED(result)) return result;
        mPreviewRequested = true;
        return mEngine->StartPreview();
    }

    HRESULT takePhoto(std::uint64_t request, std::uint64_t serial) {
        DWORD count = 0;
        HRESULT result = mSource->GetDeviceStreamCount(&count);
        if (FAILED(result)) return result;
        DWORD selected = MAXDWORD;
        int selectedRank = 100;
        for (DWORD index = 0; index < count; ++index) {
            MF_CAPTURE_ENGINE_STREAM_CATEGORY category{};
            if (FAILED(mSource->GetDeviceStreamCategory(index, &category))) continue;
            const int rank = category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_PHOTO_INDEPENDENT ? 0
                             : category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_PHOTO_DEPENDENT ? 1
                             : category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_VIDEO_CAPTURE   ? 2
                                                                                             : 100;
            if (rank < selectedRank) {
                selected = index;
                selectedRank = rank;
            }
        }
        if (selected == MAXDWORD) return MF_E_INVALIDSTREAMNUMBER;
        ComPtr<IMFMediaType> native;
        result = mSource->GetCurrentDeviceMediaType(selected, &native);
        if (FAILED(result)) return result;
        ComPtr<IMFMediaType> output;
        result = MFCreateMediaType(&output);
        if (FAILED(result)) return result;
        GUID major{}, subtype{};
        native->GetGUID(MF_MT_MAJOR_TYPE, &major);
        native->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (major == MFMediaType_Image && (subtype == GUID_ContainerFormatJpeg || subtype == MFVideoFormat_MJPG)) {
            // Native JPEG 必须透传完整 type，不能重造后丢失驱动所需属性。
            result = native->CopyAllItems(output.Get());
        } else {
            UINT32 width = 0, height = 0;
            result = MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height);
            if (SUCCEEDED(result)) result = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Image);
            if (SUCCEEDED(result)) result = output->SetGUID(MF_MT_SUBTYPE, GUID_ContainerFormatJpeg);
            if (SUCCEEDED(result)) result = MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
        }
        if (FAILED(result)) return result;
        ComPtr<IMFCaptureSink> sink;
        result = mEngine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PHOTO, &sink);
        if (FAILED(result)) return result;
        result = sink.As(&mPhoto);
        if (FAILED(result)) return result;
        result = mPhoto->RemoveAllStreams();
        if (FAILED(result)) return result;
        DWORD photoIndex = 0;
        result = mPhoto->AddStream(selected, output.Get(), nullptr, &photoIndex);
        if (FAILED(result)) return result;
        MaiCameraNativeSample token;
        token.request = request;
        token.serial = serial;
        mPhotoSamples.Attach(new MaiCameraEngineSamples(mMailbox, token));
        result = mPhoto->SetSampleCallback(mPhotoSamples.Get());
        if (FAILED(result)) return result;
        mMailbox->photoSerial = serial;
        mMailbox->photoRequest = request;
        return mEngine->TakePhoto();
    }

    void close() noexcept {
        mMailbox->stopping = true;
        if (mEngine && mPreviewRequested && SUCCEEDED(mEngine->StopPreview())) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            bool stopped = false;
            while (!stopped && std::chrono::steady_clock::now() < deadline) {
                MaiCameraNativeSample discardedPreview, discardedPhoto;
                {
                    std::lock_guard<std::mutex> lock(mMailbox->mutex);
                    for (const auto& event : mMailbox->events)
                        if (event.type == MF_CAPTURE_ENGINE_PREVIEW_STOPPED) stopped = true;
                    mMailbox->events.clear();
                    discardedPreview = std::move(mMailbox->preview);
                    discardedPhoto = std::move(mMailbox->photo);
                    mMailbox->preview = {};
                    mMailbox->photo = {};
                    mMailbox->photoWaiting = false;
                }
                if (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        mPreviewRequested = false;
        mMailbox->closed = true;
        // 未启动预览及 StopPreview 失败也必须在 MF owner 线程、source/engine 释放前清空样本。
        {
            MaiCameraNativeSample discardedPreview, discardedPhoto;
            {
                std::lock_guard<std::mutex> lock(mMailbox->mutex);
                discardedPreview = std::move(mMailbox->preview);
                discardedPhoto = std::move(mMailbox->photo);
                mMailbox->preview = {};
                mMailbox->photo = {};
                mMailbox->photoWaiting = false;
                mMailbox->events.clear();
            }
        }
        // 引擎初始化途中取消也要释放自己的 activation/source；不接触任何其他应用的设备实例。
        if (mSource) {
            ComPtr<IMFMediaSource> video;
            if (SUCCEEDED(mSource->GetCaptureDeviceSource(MF_CAPTURE_ENGINE_DEVICE_TYPE_VIDEO, &video)))
                video->Shutdown();
        }
        mPhoto.Reset();
        mPreview.Reset();
        mSource.Reset();
        mEngine.Reset();
        if (mActivate) mActivate->ShutdownObject();
        mActivate.Reset();
        mPhotoSamples.Reset();
        mPreviewSamples.Reset();
        mEvents.Reset();
    }

private:
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TESTS)
    friend struct MaiCameraWindowsTestAccess;
#endif
    std::shared_ptr<MaiCameraNativeMailbox> mMailbox;
    ComPtr<IMFActivate> mActivate;
    ComPtr<IMFCaptureEngine> mEngine;
    ComPtr<IMFCaptureSource> mSource;
    ComPtr<IMFCapturePreviewSink> mPreview;
    ComPtr<IMFCapturePhotoSink> mPhoto;
    ComPtr<IMFCaptureEngineOnEventCallback> mEvents;
    ComPtr<IMFCaptureEngineOnSampleCallback> mPreviewSamples;
    ComPtr<IMFCaptureEngineOnSampleCallback> mPhotoSamples;
    DWORD mPreviewIndex = 0;
    bool mPreviewRequested = false;
};

}  // namespace

struct MaiCameraCapture::Runtime : public std::enable_shared_from_this<MaiCameraCapture::Runtime> {
    explicit Runtime(Callbacks callbacks) : session(std::make_shared<MaiCameraWindowsSession>(std::move(callbacks))) {}
    std::shared_ptr<MaiCameraWindowsSession> session;
    std::mutex lifecycle;
    std::thread worker;
    std::atomic<bool> workerActive{false};
    std::atomic<DWORD> workerId{0};
    std::shared_ptr<MaiCameraNativeMailbox> mailbox;

    bool start() {
        // 用户回调可查询/提交下一张照片，但不能同步销毁/停止自身；重入 start 也不 self-join。
        if (workerId.load() == GetCurrentThreadId()) return false;
        std::unique_lock<std::mutex> lock(lifecycle, std::try_to_lock);
        if (!lock.owns_lock() || workerActive.load()) return false;
        if (worker.joinable()) worker.join();
        const auto generation = cameraBeginSession(session);
        auto next = std::make_shared<MaiCameraNativeMailbox>();
        next->session = session;
        next->generation = generation;
        std::atomic_store(&mailbox, next);
        workerActive = true;
        try {
            worker = std::thread([owner = shared_from_this(), next] { owner->run(next); });
        } catch (...) {
            workerActive = false;
            cameraCloseSession(session);
            std::atomic_store(&mailbox, std::shared_ptr<MaiCameraNativeMailbox>{});
            return false;
        }
        return true;
    }

    bool capture(std::uint64_t request) {
        const auto native = std::atomic_load(&mailbox);
        if (!native || native->stopping.load() || native->closed.load() || native->photoRequest.load()) return false;
        return cameraBeginPhoto(session, native->generation, request);
    }

    void stop() {
        if (workerId.load() == GetCurrentThreadId()) return;  // 公开契约禁止从用户回调同步 stop。
        std::lock_guard<std::mutex> lock(lifecycle);
        cameraCloseSession(session);
        const auto native = std::atomic_load(&mailbox);
        if (native) native->stopping = true;
        session->changed.notify_all();
        if (worker.joinable()) worker.join();
        std::atomic_store(&mailbox, std::shared_ptr<MaiCameraNativeMailbox>{});
    }

    void run(const std::shared_ptr<MaiCameraNativeMailbox>& native) {
        workerId = GetCurrentThreadId();
        // 与 Chromium PlatformThread 同样动态使用系统线程描述 API，Media 不因此依赖 Agent/Qt。
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        const auto describe =
            reinterpret_cast<SetDescription>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "SetThreadDescription"));
        if (describe) describe(GetCurrentThread(), L"mai-camera");
        const auto generation = native->generation;
        try {
            MaiCameraMfApartment apartment;
            if (FAILED(apartment.result())) {
                cameraNativeFailure(session, generation, cameraError("Camera MF initialization", apartment.result()));
            } else {
                MaiCameraWindowsNative engine(native);
                for (unsigned count = 0; count < 8 && cameraDrainOne(session); ++count) {
                }
                HRESULT initialized = S_OK;
                if (!native->stopping.load()) {
                    bool current = false;
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        current = cameraIsCurrentLocked(*session, generation);
                    }
                    if (current) initialized = engine.initialize();
                }
                if (FAILED(initialized))
                    cameraNativeFailure(session, generation, cameraError("Camera initialization", initialized));
                bool configured = false;
                const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                auto photoDeadline = std::chrono::steady_clock::time_point{};
                std::uint64_t pendingNativePhoto = 0;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        if (!cameraIsCurrentLocked(*session, generation)) break;
                    }
                    if (native->stopping.load()) break;
                    std::deque<MaiCameraNativeEvent> events;
                    MaiCameraNativeSample preview, photo;
                    bool havePhoto = false;
                    {
                        std::lock_guard<std::mutex> lock(native->mutex);
                        events.swap(native->events);
                        preview = std::move(native->preview);
                        native->preview = {};
                        if (native->photoWaiting) {
                            photo = std::move(native->photo);
                            native->photo = {};
                            native->photoWaiting = false;
                            havePhoto = true;
                        }
                    }
                    if (native->overflow.load()) {
                        cameraNativeFailure(session, generation, "Camera native event queue exceeded its limit");
                        break;
                    }
                    for (const auto& event : events) {
                        {
                            std::lock_guard<std::mutex> lock(session->mutex);
                            if (!cameraIsCurrentLocked(*session, generation)) break;
                        }
                        if (event.type == MF_CAPTURE_ENGINE_INITIALIZED) {
                            cameraNativeInitialized(session, generation, event.result);
                            if (SUCCEEDED(event.result) && !configured && !native->stopping.load()) {
                                configured = true;
                                const HRESULT result = engine.startPreview();
                                if (FAILED(result))
                                    cameraNativeFailure(session, generation, cameraError("Camera preview", result));
                            }
                        } else if (event.type == MF_CAPTURE_ENGINE_PREVIEW_STARTED) {
                            cameraNativePreviewStarted(session, generation, event.result);
                        } else if (event.type == MF_CAPTURE_ENGINE_PHOTO_TAKEN) {
                            bool current = false;
                            {
                                std::lock_guard<std::mutex> lock(session->mutex);
                                current = session->photoSerial == event.serial;
                            }
                            if (native->photoRequest.load() == event.request &&
                                native->photoSerial.load() == event.serial)
                                native->photoRequest = 0;
                            if (current) cameraNativePhotoTaken(session, generation, event.request, event.result);
                        } else if (event.type == MF_CAPTURE_ENGINE_ERROR ||
                                   event.type == MF_CAPTURE_ENGINE_CAMERA_STREAM_BLOCKED) {
                            cameraNativeFailure(
                                session, generation,
                                cameraError("Camera capture", FAILED(event.result) ? event.result : E_FAIL));
                        } else if (FAILED(event.result)) {
                            cameraNativeFailure(session, generation, cameraError("Camera native event", event.result));
                        }
                    }
                    if (preview.sample)
                        cameraReceivePreview(session, generation, preview.sample.Get(), preview.width, preview.height,
                                             preview.stride);
                    if (havePhoto) {
                        bool current = false;
                        {
                            std::lock_guard<std::mutex> lock(session->mutex);
                            current = session->photoSerial == photo.serial;
                        }
                        if (current) cameraReceivePhoto(session, generation, photo.request, photo.sample.Get());
                    }
                    std::uint64_t request = 0, serial = 0;
                    bool running = false, terminal = false, stillPending = false;
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        running = session->state == State::Running;
                        terminal = !cameraIsCurrentLocked(*session, generation);
                        stillPending = session->photoRequest != 0;
                        if (!terminal && running && session->photoRequest && !session->photoIssued &&
                            !native->photoRequest.load()) {
                            session->photoIssued = true;
                            request = session->photoRequest;
                            serial = session->photoSerial;
                        }
                    }
                    if (request) {
                        pendingNativePhoto = request;
                        photoDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                        const HRESULT result = engine.takePhoto(request, serial);
                        if (FAILED(result)) {
                            native->photoRequest = 0;
                            cameraNativePhotoTaken(session, generation, request, result);
                            pendingNativePhoto = 0;
                        }
                    }
                    for (unsigned count = 0; count < 8 && cameraDrainOne(session); ++count) {
                    }
                    if (terminal) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (!running && now >= startDeadline) {
                        cameraNativeFailure(session, generation, "Camera did not produce a valid preview frame");
                        break;
                    }
                    if (pendingNativePhoto && now >= photoDeadline) {
                        if (native->photoRequest.load()) {
                            // 没有完成事件就不能把迟到事件配给下一次快门；停止本代而非重用含糊状态。
                            cameraNativeFailure(session, generation, "Native camera photo did not finish");
                            break;
                        }
                        if (stillPending)
                            cameraNativePhotoTaken(session, generation, pendingNativePhoto,
                                                   HRESULT_FROM_WIN32(WAIT_TIMEOUT));
                        pendingNativePhoto = 0;
                    }
                    std::unique_lock<std::mutex> lock(session->mutex);
                    session->changed.wait_for(lock, std::chrono::milliseconds(10));
                }
                // 错误通知仍在本串行交付线程；stop 清空闸门后 drain 不会再回调。
                for (unsigned count = 0; count < 8 && cameraDrainOne(session); ++count) {
                }
            }
        } catch (...) {
            cameraNativeFailure(session, generation, "Camera worker failed unexpectedly");
            for (unsigned count = 0; count < 8 && cameraDrainOne(session); ++count) {
            }
        }
        for (unsigned count = 0; count < 8 && cameraDrainOne(session); ++count) {
        }
        native->closed = true;
        workerId = 0;
        workerActive = false;
    }
};

MaiCameraCapture::MaiCameraCapture(Callbacks callbacks) : mRuntime(std::make_shared<Runtime>(std::move(callbacks))) {}

MaiCameraCapture::~MaiCameraCapture() {
    stop();
}

bool MaiCameraCapture::isAvailable() {
    bool available = false;
    try {
        // 枚举对象不激活 sensor；隔离到短生命周期 MTA，避免改变 Qt/OLE 已有的 STA。
        std::thread enumerate([&] {
            MaiCameraMfApartment apartment;
            if (SUCCEEDED(apartment.result())) cameraEnumerate(nullptr, available);
        });
        enumerate.join();
    } catch (...) {
        return false;
    }
    return available;
}

bool MaiCameraCapture::start() {
    return mRuntime->start();
}

bool MaiCameraCapture::capturePhoto(std::uint64_t requestId) {
    return mRuntime->capture(requestId);
}

void MaiCameraCapture::stop() {
    mRuntime->stop();
}

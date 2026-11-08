// 文件作用：用合成 MF 样本核对 Windows 相机状态、像素、原生照片交付和停止屏障。
// 直接包含私有实现仅供独立离线 target；不调用枚举、start、Initialize 或任何摄像头设备。
#define MAI_CAMERA_CAPTURE_OFFLINE_TESTS 1
#include "../MaiCameraCaptureWindows.cpp"

#include <objidl.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <thread>

namespace {

int failures = 0;
#define CHECK_CAMERA(expression)                                                      \
    do {                                                                              \
        if (!(expression)) {                                                          \
            std::fprintf(stderr, "FAIL camera line %d: %s\n", __LINE__, #expression); \
            ++failures;                                                               \
        }                                                                             \
    } while (false)

bool waitUntil(const std::function<bool()>& condition, unsigned milliseconds = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

ComPtr<IMFSample> previewSample(bool bottomUp) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMF2DBuffer> surface;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreate2DMediaBuffer(2, 2, MFVideoFormat_RGB32.Data1, bottomUp, &buffer)) ||
        FAILED(buffer.As(&surface)) || FAILED(MFCreateSample(&sample)))
        return {};
    BYTE* top = nullptr;
    LONG stride = 0;
    if (FAILED(surface->Lock2D(&top, &stride))) return {};
    for (int row = 0; row < 2; ++row) {
        BYTE* pixels = top + static_cast<std::ptrdiff_t>(stride) * row;
        for (int column = 0; column < 2; ++column) {
            pixels[column * 4 + 0] = row ? 220 : 10;
            pixels[column * 4 + 1] = 20;
            pixels[column * 4 + 2] = row ? 10 : 220;
            pixels[column * 4 + 3] = 0;
        }
    }
    surface->Unlock2D();
    if (FAILED(sample->AddBuffer(buffer.Get()))) return {};
    return sample;
}

ComPtr<IMFSample> bytesSample(const std::vector<std::uint8_t>& bytes, bool split = false) {
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return {};
    const std::size_t pieces = split ? 3 : 1;
    for (std::size_t piece = 0; piece < pieces; ++piece) {
        const std::size_t begin = bytes.size() * piece / pieces;
        const std::size_t end = bytes.size() * (piece + 1) / pieces;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(end - begin), &buffer))) return {};
        BYTE* data = nullptr;
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return {};
        if (end != begin) std::memcpy(data, bytes.data() + begin, end - begin);
        buffer->Unlock();
        if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(end - begin))) ||
            FAILED(sample->AddBuffer(buffer.Get())))
            return {};
    }
    return sample;
}

std::vector<std::uint8_t> jpegFixture() {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(16, 16)))
        return {};
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) return {};
    std::vector<BYTE> pixels(16 * 16 * 3);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 3) {
        pixels[offset] = 10;
        pixels[offset + 1] = 20;
        pixels[offset + 2] = 220;
    }
    if (FAILED(frame->WritePixels(16, 16 * 3, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    STATSTG status{};
    if (FAILED(stream->Stat(&status, STATFLAG_NONAME)) || status.cbSize.QuadPart > 1024 * 1024) return {};
    LARGE_INTEGER beginning{};
    if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.cbSize.QuadPart));
    ULONG copied = 0;
    if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &copied)) || copied != bytes.size())
        return {};
    return bytes;
}

bool isFixtureJpeg(const std::vector<std::uint8_t>& bytes) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (bytes.empty() ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return false;
    UINT width = 0, height = 0;
    return SUCCEEDED(frame->GetSize(&width, &height)) && width == 16 && height == 16;
}

bool fixturePixelsDecode(const std::vector<std::uint8_t>& bytes) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    std::uint8_t pixels[16 * 16 * 4]{};
    return !bytes.empty() &&
           SUCCEEDED(
               CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
           SUCCEEDED(factory->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
           SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
           SUCCEEDED(decoder->GetFrame(0, &frame)) &&
           SUCCEEDED(frame->CopyPixels(nullptr, 16 * 4, sizeof(pixels), pixels));
}

std::vector<std::uint8_t> missingHuffmanTableJpeg(const std::vector<std::uint8_t>& jpeg) {
    // probe 已在本平台证实：table 2 不存在时尺寸仍可读取，但像素解码明确失败。
    for (std::size_t marker = 0; marker + 5 < jpeg.size(); ++marker) {
        if (jpeg[marker] != 0xff || jpeg[marker + 1] != 0xda) continue;
        const std::size_t segment = (static_cast<std::size_t>(jpeg[marker + 2]) << 8) | jpeg[marker + 3];
        const std::size_t entropy = marker + 2 + segment;
        const std::size_t components = jpeg[marker + 4];
        if (!components || entropy > jpeg.size() || marker + 5 + 2 * components + 3 != entropy) return {};
        auto broken = jpeg;
        for (std::size_t component = 0; component < components; ++component) broken[marker + 6 + 2 * component] = 0x22;
        return broken;
    }
    return {};
}

struct JpegIntegrityProbe {
    std::size_t attempted = 0;
    std::size_t metadataReadable = 0;
    const char* corruption = "none";
    std::vector<std::uint8_t> broken;
};

JpegIntegrityProbe corruptJpegEntropy(const std::vector<std::uint8_t>& jpeg) {
    // 有界探测当前 WIC 的延迟解码边界；只有元数据成功且独立像素解码失败才构成生产行为 RED。
    JpegIntegrityProbe probe;
    const auto consider = [&](std::vector<std::uint8_t> candidate, const char* corruption) {
        ++probe.attempted;
        if (!isFixtureJpeg(candidate)) return false;
        ++probe.metadataReadable;
        if (fixturePixelsDecode(candidate)) return false;
        probe.corruption = corruption;
        probe.broken = std::move(candidate);
        return true;
    };
    for (std::size_t marker = 0; marker + 4 < jpeg.size(); ++marker) {
        if (jpeg[marker] != 0xff || jpeg[marker + 1] != 0xda) continue;
        const std::size_t segment = (static_cast<std::size_t>(jpeg[marker + 2]) << 8) | jpeg[marker + 3];
        const std::size_t entropy = marker + 2 + segment;
        if (entropy >= jpeg.size() - 2) continue;
        const auto attempts = std::min<std::size_t>(64, jpeg.size() - entropy - 2);
        for (std::size_t remaining = 0; remaining < attempts; ++remaining) {
            std::vector<std::uint8_t> broken(jpeg.begin(), jpeg.begin() + entropy + remaining);
            broken.push_back(0xff);
            broken.push_back(0xd9);
            if (consider(std::move(broken), "truncated entropy")) return probe;
        }
        const std::size_t components = jpeg[marker + 4];
        if (!components || marker + 5 + 2 * components + 3 != entropy) continue;
        for (std::uint8_t selector : {0x11, 0x22, 0x33, 0x30, 0x03}) {
            auto broken = jpeg;
            for (std::size_t component = 0; component < components; ++component)
                broken[marker + 6 + 2 * component] = selector;
            if (consider(std::move(broken), "missing scan Huffman table")) return probe;
        }
        for (std::size_t parameter = entropy - 3; parameter < entropy; ++parameter) {
            for (std::uint8_t value : {0x01, 0x3f, 0xf0, 0xff}) {
                auto broken = jpeg;
                broken[parameter] = value;
                if (consider(std::move(broken), "invalid scan parameters")) return probe;
            }
        }
        for (std::uint8_t value : {0x00, 0xff}) {
            auto broken = jpeg;
            std::fill(broken.begin() + entropy, broken.end() - 2, value);
            if (consider(std::move(broken), "invalid entropy payload")) return probe;
        }
    }
    for (std::size_t marker = 0; marker + 4 < jpeg.size(); ++marker) {
        if (jpeg[marker] != 0xff || jpeg[marker + 1] != 0xc0) continue;
        auto broken = jpeg;
        broken[marker + 1] = 0xc2;
        if (consider(std::move(broken), "progressive frame with baseline scan")) return probe;
    }
    return probe;
}

struct Recorder {
    std::vector<MaiCameraCapture::State> states;
    std::vector<MaiCameraCapture::Frame> frames;
    std::vector<MaiCameraCapture::Photo> photos;

    std::shared_ptr<MaiCameraWindowsSession> session() {
        MaiCameraCapture::Callbacks callbacks;
        callbacks.state = [this](auto state, const std::string&) { states.push_back(state); };
        callbacks.frame = [this](const auto& frame) { frames.push_back(frame); };
        callbacks.photo = [this](const auto& photo) { photos.push_back(photo); };
        return std::make_shared<MaiCameraWindowsSession>(std::move(callbacks));
    }
};

void drain(const std::shared_ptr<MaiCameraWindowsSession>& session) {
    for (unsigned iteration = 0; iteration < 32 && cameraDrainOne(session); ++iteration) {
    }
}

void test_preview_layout_and_owned_copy() {
    for (bool bottomUp : {false, true}) {
        auto sample = previewSample(bottomUp);
        CHECK_CAMERA(sample != nullptr);
        if (!sample) continue;
        MaiCameraCapture::Frame frame;
        std::string error;
        const bool copied = cameraCopyPreviewSample(sample.Get(), 2, 2, bottomUp ? -8 : 8, frame, error);
        CHECK_CAMERA(copied && error.empty());
        if (!copied) continue;
        sample.Reset();  // 输出必须拥有内存，不能引用 MF buffer。
        CHECK_CAMERA(frame.width == 2 && frame.height == 2 && frame.stride >= 8);
        CHECK_CAMERA(frame.pixels.size() >= static_cast<std::size_t>(frame.stride) * 2);
        if (frame.pixels.size() < static_cast<std::size_t>(frame.stride) * 2) continue;
        CHECK_CAMERA(frame.pixels[0] == 10 && frame.pixels[2] == 220);
        CHECK_CAMERA(frame.pixels[frame.stride] == 220 && frame.pixels[frame.stride + 2] == 10);
        for (unsigned row = 0; row < 2; ++row)
            for (unsigned column = 0; column < 2; ++column)
                CHECK_CAMERA(frame.pixels[frame.stride * row + column * 4 + 3] == 255);
    }
    MaiCameraCapture::Frame empty;
    std::string error;
    CHECK_CAMERA(!cameraCopyPreviewSample(nullptr, 2, 2, 8, empty, error));
    auto shortSample = bytesSample({1, 2, 3, 4});
    CHECK_CAMERA(!cameraCopyPreviewSample(shortSample.Get(), 2, 2, 8, empty, error));
    CHECK_CAMERA(!cameraCopyPreviewSample(shortSample.Get(), 0xffffffffU, 2, 8, empty, error));
    CHECK_CAMERA(empty.pixels.empty());
}

void test_native_jpeg_bytes(const std::vector<std::uint8_t>& expected) {
    auto sample = bytesSample(expected, true);
    CHECK_CAMERA(sample != nullptr && isFixtureJpeg(expected));
    std::vector<std::uint8_t> bytes;
    std::string error;
    CHECK_CAMERA(cameraCopyPhotoSample(sample.Get(), bytes, error));
    CHECK_CAMERA(bytes == expected && error.empty() && isFixtureJpeg(bytes));
    auto corrupt = bytesSample({0xff, 0xd8, 0xff, 0xe0});
    CHECK_CAMERA(!cameraCopyPhotoSample(corrupt.Get(), bytes, error));
    CHECK_CAMERA(bytes.empty() && !error.empty());
}

void test_corrupt_jpeg_payload_is_not_photo_success(const std::vector<std::uint8_t>& broken) {
    CHECK_CAMERA(isFixtureJpeg(broken) && !fixturePixelsDecode(broken));
    auto sample = bytesSample(broken, true);
    std::vector<std::uint8_t> copied;
    std::string error;
    CHECK_CAMERA(!cameraCopyPhotoSample(sample.Get(), copied, error));
    CHECK_CAMERA(copied.empty() && !error.empty());
}

void makeRunning(const std::shared_ptr<MaiCameraWindowsSession>& session, std::uint64_t generation) {
    cameraNativeInitialized(session, generation, S_OK);
    cameraNativePreviewStarted(session, generation, S_OK);
    auto sample = previewSample(false);
    cameraReceivePreview(session, generation, sample.Get(), 2, 2, 8);
}

void test_first_frame_readiness_and_terminal_latch() {
    Recorder record;
    auto session = record.session();
    const auto generation = cameraBeginSession(session);
    cameraNativeInitialized(session, generation, S_OK);
    cameraNativePreviewStarted(session, generation, S_OK);
    cameraReceivePreview(session, generation, nullptr, 2, 2, 8);
    drain(session);
    CHECK_CAMERA(record.states.size() == 1 && record.states[0] == MaiCameraCapture::State::Starting);
    CHECK_CAMERA(record.frames.empty());
    makeRunning(session, generation);
    drain(session);
    CHECK_CAMERA(record.states.size() == 2 && record.states.back() == MaiCameraCapture::State::Running);
    CHECK_CAMERA(record.frames.size() == 1 && record.frames[0].sequence > 0);
    cameraNativeFailure(session, generation, "controlled camera interruption");
    makeRunning(session, generation);
    drain(session);
    CHECK_CAMERA(record.states.size() == 3 && record.states.back() == MaiCameraCapture::State::Error);
    CHECK_CAMERA(record.frames.size() == 1);
    CHECK_CAMERA(!cameraBeginPhoto(session, generation, 1));
    const auto restarted = cameraBeginSession(session);
    CHECK_CAMERA(restarted > generation);
    makeRunning(session, generation);  // 旧代回调不能启用新代快门。
    drain(session);
    CHECK_CAMERA(record.frames.size() == 1);
    makeRunning(session, restarted);
    drain(session);
    CHECK_CAMERA(record.frames.size() == 2 && record.frames.back().sequence > record.frames.front().sequence);
}

void test_photo_order_identity_and_errors(const std::vector<std::uint8_t>& jpeg) {
    Recorder record;
    auto session = record.session();
    const auto generation = cameraBeginSession(session);
    makeRunning(session, generation);
    drain(session);
    auto sample = bytesSample(jpeg, true);
    CHECK_CAMERA(!cameraBeginPhoto(session, generation, 0));
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 11));
    CHECK_CAMERA(!cameraBeginPhoto(session, generation, 12));
    cameraReceivePhoto(session, generation, 11, sample.Get());
    drain(session);
    CHECK_CAMERA(record.photos.empty());
    cameraNativePhotoTaken(session, generation, 11, S_OK);
    drain(session);
    CHECK_CAMERA(record.photos.size() == 1 && record.photos.back().requestId == 11 &&
                 record.photos.back().jpeg == jpeg && record.photos.back().error.empty());
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 12));
    cameraReceivePhoto(session, generation, 11, sample.Get());
    cameraNativePhotoTaken(session, generation, 11, S_OK);
    cameraNativePhotoTaken(session, generation, 12, S_OK);
    drain(session);
    CHECK_CAMERA(record.photos.size() == 1);
    cameraReceivePhoto(session, generation, 12, sample.Get());
    drain(session);
    CHECK_CAMERA(record.photos.size() == 2 && record.photos.back().requestId == 12);
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 13));
    cameraReceivePhoto(session, generation, 13, sample.Get());
    cameraNativePhotoTaken(session, generation, 13, E_ACCESSDENIED);
    drain(session);
    CHECK_CAMERA(record.photos.size() == 3 && record.photos.back().requestId == 13 &&
                 record.photos.back().jpeg.empty() && !record.photos.back().error.empty());
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 14));
    auto corrupt = bytesSample({0xff, 0xd8, 0xff, 0xe0});
    cameraReceivePhoto(session, generation, 14, corrupt.Get());
    cameraNativePhotoTaken(session, generation, 14, S_OK);
    drain(session);
    CHECK_CAMERA(record.photos.size() == 4 && record.photos.back().requestId == 14 &&
                 record.photos.back().jpeg.empty() && !record.photos.back().error.empty());
}

void test_stop_gate_and_callback_exception() {
    std::atomic<bool> entered{false}, release{false}, stopped{false};
    std::atomic<unsigned> calls{0};
    MaiCameraCapture::Callbacks callbacks;
    callbacks.frame = [&](const MaiCameraCapture::Frame&) {
        ++calls;
        entered = true;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto session = std::make_shared<MaiCameraWindowsSession>(std::move(callbacks));
    const auto generation = cameraBeginSession(session);
    makeRunning(session, generation);
    std::thread delivery([&] { drain(session); });
    const bool delivering = waitUntil([&] { return entered.load(); }, 300);
    CHECK_CAMERA(delivering);
    std::thread stopper([&] {
        cameraCloseSession(session);
        stopped = true;
    });
    if (delivering) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_CAMERA(!stopped.load());
    }
    release = true;
    delivery.join();
    stopper.join();
    CHECK_CAMERA(stopped.load());
    makeRunning(session, generation);
    drain(session);
    CHECK_CAMERA(calls.load() == 1);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        CHECK_CAMERA(session->closing && session->generation > generation && !session->preview);
    }
    MaiCameraCapture::Callbacks throwing;
    throwing.frame = [](const auto&) { throw std::runtime_error("controlled callback exception"); };
    auto isolated = std::make_shared<MaiCameraWindowsSession>(std::move(throwing));
    const auto active = cameraBeginSession(isolated);
    makeRunning(isolated, active);
    bool escaped = false;
    try {
        drain(isolated);
    } catch (...) {
        escaped = true;
    }
    CHECK_CAMERA(!escaped);
    cameraCloseSession(isolated);
}

struct NativeCloseEvidence {
    std::atomic<unsigned> order{0};
    std::atomic<unsigned> sampleReleased{0};
    std::atomic<unsigned> engineReleased{0};
    std::atomic<unsigned> stopCalls{0};
    std::atomic<DWORD> sampleThread{0};
};

class TrackedCameraBuffer final : public IMFMediaBuffer {
public:
    explicit TrackedCameraBuffer(NativeCloseEvidence& evidence) : mEvidence(evidence) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFMediaBuffer)) return E_NOINTERFACE;
        *output = static_cast<IMFMediaBuffer*>(this);
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
    HRESULT STDMETHODCALLTYPE Lock(BYTE** data, DWORD* maximum, DWORD* current) override {
        if (!data) return E_POINTER;
        *data = mPixels;
        if (maximum) *maximum = sizeof(mPixels);
        if (current) *current = mLength;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentLength(DWORD* length) override {
        if (!length) return E_POINTER;
        *length = mLength;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentLength(DWORD length) override {
        if (length > sizeof(mPixels)) return E_INVALIDARG;
        mLength = length;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD* length) override {
        if (!length) return E_POINTER;
        *length = sizeof(mPixels);
        return S_OK;
    }

private:
    ~TrackedCameraBuffer() {
        mEvidence.sampleThread = GetCurrentThreadId();
        mEvidence.sampleReleased = ++mEvidence.order;
    }
    NativeCloseEvidence& mEvidence;
    std::atomic<ULONG> mReferences{1};
    BYTE mPixels[16]{};
    DWORD mLength = sizeof(mPixels);
};

class FailingCameraEngine final : public IMFCaptureEngine {
public:
    FailingCameraEngine(NativeCloseEvidence& evidence, std::weak_ptr<MaiCameraNativeMailbox> mailbox,
                        HRESULT stopResult)
        : mEvidence(evidence), mMailbox(std::move(mailbox)), mStopResult(stopResult) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFCaptureEngine)) return E_NOINTERFACE;
        *output = static_cast<IMFCaptureEngine*>(this);
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
    HRESULT STDMETHODCALLTYPE Initialize(IMFCaptureEngineOnEventCallback*, IMFAttributes*, IUnknown*,
                                         IUnknown*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StartPreview() override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StopPreview() override {
        ++mEvidence.stopCalls;
        if (SUCCEEDED(mStopResult)) {
            if (const auto mailbox = mMailbox.lock()) {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                MaiCameraNativeEvent stopped;
                stopped.type = MF_CAPTURE_ENGINE_PREVIEW_STOPPED;
                mailbox->events.push_back(stopped);
            }
        }
        return mStopResult;
    }
    HRESULT STDMETHODCALLTYPE StartRecord() override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StopRecord(BOOL, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE TakePhoto() override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetSink(MF_CAPTURE_ENGINE_SINK_TYPE, IMFCaptureSink** output) override {
        if (output) *output = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetSource(IMFCaptureSource** output) override {
        if (output) *output = nullptr;
        return E_NOTIMPL;
    }

private:
    ~FailingCameraEngine() {
        mEvidence.engineReleased = ++mEvidence.order;
    }
    NativeCloseEvidence& mEvidence;
    std::weak_ptr<MaiCameraNativeMailbox> mMailbox;
    HRESULT mStopResult;
    std::atomic<ULONG> mReferences{1};
};

// 只给独立测试 target 的友元实现，不增加公共 API，也不创建任何实际 MF 引擎。
struct MaiCameraWindowsTestAccess {
    static void install(MaiCameraWindowsNative& camera, IMFCaptureEngine* engine, bool previewRequested) {
        camera.mEngine.Attach(engine);
        camera.mPreviewRequested = previewRequested;
    }
};

void test_every_native_close_path_releases_samples_on_owner() {
    for (int mode = 0; mode < 3; ++mode) {
        NativeCloseEvidence evidence;
        auto mailbox = std::make_shared<MaiCameraNativeMailbox>();
        ComPtr<IMFSample> sample;
        CHECK_CAMERA(SUCCEEDED(MFCreateSample(&sample)));
        if (!sample) continue;
        auto* buffer = new TrackedCameraBuffer(evidence);
        CHECK_CAMERA(SUCCEEDED(sample->AddBuffer(buffer)));
        buffer->Release();
        mailbox->preview.sample = sample;
        mailbox->photo.sample = sample;
        mailbox->photoWaiting = true;
        sample.Reset();
        std::atomic<DWORD> ownerId{0};
        std::thread owner([&] {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ownerId = GetCurrentThreadId();
            {
                MaiCameraWindowsNative camera(mailbox);
                MaiCameraWindowsTestAccess::install(
                    camera, new FailingCameraEngine(evidence, mailbox, mode == 2 ? S_OK : E_FAIL), mode != 0);
                camera.close();
            }
            if (SUCCEEDED(initialized)) CoUninitialize();
        });
        owner.join();
        CHECK_CAMERA(evidence.stopCalls.load() == (mode == 0 ? 0U : 1U));
        CHECK_CAMERA(evidence.sampleReleased.load() != 0);
        CHECK_CAMERA(evidence.sampleReleased.load() < evidence.engineReleased.load());
        CHECK_CAMERA(evidence.sampleThread.load() == ownerId.load());
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            CHECK_CAMERA(!mailbox->preview.sample && !mailbox->photo.sample && !mailbox->photoWaiting);
        }
        mailbox.reset();  // RED 也在测试的 MFShutdown 前释放残留的合成样本。
    }
}

void test_stale_native_photo_cannot_occupy_new_request(const std::vector<std::uint8_t>& jpeg) {
    Recorder record;
    auto session = record.session();
    const auto generation = cameraBeginSession(session);
    makeRunning(session, generation);
    drain(session);
    auto mailbox = std::make_shared<MaiCameraNativeMailbox>();
    mailbox->session = session;
    mailbox->generation = generation;
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 31));
    MaiCameraNativeSample oldFormat;
    oldFormat.request = 31;
    oldFormat.serial = session->photoSerial;
    ComPtr<IMFCaptureEngineOnSampleCallback> oldCallback;
    oldCallback.Attach(new MaiCameraEngineSamples(mailbox, oldFormat));
    auto sample = bytesSample(jpeg, true);
    std::atomic<bool> checked{false}, resume{false};
    std::atomic<unsigned> checks{0};
    cameraSampleValidationTestBarrier = [&] {
        if (++checks == 1) {
            checked = true;
            while (!resume.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    std::thread delayed([&] { oldCallback->OnSample(sample.Get()); });
    const bool paused = waitUntil([&] { return checked.load(); });
    CHECK_CAMERA(paused);
    cameraNativePhotoTaken(session, generation, 31, E_FAIL);
    drain(session);
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 32));
    MaiCameraNativeSample nextFormat;
    nextFormat.request = 32;
    nextFormat.serial = session->photoSerial;
    resume = true;
    delayed.join();
    cameraSampleValidationTestBarrier = {};
    ComPtr<IMFCaptureEngineOnSampleCallback> nextCallback;
    nextCallback.Attach(new MaiCameraEngineSamples(mailbox, nextFormat));
    nextCallback->OnSample(sample.Get());
    MaiCameraNativeSample received;
    {
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        CHECK_CAMERA(mailbox->photoWaiting && mailbox->photo.request == 32 &&
                     mailbox->photo.serial == nextFormat.serial);
        received = std::move(mailbox->photo);
        mailbox->photoWaiting = false;
    }
    if (received.serial == nextFormat.serial)
        cameraReceivePhoto(session, generation, received.request, received.sample.Get());
    cameraNativePhotoTaken(session, generation, 32, S_OK);
    drain(session);
    CHECK_CAMERA(record.photos.size() == 2 && record.photos.back().requestId == 32 &&
                 record.photos.back().jpeg == jpeg && record.photos.back().error.empty());
    // 已存在的过期槽同样不能挡住当前请求，不能仅补一个提前检查来掩盖 mailbox 状态。
    CHECK_CAMERA(cameraBeginPhoto(session, generation, 33));
    MaiCameraNativeSample finalFormat;
    finalFormat.request = 33;
    finalFormat.serial = session->photoSerial;
    ComPtr<IMFCaptureEngineOnSampleCallback> finalCallback;
    finalCallback.Attach(new MaiCameraEngineSamples(mailbox, finalFormat));
    {
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        mailbox->photo = oldFormat;
        mailbox->photo.sample = sample;
        mailbox->photoWaiting = true;
    }
    finalCallback->OnSample(sample.Get());
    {
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        CHECK_CAMERA(mailbox->photo.request == 33 && mailbox->photo.serial == finalFormat.serial);
    }
    cameraCloseSession(session);
}

void test_public_cold_owner_does_not_activate() {
    std::atomic<unsigned> callbacks{0};
    {
        MaiCameraCapture::Callbacks output;
        output.state = [&](auto, const auto&) { ++callbacks; };
        output.photo = [&](const auto&) { ++callbacks; };
        MaiCameraCapture camera(std::move(output));
        CHECK_CAMERA(!camera.capturePhoto(1));
        camera.stop();
        camera.stop();
    }
    CHECK_CAMERA(callbacks.load() == 0);
}

}  // namespace

int main(int argc, char** argv) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::fprintf(stderr, "FAIL camera offline fixture initialization\n");
        return 2;
    }
    const auto jpeg = jpegFixture();
    if (!isFixtureJpeg(jpeg)) {
        std::fprintf(stderr, "FAIL synthetic WIC JPEG fixture\n");
        MFShutdown();
        CoUninitialize();
        return 2;
    }
    if (argc == 2 && std::strcmp(argv[1], "--jpeg-integrity-probe") == 0) {
        CHECK_CAMERA(fixturePixelsDecode(jpeg));
        const auto probe = corruptJpegEntropy(jpeg);
        std::printf("JPEG integrity probe: attempted=%zu metadata-readable=%zu candidate=%s\n", probe.attempted,
                    probe.metadataReadable, probe.corruption);
        if (!probe.broken.empty()) test_corrupt_jpeg_payload_is_not_photo_success(probe.broken);
        MFShutdown();
        CoUninitialize();
        if (probe.broken.empty()) {
            std::fprintf(stderr,
                         "INCONCLUSIVE: no metadata-readable JPEG failed independent pixel decoding; "
                         "no production rejection claim\n");
            return 2;
        }
        std::printf("JPEG integrity behavior: failures=%d (synthetic bytes; no device access)\n", failures);
        return failures ? 1 : 0;
    }
    test_preview_layout_and_owned_copy();
    test_native_jpeg_bytes(jpeg);
    const auto brokenJpeg = missingHuffmanTableJpeg(jpeg);
    CHECK_CAMERA(!brokenJpeg.empty());
    if (!brokenJpeg.empty()) test_corrupt_jpeg_payload_is_not_photo_success(brokenJpeg);
    test_first_frame_readiness_and_terminal_latch();
    test_photo_order_identity_and_errors(jpeg);
    test_stop_gate_and_callback_exception();
    test_every_native_close_path_releases_samples_on_owner();
    test_stale_native_photo_cannot_occupy_new_request(jpeg);
    test_public_cold_owner_does_not_activate();
    MFShutdown();
    CoUninitialize();
    std::printf("Windows camera offline tests: failures=%d (synthetic samples; no device access)\n", failures);
    return failures ? 1 : 0;
}

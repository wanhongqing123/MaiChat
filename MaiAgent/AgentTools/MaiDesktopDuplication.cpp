// 文件作用：Windows 整屏使用 Desktop Duplication，避免常驻 WGC 改变系统指针合成。
// 直接依赖：D3D11、DXGI；不依赖 Graphics 的全局设备生命周期，不写系统指针状态。
#include "MaiDesktopDuplication.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include <windows.h>

#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

bool maiCopyDesktopBgra(const std::uint8_t* source, std::size_t sourceBytes, std::uint32_t width,
                        std::uint32_t height, std::size_t stride, MaiDesktopRotation rotation,
                        std::vector<std::uint8_t>& pixels, std::uint32_t& outputWidth,
                        std::uint32_t& outputHeight) {
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (!source || !width || !height || width > maximum / 4 ||
        rotation < MaiDesktopRotation::Identity || rotation > MaiDesktopRotation::Clockwise270)
        return false;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    if (stride < rowBytes || height > maximum / rowBytes ||
        (height > 1 && stride > (maximum - rowBytes) / (height - 1)) ||
        sourceBytes < stride * (height - 1) + rowBytes || rowBytes * height > pixels.max_size())
        return false;
    const bool swapsAxes =
        rotation == MaiDesktopRotation::Clockwise90 || rotation == MaiDesktopRotation::Clockwise270;
    const std::uint32_t resultWidth = swapsAxes ? height : width;
    const std::uint32_t resultHeight = swapsAxes ? width : height;
    std::vector<std::uint8_t> result(rowBytes * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t targetX = x, targetY = y;
            switch (rotation) {
                case MaiDesktopRotation::Identity: break;
                case MaiDesktopRotation::Clockwise90:
                    targetX = height - 1 - y;
                    targetY = x;
                    break;
                case MaiDesktopRotation::Clockwise180:
                    targetX = width - 1 - x;
                    targetY = height - 1 - y;
                    break;
                case MaiDesktopRotation::Clockwise270:
                    targetX = y;
                    targetY = width - 1 - x;
                    break;
            }
            auto* destination =
                result.data() + (static_cast<std::size_t>(targetY) * resultWidth + targetX) * 4;
            const auto* pixel = source + stride * y + static_cast<std::size_t>(x) * 4;
            std::memcpy(destination, pixel, 3);
            destination[3] = 255;
        }
    }
    pixels = std::move(result);
    outputWidth = resultWidth;
    outputHeight = resultHeight;
    return true;
}

std::int64_t maiDesktopSourceProducedAtMs(std::int64_t sourceQpc, std::int64_t unix100ns,
                                          std::int64_t qpcAfter, std::int64_t frequency) {
    constexpr std::int64_t ticksPerSecond = 10000000;
    if (sourceQpc <= 0 || unix100ns <= 0 || frequency <= 0 || qpcAfter < sourceQpc) return 0;
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    const auto age = qpcAfter - sourceQpc;
    const auto seconds = age / frequency;
    const auto remainder = age % frequency;
    if (seconds > maximum / ticksPerSecond || remainder > maximum / ticksPerSecond) return 0;
    const auto scaled = remainder * ticksPerSecond;
    const auto fraction = scaled / frequency + (scaled % frequency != 0);
    const auto whole = seconds * ticksPerSecond;
    if (whole > maximum - fraction || whole + fraction >= unix100ns) return 0;
    return (unix100ns - whole - fraction) / 10000;
}

bool maiCopyDesktopScRgb(const std::uint8_t* source, std::size_t sourceBytes, std::uint32_t width,
                         std::uint32_t height, std::size_t stride, MaiDesktopRotation rotation,
                         double sdrWhiteNits, std::vector<std::uint8_t>& pixels,
                         std::uint32_t& outputWidth, std::uint32_t& outputHeight) {
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (!source || !width || !height || width > maximum / 8 || !std::isfinite(sdrWhiteNits) ||
        sdrWhiteNits <= 0 || !std::isfinite(80.0 / sdrWhiteNits) ||
        rotation < MaiDesktopRotation::Identity || rotation > MaiDesktopRotation::Clockwise270)
        return false;
    const std::size_t sourceRowBytes = static_cast<std::size_t>(width) * 8;
    const std::size_t outputRowBytes = static_cast<std::size_t>(width) * 4;
    if (stride < sourceRowBytes || height > maximum / outputRowBytes ||
        (height > 1 && stride > (maximum - sourceRowBytes) / (height - 1)) ||
        sourceBytes < stride * (height - 1) + sourceRowBytes ||
        outputRowBytes * height > pixels.max_size())
        return false;
    // 半浮点仅有 65536 个位型，先建一次查表，避免逐个桌面像素执行 pow。
    // 解码复用 Windows SDK DirectXMath，包含次正规数、NaN 和无穷的 IEEE 754 处理。
    std::array<std::uint8_t, 65536> encoded{};
    const double multiplier = 80.0 / sdrWhiteNits;
    for (std::size_t bits = 0; bits < encoded.size(); ++bits) {
        const double linear = DirectX::PackedVector::XMConvertHalfToFloat(
                                  static_cast<DirectX::PackedVector::HALF>(bits)) *
                              multiplier;
        if (!(linear > 0)) continue;
        if (linear >= 1) {
            encoded[bits] = 255;
            continue;
        }
        // 与 OBS color.effect 相同的 sRGB 分段传递函数；不是简单的 gamma 2.2。
        const double nonlinear =
            linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        encoded[bits] = static_cast<std::uint8_t>(std::lround(nonlinear * 255.0));
    }
    std::vector<std::uint8_t> unrotated(outputRowBytes * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            const auto* pixel = source + stride * row + static_cast<std::size_t>(column) * 8;
            auto* destination =
                unrotated.data() + outputRowBytes * row + static_cast<std::size_t>(column) * 4;
            for (unsigned channel = 0; channel < 3; ++channel) {
                std::uint16_t half = 0;
                std::memcpy(&half, pixel + channel * 2, sizeof(half));
                destination[2 - channel] = encoded[half];
            }
            destination[3] = 255;
        }
    }
    return maiCopyDesktopBgra(unrotated.data(), unrotated.size(), width, height, outputRowBytes,
                              rotation, pixels, outputWidth, outputHeight);
}

static MaiError desktopHresultError(const char* operation, HRESULT result) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
    return MaiError::make(MaiErrorCode::Internal,
                          std::string(operation) + " failed (0x" + code + ")");
}

// 与 OBS GetSdrMaxNits 相同：通过当前 HMONITOR 的 GDI source 找实际 target，
// 只读该显示器白电平，不把主屏、另一个 adapter 或 HDR 关闭前的值当作当前配置。
static MaiError desktopSdrWhiteNits(IDXGIOutput6* output, const DXGI_OUTPUT_DESC& description,
                                    double& nits) {
    DXGI_OUTPUT_DESC1 color{};
    const HRESULT result = output->GetDesc1(&color);
    if (FAILED(result)) return desktopHresultError("desktop color GetDesc1", result);
    if (color.Monitor != description.Monitor)
        return MaiError::make(MaiErrorCode::NotFound, "desktop color monitor changed");
    // SDR Advanced Color 的 1.0 即显示器的 SDR 白；HDR 才应用用户配置的绝对白电平。
    if (color.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        nits = 80;
        return {};
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        UINT pathCount = 0, modeCount = 0;
        LONG status = ::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS)
            return desktopHresultError("GetDisplayConfigBufferSizes", HRESULT_FROM_WIN32(status));
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        status = ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                      modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS)
            return desktopHresultError("QueryDisplayConfig", HRESULT_FROM_WIN32(status));
        for (UINT index = 0; index < pathCount; ++index) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[index].sourceInfo.adapterId;
            source.header.id = paths[index].sourceInfo.id;
            if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                ::lstrcmpiW(source.viewGdiDeviceName, description.DeviceName) != 0)
                continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = paths[index].targetInfo.adapterId;
            white.header.id = paths[index].targetInfo.id;
            status = ::DisplayConfigGetDeviceInfo(&white.header);
            if (status != ERROR_SUCCESS)
                return desktopHresultError("desktop SDR white DisplayConfigGetDeviceInfo",
                                           HRESULT_FROM_WIN32(status));
            if (!white.SDRWhiteLevel)
                return MaiError::make(MaiErrorCode::Internal, "desktop SDR white level is zero");
            nits = static_cast<double>(white.SDRWhiteLevel) * 80.0 / 1000.0;
            return {};
        }
        return MaiError::make(MaiErrorCode::NotFound, "desktop SDR white monitor is disconnected");
    }
    return MaiError::make(MaiErrorCode::Busy, "desktop configuration changed during color query");
}

bool maiDesktopFrameUpdatesCache(bool hasFrame, std::int64_t previousSourceQpc,
                                 std::int64_t sourceQpc) {
    return sourceQpc > 0 && (!hasFrame || sourceQpc > previousSourceQpc);
}

struct MaiDesktopDuplication::State {
    HMONITOR monitor = nullptr;
    DXGI_OUTPUT_DESC output{};
    DXGI_OUTDUPL_DESC description{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<IDXGIOutput6> colorOutput;
    ComPtr<ID3D11Texture2D> latest;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t sourceQpc = 0;
    std::int64_t pixelsSourceQpc = 0;
    double pixelsWhiteNits = 0;
    bool separatePointerVisible = false;
};

MaiDesktopDuplication::MaiDesktopDuplication() = default;
MaiDesktopDuplication::~MaiDesktopDuplication() = default;

void MaiDesktopDuplication::close() noexcept {
    mState.reset();
}

bool MaiDesktopDuplication::isOpen() const {
    return mState && mState->duplication;
}

MaiError MaiDesktopDuplication::open(std::uintptr_t monitor, bool* deviceCreated) {
    if (deviceCreated) *deviceCreated = false;
    close();
    if (!monitor) return MaiError::make(MaiErrorCode::InvalidInput, "monitor identifier is empty");
    auto state = std::make_unique<State>();
    state->monitor = reinterpret_cast<HMONITOR>(monitor);
    ComPtr<IDXGIFactory1> factory;
    HRESULT result = ::CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) return desktopHresultError("CreateDXGIFactory1", result);
    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput5> selectedOutput;
    // 必须在实际拥有 HMONITOR 的 adapter 创建设备；独显、集显和多输出不能假定 adapter 0。
    for (UINT adapterIndex = 0; !selectedOutput; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) return desktopHresultError("IDXGIFactory1::EnumAdapters1", result);
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            result = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result)) return desktopHresultError("IDXGIAdapter::EnumOutputs", result);
            DXGI_OUTPUT_DESC description{};
            result = output->GetDesc(&description);
            if (FAILED(result)) return desktopHresultError("IDXGIOutput::GetDesc", result);
            if (description.Monitor != state->monitor || !description.AttachedToDesktop) continue;
            result = output.As(&selectedOutput);
            if (FAILED(result)) return desktopHresultError("IDXGIOutput5::QueryInterface", result);
            selectedAdapter = adapter;
            state->output = description;
            break;
        }
    }
    if (!selectedOutput)
        return MaiError::make(MaiErrorCode::NotFound, "capture monitor is disconnected");
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
    result =
        ::D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION,
                            state->device.GetAddressOf(), nullptr, state->context.GetAddressOf());
    if (FAILED(result)) return desktopHresultError("desktop D3D11CreateDevice", result);
    if (deviceCreated) *deviceCreated = true;
    // 显式保留 HDR 的线性 FP16：legacy DuplicateOutput 在并存采集时可能先裁损成 BGRA，
    // 之后再按 SDR 白还原已无法恢复高亮通道。沿用 OBS 的原生格式列表，不做失败回退。
    constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
    result = selectedOutput->DuplicateOutput1(state->device.Get(), 0, 2, formats,
                                              state->duplication.GetAddressOf());
    if (FAILED(result)) return desktopHresultError("IDXGIOutput5::DuplicateOutput1", result);
    state->duplication->GetDesc(&state->description);
    if (state->description.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        state->description.ModeDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return MaiError::make(
            MaiErrorCode::NotSupported,
            "unsupported desktop duplication format " +
                std::to_string(static_cast<unsigned>(state->description.ModeDesc.Format)));
    if (state->description.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        result = selectedOutput.As(&state->colorOutput);
        if (FAILED(result))
            return desktopHresultError("desktop color IDXGIOutput6 QueryInterface", result);
    }
    mState = std::move(state);
    return {};
}

// Acquire 必须成对 Release；映射或分配抛异常时也不能把 duplication 留在持帧状态。
struct MaiAcquiredDesktopFrame {
    ComPtr<IDXGIOutputDuplication> duplication;
    bool acquired = true;

    ~MaiAcquiredDesktopFrame() {
        if (acquired) duplication->ReleaseFrame();
    }

    HRESULT release() {
        acquired = false;
        return duplication->ReleaseFrame();
    }
};

MaiError MaiDesktopDuplication::poll(MaiDesktopDuplicationFrame& frame) {
    frame = {};
    if (!isOpen())
        return MaiError::make(MaiErrorCode::NotConfigured, "desktop duplication is inactive");
    const auto fail = [this](MaiError error) {
        close();
        return error;
    };
    DXGI_OUTDUPL_FRAME_INFO information{};
    ComPtr<IDXGIResource> resource;
    HRESULT result =
        mState->duplication->AcquireNextFrame(0, &information, resource.GetAddressOf());
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        result = mState->device->GetDeviceRemovedReason();
        return FAILED(result) ? fail(desktopHresultError("desktop D3D11 device", result))
                              : MaiError{};
    }
    if (FAILED(result))
        return fail(desktopHresultError("IDXGIOutputDuplication::AcquireNextFrame", result));
    MaiAcquiredDesktopFrame acquired{mState->duplication};
    if (information.LastMouseUpdateTime.QuadPart > 0)
        mState->separatePointerVisible = information.PointerPosition.Visible != FALSE;
    const auto sourceQpc = information.LastPresentTime.QuadPart;
    if (maiDesktopFrameUpdatesCache(mState->latest != nullptr, mState->sourceQpc, sourceQpc)) {
        ComPtr<ID3D11Texture2D> texture;
        result = resource.As(&texture);
        if (FAILED(result))
            return fail(desktopHresultError("desktop texture QueryInterface", result));
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            return fail(
                MaiError::make(MaiErrorCode::NotSupported,
                               "unsupported desktop texture format " +
                                   std::to_string(static_cast<unsigned>(description.Format))));
        if (!description.Width || !description.Height)
            return fail(
                MaiError::make(MaiErrorCode::Internal, "invalid desktop duplication surface"));
        D3D11_TEXTURE2D_DESC existing{};
        if (mState->latest) mState->latest->GetDesc(&existing);
        if (!mState->latest || existing.Width != description.Width ||
            existing.Height != description.Height || existing.Format != description.Format) {
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = 0;
            description.CPUAccessFlags = 0;
            description.MiscFlags = 0;
            mState->latest.Reset();
            result = mState->device->CreateTexture2D(&description, nullptr,
                                                     mState->latest.GetAddressOf());
            if (FAILED(result))
                return fail(desktopHresultError("desktop cache CreateTexture2D", result));
        }
        // 常驻轮询只维护 GPU 完整图；CPU readback、旋转和指针绘制留到请求截图时。
        mState->context->CopyResource(mState->latest.Get(), texture.Get());
        mState->sourceQpc = sourceQpc;
        frame.desktopChanged = true;
        frame.sourceQpc = sourceQpc;
    }
    result = acquired.release();
    if (FAILED(result))
        return fail(desktopHresultError("IDXGIOutputDuplication::ReleaseFrame", result));
    result = mState->device->GetDeviceRemovedReason();
    if (FAILED(result)) return fail(desktopHresultError("desktop D3D11 device", result));
    return {};
}

MaiError MaiDesktopDuplication::copyPixels(std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                                           std::uint32_t& height) {
    pixels.clear();
    width = height = 0;
    if (!isOpen() || !mState->latest)
        return MaiError::make(MaiErrorCode::NotConfigured, "desktop duplication has no frame");
    const auto fail = [this, &pixels, &width, &height](MaiError error) {
        pixels.clear();
        width = height = 0;
        close();
        return error;
    };
    D3D11_TEXTURE2D_DESC description{};
    mState->latest->GetDesc(&description);
    double whiteNits = 80;
    if (description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        if (!mState->colorOutput)
            return fail(
                MaiError::make(MaiErrorCode::Internal, "desktop FP16 color output is unavailable"));
        const MaiError error =
            desktopSdrWhiteNits(mState->colorOutput.Get(), mState->output, whiteNits);
        if (error.hasError()) return fail(error);
    }
    if (mState->pixels.empty() || mState->pixelsSourceQpc != mState->sourceQpc ||
        mState->pixelsWhiteNits != whiteNits) {
        D3D11_TEXTURE2D_DESC existing{};
        if (mState->staging) mState->staging->GetDesc(&existing);
        if (!mState->staging || existing.Width != description.Width ||
            existing.Height != description.Height || existing.Format != description.Format) {
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            mState->staging.Reset();
            const HRESULT result = mState->device->CreateTexture2D(&description, nullptr,
                                                                   mState->staging.GetAddressOf());
            if (FAILED(result))
                return fail(desktopHresultError("desktop staging CreateTexture2D", result));
        }
        mState->context->CopyResource(mState->staging.Get(), mState->latest.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result =
            mState->context->Map(mState->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result)) return fail(desktopHresultError("desktop staging Map", result));
        bool copied = false;
        {
            struct MaiDesktopMapping {
                ID3D11DeviceContext* context;
                ID3D11Texture2D* texture;
                ~MaiDesktopMapping() {
                    context->Unmap(texture, 0);
                }
            } mapping{mState->context.Get(), mState->staging.Get()};
            MaiDesktopRotation rotation;
            switch (mState->description.Rotation) {
                case DXGI_MODE_ROTATION_UNSPECIFIED:
                case DXGI_MODE_ROTATION_IDENTITY: rotation = MaiDesktopRotation::Identity; break;
                case DXGI_MODE_ROTATION_ROTATE90: rotation = MaiDesktopRotation::Clockwise90; break;
                case DXGI_MODE_ROTATION_ROTATE180:
                    rotation = MaiDesktopRotation::Clockwise180;
                    break;
                case DXGI_MODE_ROTATION_ROTATE270:
                    rotation = MaiDesktopRotation::Clockwise270;
                    break;
                default: rotation = static_cast<MaiDesktopRotation>(-1); break;
            }
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
            const auto bytes = static_cast<std::size_t>(mapped.RowPitch) * description.Height;
            copied = description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                         ? maiCopyDesktopScRgb(source, bytes, description.Width, description.Height,
                                               mapped.RowPitch, rotation, whiteNits, mState->pixels,
                                               mState->width, mState->height)
                         : maiCopyDesktopBgra(source, bytes, description.Width, description.Height,
                                              mapped.RowPitch, rotation, mState->pixels,
                                              mState->width, mState->height);
        }
        if (!copied)
            return fail(MaiError::make(MaiErrorCode::Internal, "invalid desktop pixel layout"));
        const auto& bounds = mState->output.DesktopCoordinates;
        if (mState->width != static_cast<UINT>(bounds.right - bounds.left) ||
            mState->height != static_cast<UINT>(bounds.bottom - bounds.top))
            return fail(MaiError::make(MaiErrorCode::Internal,
                                       "desktop dimensions changed during capture"));
        mState->pixelsSourceQpc = mState->sourceQpc;
        mState->pixelsWhiteNits = whiteNits;
    }
    const HRESULT deviceStatus = mState->device->GetDeviceRemovedReason();
    if (FAILED(deviceStatus))
        return fail(desktopHresultError("desktop D3D11 device", deviceStatus));
    pixels = mState->pixels;
    width = mState->width;
    height = mState->height;
    if (!mState->separatePointerVisible) return {};
    // 独立指针尚未包含在 DXGI 像素里。只在内存 DIB 的截图副本上画当前图标，
    // 不调用 ShowCursor、SetCursorPos，也不改变缓存帧或桌面内容的源时间。
    CURSORINFO cursor{sizeof(cursor)};
    POINT position{};
    if (!::GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING) ||
        !::GetPhysicalCursorPos(&position))
        return {};
    HICON icon = ::CopyIcon(cursor.hCursor);
    if (!icon)
        return fail(MaiError::make(MaiErrorCode::Internal, "CopyIcon failed for captured pointer"));
    ICONINFO information{};
    const bool iconInfo = ::GetIconInfo(icon, &information) != FALSE;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* memory = nullptr;
    HDC context = ::CreateCompatibleDC(nullptr);
    HBITMAP dib = context
                      ? ::CreateDIBSection(context, &bitmap, DIB_RGB_COLORS, &memory, nullptr, 0)
                      : nullptr;
    HGDIOBJ previous = dib ? ::SelectObject(context, dib) : nullptr;
    bool drawn = false;
    if (iconInfo && memory && previous && previous != HGDI_ERROR) {
        std::memcpy(memory, pixels.data(), pixels.size());
        const int x = position.x - mState->output.DesktopCoordinates.left -
                      static_cast<int>(information.xHotspot);
        const int y = position.y - mState->output.DesktopCoordinates.top -
                      static_cast<int>(information.yHotspot);
        drawn = ::DrawIconEx(context, x, y, icon, 0, 0, 0, nullptr, DI_NORMAL) != FALSE;
        // DIB 位图可由 CPU 直接读取，先提交本线程 GDI 批次，避免读取尚未完成的图标绘制。
        if (drawn) drawn = ::GdiFlush() != FALSE;
        if (drawn) {
            std::memcpy(pixels.data(), memory, pixels.size());
            for (std::size_t offset = 3; offset < pixels.size(); offset += 4) pixels[offset] = 255;
        }
    }
    if (previous && previous != HGDI_ERROR) ::SelectObject(context, previous);
    if (dib) ::DeleteObject(dib);
    if (context) ::DeleteDC(context);
    if (information.hbmColor) ::DeleteObject(information.hbmColor);
    if (information.hbmMask) ::DeleteObject(information.hbmMask);
    ::DestroyIcon(icon);
    return drawn ? MaiError{}
                 : fail(MaiError::make(MaiErrorCode::Internal, "drawing captured pointer failed"));
}

// 文件作用：不读取桌面像素，独立核对旋转、行填充和 DXGI 原始 QPC 时间契约。
#include "MaiDesktopDuplication.h"

#include <array>
#include <cstdio>
#include <limits>
#include <vector>

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            return false;                                                     \
        }                                                                     \
    } while (false)

static bool checkRotation(MaiDesktopRotation rotation, const std::vector<unsigned>& expected,
                          std::uint32_t expectedWidth, std::uint32_t expectedHeight) {
    // 两行三列；每行末尾故意留 4 字节填充，alpha 也故意不是 255。
    const std::array<std::uint8_t, 32> source = {1,  11, 21, 0,  2,  12, 22, 0,  3,  13, 23,
                                                 0,  99, 99, 99, 99, 4,  14, 24, 0,  5,  15,
                                                 25, 0,  6,  16, 26, 0,  99, 99, 99, 99};
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0, height = 0;
    REQUIRE(maiCopyDesktopBgra(source.data(), source.size(), 3, 2, 16, rotation, pixels, width,
                               height));
    REQUIRE(width == expectedWidth && height == expectedHeight);
    REQUIRE(pixels.size() == expected.size() * 4);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE(pixels[index * 4] == expected[index]);
        REQUIRE(pixels[index * 4 + 1] == expected[index] + 10);
        REQUIRE(pixels[index * 4 + 2] == expected[index] + 20);
        REQUIRE(pixels[index * 4 + 3] == 255);
    }
    return true;
}

static bool pixelContracts() {
    REQUIRE(checkRotation(MaiDesktopRotation::Identity, {1, 2, 3, 4, 5, 6}, 3, 2));
    REQUIRE(checkRotation(MaiDesktopRotation::Clockwise90, {4, 1, 5, 2, 6, 3}, 2, 3));
    REQUIRE(checkRotation(MaiDesktopRotation::Clockwise180, {6, 5, 4, 3, 2, 1}, 3, 2));
    REQUIRE(checkRotation(MaiDesktopRotation::Clockwise270, {3, 6, 2, 5, 1, 4}, 2, 3));
    std::array<std::uint8_t, 16> source{};
    std::vector<std::uint8_t> pixels{7};
    std::uint32_t width = 8, height = 9;
    REQUIRE(!maiCopyDesktopBgra(source.data(), source.size(), 2, 2, 7, MaiDesktopRotation::Identity,
                                pixels, width, height));
    REQUIRE(!maiCopyDesktopBgra(source.data(), 15, 2, 2, 8, MaiDesktopRotation::Identity, pixels,
                                width, height));
    REQUIRE(!maiCopyDesktopBgra(source.data(), source.size(), 2, 2, 8,
                                static_cast<MaiDesktopRotation>(99), pixels, width, height));
    REQUIRE(!maiCopyDesktopBgra(nullptr, 0, 0, 0, 0, MaiDesktopRotation::Identity, pixels, width,
                                height));
    REQUIRE(pixels == std::vector<std::uint8_t>{7} && width == 8 && height == 9);
    return true;
}

static bool sourceTimeContracts() {
    // 3 MHz QPC，后采样比呈现晚半秒；不能把 raw tick 当成 100ns。
    REQUIRE(maiDesktopSourceProducedAtMs(1500000, 100000000, 3000000, 3000000) == 9500);
    // 帧龄有不足 100ns 的部分时向上取整，不能把刚好跨毫秒的旧图变新。
    REQUIRE(maiDesktopSourceProducedAtMs(2999999, 100000000, 3000000, 3000000) == 9999);
    REQUIRE(maiDesktopSourceProducedAtMs(0, 100000000, 3000000, 3000000) == 0);
    REQUIRE(maiDesktopSourceProducedAtMs(3000001, 100000000, 3000000, 3000000) == 0);
    REQUIRE(maiDesktopSourceProducedAtMs(1, 100000000, 3000000, 0) == 0);
    REQUIRE(maiDesktopSourceProducedAtMs(1, 1, 3000000, 3000000) == 0);
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    REQUIRE(maiDesktopSourceProducedAtMs(maximum - 1, 100000000, maximum, 10000000) == 9999);
    return true;
}

static bool inactiveNativeContracts() {
    MaiDesktopDuplication capture;
    REQUIRE(!capture.isOpen());
    bool deviceCreated = true;
    REQUIRE(capture.open(0, &deviceCreated).code() == MaiErrorCode::InvalidInput);
    REQUIRE(!deviceCreated);
    REQUIRE(capture.open(1).code() == MaiErrorCode::NotFound);
    REQUIRE(!capture.isOpen());
    MaiDesktopDuplicationFrame frame{true, 123};
    REQUIRE(capture.poll(frame).code() == MaiErrorCode::NotConfigured);
    REQUIRE(!frame.desktopChanged && frame.sourceQpc == 0);
    std::vector<std::uint8_t> pixels{1, 2, 3, 4};
    std::uint32_t width = 7, height = 8;
    REQUIRE(capture.copyPixels(pixels, width, height).code() == MaiErrorCode::NotConfigured);
    REQUIRE(pixels.empty() && width == 0 && height == 0);
    capture.close();
    capture.close();
    REQUIRE(!capture.isOpen());
    return true;
}

static bool firstAndPointerOnlyFrameContracts() {
    // DXGI 首帧可能只有指针元数据，资源描述有效但像素尚不是完整桌面。
    // 本机原生对照已确认 sourceQpc=0 的首次缓存输出黑图，必须等待真实呈现。
    REQUIRE(!maiDesktopFrameUpdatesCache(false, 0, 0));
    REQUIRE(!maiDesktopFrameUpdatesCache(false, 0, -1));
    REQUIRE(maiDesktopFrameUpdatesCache(false, 0, 100));
    REQUIRE(!maiDesktopFrameUpdatesCache(true, 0, 0));
    REQUIRE(!maiDesktopFrameUpdatesCache(true, 100, 0));
    REQUIRE(!maiDesktopFrameUpdatesCache(true, 100, 100));
    REQUIRE(!maiDesktopFrameUpdatesCache(true, 100, 99));
    REQUIRE(maiDesktopFrameUpdatesCache(true, 100, 101));
    return true;
}

static bool scRgbContracts() {
    // IEEE binary16 固定样本，不借用被测转换函数生成输入或期望：
    // R/G/B/A 顺序；每行两个像素后有 8 字节填充，包含负数、次正规数、NaN 和无穷。
    const std::array<std::uint16_t, 24> source = {
        0x3c00, 0,      0,      0x3c00, 0,      0x3800, 0x3c00, 0, 0xffff, 0xffff, 0xffff, 0xffff,
        0x3400, 0xbc00, 0x4000, 0x3c00, 0x0001, 0x7c00, 0x7e00, 0, 0xffff, 0xffff, 0xffff, 0xffff};
    const std::array<std::array<unsigned, 4>, 4> expected = {
        {{0, 0, 255, 255}, {255, 188, 0, 255}, {255, 0, 137, 255}, {0, 255, 0, 255}}};
    const std::array<std::array<unsigned, 4>, 4> orders = {
        {{0, 1, 2, 3}, {2, 0, 3, 1}, {3, 2, 1, 0}, {1, 3, 0, 2}}};
    for (unsigned turn = 0; turn < orders.size(); ++turn) {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0, height = 0;
        REQUIRE(maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(source.data()),
                                    sizeof(source), 2, 2, 24, static_cast<MaiDesktopRotation>(turn),
                                    80, pixels, width, height));
        REQUIRE(width == 2 && height == 2 && pixels.size() == 16);
        for (unsigned pixel = 0; pixel < 4; ++pixel)
            for (unsigned channel = 0; channel < 4; ++channel)
                REQUIRE(pixels[pixel * 4 + channel] == expected[orders[turn][pixel]][channel]);
    }
    // 本机实测 SDRWhiteLevel=3500，即 280 nits：3.5 scRGB 白，1.75 是线性一半。
    const std::array<std::uint16_t, 8> whiteAndDark = {0x4300, 0x3f00, 0,      0,
                                                       0xfc00, 0x8000, 0x1000, 0};
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0, height = 0;
    REQUIRE(maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(whiteAndDark.data()),
                                sizeof(whiteAndDark), 2, 1, 16, MaiDesktopRotation::Identity, 280,
                                pixels, width, height));
    REQUIRE(pixels == std::vector<std::uint8_t>({0, 188, 255, 255, 0, 0, 0, 255}));
    // 实机自有 RGB(220,30,40) 色块的 FP16 样本；前后 HWND/PID/3x3 ownership 均独立核对。
    // 由 280 nits 白还原为普通 sRGB PNG，不能把 legacy API 已裁损的 BGRA 当成原始色值。
    const std::array<std::uint16_t, 4> ownedRed = {16642, 10704, 11456, 0x3c00};
    REQUIRE(maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(ownedRed.data()),
                                sizeof(ownedRed), 1, 1, 8, MaiDesktopRotation::Identity, 280,
                                pixels, width, height));
    REQUIRE(pixels == std::vector<std::uint8_t>({40, 30, 220, 255}));
    const std::array<std::uint16_t, 4> dark = {0x1000, 0x0400, 0x7bff, 0};
    REQUIRE(maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(dark.data()), sizeof(dark), 1,
                                1, 8, MaiDesktopRotation::Identity, 80, pixels, width, height));
    REQUIRE(pixels == std::vector<std::uint8_t>({255, 0, 2, 255}));
    const auto unchanged = pixels;
    REQUIRE(!maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(source.data()),
                                 sizeof(source), 2, 2, 15, MaiDesktopRotation::Identity, 80, pixels,
                                 width, height));
    REQUIRE(!maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(source.data()), 39, 2, 2, 24,
                                 MaiDesktopRotation::Identity, 80, pixels, width, height));
    REQUIRE(!maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(source.data()),
                                 sizeof(source), 2, 2, 24, MaiDesktopRotation::Identity, 0, pixels,
                                 width, height));
    REQUIRE(!maiCopyDesktopScRgb(reinterpret_cast<const std::uint8_t*>(source.data()),
                                 sizeof(source), 2, 2, 24, MaiDesktopRotation::Identity,
                                 std::numeric_limits<double>::infinity(), pixels, width, height));
    REQUIRE(pixels == unchanged && width == 1 && height == 1);
    return true;
}

int main() {
    const bool pixels = pixelContracts();
    const bool sourceTime = sourceTimeContracts();
    const bool native = inactiveNativeContracts();
    const bool frames = firstAndPointerOnlyFrameContracts();
    const bool scRgb = scRgbContracts();
    if (!pixels || !sourceTime || !native || !frames || !scRgb) return 1;
    std::printf("desktop duplication pixel, source-time and inactive-resource contracts passed\n");
    return 0;
}

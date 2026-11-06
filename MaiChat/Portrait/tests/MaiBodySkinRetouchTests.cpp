// 无应用/模型依赖的身体净肤 QA；可选 PNG 输入用于与固定私有原型逐像素核对。
#include "MaiBodySkinRetouch.h"
#include "MaiPortraitWhiteningLut.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Image {
    int width = 0, height = 0, channels = 0;
    std::vector<unsigned char> bytes;
};

Image load(const char* path, int channels) {
    Image image;
    int original = 0;
    unsigned char* bytes = stbi_load(path, &image.width, &image.height, &original, channels);
    if (!bytes) throw std::runtime_error(std::string("Cannot decode QA input: ") + path);
    image.channels = channels;
    image.bytes.assign(bytes, bytes + static_cast<std::size_t>(image.width) * image.height * channels);
    stbi_image_free(bytes);
    return image;
}

void save(const std::string& path, const std::vector<unsigned char>& bytes, int width, int height, bool rgba) {
    std::ofstream stream(path, std::ios::binary);
    stream << (rgba ? "P6\n" : "P5\n") << width << " " << height << "\n255\n";
    if (rgba) {
        for (std::size_t i = 0; i < bytes.size(); i += 4)
            stream.write(reinterpret_cast<const char*>(bytes.data() + i), 3);
    } else stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "Cannot save QA artifact");
}

void synthetic() {
    constexpr int width = 64, height = 64, stride = width * 4 + 12, maskStride = width + 7;
    std::vector<unsigned char> source(stride * height, 173), scope(maskStride * height, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char* pixel = source.data() + y * stride + x * 4;
            pixel[0] = static_cast<unsigned char>(170 + x / 4);
            pixel[1] = static_cast<unsigned char>(130 + y / 4);
            pixel[2] = 110;
            pixel[3] = static_cast<unsigned char>((x * 7 + y * 3) % 256);
            if (x >= 12 && x < 52 && y >= 12 && y < 52) scope[y * maskStride + x] = 255;
            // 同一暗线在范围内外各出现一次，外部必须逐字节保留。
            if ((y == 30 || y == 4) && x >= 24 && x <= 36) pixel[0] = pixel[1] = pixel[2] = 55;
        }
    }
    const auto original = source, originalScope = scope;
    const auto zero = MaiBodySkinRetouch::apply(source.data(), width, height, stride, scope.data(), maskStride, 0);
    const auto full = MaiBodySkinRetouch::apply(source.data(), width, height, stride, scope.data(), maskStride, 1);
    const auto half = MaiBodySkinRetouch::apply(source.data(), width, height, stride, scope.data(), maskStride, .5f);
    require(full.acceptedComponents > 0 && full.repairPixels > 0 && full.changedPixels > 0,
            "Synthetic fine hair was not repaired");
    std::uint64_t insideChanges = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            const unsigned char* before = source.data() + y * stride + x * 4;
            require(std::memcmp(zero.rgba.data() + pixel * 4, before, 4) == 0, "Zero identity failed");
            require(zero.mask[pixel] == 0 && zero.repairMask[pixel] == 0, "Zero diagnostics must be empty");
            require(full.rgba[pixel * 4 + 3] == before[3] && half.rgba[pixel * 4 + 3] == before[3],
                    "Alpha changed");
            if (scope[y * maskStride + x] == 0) {
                require(std::memcmp(full.rgba.data() + pixel * 4, before, 4) == 0, "Outside body scope changed");
                require(full.mask[pixel] == 0 && full.repairMask[pixel] == 0, "Outside diagnostic changed");
            } else {
                bool changed = false;
                for (int c = 0; c < 3; ++c) {
                    const double expected = (before[c] + full.rgba[pixel * 4 + c]) * .5;
                    require(std::fabs(half.rgba[pixel * 4 + c] - expected) <= .5, "Strength interpolation changed");
                    changed |= before[c] != full.rgba[pixel * 4 + c];
                }
                insideChanges += changed;
            }
        }
    }
    require(full.rgba[(30 * width + 30) * 4] > 100, "Fine hair remains dark");
    require(source == original && scope == originalScope, "Input mutated");
    require(insideChanges == full.changedPixels, "Changed-pixel diagnostic differs");
    const std::vector<unsigned char> empty(maskStride * height, 0);
    const auto noScope = MaiBodySkinRetouch::apply(source.data(), width, height, stride, empty.data(), maskStride, 1);
    require(noScope.rgba == zero.rgba && noScope.changedPixels == 0, "Empty scope identity failed");
    int rejected = 0;
    for (float value : {-1.0f, 1.01f, std::numeric_limits<float>::quiet_NaN()}) {
        try { MaiBodySkinRetouch::apply(source.data(), width, height, stride, scope.data(), maskStride, value); }
        catch (const std::invalid_argument&) { ++rejected; }
    }
    scope[0] = 1;
    try { MaiBodySkinRetouch::apply(source.data(), width, height, stride, scope.data(), maskStride, 0); }
    catch (const std::invalid_argument&) { ++rejected; }
    require(rejected == 4, "Invalid arguments were accepted");
    std::printf("{\"synthetic\":\"passed\",\"zero_identity\":true,\"outside_exact\":true,"
                "\"alpha_exact\":true,\"strength_interpolation\":true,\"input_immutable\":true,"
                "\"accepted_components\":%d,\"repair_pixels\":%llu,\"changed_pixels\":%llu}\n",
                full.acceptedComponents, static_cast<unsigned long long>(full.repairPixels),
                static_cast<unsigned long long>(full.changedPixels));
}

void compare(char** argv) {
    const Image source = load(argv[2], 4), scope = load(argv[3], 1), expected = load(argv[4], 4);
    const Image expectedRepair = load(argv[5], 1);
    require(source.width == scope.width && source.height == scope.height && source.width == expected.width &&
            source.height == expected.height && source.width == expectedRepair.width &&
            source.height == expectedRepair.height, "Comparison dimensions differ");
    const auto result = MaiBodySkinRetouch::apply(source.bytes.data(), source.width, source.height, source.width * 4,
                                                 scope.bytes.data(), source.width, 1);
    std::uint64_t changedChannels = 0, repairDifferences = 0, outsideChanges = 0, alphaChanges = 0;
    int maximum = 0;
    for (std::size_t i = 0; i < scope.bytes.size(); ++i) {
        bool changed = false;
        for (int c = 0; c < 3; ++c) {
            const int delta = std::abs(result.rgba[i * 4 + c] - expected.bytes[i * 4 + c]);
            changedChannels += delta != 0;
            maximum = std::max(maximum, delta);
            changed |= result.rgba[i * 4 + c] != source.bytes[i * 4 + c];
        }
        outsideChanges += changed && scope.bytes[i] == 0;
        alphaChanges += result.rgba[i * 4 + 3] != source.bytes[i * 4 + 3];
        repairDifferences += result.repairMask[i] != expectedRepair.bytes[i];
    }
    const std::string prefix = argv[6];
    save(prefix + "-clean.ppm", result.rgba, source.width, source.height, true);
    save(prefix + "-scope.pgm", result.mask, source.width, source.height, false);
    save(prefix + "-repair.pgm", result.repairMask, source.width, source.height, false);
    std::printf("{\"prototype_parity\":\"%s\",\"width\":%d,\"height\":%d,"
                "\"max_abs_rgb_delta\":%d,\"different_rgb_channels\":%llu,\"repair_mask_differences\":%llu,"
                "\"outside_scope_changed_pixels\":%llu,\"alpha_changed_pixels\":%llu,"
                "\"scope_pixels\":%llu,\"repair_pixels\":%llu,\"candidate_components\":%d,"
                "\"accepted_components\":%d,\"changed_pixels\":%llu}\n",
                maximum == 0 && repairDifferences == 0 ? "passed" : "failed", source.width, source.height, maximum,
                static_cast<unsigned long long>(changedChannels), static_cast<unsigned long long>(repairDifferences),
                static_cast<unsigned long long>(outsideChanges), static_cast<unsigned long long>(alphaChanges),
                static_cast<unsigned long long>(result.scopePixels), static_cast<unsigned long long>(result.repairPixels),
                result.candidateComponents, result.acceptedComponents, static_cast<unsigned long long>(result.changedPixels));
    require(maximum == 0 && repairDifferences == 0 && outsideChanges == 0 && alphaChanges == 0,
            "Fixed prototype pixel parity failed");
}

void compareWhitening(char** argv) {
    const Image source = load(argv[2], 4), scope = load(argv[3], 1), expected = load(argv[4], 4);
    require(source.width == scope.width && source.height == scope.height && source.width == expected.width &&
            source.height == expected.height, "Whitening composition dimensions differ");
    std::vector<unsigned char> whitened(source.bytes.size()), output = source.bytes;
    MaiPortraitWhiteningLut::apply(source.bytes.data(), whitened.data(), scope.bytes.size(), .25f);
    cv::Mat scopeMat(source.height, source.width, CV_8UC1, const_cast<unsigned char*>(scope.bytes.data())), distance;
    cv::distanceTransform(scopeMat, distance, cv::DIST_L2, cv::DIST_MASK_PRECISE);
    // 与固定 v3 --compose 一致：美白使用净肤后的新源，以同一个 body scope 内侧 6px 合成。
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * source.width + x;
            if (scope.bytes[pixel] == 0) continue;
            const float t = std::min(distance.at<float>(y, x) / 6.0f, 1.0f), alpha = t * t * (3 - 2 * t);
            for (int c = 0; c < 3; ++c)
                output[pixel * 4 + c] = cv::saturate_cast<unsigned char>(
                    source.bytes[pixel * 4 + c] * (1 - alpha) + whitened[pixel * 4 + c] * alpha);
        }
    }
    std::uint64_t sum = 0, changedChannels = 0, outsideChanges = 0;
    int maximum = 0;
    for (std::size_t pixel = 0; pixel < scope.bytes.size(); ++pixel) {
        for (int c = 0; c < 3; ++c) {
            const int delta = std::abs(output[pixel * 4 + c] - expected.bytes[pixel * 4 + c]);
            maximum = std::max(maximum, delta);
            sum += delta;
            changedChannels += delta != 0;
            outsideChanges += scope.bytes[pixel] == 0 && output[pixel * 4 + c] != source.bytes[pixel * 4 + c];
        }
        require(output[pixel * 4 + 3] == source.bytes[pixel * 4 + 3], "Whitening alpha changed");
    }
    save(std::string(argv[5]) + "-whiten.ppm", output, source.width, source.height, true);
    std::printf("{\"sequential_whitening_parity\":\"%s\",\"shader_whiten\":0.25,"
                "\"max_abs_rgb_delta\":%d,\"mean_abs_rgb_delta\":%.10f,\"different_rgb_channels\":%llu,"
                "\"outside_scope_changed_channels\":%llu,\"alpha_exact\":true}\n",
                maximum <= 1 && outsideChanges == 0 ? "passed" : "failed", maximum,
                static_cast<double>(sum) / (scope.bytes.size() * 3), static_cast<unsigned long long>(changedChannels),
                static_cast<unsigned long long>(outsideChanges));
    require(maximum <= 1 && outsideChanges == 0, "Sequential CPU/GPU whitening parity failed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) synthetic();
        else if (argc == 7 && std::strcmp(argv[1], "--compare") == 0) compare(argv);
        else if (argc == 6 && std::strcmp(argv[1], "--compare-whitening") == 0) compareWhitening(argv);
        else throw std::invalid_argument("usage: tests [--compare source scope expected_clean expected_repair output_prefix] "
                                         "[--compare-whitening clean scope expected_body_whiten output_prefix]");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

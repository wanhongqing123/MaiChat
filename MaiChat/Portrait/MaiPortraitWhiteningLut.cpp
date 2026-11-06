/*
 * Portable adaptation of GPUPixel's beauty_face_unit_filter.cc whitening shader.
 * Copyright (c) 2021 PixPark. Licensed under the Apache License, Version 2.0.
 * Upstream commit: ef552bf8ce2d0d41fa9b979bfb5c7cf79374ca88.
 * See whitening/NOTICE and whitening/LICENSE for source and asset attribution.
 */
#include "MaiPortraitWhiteningLut.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

#include "whitening/MaiPortraitWhiteningLutData.inc"

struct Color {
    float r, g, b;
};

float clamp(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

Color clamp(Color value) {
    return {clamp(value.r), clamp(value.g), clamp(value.b)};
}

float mix(float first, float second, float weight) {
    return first * (1.0f - weight) + second * weight;
}

Color mix(Color first, Color second, float weight) {
    return {mix(first.r, second.r, weight), mix(first.g, second.g, weight),
            mix(first.b, second.b, weight)};
}

int base64Value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    return value == '+' ? 62 : 63;
}

int paeth(int left, int above, int upperLeft) {
    const int estimate = left + above - upperLeft;
    const int dl = std::abs(estimate - left);
    const int da = std::abs(estimate - above);
    const int du = std::abs(estimate - upperLeft);
    return dl <= da && dl <= du ? left : da <= du ? above : upperLeft;
}

// These fixed assets are decoded once into immutable, process-local RGB bytes.
// The small decoder avoids platform image loaders, resource paths and link deps.
std::vector<unsigned char> decode(const unsigned char* lengths, const char* const* data,
                                  int width, int height) {
    struct Node {
        int children[2] = {-1, -1};
        int symbol = -1;
    };
    std::array<Node, 512> tree{};
    int nodeCount = 1;
    unsigned int code = 0;
    for (int length = 1; length <= 32; ++length) {
        for (int symbol = 0; symbol < 256; ++symbol) {
            if (lengths[symbol] != length) continue;
            int node = 0;
            for (int bit = length - 1; bit >= 0; --bit) {
                const int direction = (code >> bit) & 1u;
                if (tree[node].children[direction] < 0) {
                    tree[node].children[direction] = nodeCount++;
                }
                node = tree[node].children[direction];
            }
            tree[node].symbol = symbol;
            ++code;
        }
        code <<= 1;
    }
    std::vector<unsigned char> compressed;
    std::size_t encodedSize = 0;
    for (const char* const* shard = data; *shard; ++shard) encodedSize += std::strlen(*shard);
    compressed.reserve(encodedSize * 3 / 4);
    unsigned int accumulator = 0;
    int bitCount = 0;
    // Shards preserve a single bit stream and avoid compiler string-size limits.
    for (const char* const* shard = data; *shard; ++shard) {
        for (const char* cursor = *shard; *cursor && *cursor != '='; ++cursor) {
            accumulator = (accumulator << 6) | base64Value(static_cast<unsigned char>(*cursor));
            bitCount += 6;
            if (bitCount >= 8) {
                bitCount -= 8;
                compressed.push_back(static_cast<unsigned char>(accumulator >> bitCount));
            }
        }
    }
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    std::vector<unsigned char> rgb(row * height);
    std::size_t position = 0;
    for (std::size_t index = 0; index < rgb.size(); ++index) {
        int node = 0;
        while (tree[node].symbol < 0) {
            if (position >= compressed.size() * 8) {
                throw std::logic_error("Invalid embedded whitening LUT");
            }
            const int bit = (compressed[position / 8] >> (7 - position % 8)) & 1;
            ++position;
            node = tree[node].children[bit];
            if (node < 0) throw std::logic_error("Invalid embedded whitening code");
        }
        const bool hasLeft = index % row >= 3;
        const int left = hasLeft ? rgb[index - 3] : 0;
        const int above = index >= row ? rgb[index - row] : 0;
        const int upperLeft = index >= row && hasLeft ? rgb[index - row - 3] : 0;
        rgb[index] = static_cast<unsigned char>(tree[node].symbol + paeth(left, above, upperLeft));
    }
    return rgb;
}

struct Lookup {
    int width, height;
    std::vector<unsigned char> rgb;

    Color at(int x, int y) const {
        const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 3;
        return {rgb[index] / 255.0f, rgb[index + 1] / 255.0f, rgb[index + 2] / 255.0f};
    }

    // GL_LINEAR + GL_CLAMP_TO_EDGE with normalized coordinates. PNG rows remain
    // in stbi_load order, matching upstream's glTexImage2D upload without a flip.
    Color sample(float u, float v) const {
        const float x = std::max(0.0f, std::min(static_cast<float>(width - 1), u * width - 0.5f));
        const float y = std::max(0.0f, std::min(static_cast<float>(height - 1), v * height - 0.5f));
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        return mix(mix(at(x0, y0), at(x1, y0), x - x0),
                   mix(at(x0, y1), at(x1, y1), x - x0), y - y0);
    }
};

struct Lookups {
    Lookup gray{256, 1, decode(kGrayLengths, kGrayData, 256, 1)};
    Lookup origin{64, 64, decode(kOriginLengths, kOriginData, 64, 64)};
    Lookup skin{64, 64, decode(kSkinLengths, kSkinData, 64, 64)};
    Lookup light{512, 512, decode(kLightLengths, kLightData, 512, 512)};
};

Color sample16(const Lookup& lookup, Color color) {
    const float blue = color.b * 15.0f;
    const float lower = std::floor(blue), upper = std::ceil(blue);
    const float lowerY = std::floor(lower * 0.25f), upperY = std::floor(upper * 0.25f);
    const float lowerX = lower - lowerY * 4.0f, upperX = upper - upperY * 4.0f;
    const float x = color.r * 0.234375f + 0.0078125f;
    const float y = color.g * 0.234375f + 0.0078125f;
    return mix(lookup.sample(lowerX * 0.25f + x, lowerY * 0.25f + y),
               lookup.sample(upperX * 0.25f + x, upperY * 0.25f + y), blue - lower);
}

Color sample64(const Lookup& lookup, Color color) {
    const float blue = color.b * 63.0f;
    const float lower = std::floor(blue), upper = std::ceil(blue);
    const float lowerY = std::floor(lower / 8.0f), upperY = std::floor(upper / 8.0f);
    const float lowerX = lower - lowerY * 8.0f, upperX = upper - upperY * 8.0f;
    const float x = 0.5f / 512.0f + (1.0f / 8.0f - 1.0f / 512.0f) * color.r;
    const float y = 0.5f / 512.0f + (1.0f / 8.0f - 1.0f / 512.0f) * color.g;
    return mix(lookup.sample(lowerX / 8.0f + x, lowerY / 8.0f + y),
               lookup.sample(upperX / 8.0f + x, upperY / 8.0f + y), blue - lower);
}

Color whiten(Color input, float weight, const Lookups& lookup) {
    constexpr float levelRangeInv = 1.02657f, levelBlack = 0.0258820f, alpha = 0.7f;
    const Color leveled = clamp({(input.r - levelBlack) * levelRangeInv,
                                 (input.g - levelBlack) * levelRangeInv,
                                 (input.b - levelBlack) * levelRangeInv});
    const Color gray = {lookup.gray.sample(leveled.r, 0.5f).r,
                        lookup.gray.sample(leveled.g, 0.5f).g,
                        lookup.gray.sample(leveled.b, 0.5f).b};
    Color texel = clamp(mix(input, mix(leveled, gray, 0.5f), alpha));
    texel = clamp(mix(sample16(lookup.origin, texel), leveled, alpha));
    const Color skin = clamp(sample16(lookup.skin, texel));
    return mix(skin, sample64(lookup.light, skin), weight);
}

unsigned char byte(float value) {
    return static_cast<unsigned char>(std::lround(clamp(value) * 255.0f));
}

}  // namespace

void MaiPortraitWhiteningLut::apply(const unsigned char* rgba, unsigned char* output,
                                   std::size_t pixelCount, float shaderWhiten) {
    if (!std::isfinite(shaderWhiten) || shaderWhiten < 0.0f || shaderWhiten > 1.0f ||
        pixelCount > std::numeric_limits<std::size_t>::max() / 4 ||
        (pixelCount && (!rgba || !output))) {
        throw std::invalid_argument("Invalid whitening RGBA buffer or shader uniform");
    }
    if (!pixelCount) return;
    if (shaderWhiten == 0.0f) {
        if (rgba != output) std::memcpy(output, rgba, pixelCount * 4);
        return;
    }
    static const Lookups lookup;
    for (std::size_t index = 0; index < pixelCount; ++index) {
        const unsigned char* input = rgba + index * 4;
        unsigned char* destination = output + index * 4;
        const Color result = whiten({input[0] / 255.0f, input[1] / 255.0f, input[2] / 255.0f},
                                    shaderWhiten, lookup);
        destination[0] = byte(result.r);
        destination[1] = byte(result.g);
        destination[2] = byte(result.b);
        destination[3] = input[3];
    }
}

std::array<unsigned char, 4> MaiPortraitWhiteningLut::pixel(
    const std::array<unsigned char, 4>& rgba, float shaderWhiten) {
    std::array<unsigned char, 4> result{};
    apply(rgba.data(), result.data(), 1, shaderWhiten);
    return result;
}

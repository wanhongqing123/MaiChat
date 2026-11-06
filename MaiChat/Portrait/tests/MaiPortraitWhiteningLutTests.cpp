// Standalone CPU checks and raw-RGBA parity CLI; no host SDK or image loader.
#include "../MaiPortraitWhiteningLut.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    const std::array<unsigned char, 4> sample{143, 111, 89, 37};
    assert(MaiPortraitWhiteningLut::pixel(sample, 0.0f) == sample);
    const auto positive = MaiPortraitWhiteningLut::pixel(sample, 0.25f);
    assert(positive[3] == sample[3]);
    assert(positive != sample);
    assert(MaiPortraitWhiteningLut::pixel(sample, 0.000001f) != sample);
    std::array<unsigned char, 4> inPlace = sample;
    MaiPortraitWhiteningLut::apply(inPlace.data(), inPlace.data(), 1, 0.25f);
    assert(inPlace == positive);
    MaiPortraitWhiteningLut::apply(nullptr, nullptr, 0, 0.25f);
    // Independent goldens: original pinned GPUPixel macOS GPU, API 2.5 -> shader .25.
    // One byte accommodates hardware interpolation and RGBA8 rounding differences.
    const std::array<std::array<unsigned char, 3>, 10> sourceColors{{
        {{0, 0, 0}}, {{255, 255, 255}}, {{255, 0, 0}}, {{0, 255, 0}}, {{0, 0, 255}},
        {{128, 128, 128}}, {{143, 111, 89}}, {{223, 175, 147}}, {{0, 1, 255}}, {{255, 254, 1}}
    }};
    const std::array<std::array<unsigned char, 3>, 10> gpuColors{{
        {{0, 0, 0}}, {{241, 248, 253}}, {{250, 0, 8}}, {{0, 251, 35}}, {{12, 8, 247}},
        {{142, 150, 155}}, {{159, 135, 118}}, {{222, 187, 172}}, {{12, 8, 247}}, {{236, 251, 40}}
    }};
    for (std::size_t i = 0; i < sourceColors.size(); ++i) {
        const auto& rgb = sourceColors[i];
        const auto actual = MaiPortraitWhiteningLut::pixel({rgb[0], rgb[1], rgb[2], 0}, 0.25f);
        assert(actual[3] == 0);
        for (int channel = 0; channel < 3; ++channel) {
            assert(std::abs(static_cast<int>(actual[channel]) - gpuColors[i][channel]) <= 1);
        }
    }
    for (float invalid : {-1.0f, 1.01f, std::numeric_limits<float>::quiet_NaN()}) {
        bool rejected = false;
        try { MaiPortraitWhiteningLut::pixel(sample, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    bool nullRejected = false;
    try { MaiPortraitWhiteningLut::apply(nullptr, inPlace.data(), 1, 0.25f); }
    catch (const std::invalid_argument&) { nullRejected = true; }
    assert(nullRejected);
    if (argc == 1) return 0;
    if (argc != 4) { std::fprintf(stderr, "usage: whitening source.rgba output.rgba shaderWhiten\n"); return 2; }
    std::ifstream source(argv[1], std::ios::binary);
    std::vector<unsigned char> pixels(std::istreambuf_iterator<char>(source), {});
    if (pixels.empty() || pixels.size() % 4) return 3;
    MaiPortraitWhiteningLut::apply(pixels.data(), pixels.data(), pixels.size() / 4,
                                  std::strtof(argv[3], nullptr));
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    return output.good() ? 0 : 4;
}

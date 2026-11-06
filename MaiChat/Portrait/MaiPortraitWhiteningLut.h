// Portable, pointwise GPUPixel whitening. See whitening/NOTICE for provenance.
#pragma once

#include <array>
#include <cstddef>

class MaiPortraitWhiteningLut {
public:
    // shaderWhiten is the actual [0, 1] shader uniform, not macOS SetWhite's
    // API value (which upstream divides by ten). Blur and sharpen are both zero.
    // Zero copies RGBA unchanged. Every positive value runs the original levels,
    // gray, origin and skin stages before mixing the light LUT: zero -> tiny is
    // intentionally discontinuous, as in the upstream shader. Alpha is retained.
    // Buffers contain tightly packed RGBA8 pixels and may be the same buffer;
    // other overlaps are unsupported. Null buffers are accepted only for zero pixels.
    // Invalid buffers, overflow or nonfinite/out-of-range uniforms throw invalid_argument.
    static void apply(const unsigned char* rgba, unsigned char* output,
                      std::size_t pixelCount, float shaderWhiten);
    static std::array<unsigned char, 4> pixel(const std::array<unsigned char, 4>& rgba,
                                             float shaderWhiten);
};

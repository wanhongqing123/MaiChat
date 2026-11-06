// 验证入口：共享净肤/LUT 工作场、原生 tile 边界、皮肤与衣物边界及计划生命周期。
// 测试使用公开示例图片与合成反馈；不读用户相册、不发模型网络请求。
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>

#include <openssl/evp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <iostream>
#include <json.hpp>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <stdexcept>
#include "MaiPortraitBridge.h"
#include "MaiPortraitRegion.h"

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPortraitEngine.h"
#include "MaiPortraitPersonalization.h"
#include "MaiPortraitWhiteningLut.h"

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static std::vector<unsigned char> loadImage(const char* path, int& width, int& height) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    check(source != nullptr, "Cannot open test image");
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    check(image != nullptr, "Cannot decode test image");
    width = static_cast<int>(CGImageGetWidth(image));
    height = static_cast<int>(CGImageGetHeight(image));
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, color,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color);
    check(context != nullptr, "Cannot create test pixels");
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGImageRelease(image);
    return pixels;
}
static void writeImage(const char* path, const std::vector<unsigned char>& pixels, int width, int height) {
    CFDataRef data = CFDataCreate(nullptr, pixels.data(), pixels.size());
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image =
        CGImageCreate(width, height, 8, 32, width * 4, color, kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider,
                      nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(color);
    CGDataProviderRelease(provider);
    check(image != nullptr, "Cannot create output image");
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    check(destination != nullptr, "Cannot create output destination");
    CGImageDestinationAddImage(destination, image, nullptr);
    CGImageRelease(image);
    check(CGImageDestinationFinalize(destination), "Cannot write output PNG");
    CFRelease(destination);
}
static std::vector<MaiPortraitFaceHint> faces(const char* path, int width, int height) {
    VNDetectFaceLandmarksRequest* request = [VNDetectFaceLandmarksRequest new];
    VNImageRequestHandler* handler =
        [[VNImageRequestHandler alloc] initWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
                                           options:@{}];
    NSError* error = nil;
    check([handler performRequests:@[ request ] error:&error], "Vision face detection failed");
    std::vector<MaiPortraitFaceHint> hints;
    for (VNFaceObservation* face in request.results) {
        const CGRect box = face.boundingBox;
        MaiPortraitFaceHint hint;
        hint.x = box.origin.x * width;
        hint.y = (1 - CGRectGetMaxY(box)) * height;
        hint.width = box.size.width * width;
        hint.height = box.size.height * height;
        auto center = [&](VNFaceLandmarkRegion2D* region) {
            float x = 0, y = 0;
            if (region.pointCount) {
                for (NSUInteger i = 0; i < region.pointCount; ++i) {
                    x += region.normalizedPoints[i].x;
                    y += region.normalizedPoints[i].y;
                }
                x /= region.pointCount;
                y /= region.pointCount;
            }
            return std::pair<float, float>((box.origin.x + x * box.size.width) * width,
                                           (1 - box.origin.y - y * box.size.height) * height);
        };
        auto left = center(face.landmarks.leftEye), right = center(face.landmarks.rightEye);
        hint.leftEyeX = left.first;
        hint.leftEyeY = left.second;
        hint.rightEyeX = right.first;
        hint.rightEyeY = right.second;
        hints.push_back(hint);
    }
    return hints;
}

// Legacy public color utilities remain independently supported; these do not
// describe the v8 neutral whitening policy or body-material production backend.
static void toneContract() {
    for (const auto& input :
         {std::array<float, 3>{25, 6.24f, 8.32f}, {60, 24, 32}, {0, 0, 0}, {100, 0, 0}, {2, 2, 3}, {98, 3, 4}}) {
        check(MaiPortraitEngine::toneSkinLab(input, 0) == input, "Zero skin tone must be exact identity");
        float previous = input[0];
        for (float amount : {.3f, .6f, 1.f}) {
            const auto output = MaiPortraitEngine::toneSkinLab(input, amount);
            check(std::isfinite(output[0]) && output[0] >= previous - 1e-4 && output[0] <= 100.001f,
                  "Tone must be finite, monotonic and bounded");
            check(std::abs(input[1] * output[2] - input[2] * output[1]) < .001f, "Tone reversed skin hue");
            previous = output[0];
        }
    }
    const auto dark = MaiPortraitEngine::toneSkinLab({25, 6.24f, 8.32f}, 1);
    check(std::hypot(dark[1], dark[2]) > 10.3f, "Low-chroma dark skin was desaturated");
    check(dark[0] > 29 && dark[0] < 34, "Dark exposure curve must stay within reviewed response");
    const auto vivid = MaiPortraitEngine::toneSkinLab({60, 24, 32}, 1);
    std::cout << "Known Lab: " << nlohmann::json{{"L25_C10.4", dark}, {"L60_C40", vivid}}.dump() << '\n';
    check(std::hypot(vivid[1], vivid[2]) > 37.5f, "High-chroma skin lost more than the reviewed six percent");
    check(MaiPortraitEngine::toneSkinLab({0, 0, 0}, 1)[0] == 0, "Black endpoint must remain black");
    check(std::abs(MaiPortraitEngine::toneSkinLab({100, 0, 0}, 1)[0] - 100) < .001f,
          "White endpoint must remain white");
}
static void gamutContract() {
    check(std::abs(MaiPortraitEngine::whiteningResponse(.6f) - .84f) < 1e-6f, "Requested .6 response changed");
    check(MaiPortraitEngine::skinRgbFromLab({0, 0, 0}, 1) == std::array<float, 3>{0, 0, 0},
          "Black gamut endpoint changed");
    check(MaiPortraitEngine::skinRgbFromLab({100, 0, 0}, 1) == std::array<float, 3>{1, 1, 1},
          "White gamut endpoint changed");
    cv::Mat input(1, 219, CV_32FC3);
    for (int i = 0; i < 219; ++i) {
        const float r = (i + 32) / 255.f;
        input.at<cv::Vec3f>(0, i) = {r, std::round(r * 255 * .65f) / 255, std::round(r * 255 * .45f) / 255};
    }
    cv::Mat lab;
    cv::cvtColor(input, lab, cv::COLOR_RGB2Lab);
    float previous = -1, previousR = -1;
    cv::Vec3b last;
    int run = 0;
    for (int i = 0; i < 219; ++i) {
        const auto source = lab.at<cv::Vec3f>(0, i);
        const auto goal = MaiPortraitEngine::toneSkinLab({source[0], source[1], source[2]},
                                                         MaiPortraitEngine::whiteningResponse(.6f));
        const auto mapped = MaiPortraitEngine::skinRgbFromLab(goal, MaiPortraitEngine::whiteningResponse(.6f));
        cv::Vec3f rgb(mapped[0], mapped[1], mapped[2]);
        for (float value : mapped)
            check(std::isfinite(value) && value >= 0 && value <= 1, "Gamut mapping escaped finite RGB range");
        cv::Mat pixel(1, 1, CV_32FC3, &rgb), represented;
        cv::cvtColor(pixel, represented, cv::COLOR_RGB2Lab);
        const float L = represented.at<cv::Vec3f>(0, 0)[0];
        check(L > previous && rgb[0] > previousR, "Gamut shoulder flattened or reversed the skin ramp");
        cv::Vec3b quant;
        for (int k = 0; k < 3; ++k) {
            quant[k] = cv::saturate_cast<unsigned char>(rgb[k] * 255);
            check(quant[k] < 255, "Reviewed .6 skin ramp gained a clipped white channel");
        }
        run = i > 0 && quant == last ? run + 1 : 1;
        check(run <= 2, "Gamut mapping pinned consecutive skin colors to one RGB8 value");
        previous = L;
        previousR = rgb[0];
        last = quant;
    }
    const auto dark = MaiPortraitEngine::toneSkinLab({25, 6.24f, 8.32f}, MaiPortraitEngine::whiteningResponse(.6f));
    const auto mapped = MaiPortraitEngine::skinRgbFromLab(dark, MaiPortraitEngine::whiteningResponse(.6f));
    cv::Vec3f rgb(mapped[0], mapped[1], mapped[2]);
    cv::Mat pixel(1, 1, CV_32FC3, &rgb), represented;
    cv::cvtColor(pixel, represented, cv::COLOR_RGB2Lab);
    const auto out = represented.at<cv::Vec3f>(0, 0);
    check(std::abs(std::hypot(out[1], out[2]) - 10.4f) < .25f, "Low-chroma dark skin lost its color");
}
static void epsilonContract() {
    for (const auto value : std::array<cv::Vec3f, 3>{{{250 / 255.f, 100 / 255.f, 70 / 255.f},
                                                      {240 / 255.f, 160 / 255.f, 120 / 255.f},
                                                      {254 / 255.f, 200 / 255.f, 160 / 255.f}}}) {
        cv::Mat rgb(1, 1, CV_32FC3, const_cast<cv::Vec3f*>(&value)), lab;
        cv::cvtColor(rgb, lab, cv::COLOR_RGB2Lab);
        const auto original = lab.at<cv::Vec3f>(0, 0);
        for (float strength : {.0001f, .001f}) {
            const float amount = MaiPortraitEngine::whiteningResponse(strength);
            const auto goal = MaiPortraitEngine::toneSkinLab({original[0], original[1], original[2]}, amount);
            const auto mapped = MaiPortraitEngine::skinRgbFromLab(goal, amount);
            for (int c = 0; c < 3; ++c)
                check(std::abs(mapped[c] * 255 - value[c] * 255) < 1,
                      "Near-zero whitening made a fixed gamut color jump");
        }
    }
}
static cv::Mat representedLab(const std::vector<unsigned char>& pixels, int width, int height) {
    cv::Mat rgba(height, width, CV_8UC4, const_cast<unsigned char*>(pixels.data())), rgb, lab;
    cv::cvtColor(rgba, rgb, cv::COLOR_RGBA2RGB);
    rgb.convertTo(rgb, CV_32F, 1. / 255);
    cv::cvtColor(rgb, lab, cv::COLOR_RGB2Lab);
    return lab;
}
static std::vector<unsigned char> representedRgba(const cv::Mat& lab) {
    cv::Mat rgb, rgba;
    cv::cvtColor(lab, rgb, cv::COLOR_Lab2RGB);
    rgb.convertTo(rgb, CV_8U, 255);
    cv::cvtColor(rgb, rgba, cv::COLOR_RGB2RGBA);
    return {rgba.data, rgba.data + rgba.total() * 4};
}
static void partitionSkinTiles(MaiPortraitSkinPlan& plan, int step) {
    plan.tiles.clear();
    for (int x = 0; x < plan.sourceWidth; x += step) {
        const int end = std::min(plan.sourceWidth, x + step), left = std::max(0, x - 8),
                  right = std::min(plan.sourceWidth, end + 8);
        plan.tiles.push_back({{left, 0, right - left, plan.sourceHeight}, {x, 0, end - x, plan.sourceHeight}});
    }
}
static MaiPortraitSkinPlan syntheticPlan(int width, int height, int step) {
    MaiPortraitSkinPlan plan;
    plan.sourceWidth = width;
    plan.sourceHeight = height;
    plan.analysisSourceRegion = {0, 0, width, height};
    plan.proxyWidth = 2;
    plan.proxyHeight = 1;
    plan.faceProtected = true;
    plan.alpha = {255, 255};
    plan.proxyRgba = {150, 110, 90, 255, 150, 110, 90, 255};
    plan.bodyAlpha = {255, 255};
    plan.ownership = {255, 255};
    plan.hasPersonOwnership = true;
    plan.bodyConfidencePixels = 2;
    partitionSkinTiles(plan, step);
    return plan;
}
static std::vector<unsigned char> applyTiles(const std::vector<unsigned char>& pixels, int width, int height,
                                             const MaiPortraitSkinPlan& plan, const MaiPortraitParameters& parameters) {
    auto prepared = plan;
    if (!prepared.materialPrepared) MaiPortraitEngine::prepareSkinMaterial(prepared, parameters);
    auto result = pixels;
    for (size_t i = 0; i < prepared.tiles.size(); ++i) {
        const auto& tile = plan.tiles[i];
        auto output = MaiPortraitEngine::applySkinTile(pixels.data() + tile.sample[0] * 4, tile.sample[2], height,
                                                       width * 4, prepared, static_cast<int>(i), parameters);
        for (int y = 0; y < height; ++y)
            for (int x = tile.write[0]; x < tile.write[0] + tile.write[2]; ++x)
                std::memcpy(result.data() + (static_cast<size_t>(y) * width + x) * 4,
                            output.rgba.data() + (static_cast<size_t>(y) * tile.sample[2] + x - tile.sample[0]) * 4, 4);
    }
    return result;
}
static std::vector<unsigned char> tiled(const std::vector<unsigned char>& pixels, int width, int height, int step,
                                        const MaiPortraitParameters& parameters) {
    return applyTiles(pixels, width, height, syntheticPlan(width, height, step), parameters);
}
// Independent double-precision CIE forward reference. Invert the established
// D65 linear-sRGB matrix, rather than calling the production gamut mapper.
static std::array<double, 3> referenceLab(const std::array<float, 3>& rgb) {
    cv::Vec3d linear;
    for (int c = 0; c < 3; ++c) linear[c] = rgb[c] <= .04045 ? rgb[c] / 12.92 : std::pow((rgb[c] + .055) / 1.055, 2.4);
    const cv::Matx33d xyzToRgb(3.240479, -1.53715, -.498535, -.969256, 1.875991, .041556, .055648, -.204043, 1.057311);
    const auto xyz = xyzToRgb.inv() * linear;
    auto f = [](double value) {
        constexpr double d = 6. / 29;
        return value > d * d * d ? std::cbrt(value) : value / (3 * d * d) + 4. / 29;
    };
    const double x = f(xyz[0] / .950456), y = f(xyz[1]), z = f(xyz[2] / 1.088754);
    return {116 * y - 16, 500 * (x - y), 200 * (y - z)};
}
static void neutralWhiteningContract() {
    constexpr float eps = std::numeric_limits<float>::epsilon();
    check(MaiPortraitEngine::whiteningShadowAmount(0) == 0 && MaiPortraitEngine::whiteningShadowAmount(.5f) == 18.75f &&
              MaiPortraitEngine::whiteningShadowAmount(1) == 25,
          "Fixed shadow amplitude does not match the public whitening strength");
    for (float strength : {0.f, .0001f, .05f, .3f, .5f, 1.f}) {
        float previous = -1;
        for (int i = 0; i <= 1000; ++i) {
            const float L = i / 10.f;
            const float adjusted = MaiPortraitEngine::whiteningShadowLightness(L, strength);
            check(std::isfinite(adjusted) && adjusted >= L && adjusted <= 100 && adjusted > previous,
                  "Per-request shadow curve lost monotonicity, boundedness or finite output");
            if (strength == 0 || L <= 3 || L >= 50)
                check(adjusted == L, "Zero/highlight/black shadow endpoint was not exact identity");
            previous = adjusted;
        }
    }
    for (const auto& input : std::array<std::array<float, 3>, 6>{
             {{25, 8, 10}, {35, 12, 18}, {53, 18, 22}, {80, 25, 20}, {20, -12, 8}, {40, 0, 0}}}) {
        for (const auto& lut :
             std::array<std::array<float, 3>, 4>{{{32, 17, 14}, {44, -25, 31}, {70, 50, -15}, {100, 0, 0}}}) {
            check(MaiPortraitEngine::whiteningTargetLab(input, lut, 0) == input,
                  "Zero whitening changed the ordinary Lab target");
            for (float strength : {.1f, .5f, 1.f}) {
                const auto target = MaiPortraitEngine::whiteningTargetLab(input, lut, strength);
                const float beforeC = std::hypot(input[1], input[2]);
                const float lutC = std::hypot(lut[1], lut[2]);
                const float afterC = std::hypot(target[1], target[2]);
                // The tolerance scales only with float arithmetic precision, not with
                // a visual hue allowance or a relaxed RGB8 dark-skin chroma budget.
                const float precision = 8 * eps * std::max({1.f, beforeC, lutC});
                check(afterC <= std::min(beforeC, lutC) + precision,
                      "Neutral whitening increased chroma beyond its input or LUT target");
                const double cross = double(input[1]) * target[2] - double(input[2]) * target[1];
                const double dot = double(input[1]) * target[1] + double(input[2]) * target[2];
                check(std::abs(cross) <= 8 * eps * std::max(1.f, beforeC * afterC) && dot >= 0,
                      "Neutral whitening rotated or reversed the post-adjustment input hue");
                check(target[0] == MaiPortraitEngine::whiteningShadowLightness(lut[0], strength),
                      "Neutral target used a different or image-dependent shadow curve");
            }
        }
    }
    for (const auto& input :
         std::array<std::array<float, 3>, 4>{{{25, 120, 120}, {40, -120, 100}, {35, 90, -140}, {65, 150, 75}}}) {
        const std::array<float, 3> lut{45, 130, 130};
        const auto target = MaiPortraitEngine::whiteningTargetLab(input, lut, .5f);
        const auto rgb = MaiPortraitEngine::whiteningTargetRgb(input, lut, .5f);
        for (float value : rgb)
            check(std::isfinite(value) && value >= 0 && value <= 1, "Neutral gamut mapping escaped finite float sRGB");
        const auto represented = referenceLab(rgb);
        const double inputC = std::hypot(input[1], input[2]);
        const double targetC = std::hypot(target[1], target[2]);
        const double representedC = std::hypot(represented[1], represented[2]);
        // Float gamma/matrix operations plus the documented 20-step chroma search;
        // this is far below one RGB8 cell and independent of skin appearance.
        const double precision = 64 * eps * std::max(100., inputC);
        check(std::abs(represented[0] - target[0]) <= precision && representedC <= targetC + precision,
              "Gamut mapping changed target L or added chroma");
        check(std::abs(double(input[1]) * represented[2] - double(input[2]) * represented[1]) <= precision * inputC,
              "Gamut mapping clipped RGB channels away from the input Lab hue ray");
    }
    constexpr int width = 32, height = 32;
    const std::array<std::array<unsigned char, 4>, 6> colors{{{80, 62, 54, 255},
                                                              {95, 78, 68, 255},
                                                              {65, 52, 45, 255},
                                                              {74, 62, 56, 255},
                                                              {88, 66, 58, 255},
                                                              {106, 82, 70, 255}}};
    int rawRedCases = 0;
    for (const auto& color : colors) {
        std::vector<unsigned char> pixels(width * height * 4);
        for (int i = 0; i < width * height; ++i) std::memcpy(pixels.data() + i * 4, color.data(), 4);
        auto plan = syntheticPlan(width, height, width);
        plan.proxyWidth = width;
        plan.proxyHeight = height;
        plan.proxyRgba = pixels;
        plan.alpha.assign(width * height, 255);
        plan.bodyAlpha = plan.ownership = plan.alpha;
        plan.bodyConfidencePixels = width * height;
        MaiPortraitParameters parameters;
        parameters.values.fill(0);
        parameters.removeBodyHair = false;
        const auto originalLab = representedLab(pixels, width, height).at<cv::Vec3f>(16, 16);
        for (float strength : {0.f, 1e-6f, .0001f, .001f}) {
            parameters.values[7] = strength;
            const auto tiny = applyTiles(pixels, width, height, plan, parameters);
            for (size_t i = 0; i < tiny.size(); ++i)
                check(std::abs(int(tiny[i]) - int(pixels[i])) <= 1,
                      "Actual dark-skin RGBA path lost zero/tiny continuity");
        }
        parameters.values[7] = .5f;
        const auto output = applyTiles(pixels, width, height, plan, parameters);
        const auto raw = MaiPortraitWhiteningLut::pixel(color, MaiPortraitEngine::whiteningShaderStrength(.5f));
        const std::vector<unsigned char> rawPixel(raw.begin(), raw.end());
        const auto rawLab = representedLab(rawPixel, 1, 1).at<cv::Vec3f>(0, 0);
        if (std::hypot(rawLab[1], rawLab[2]) > std::hypot(originalLab[1], originalLab[2]) + 2) ++rawRedCases;
        const auto finalLab = representedLab(output, width, height).at<cv::Vec3f>(16, 16);
        check(finalLab[0] > originalLab[0], "Neutral whitening did not lift the dark-skin cohort");
        for (float warmth : {-.5f, 0.f, .5f}) {
            parameters.values[2] = warmth;
            parameters.values[7] = 0;
            const auto adjusted = applyTiles(pixels, width, height, plan, parameters);
            const auto adjustedLab = representedLab(adjusted, width, height).at<cv::Vec3f>(16, 16);
            const size_t middle = (16 * width + 16) * 4;
            const std::array<unsigned char, 4> adjustedPixel{adjusted[middle], adjusted[middle + 1],
                                                             adjusted[middle + 2], adjusted[middle + 3]};
            const auto lutPixel =
                MaiPortraitWhiteningLut::pixel(adjustedPixel, MaiPortraitEngine::whiteningShaderStrength(.5f));
            const std::vector<unsigned char> lutBytes(lutPixel.begin(), lutPixel.end());
            const auto lutLab = representedLab(lutBytes, 1, 1).at<cv::Vec3f>(0, 0);
            const auto target = MaiPortraitEngine::whiteningTargetRgb({adjustedLab[0], adjustedLab[1], adjustedLab[2]},
                                                                      {lutLab[0], lutLab[1], lutLab[2]}, .5f);
            parameters.values[7] = .5f;
            const auto actual = applyTiles(pixels, width, height, plan, parameters);
            for (int c = 0; c < 3; ++c) {
                const auto quantized = cv::saturate_cast<unsigned char>(255 * target[c]);
                check(std::abs(int(actual[middle + c]) - int(quantized)) <= 1,
                      "Actual RGBA whitening bypassed the neutral target or erased explicit warmth");
            }
            check(actual[middle + 3] == color[3], "Neutral whitening changed source alpha");
        }
    }
    check(rawRedCases > 0, "Dark regression cohort no longer exercises raw-LUT excess chroma");
}

static void personCropContract() {
    const std::array<float, 4> first{403.057f, 326.192f, 402.285f, 282.508f};
    const std::array<float, 4> second{402.247f, 328.114f, 403.198f, 281.378f};
    check(MaiPortraitRegionPlanner::samePersonCrop(first, second) &&
              MaiPortraitRegionPlanner::samePersonCrop(second, first),
          "Actual same-person cross-scale virtual crops were not deduplicated symmetrically");
    check(!MaiPortraitRegionPlanner::samePersonCrop(first, {413.057f, 326.192f, 412.285f, 282.508f}),
          "Separate hips merged");
    check(!MaiPortraitRegionPlanner::samePersonCrop(first, {403.057f, 326.192f, 446.741f, 325.420f}),
          "Different body directions merged");
    check(!MaiPortraitRegionPlanner::samePersonCrop(first, {403.057f, 326.192f, 402.285f, 260.508f}),
          "Different body scales merged");
    check(!MaiPortraitRegionPlanner::samePersonCrop({0, 0, 0, 0}, {0, 0, 0, 0}), "Degenerate virtual crop merged");
}
static void tileContract() {
    constexpr int width = 2050, height = 16;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int texture = (x * 7 + y * 3) % 5 - 2;
            const size_t o = (static_cast<size_t>(y) * width + x) * 4;
            pixels[o] = 150 + texture;
            pixels[o + 1] = 110 + texture;
            pixels[o + 2] = 90 + texture;
            pixels[o + 3] = 255;
            if (x >= 1019 && x <= 1029) {
                pixels[o] = 10;
                pixels[o + 1] = 30;
                pixels[o + 2] = 220;
            }
        }
    MaiPortraitParameters p;
    p.values.fill(0);
    p.removeBodyHair = false;
    check(tiled(pixels, width, height, 1024, p) == pixels, "Zero tile parameters changed pixels");
    p.values[0] = .4f;
    p.values[7] = .6f;
    const auto first = tiled(pixels, width, height, 1024, p), second = tiled(pixels, width, height, 1000, p);
    check(first == second, "Tile border depends on tile partition or repeated halo processing");
    for (int y = 0; y < height; ++y)
        for (int x = 1019; x <= 1029; ++x) {
            const size_t o = (static_cast<size_t>(y) * width + x) * 4;
            check(std::memcmp(first.data() + o, pixels.data() + o, 4) == 0,
                  "Absolute color support failed on blue cloth inside positive low-res mask");
        }
    auto material = syntheticPlan(64, 32, 64);
    material.proxyWidth = 64;
    material.proxyHeight = 32;
    material.alpha.assign(64 * 32, 255);
    material.bodyAlpha = material.ownership = material.alpha;
    material.proxyRgba.resize(64 * 32 * 4);
    for (int i = 0; i < 64 * 32; ++i) {
        material.proxyRgba[i * 4] = 150;
        material.proxyRgba[i * 4 + 1] = 110;
        material.proxyRgba[i * 4 + 2] = 90;
        material.proxyRgba[i * 4 + 3] = 255;
    }
    auto cleaning = p;
    cleaning.removeBodyHair = true;
    MaiPortraitEngine::prepareSkinMaterial(material, cleaning);
    for (int field = 0; field < 5; ++field) {
        auto malformed = material;
        if (field == 0) malformed.alpha.clear();
        if (field == 1) malformed.bodyAlpha.clear();
        if (field == 2) malformed.materialOriginal.clear();
        if (field == 3) malformed.materialTarget.clear();
        if (field == 4) malformed.materialPrepared = false;
        bool rejected = false;
        try {
            MaiPortraitEngine::applySkinTile(material.proxyRgba.data(), 64, 32, 256, malformed, 0, cleaning);
        } catch (...) {
            rejected = true;
        }
        check(rejected, "Malformed public plan or missing shared color field was not rejected");
    }
    auto plan = syntheticPlan(32, 32, 32);
    plan.faceCount = 1;
    plan.selectedFace = {10, 10, 10, 10};
    plan.faceRegion = {0, 0, 32, 32};
    plan.faceProtection.assign(1024, 0);
    plan.faceBlend.assign(1024, 0);
    plan.faceExclusion.assign(1024, 255);
    for (int y = 12; y < 18; ++y)
        for (int x = 12; x < 18; ++x) {
            plan.faceBlend[y * 32 + x] = 255;
            plan.faceProtection[y * 32 + x] = 255;
        }
    plan.faceExclusion[15 * 32 + 15] = 0;
    std::vector<unsigned char> solid(32 * 32 * 4);
    for (int i = 0; i < 1024; ++i) std::memcpy(solid.data() + i * 4, plan.proxyRgba.data(), 4);
    MaiPortraitEngine::prepareSkinMaterial(plan, p);
    auto output = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, plan, 0, p);
    check(output.skinMask[22 * 32 + 15] == 255 && output.skinMask[23 * 32 + 15] == 255,
          "Face-box edge removed valid neck/body mask");
    check(output.skinMask[15 * 32 + 15] == 0, "Protected facial feature was colored");
    check(std::memcmp(output.rgba.data() + (15 * 32 + 15) * 4, solid.data() + (15 * 32 + 15) * 4, 4) == 0,
          "Shared color field changed a protected facial feature");
    plan.excludedFaces = {{14, 14, 4, 4}};
    const auto overlap = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, plan, 0, p);
    check(overlap.skinMask[16 * 32 + 16] == 0,
          "Selected native face alpha restored a forbidden overlapping other-face region");
    plan.excludedFaces.clear();
    plan.globalForeignVeto.assign(32 * 32, 0);
    plan.foreignProxyWidth = plan.foreignProxyHeight = 32;
    plan.globalForeignVeto[16 * 32 + 16] = 255;
    const auto foreign = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, plan, 0, p);
    check(foreign.skinMask[16 * 32 + 16] == 0,
          "Original whole-image foreign veto was lost after native face restoration");
    check(std::memcmp(foreign.rgba.data() + (16 * 32 + 16) * 4, solid.data() + (16 * 32 + 16) * 4, 4) == 0,
          "Shared color field changed a pixel forbidden by whole-image foreign veto");
    plan.globalForeignVeto.clear();
    plan.excludedFaces.clear();
    plan.hasPersonOwnership = true;
    plan.ownership = {0, 0};
    const auto unowned = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, plan, 0, p);
    check(unowned.skinMask[16 * 32 + 16] == 0, "Native face alpha bypassed explicit person ownership veto");
    plan.hasPersonOwnership = false;
    plan.analysisSourceRegion = {8, 8, 16, 16};
    plan.globalForeignVeto.assign(1024, 0);
    plan.foreignProxyWidth = plan.foreignProxyHeight = 32;
    plan.globalForeignVeto[12 * 32 + 12] = 255;
    const auto regional = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, plan, 0, p);
    check(regional.skinMask[12 * 32 + 12] == 0, "Refined-domain mapping lost original-coordinate foreign veto");
    check(regional.skinMask[6 * 32 + 6] == 0, "Regional proxy was clamped onto pixels outside its source domain");
}
static void ambiguousBodyFaceOnlyContract() {
    auto plan = syntheticPlan(32, 32, 32);
    plan.faceCount = 1;
    plan.selectedFace = {10, 10, 10, 10};
    plan.faceRegion = {0, 0, 32, 32};
    plan.hasPersonOwnership = false;
    plan.bodyAlpha.assign(2, 0);
    plan.bodyConfidencePixels = 0;
    plan.bodyOwnershipSkipReason = "overlapping_person_seeds";
    plan.faceProtection.assign(1024, 0);
    plan.faceBlend.assign(1024, 0);
    plan.faceExclusion.assign(1024, 255);
    for (int y = 12; y < 18; ++y)
        for (int x = 12; x < 18; ++x) plan.faceProtection[y * 32 + x] = 255;
    plan.faceExclusion[15 * 32 + 15] = 0;
    plan.globalForeignVeto.assign(1024, 0);
    plan.foreignProxyWidth = plan.foreignProxyHeight = 32;
    plan.globalForeignVeto[13 * 32 + 13] = 255;
    std::vector<unsigned char> solid(32 * 32 * 4);
    for (int i = 0; i < 1024; ++i) std::memcpy(solid.data() + i * 4, plan.proxyRgba.data(), 4);
    MaiPortraitParameters parameters;
    parameters.values.fill(0);
    parameters.values[7] = .6f;
    MaiPortraitEngine::prepareSkinMaterial(plan, parameters);
    check(!plan.cleanApplied && plan.materialTarget.empty(), "Ambiguous person ownership enabled body material");
    const auto output = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, plan, 0, parameters);
    check(output.changedPixels > 0 && output.bodyOwnershipSkipReason == "overlapping_person_seeds",
          "Reliable native face did not remain editable or lost its body skip reason");
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const size_t i = static_cast<size_t>(y) * 32 + x;
            const bool permitted =
                plan.faceProtection[i] != 0 && plan.faceExclusion[i] != 0 && plan.globalForeignVeto[i] == 0;
            if (!permitted) {
                check(output.skinMask[i] == 0,
                      "Face-only fallback retained coarse alpha outside protected native face");
                check(std::memcmp(output.rgba.data() + i * 4, solid.data() + i * 4, 4) == 0,
                      "Face-only fallback changed an unprotected or foreign pixel");
            }
        }
    auto skipped = plan;
    skipped.faceColorSkipped = true;
    const auto empty = MaiPortraitEngine::applySkinTile(solid.data(), 32, 32, 128, skipped, 0, parameters);
    check(empty.rgba == solid && empty.changedPixels == 0, "Failed mesh fallback edited pixels");
    auto explicitPlan = plan;
    explicitPlan.materialPrepared = false;
    parameters.removeBodyHair = true;
    bool rejected = false;
    try {
        MaiPortraitEngine::prepareSkinMaterial(explicitPlan, parameters);
    } catch (const MaiPortraitFailure& failure) {
        rejected = failure.code() == "body_skin_unavailable";
    }
    check(rejected, "Face-only fallback pretended explicit body removal succeeded");
}

static void parameterContract() {
    static_assert(MaiPortraitParameters::kCount == 8, "Body cleaning must not widen personal attributes");
    check(!MaiPortraitParameters().removeBodyHair.has_value(), "Omitted body-hair removal must remain auto");
    for (const char* json : {"{}", "{\"remove_body_hair\":false}", "{\"remove_body_hair\":true}"}) {
        char* error = maiPortraitValidateArguments("beautify", json, 0);
        const std::string detail = error ? error : "";
        maiPortraitFree(error);
        check(detail.empty(), "Documented beautify body-hair switch was rejected");
    }
    for (const char* json : {"{\"remove_body_hair\":1}", "{\"remove_body_hair\":0}", "{\"remove_body_hair\":null}",
                             "{\"remove_body_hair\":\"auto\"}", "{\"remove_body_hair\":\"false\"}"}) {
        char* error = maiPortraitValidateArguments("beautify", json, 0);
        check(error != nullptr, "Non-boolean body-hair removal was accepted");
        const auto detail = nlohmann::json::parse(error);
        maiPortraitFree(error);
        check(detail.at("code") == "invalid_parameters", "Switch validation returned the wrong failure category");
    }
    char* bodyError = maiPortraitValidateArguments("reshape_body", "{\"remove_body_hair\":true}", 0);
    check(bodyError != nullptr, "Body reshape accepted the beautify-only switch");
    maiPortraitFree(bodyError);
    for (const char* encoded :
         {"{\"whitening_shadow_amount\":18.75}", "{\"whitening_color_policy\":\"source_hue_chroma_cap\"}"}) {
        char* error = maiPortraitValidateArguments("beautify", encoded, 0);
        check(error != nullptr, "Read-only whitening metadata was accepted as a public model parameter");
        const auto detail = nlohmann::json::parse(error);
        maiPortraitFree(error);
        check(detail.at("code") == "unknown_parameter", "Read-only policy metadata lost strict parameter validation");
    }
    check(std::abs(MaiPortraitEngine::whiteningShaderStrength(.6f) - .25f) < 1e-6f,
          "Public whitening .6 no longer maps to shader .25");
    check(MaiPortraitEngine::whiteningBlendGate(0) == 0 && MaiPortraitEngine::whiteningBlendGate(.05f) == 1,
          "Whitening C1 gate endpoints changed");
    float previous = -1;
    for (float strength : {0.f, 1e-6f, .0001f, .001f, .01f, .05f, .6f, 1.f}) {
        const float gate = MaiPortraitEngine::whiteningBlendGate(strength);
        check(std::isfinite(gate) && gate >= previous && gate <= 1, "Whitening gate is not finite and monotonic");
        previous = gate;
    }
}

static void sharedBodyMaterialContract() {
    constexpr int width = 96, height = 64;
    std::vector<unsigned char> pixels(width * height * 4, 255);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int detail = (x + y) % 4 - 2;
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i] = 160 + detail;
            pixels[i + 1] = 120 + detail;
            pixels[i + 2] = 100 + detail;
            // A dark, one-pixel line exercises actual repair, not merely a nonempty mask.
            if (x == 47 && y >= 23 && y <= 41) {
                pixels[i] = 75;
                pixels[i + 1] = 55;
                pixels[i + 2] = 45;
            }
        }
    auto planFor = [&] {
        auto plan = syntheticPlan(width, height, 48);
        plan.proxyWidth = width;
        plan.proxyHeight = height;
        plan.proxyRgba = pixels;
        plan.alpha.assign(width * height, 255);
        plan.bodyAlpha = plan.ownership = plan.alpha;
        plan.bodyConfidencePixels = width * height;
        return plan;
    };
    MaiPortraitParameters parameters;
    parameters.values.fill(0);
    parameters.removeBodyHair = false;
    const auto off = applyTiles(pixels, width, height, planFor(), parameters);
    check(off == pixels, "Explicit false with zero numeric strengths did not preserve body texture/hair");
    for (float strength : {0.f, 1e-6f, .0001f, .001f}) {
        auto tiny = parameters;
        tiny.values[7] = strength;
        const auto output = applyTiles(pixels, width, height, planFor(), tiny);
        for (size_t i = 0; i < pixels.size(); ++i)
            check(std::abs(int(output[i]) - int(pixels[i])) <= 1,
                  "C1 whitening gate introduced a fixed tiny-strength color jump");
    }
    auto noBody = planFor();
    noBody.bodyAlpha.assign(width * height, 0);
    noBody.bodyConfidencePixels = 0;
    parameters.removeBodyHair = true;
    bool rejected = false;
    try {
        MaiPortraitEngine::prepareSkinMaterial(noBody, parameters);
    } catch (const MaiPortraitFailure& failure) {
        rejected = failure.code() == "body_skin_unavailable" || failure.code() == "cannot_remove_body_hair";
    }
    check(rejected, "Explicit body-hair removal without body scope silently succeeded");
    auto plan = planFor();
    MaiPortraitEngine::prepareSkinMaterial(plan, parameters);
    check(plan.materialPrepared && plan.materialOriginal.size() == width * height &&
              plan.materialTarget.size() == width * height,
          "Shared material W/T was not prepared for the complete work plane");
    check(plan.cleanApplied && plan.cleanStrength == 1 && !plan.cleanScope.empty(),
          "Explicit body cleaning did not report its applied strength/scope");
    const auto first = applyTiles(pixels, width, height, plan, parameters);
    partitionSkinTiles(plan, 47);
    check(first == applyTiles(pixels, width, height, plan, parameters),
          "Shared body material changed with tile partition");
    check(first != pixels, "Explicit cleaner did not repair the synthetic body line");
    auto lineContrast = [&](const std::vector<unsigned char>& rgba) {
        double line = 0, neighboringSkin = 0;
        for (int y = 25; y <= 39; ++y) {
            line += rgba[(static_cast<size_t>(y) * width + 47) * 4];
            neighboringSkin += .5 * (rgba[(static_cast<size_t>(y) * width + 44) * 4] +
                                     rgba[(static_cast<size_t>(y) * width + 50) * 4]);
        }
        return std::abs(neighboringSkin - line) / 15;
    };
    const double originalContrast = lineContrast(pixels), cleanedContrast = lineContrast(first);
    check(originalContrast > 50 && cleanedContrast <= .5 * originalContrast,
          "Actual body cleaner did not halve dark-line contrast against neighboring skin");
    for (size_t i = 3; i < pixels.size(); i += 4) check(first[i] == pixels[i], "Body material changed source alpha");
    // W is a constant clean source; the native fine line is absent from W.
    // Full mature-body alpha must retain only the specified 20% native residual,
    // rather than color-guidance suppressing the mask or adding 100% hair back.
    auto constant = planFor();
    partitionSkinTiles(constant, width);
    for (size_t i = 0; i < constant.proxyRgba.size(); i += 4) {
        constant.proxyRgba[i] = 160;
        constant.proxyRgba[i + 1] = 120;
        constant.proxyRgba[i + 2] = 100;
    }
    auto nativeHair = constant.proxyRgba;
    for (int y = 23; y <= 41; ++y) {
        const size_t index = (static_cast<size_t>(y) * width + 47) * 4;
        nativeHair[index] = 75;
        nativeHair[index + 1] = 55;
        nativeHair[index + 2] = 45;
    }
    MaiPortraitEngine::prepareSkinMaterial(constant, parameters);
    const auto reconstructed =
        MaiPortraitEngine::applySkinTile(nativeHair.data(), width, height, width * 4, constant, 0, parameters);
    for (int y = 25; y <= 39; ++y) {
        const size_t index = static_cast<size_t>(y) * width + 47;
        check(reconstructed.skinMask[index] == 255 && constant.bodyAlpha[index] == 255,
              "Native dark hair punched a hole in mature body alpha");
        check(std::abs(int(reconstructed.rgba[index * 4]) - 143) <= 1,
              "Constant W/native dark-line reconstruction did not preserve only 20% residual");
    }
    const double reconstructedRatio = lineContrast(reconstructed.rgba) / lineContrast(nativeHair);
    check(reconstructedRatio >= .18 && reconstructedRatio <= .22,
          "Native fine-hair contrast was not attenuated by approximately 80 percent");
    std::cout << "Shared body material contrast: "
              << nlohmann::json{{"source", originalContrast},
                                {"cleaned", cleanedContrast},
                                {"cleaned_ratio", cleanedContrast / originalContrast},
                                {"native_residual_ratio", reconstructedRatio}}
                     .dump()
              << '\n';
    parameters.removeBodyHair = false;
    parameters.values[7] = .6f;
    auto color = planFor();
    MaiPortraitEngine::prepareSkinMaterial(color, parameters);
    check(!color.cleanApplied && color.cleanStrength == 0, "False body-hair switch still ran cleaning");
    check(std::abs(color.whiteningShadowAmount - 21.f) < 1e-5f,
          "Prepared material lost its fixed shadow-curve amplitude");
    check(std::abs(color.whiteningShaderStrength - .25f) < 1e-6f && color.whiteningBlendGate == 1,
          "Prepared material lost actual shader/gate metadata");
    const auto colored = applyTiles(pixels, width, height, color, parameters);
    partitionSkinTiles(color, 47);
    check(colored == applyTiles(pixels, width, height, color, parameters),
          "Nonconstant shared LUT W/T field changed with tile partition");
}
static std::string sourceFingerprint(const std::vector<unsigned char>& pixels, int width, int height) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    EVP_DigestInit_ex(context, EVP_sha256(), nullptr);
    const std::string dimensions = std::to_string(width) + "x" + std::to_string(height) + ":";
    EVP_DigestUpdate(context, dimensions.data(), dimensions.size());
    EVP_DigestUpdate(context, pixels.data(), pixels.size());
    unsigned char hash[32];
    unsigned int size = 0;
    EVP_DigestFinal_ex(context, hash, &size);
    EVP_MD_CTX_free(context);
    std::ostringstream text;
    for (auto c : hash) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    return text.str();
}
static nlohmann::json metricDistribution(const cv::Mat& values, const cv::Rect& region) {
    std::vector<float> sorted;
    for (int y = region.y; y < region.y + region.height; ++y)
        for (int x = region.x; x < region.x + region.width; ++x) {
            const float value = values.at<float>(y, x);
            check(std::isfinite(value), "Uniformity metric contains a nonfinite pixel");
            sorted.push_back(value);
        }
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double fraction) {
        const double position = fraction * (sorted.size() - 1);
        const size_t lo = static_cast<size_t>(position), hi = std::min(lo + 1, sorted.size() - 1);
        return sorted[lo] + (sorted[hi] - sorted[lo]) * (position - lo);
    };
    return {{"pixels", sorted.size()}, {"mean", cv::mean(values(region))[0]},
            {"p5", percentile(.05)},   {"p50", percentile(.5)},
            {"p95", percentile(.95)},  {"p95_minus_p5", percentile(.95) - percentile(.05)}};
}
static bool finiteMetricReport(const nlohmann::json& value) {
    if (value.is_null()) return false;
    if (value.is_number_float()) return std::isfinite(value.get<double>());
    if (value.is_array() || value.is_object())
        for (const auto& entry : value)
            if (!finiteMetricReport(entry)) return false;
    return true;
}
static void uniformityMetrics(const char* sourcePath, const char* outputPath, const char* jsonPath) {
    int width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    const auto source = loadImage(sourcePath, width, height), output = loadImage(outputPath, outputWidth, outputHeight);
    check(width == 5712 && height == 4284 && outputWidth == width && outputHeight == height,
          "Uniformity report requires the fixed 5712x4284 source/output pair");
    // Include full box-filter halos, then convert only this small native domain to Lab.
    // These reports use all rectangle pixels; the independent Camera contract retains
    // its frozen source-reference cohort and remains the visual/measurement authority.
    const cv::Rect domain(4152, 3222, 706, 816);
    auto cropLab = [&](const std::vector<unsigned char>& pixels) {
        const cv::Mat rgba(height, width, CV_8UC4, const_cast<unsigned char*>(pixels.data()));
        const auto crop = rgba(domain).clone();
        return representedLab({crop.data, crop.data + crop.total() * 4}, crop.cols, crop.rows);
    };
    const auto before = cropLab(source), after = cropLab(output);
    check(cv::checkRange(before) && cv::checkRange(after), "Uniformity Lab conversion is not finite");
    std::vector<cv::Mat> first, second;
    cv::split(before, first);
    cv::split(after, second);
    const cv::Mat delta = second[0] - first[0];
    const cv::Rect gain(4190 - domain.x, 3888 - domain.y, 25, 27),
        gainWithHalo(gain.x - 4, gain.y - 4, gain.width + 8, gain.height + 8);
    cv::Mat normalized = cv::Mat::zeros(delta.size(), CV_32F);
    const float amount = MaiPortraitEngine::whiteningResponse(.6f);
    for (int y = 0; y < delta.rows; ++y)
        for (int x = 0; x < delta.cols; ++x) {
            const auto lab = before.at<cv::Vec3f>(y, x);
            const float expected = MaiPortraitEngine::toneSkinLab({lab[0], lab[1], lab[2]}, amount)[0] - lab[0];
            // Only the frozen gain rectangle and its largest box halo require a
            // reference response. Dark hair/background elsewhere is valid image data.
            check(expected > 1e-6f || !gainWithHalo.contains(cv::Point(x, y)),
                  "Fixed gain rectangle contains an undefined normalized response");
            if (expected > 1e-6f) normalized.at<float>(y, x) = delta.at<float>(y, x) / expected;
        }
    auto local = [&](cv::Rect global) {
        return cv::Rect(global.x - domain.x, global.y - domain.y, global.width, global.height);
    };
    auto box = [](const cv::Mat& value, int size) {
        cv::Mat result;
        cv::blur(value, result, {size, size}, {-1, -1}, cv::BORDER_REFLECT_101);
        return result;
    };
    auto rms = [](const cv::Mat& value, const cv::Rect& region) {
        return cv::norm(value(region), cv::NORM_L2) / std::sqrt(double(region.area()));
    };
    nlohmann::json report{
        {"status", "measurement_only_no_visual_or_product_acceptance"},
        {"source_path", sourcePath},
        {"output_path", outputPath},
        {"source_rgba_sha256_with_dimensions", sourceFingerprint(source, width, height)},
        {"output_rgba_sha256_with_dimensions", sourceFingerprint(output, width, height)},
        {"source_size", {width, height}},
        {"strength", .6},
        {"lab_method",
         "OpenCV float sRGB to D65 Lab; legacy public toneSkinLab utility normalization, not v8 backend reference"},
        {"cohort", "all pixels in fixed rectangles; independent Camera frozen-mask metrics remain separate"},
        {"gain_global_xywh", {4190, 3888, 25, 27}},
        {"actual_delta_L", metricDistribution(delta, gain)},
        {"normalized_actual_delta_L", metricDistribution(normalized, gain)}};
    for (int size : {3, 5, 9})
        report["gain_local_RMS"][std::to_string(size)] = {
            {"actual_delta_L", rms(delta - box(delta, size), gain)},
            {"normalized_actual_delta_L", rms(normalized - box(normalized, size), gain)}};
    const cv::Mat firstHigh = first[0] - box(first[0], 3), secondHigh = second[0] - box(second[0], 3),
                  firstBand = box(before, 5) - box(before, 17), secondBand = box(after, 5) - box(after, 17);
    std::vector<cv::Mat> sourceBand, outputBand;
    cv::split(firstBand, sourceBand);
    cv::split(secondBand, outputBand);
    for (const auto& item : std::vector<std::pair<std::string, cv::Rect>>{{"calf", {4160, 3740, 160, 290}},
                                                                          {"upper", {4350, 3230, 500, 230}}}) {
        const auto region = local(item.second);
        const double sourceRms = rms(firstHigh, region), outputRms = rms(secondHigh, region);
        const cv::Mat centeredSource = firstHigh(region) - cv::mean(firstHigh(region))[0],
                      centeredOutput = secondHigh(region) - cv::mean(secondHigh(region))[0];
        const double denominator = cv::norm(centeredSource) * cv::norm(centeredOutput);
        check(sourceRms > 1e-9 && denominator > 1e-9, "Fixed texture rectangle is degenerate");
        const double sourceAb = std::hypot(rms(sourceBand[1], region), rms(sourceBand[2], region)),
                     outputAb = std::hypot(rms(outputBand[1], region), rms(outputBand[2], region));
        check(sourceAb > 1e-9, "Fixed chroma rectangle is degenerate");
        report["fixed_regions"][item.first] = {
            {"global_xywh", {item.second.x, item.second.y, item.second.width, item.second.height}},
            {"source_L3_RMS", sourceRms},
            {"output_L3_RMS", outputRms},
            {"L3_RMS_ratio", outputRms / sourceRms},
            {"L3_correlation", centeredSource.dot(centeredOutput) / denominator},
            {"source_ab5_minus_ab17_RMS", sourceAb},
            {"output_ab5_minus_ab17_RMS", outputAb},
            {"ab5_minus_ab17_RMS_ratio", outputAb / sourceAb}};
    }
    const auto shadow = local({4186, 3838, 5, 5}), lit = local({4212, 3838, 5, 5});
    const double sourceContrast = cv::mean(first[0](lit))[0] - cv::mean(first[0](shadow))[0],
                 outputContrast = cv::mean(second[0](lit))[0] - cv::mean(second[0](shadow))[0];
    check(std::abs(sourceContrast) > 1e-9, "Fixed shadow/lit pair is degenerate");
    report["broad_shadow"] = {{"shadow_global_xywh", {4186, 3838, 5, 5}},
                              {"lit_global_xywh", {4212, 3838, 5, 5}},
                              {"source_contrast_L", sourceContrast},
                              {"output_contrast_L", outputContrast},
                              {"contrast_ratio", outputContrast / sourceContrast}};
    // JSON must contain no NaN/Inf serialized as null; numerical beauty thresholds are
    // deliberately absent so a measurement cannot silently promote a rejected contract.
    check(finiteMetricReport(report), "Uniformity report contains a nonfinite or null metric");
    check(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(jsonPath), report.dump(2)), "Cannot save uniformity metrics");
}
static nlohmann::json wholeSkin(const char* models, const char* runtime, const char* path, const std::string& output,
                                float strength, std::optional<bool> removeBodyHair = std::nullopt) {
    int width = 0, height = 0;
    const auto pixels = loadImage(path, width, height);
    const auto found = faces(path, width, height);
    nlohmann::json hints = nlohmann::json::array();
    for (const auto& f : found)
        hints.push_back({{"box", {{"x", f.x}, {"y", f.y}, {"width", f.width}, {"height", f.height}}},
                         {"landmarks",
                          {{"left_eye", {{"x", f.leftEyeX}, {"y", f.leftEyeY}}},
                           {"right_eye", {{"x", f.rightEyeX}, {"y", f.rightEyeY}}}}}});
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(pixels.data())), proxy, global;
    cv::resize(source, proxy, {}, std::min(1., 512. / std::max(width, height)),
               std::min(1., 512. / std::max(width, height)));
    cv::resize(source, global, {64, 64});
    auto values =
        nlohmann::json{{"operation", "beautify"}, {"smoothing", 0}, {"brightening", 0},  {"warmth", 0},
                       {"whitening", strength},   {"slim_face", 0}, {"enlarge_eyes", 0}, {"use_personal_model", false}};
    if (removeBodyHair) values["remove_body_hair"] = *removeBodyHair;
    const std::string arguments = values.dump();
    const auto encodedHints = nlohmann::json{{"_face_hints", hints}}.dump(),
               fingerprint = sourceFingerprint(pixels, width, height);
    const auto analysis =
        maiPortraitAnalyzeSkinRgba(proxy.data, proxy.cols, proxy.rows, static_cast<int>(proxy.step), width, height,
                                   encodedHints.c_str(), arguments.c_str(), (output + "/own-profile").c_str(), path,
                                   fingerprint.c_str(), global.data, static_cast<int>(global.step));
    std::unique_ptr<MaiPortraitSkinContext, decltype(&maiPortraitFreeSkinContext)> context(analysis.context,
                                                                                           maiPortraitFreeSkinContext);
    std::string error = analysis.error ? analysis.error : "";
    std::string encoded = analysis.metadata ? analysis.metadata : "";
    maiPortraitFree(analysis.error);
    maiPortraitFree(analysis.metadata);
    check(context != nullptr, error.c_str());
    const auto plan = nlohmann::json::parse(encoded).at("output");
    cv::Mat face;
    if (!plan.at("face_roi").is_null()) {
        const auto& r = plan.at("face_roi");
        face = source(cv::Rect(r.at("x"), r.at("y"), r.at("width"), r.at("height")));
    }
    if (plan.contains("skin_refinement_request") && !plan.at("skin_refinement_request").is_null()) {
        const auto& request = plan.at("skin_refinement_request");
        const auto region = request.at("source_rect").get<std::array<int, 4>>();
        cv::Mat refined;
        cv::resize(source(cv::Rect(region[0], region[1], region[2], region[3])), refined,
                   {request.at("proxy_width").get<int>(), request.at("proxy_height").get<int>()});
        char* response = maiPortraitRefineSkinRegionRgba(context.get(), refined.data, refined.cols, refined.rows,
                                                         static_cast<int>(refined.step));
        check(response != nullptr, "Refinement returned no envelope");
        const auto result = nlohmann::json::parse(response);
        maiPortraitFree(response);
        check(result.at("ok").get<bool>(), result.dump().c_str());
        check(result.at("output").at("source_rect") == request.at("source_rect"), "Refinement changed original domain");
    }
    if (width == 1000 && height == 667 && std::abs(strength - .6f) < .001f) {
        const auto request = plan.at("skin_refinement_request");
        check(!request.is_null(), "Body analysis did not request higher-resolution regional sampling");
        const auto region = request.at("source_rect").get<std::array<int, 4>>();
        cv::Mat native;
        cv::resize(source(cv::Rect(region[0], region[1], region[2], region[3])), native,
                   {request.at("proxy_width").get<int>(), request.at("proxy_height").get<int>()});
        auto fresh = [&]() {
            auto r = maiPortraitAnalyzeSkinRgba(proxy.data, proxy.cols, proxy.rows, static_cast<int>(proxy.step), width,
                                                height, encodedHints.c_str(), arguments.c_str(),
                                                (output + "/own-profile").c_str(), path, fingerprint.c_str(),
                                                global.data, static_cast<int>(global.step));
            check(r.context != nullptr, "Cannot prepare independent lifecycle context");
            maiPortraitFree(r.metadata);
            maiPortraitFree(r.error);
            return std::unique_ptr<MaiPortraitSkinContext, decltype(&maiPortraitFreeSkinContext)>(
                r.context, maiPortraitFreeSkinContext);
        };
        auto envelope = [&](char* value, bool ok) {
            check(value != nullptr, "Missing lifecycle response");
            auto result = nlohmann::json::parse(value);
            maiPortraitFree(value);
            check(result.at("ok").get<bool>() == ok, "Incorrect lifecycle success/failure");
        };
        auto missing = fresh();
        envelope(maiPortraitProtectSkinFaceRegion(missing.get(), face.data, face.cols, face.rows,
                                                  static_cast<int>(face.step)),
                 false);
        envelope(maiPortraitFinishSkin(missing.get()), false);
        auto wrong = fresh();
        envelope(maiPortraitRefineSkinRegionRgba(wrong.get(), native.data, native.cols - 1, native.rows,
                                                 static_cast<int>(native.step)),
                 false);
        envelope(maiPortraitRefineSkinRegionRgba(wrong.get(), native.data, native.cols, native.rows,
                                                 static_cast<int>(native.step)),
                 false);
        envelope(maiPortraitFinishSkin(wrong.get()), false);
        auto duplicate = fresh();
        envelope(maiPortraitRefineSkinRegionRgba(duplicate.get(), native.data, native.cols, native.rows,
                                                 static_cast<int>(native.step)),
                 true);
        envelope(maiPortraitRefineSkinRegionRgba(duplicate.get(), native.data, native.cols, native.rows,
                                                 static_cast<int>(native.step)),
                 false);
        envelope(maiPortraitFinishSkin(duplicate.get()), false);
        auto late = fresh();
        envelope(maiPortraitRefineSkinRegionRgba(late.get(), native.data, native.cols, native.rows,
                                                 static_cast<int>(native.step)),
                 true);
        envelope(
            maiPortraitProtectSkinFaceRegion(late.get(), face.data, face.cols, face.rows, static_cast<int>(face.step)),
            true);
        envelope(maiPortraitRefineSkinRegionRgba(late.get(), native.data, native.cols, native.rows,
                                                 static_cast<int>(native.step)),
                 false);
        envelope(maiPortraitFinishSkin(late.get()), false);
    }
    // 配置切换不能使持有的计划/面部感知对象悬空，tile 不需要再次读取 ORT。
    maiPortraitConfigure("", "", nullptr);
    char* protection =
        maiPortraitProtectSkinFaceRegion(context.get(), face.data, face.cols, face.rows, static_cast<int>(face.step));
    check(protection != nullptr, "Face protection returned no envelope");
    auto protectedResult = nlohmann::json::parse(protection);
    maiPortraitFree(protection);
    check(protectedResult.at("ok").get<bool>(), protectedResult.dump().c_str());
    maiPortraitConfigure(models, runtime, nullptr);
    auto edited = pixels;
    std::vector<unsigned char> mask(static_cast<size_t>(width) * height, 0);
    int index = 0;
    for (const auto& t : plan.at("tiles")) {
        const auto sample = t.at("sample_rect").get<std::array<int, 4>>(),
                   write = t.at("write_rect").get<std::array<int, 4>>();
        const auto tile = source(cv::Rect(sample[0], sample[1], sample[2], sample[3]));
        auto result = maiPortraitProcessSkinTile(context.get(), index++, tile.data, tile.cols, tile.rows,
                                                 static_cast<int>(tile.step));
        const std::string failure = result.error ? result.error : "";
        check(result.rgba != nullptr, failure.c_str());
        for (int y = write[1]; y < write[1] + write[3]; ++y)
            for (int x = write[0]; x < write[0] + write[2]; ++x) {
                const size_t dst = static_cast<size_t>(y) * width + x,
                             src = static_cast<size_t>(y - sample[1]) * sample[2] + x - sample[0];
                std::memcpy(edited.data() + dst * 4, result.rgba + src * 4, 4);
                mask[dst] = result.mask[src];
            }
        maiPortraitFree(result.rgba);
        maiPortraitFree(result.mask);
        maiPortraitFree(result.metadata);
        maiPortraitFree(result.error);
    }
    char* finished = maiPortraitFinishSkin(context.get());
    check(finished != nullptr, "No finish envelope");
    const auto final = nlohmann::json::parse(finished);
    maiPortraitFree(finished);
    check(final.at("ok").get<bool>(), final.dump().c_str());
    auto metadata = final.at("output");
    check(metadata.at("processing_version") == "native-owned-skin-tiles-20261105-v8",
          "Whole-skin output did not identify the shared body-material/LUT implementation");
    check(metadata.at("whitening_color_policy") == "source_hue_chroma_cap" &&
              std::abs(metadata.at("whitening_shadow_amount").get<float>() -
                       MaiPortraitEngine::whiteningShadowAmount(strength)) < 1e-5f,
          "Whole-skin metadata lost the neutral policy or actual fixed shadow amplitude");
    check(metadata.at("parameters") == plan.at("parameters") && metadata.at("features") == plan.at("features") &&
              metadata.at("source_fingerprint") == plan.at("source_fingerprint") &&
              metadata.at("source_path") == plan.at("source_path"),
          "Regional refinement changed resolved parameters, original features or source identity");
    check(metadata.at("parameters").size() == 8, "Body-hair switch widened personal model values");
    if (removeBodyHair)
        check(metadata.at("effective_parameters").at("remove_body_hair").is_boolean() &&
                  metadata.at("effective_parameters").at("remove_body_hair") == *removeBodyHair,
              "Explicit body-hair parameter did not round-trip as a strict boolean");
    else
        check(!metadata.at("effective_parameters").contains("remove_body_hair"),
              "Auto effective parameters gained a switch value that cannot be passed back");
    check(metadata.at("body_cleanup_request_mode") == (!removeBodyHair   ? "auto"
                                                       : *removeBodyHair ? "enabled"
                                                                         : "disabled"),
          "Body-cleanup request mode does not describe the original request");
    check(std::abs(metadata.at("whitening_shader_strength").get<float>() -
                   MaiPortraitEngine::whiteningShaderStrength(strength)) < 1e-6f &&
              std::abs(metadata.at("whitening_blend_gate").get<float>() -
                       MaiPortraitEngine::whiteningBlendGate(strength)) < 1e-6f,
          "Native C ABI metadata does not report actual shader and gate strengths");
    if (removeBodyHair && !*removeBodyHair)
        check(!metadata.at("clean_applied").get<bool>() && metadata.at("clean_strength").get<float>() == 0,
              "Explicit false was lost before native material preparation");

    if (width == 512 && height == 512 && std::abs(strength - .6f) < .001f && !face.empty()) {
        for (float roiWhitening : {0.f, .6f}) {
            nlohmann::json roiArguments{
                {"operation", "beautify"},    {"smoothing", 0},    {"brightening", 0},  {"warmth", 0},
                {"whitening", roiWhitening},  {"slim_face", .15f}, {"enlarge_eyes", 0}, {"remove_body_hair", false},
                {"use_personal_model", false}};
            auto result = maiPortraitProcessRegionRgba(face.data, face.cols, face.rows, static_cast<int>(face.step),
                                                       roiArguments.dump().c_str(), (output + "/own-profile").c_str(),
                                                       path, plan.at("face_plan").dump().c_str());
            const std::string failure = result.error ? result.error : "Native face ROI returned no metadata";
            check(result.rgba && result.metadata, failure.c_str());
            const auto roiMetadata = nlohmann::json::parse(result.metadata);
            maiPortraitFree(result.rgba);
            maiPortraitFree(result.mask);
            maiPortraitFree(result.metadata);
            maiPortraitFree(result.error);
            if (roiWhitening > 0) {
                check(roiMetadata.at("processing_version") == "native-face-roi-20261105-v8" &&
                          roiMetadata.at("whitening_color_policy") == "source_hue_chroma_cap" &&
                          std::abs(roiMetadata.at("whitening_shadow_amount").get<float>() - 21) < 1e-5f,
                      "Whitened native face ROI did not identify its actual v8 color target");
            } else {
                check(roiMetadata.at("processing_version") == "native-face-roi-20261103-v3" &&
                          !roiMetadata.contains("whitening_color_policy"),
                      "Geometry-only ROI claimed it had run the new whitening policy");
            }
        }
    }

    if (strength == 0 && !removeBodyHair.value_or(false))
        check(edited == pixels, "Whole-skin zero parameters changed pixels");
    else {
        if (strength > .05f || removeBodyHair.value_or(false))
            check(metadata.at("changed_pixel_count").get<uint64_t>() > 0, "Whole-skin result changed no pixels");
        else
            for (size_t i = 0; i < pixels.size(); ++i)
                check(std::abs(int(edited[i]) - int(pixels[i])) <= 1,
                      "Native C ABI tiny-strength gate introduced a fixed color jump");
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i] == 0)
                check(std::memcmp(edited.data() + i * 4, pixels.data() + i * 4, 4) == 0, "Outside-mask color changed");
    }
    if (width == 1000 && height == 667) {
        for (const auto point : {std::pair<int, int>{500, 100}, {800, 450}, {800, 600}, {500, 380}, {495, 450}}) {
            const size_t index = static_cast<size_t>(point.second) * width + point.first;
            check(mask[index] == 0, "Public body fixture sky/ocean/sand/shirt/shorts was classified as edited skin");
            check(std::memcmp(edited.data() + index * 4, pixels.data() + index * 4, 4) == 0,
                  "Public body fixture verified non-skin pixel changed");
        }
        if (strength > 0)
            check(mask[320 * width + 650] > 100 && mask[570 * width + 625] > 100,
                  "Public body fixture arm and leg skin were not covered");
    }
    if (std::abs(strength - .6f) < .001f) {
        MaiPortraitEngine ordinary(models, runtime, nullptr);
        MaiPortraitParameters parameters;
        parameters.values.fill(0);
        parameters.values[7] = strength;
        parameters.removeBodyHair = removeBodyHair;
        const auto direct = ordinary.apply(pixels.data(), width, height, width * 4, found, parameters, false);
        check(direct.rgba == edited && direct.skinMask == mask,
              "Ordinary full-raster and native tile APIs produced different pixels or coverage");
        check(direct.skinAnalysisPerformed && direct.skinScope == metadata.at("skin_scope").get<std::string>(),
              "Ordinary output did not report actual skin scope");
        check(direct.cleanApplied == metadata.at("clean_applied").get<bool>() &&
                  std::abs(direct.cleanStrength - metadata.at("clean_strength").get<float>()) < 1e-6f,
              "Ordinary and native C ABI disagree about actual cleaning");
    }
    const std::string stem = output + "/skin-" + std::to_string(strength) +
                             (removeBodyHair ? (*removeBodyHair ? "-hair-on" : "-hair-off") : "");
    writeImage((stem + ".png").c_str(), edited, width, height);
    if (std::abs(strength - .6f) < .001f) {
        char* committed = maiPortraitCommitResult((output + "/own-profile").c_str(), metadata.dump().c_str(),
                                                  (stem + ".png").c_str());
        check(committed != nullptr, "Commit returned no completed-file receipt");
        const auto receipt = nlohmann::json::parse(committed);
        maiPortraitFree(committed);
        check(receipt.at("ok").get<bool>(), receipt.dump().c_str());
        check(receipt.at("output").at("whitening_color_policy") == metadata.at("whitening_color_policy") &&
                  receipt.at("output").at("whitening_shadow_amount") == metadata.at("whitening_shadow_amount") &&
                  receipt.at("output").at("processing_version") == metadata.at("processing_version"),
              "Completed-file receipt omitted the actual v8 color policy or shadow amplitude");
    }
    std::vector<unsigned char> rgbaMask(mask.size() * 4);
    for (size_t i = 0; i < mask.size(); ++i) {
        rgbaMask[i * 4] = rgbaMask[i * 4 + 1] = rgbaMask[i * 4 + 2] = mask[i];
        rgbaMask[i * 4 + 3] = 255;
    }
    writeImage((stem + "-mask.png").c_str(), rgbaMask, width, height);
    check(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(stem + ".json"), metadata.dump(2)),
          "Cannot save actual skin metrics");
    auto duplicate = maiPortraitProcessSkinTile(context.get(), 0, pixels.data(), width, height, width * 4);
    check(duplicate.error != nullptr && duplicate.rgba == nullptr, "Duplicate tile was accepted");
    maiPortraitFree(duplicate.error);
    return metadata;
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if (argc == 2 && std::strcmp(argv[1], "--policy-contract-only") == 0) {
                neutralWhiteningContract();
                std::cout << "PASS v8 neutral whitening ordinary/RGBA policy contract\n";
                return 0;
            }
            if (argc > 1 && std::strcmp(argv[1], "--uniformity-metrics") == 0) {
                check(argc == 5, "Usage: skin-tests --uniformity-metrics source.png output.png metrics.json");
                uniformityMetrics(argv[2], argv[3], argv[4]);
                std::cout << "Wrote fixed-rectangle uniformity measurements: " << argv[4] << '\n';
                return 0;
            }
            check(argc >= 6, "Usage: skin-tests models runtime face body output");
            toneContract();
            gamutContract();
            epsilonContract();
            neutralWhiteningContract();
            personCropContract();
            tileContract();
            ambiguousBodyFaceOnlyContract();
            parameterContract();
            sharedBodyMaterialContract();
            check(!MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(argv[5])), "Cannot create own output");
            maiPortraitConfigure(argv[1], argv[2], nullptr);
            for (const auto& item :
                 std::vector<std::pair<const char*, std::string>>{{argv[3], "face"}, {argv[4], "body"}}) {
                const auto directory = std::string(argv[5]) + "/" + item.second;
                MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(directory));
                for (float strength : {0.f, .0001f, .6f, 1.f}) {
                    const auto metadata = wholeSkin(argv[1], argv[2], item.first, directory, strength);
                    if (item.second == "body" && strength > 0)
                        check(metadata.at("body_skin_pixel_count").get<uint64_t>() > 1000,
                              "Real body skin was not processed");
                    std::cout << item.second << " " << strength << " " << metadata.dump() << '\n';
                }
                if (item.second == "body") wholeSkin(argv[1], argv[2], item.first, directory, .6f, false);
            }
            std::cout << "PASS skin quality: legacy color utilities, exact zero/tiny/outmask, strict body switch, "
                         "cloth-color support, shared body-material/LUT tile seams, owned fullbody and lifecycle\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << e.what() << '\n';
            return 1;
        }
    }
}

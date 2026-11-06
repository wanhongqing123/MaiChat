// 验证入口：真实模型／照片处理、零参数原图、反馈隔离、候选与验证启用、持久化和回滚。
// 测试使用公开示例图片与合成反馈；不读用户相册、不发模型网络请求。
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <json.hpp>
#include <opencv2/imgproc.hpp>
#include <sqlite3.h>

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
static void personalization(const std::string& root) {
    MaiPortraitPersonalization first(root + "/profile-a");
    MaiPortraitPersonalization other(root + "/profile-b");
    std::array<float, 8> features{1, 0.5f, 0.2f, 0.4f, 0.3f, 0.5f, 0.1f, 0};
    const float baseline = first.recommend(features).values[0];
    std::string previousPreferred;
    for (int source = 0; source < 8; ++source) {
        MaiPortraitParameters preferred, rejected;
        preferred.values[0] = 0.75f;
        rejected.values[0] = 0.15f;
        preferred.values[MaiPortraitParameters::kWhiteningIndex] = 0.65f;
        rejected.values[MaiPortraitParameters::kWhiteningIndex] = 0.1f;
        auto positive = first.recordResult("photo-" + std::to_string(source), "original-" + std::to_string(source),
                                           "positive-" + std::to_string(source), preferred, features);
        auto negative = first.recordResult("photo-" + std::to_string(source), "original-" + std::to_string(source),
                                           "negative-" + std::to_string(source), rejected, features);
        check(first.recordPreference(positive, negative, {0, MaiPortraitParameters::kWhiteningIndex}) == 2,
              "Feedback was not recorded");
        check(first.recordPreference(positive, negative, {0, MaiPortraitParameters::kWhiteningIndex}) == 0,
              "Duplicate feedback changed training count");
        if (source == 0) {
            const auto candidate = first.train();
            check(!candidate.activated && candidate.version > 0, "One photo incorrectly activated a model");
            check(first.recommend(features).values[0] == baseline, "Unvalidated candidate changed the active model");
            bool denied = false;
            try {
                other.recordPreference(positive, negative, {0});
            } catch (...) {
                denied = true;
            }
            check(denied, "Feedback crossed account profiles");
            previousPreferred = positive;
        }
        if (source == 1) {
            bool denied = false;
            try {
                first.recordPreference(previousPreferred, negative, {0});
            } catch (...) {
                denied = true;
            }
            check(denied, "Different original photos were compared");
        }
    }
    const auto trained = first.train();
    check(trained.activated && trained.validationLoss >= 0, "Grouped validation did not activate improved weights");
    check(first.recommend(features).values[0] > baseline + 0.1f,
          "Training did not change predicted strength toward user preference");
    check(first.recommend(features).values[2] == 0, "Unlabelled warmth preference changed");
    check(first.recommend(features).values[MaiPortraitParameters::kWhiteningIndex] > 0.1f,
          "Whitening feedback did not train its own preference");
    MaiPortraitParameters warmPreferred, warmRejected;
    warmPreferred.values[2] = 0.5f;
    warmRejected.values[2] = -0.5f;
    auto warmFirst = first.recordResult("photo-0", "original-0", "warm-positive", warmPreferred, features);
    auto warmSecond = first.recordResult("photo-0", "original-0", "warm-negative", warmRejected, features);
    first.recordPreference(warmFirst, warmSecond, {2});
    const auto mixed = first.train();
    check(!mixed.activated && first.status().activeVersion == trained.version,
          "Validated smoothing incorrectly activated single-photo warmth");
    bool candidateRejected = false;
    try {
        first.rollback(mixed.version);
    } catch (...) {
        candidateRejected = true;
    }
    check(candidateRejected, "Unvalidated candidate was activated through rollback");
    const std::string original = root + "/original.txt", edited = root + "/edited.txt";
    check(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(original), "original"),
          "Cannot create canonical source fixture");
    check(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(edited), "edited"), "Cannot create canonical output fixture");
    first.recordResult("canonical-photo", original, edited, warmPreferred, features);
    std::string alias = edited;
    if (alias.find("/private/tmp/") == 0) alias.replace(0, 12, "/tmp");
    check(first.originalSource(alias) == MaiFileSystem::resolve(MaiFilePath::fromUtf8(original)).toUtf8(),
          "Canonical aliases lost original source tracking");
    MaiPortraitPersonalization reopened(root + "/profile-a");
    check(reopened.status().activeVersion == trained.version, "Active model did not persist");
    reopened.rollback(0);
    check(std::abs(reopened.recommend(features).values[0] - baseline) < 0.0001f, "Rollback did not restore baseline");
    check(other.status().feedbackCount == 0 && other.status().activeVersion == 0, "Other profile was modified");
    std::cout << "PASS personalization: explicit feedback, deduplication, account and original isolation, real "
                 "learning, grouped validation, persistence, rollback\n";
}
static void legacyProfile(const std::string& root) {
    const std::array<float, 8> features{1, 0.5f, 0.2f, 0.4f, 0.3f, 0.5f, 0.1f, 0};
    float previous = 0;
    int version = 0;
    {
        MaiPortraitPersonalization profile(root);
        for (int source = 0; source < 4; ++source) {
            MaiPortraitParameters first, second;
            first.values[0] = 0.75f;
            second.values[0] = 0.1f;
            const std::string name = "legacy-" + std::to_string(source);
            auto positive = profile.recordResult(name, name, name + "-first", first, features);
            auto negative = profile.recordResult(name, name, name + "-second", second, features);
            profile.recordPreference(positive, negative, {0});
        }
        auto trained = profile.train();
        check(trained.activated, "Cannot prepare legacy model fixture");
        version = trained.version;
        previous = profile.recommend(features).values[0];
    }
    // 构造真实旧版 SQLite：结果七参数、每模型七个属性头，不依赖 JSON SQL 扩展。
    sqlite3* database = nullptr;
    check(sqlite3_open((root + "/portrait.sqlite3").c_str(), &database) == SQLITE_OK,
          "Cannot open legacy model fixture");
    for (const auto& table : {"portrait_results", "portrait_versions"}) {
        const std::string column = std::strcmp(table, "portrait_results") == 0 ? "parameters" : "weights";
        sqlite3_stmt* query = nullptr;
        const std::string select = "SELECT rowid," + column + " FROM " + table;
        check(sqlite3_prepare_v2(database, select.c_str(), -1, &query, nullptr) == SQLITE_OK,
              "Cannot read legacy records");
        std::vector<std::pair<sqlite3_int64, std::string>> rows;
        while (sqlite3_step(query) == SQLITE_ROW) {
            auto value = nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(query, 1)));
            value.erase(value.begin() + 7);
            rows.emplace_back(sqlite3_column_int64(query, 0), value.dump());
        }
        sqlite3_finalize(query);
        for (const auto& row : rows) {
            const std::string update = "UPDATE " + std::string(table) + " SET " + column + "=? WHERE rowid=?";
            check(sqlite3_prepare_v2(database, update.c_str(), -1, &query, nullptr) == SQLITE_OK,
                  "Cannot prepare legacy records");
            sqlite3_bind_text(query, 1, row.second.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(query, 2, row.first);
            check(sqlite3_step(query) == SQLITE_DONE, "Cannot save legacy records");
            sqlite3_finalize(query);
        }
    }
    sqlite3_close(database);
    MaiPortraitPersonalization reopened(root);
    check(reopened.status().activeVersion == version && reopened.status().feedbackCount == 4,
          "Legacy active version or feedback was lost");
    const auto recommendation = reopened.recommend(features);
    check(std::abs(recommendation.values[0] - previous) < 0.0001f, "Legacy learned strength changed during upgrade");
    check(recommendation.values[MaiPortraitParameters::kWhiteningIndex] == 0,
          "Upgrade enabled whitening without explicit feedback");
    reopened.train();
    reopened.rollback(0);
    reopened.rollback(version);
    check(std::abs(reopened.recommend(features).values[0] - previous) < 0.0001f,
          "Rollback could not load a seven-attribute checkpoint");
    std::cout << "PASS compatibility: seven-attribute feedback, 63-weight checkpoint, training and rollback\n";
}
static void whitening(MaiPortraitEngine& engine, const std::vector<unsigned char>& pixels, int width, int height,
                      const std::vector<MaiPortraitFaceHint>& hints, const std::string& output) {
    MaiPortraitParameters parameters;
    parameters.values.fill(0);
    parameters.removeBodyHair = false;
    parameters.values[MaiPortraitParameters::kWhiteningIndex] = 0.3f;
    const auto mild = engine.apply(pixels.data(), width, height, width * 4, hints, parameters, false);
    parameters.values[MaiPortraitParameters::kWhiteningIndex] = 0.6f;
    const auto stronger = engine.apply(pixels.data(), width, height, width * 4, hints, parameters, false);
    check(stronger.rgba != mild.rgba, "Distinct positive whitening strengths produced identical output");
    check(!stronger.cleanApplied && stronger.cleanStrength == 0,
          "Explicit false whitening path unexpectedly removed body hair");
    check(std::abs(stronger.whiteningShaderStrength - .25f) < 1e-6f && stronger.whiteningBlendGate == 1,
          "Production whitening .6 lost shader .25 / full gate metadata");
    check(std::abs(stronger.whiteningShadowAmount - 21.f) < 1e-5f,
          "Production whitening .6 lost its fixed shadow-curve amplitude");
    std::vector<unsigned char> rawLut(pixels.size());
    MaiPortraitWhiteningLut::apply(pixels.data(), rawLut.data(), pixels.size() / 4, .25f);
    cv::Mat rawRgba(height, width, CV_8UC4, rawLut.data()), rawRgb, rawLab;
    cv::cvtColor(rawRgba, rawRgb, cv::COLOR_RGBA2RGB);
    rawRgb.convertTo(rawRgb, CV_32F, 1.0 / 255);
    cv::cvtColor(rawRgb, rawLab, cv::COLOR_RGB2Lab);
    std::array<cv::Mat, 3> lab, hsv;
    const std::array<const std::vector<unsigned char>*, 3> images{&pixels, &mild.rgba, &stronger.rgba};
    for (int index = 0; index < 3; ++index) {
        cv::Mat rgba(height, width, CV_8UC4, const_cast<unsigned char*>(images[index]->data())), rgb;
        cv::cvtColor(rgba, rgb, cv::COLOR_RGBA2RGB);
        rgb.convertTo(rgb, CV_32F, 1.0 / 255);
        cv::cvtColor(rgb, lab[index], cv::COLOR_RGB2Lab);
        cv::cvtColor(rgb, hsv[index], cv::COLOR_RGB2HSV);
    }
    std::array<double, 3> lightness{}, chroma{}, saturation{};
    double hueError = 0;
    size_t count = 0;
    size_t fullPolicyPixels = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            const size_t pixelIndex = static_cast<size_t>(y) * width + x;
            if (!stronger.skinMask.empty() && stronger.skinMask[pixelIndex] == 0)
                check(std::memcmp(pixels.data() + offset, stronger.rgba.data() + offset, 4) == 0,
                      "LUT whitening changed a protected/outside-mask pixel");
            if (!stronger.skinMask.empty() && stronger.skinMask[pixelIndex] == 255) {
                const auto input = lab[0].at<cv::Vec3f>(y, x), lut = rawLab.at<cv::Vec3f>(y, x);
                const auto target = MaiPortraitEngine::whiteningTargetRgb({input[0], input[1], input[2]},
                                                                          {lut[0], lut[1], lut[2]}, .6f);
                // This checks the ordinary color API and actual owned-mask engine agree.
                // One RGB8 code covers opposite rounding at float/vector conversion boundaries;
                // float-level hue/chroma properties are checked independently by SkinTests.
                for (int channel = 0; channel < 3; ++channel) {
                    const auto expected = cv::saturate_cast<unsigned char>(target[channel] * 255);
                    check(std::abs(int(expected) - int(stronger.rgba[offset + channel])) <= 1,
                          "Full-support production whitening differs from its neutral color policy");
                }
                ++fullPolicyPixels;
            }
            if (y < height / 16)
                check(std::memcmp(pixels.data() + offset, stronger.rgba.data() + offset, 4) == 0,
                      "Whitening changed the verified non-skin top background");
            if (std::memcmp(pixels.data() + offset, stronger.rgba.data() + offset, 4) == 0) continue;
            const auto original = lab[0].at<cv::Vec3f>(y, x);
            if (std::hypot(original[1], original[2]) < 8) continue;
            for (int index = 0; index < 3; ++index) {
                const auto pixel = lab[index].at<cv::Vec3f>(y, x);
                lightness[index] += pixel[0];
                chroma[index] += std::hypot(pixel[1], pixel[2]);
                saturation[index] += hsv[index].at<cv::Vec3f>(y, x)[1] * 100;
            }
            const auto edited = lab[2].at<cv::Vec3f>(y, x);
            const double dot = original[1] * edited[1] + original[2] * edited[2];
            const double cross = original[1] * edited[2] - original[2] * edited[1];
            hueError += std::abs(std::atan2(cross, dot)) * 180 / 3.141592653589793;
            ++count;
        }
    check(count > 100, "Whitening did not affect sufficient skin pixels");
    check(fullPolicyPixels > 10, "Fixture lacks a full-support cohort for the production color-policy check");
    for (int index = 0; index < 3; ++index) {
        lightness[index] /= count;
        chroma[index] /= count;
        saturation[index] /= count;
    }
    check(lightness[0] + 0.1 < lightness[1] && lightness[1] + 0.1 < lightness[2],
          "Whitening lightness is not increasing with strength");
    // This real-image cohort measures the final masked RGB8 result. The strict
    // no-extra-chroma/input-hue contract is tested before quantization by SkinTests;
    // a masked blend is not required to decrease C strictly as strength increases.
    writeImage((output + "/portrait-whitening-03.png").c_str(), mild.rgba, width, height);
    writeImage((output + "/portrait-whitening-06.png").c_str(), stronger.rgba, width, height);
    std::cout << "PASS whitening: L* " << lightness[0] << " -> " << lightness[1] << " -> " << lightness[2] << ", C* "
              << chroma[0] << " -> " << chroma[1] << " -> " << chroma[2] << ", HSV saturation " << saturation[0]
              << " -> " << saturation[1] << " -> " << saturation[2] << ", mean hue rotation " << hueError / count
              << " degrees\n";
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            check(argc >= 6, "Usage: tests models runtime portrait body output-directory");
            const std::string output = argv[5];
            check(!MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(output)),
                  "Cannot create test output directory");
            personalization(output + "/profile-test-" + MaiIdGenerator::generate("run_"));
            legacyProfile(output + "/legacy-test-" + MaiIdGenerator::generate("run_"));
            MaiPortraitEngine engine(argv[1], argv[2], nullptr);
            int width = 0, height = 0;
            auto pixels = loadImage(argv[3], width, height);
            auto hints = faces(argv[3], width, height);
            check(!hints.empty(), "Fixture face not found");
            MaiPortraitParameters zero;
            zero.values.fill(0);
            auto identity = engine.apply(pixels.data(), width, height, width * 4, hints, zero, false);
            check(identity.rgba == pixels, "Zero portrait parameters changed pixels");
            whitening(engine, pixels, width, height, hints, output);
            MaiPortraitParameters normal;
            auto result = engine.apply(pixels.data(), width, height, width * 4, hints, normal, false);
            check(result.rgba != pixels, "Portrait processing had no effect");
            size_t backgroundChanges = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    if (y < height / 16)
                        if (std::memcmp(&pixels[(y * width + x) * 4], &result.rgba[(y * width + x) * 4], 4))
                            ++backgroundChanges;
            check(backgroundChanges == 0, "Verified non-skin top background changed during skin processing");
            writeImage((output + "/portrait-natural.png").c_str(), result.rgba, width, height);
            normal.values[3] = 0.7f;
            normal.values[4] = 0.6f;
            auto shaped = engine.apply(pixels.data(), width, height, width * 4, hints, normal, false);
            check(shaped.rgba != result.rgba, "Face geometry parameters had no effect");
            writeImage((output + "/portrait-shaped.png").c_str(), shaped.rgba, width, height);
            auto noFace = pixels;
            bool rejected = false;
            try {
                engine.apply(noFace.data(), width, height, width * 4, {}, normal, false);
            } catch (...) {
                rejected = true;
            }
            check(rejected, "Missing face silently succeeded");
            auto bodyPixels = loadImage(argv[4], width, height);
            MaiPortraitParameters body;
            body.values = {0, 0, 0, 0, 0, 0.7f, 0.4f};
            auto bodyResult = engine.apply(bodyPixels.data(), width, height, width * 4, {}, body, true);
            check(bodyResult.rgba != bodyPixels, "Body processing had no effect");
            writeImage((output + "/body-shaped.png").c_str(), bodyResult.rgba, width, height);
            std::cout << "PASS real models: zero identity, local skin/background preservation, face geometry, "
                         "missing-face error, body pose and reshaping\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 1;
        }
    }
}

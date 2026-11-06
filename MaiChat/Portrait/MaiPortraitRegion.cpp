#include "MaiPortraitRegion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

MaiPortraitFailure::MaiPortraitFailure(std::string code, const std::string& message)
    : std::runtime_error(message), mCode(std::move(code)) {}

const std::string& MaiPortraitFailure::code() const {
    return mCode;
}

std::vector<MaiPortraitFaceHint> MaiPortraitRegionPlanner::mergeFaces(const std::vector<MaiPortraitFaceHint>& faces) {
    if (faces.size() > 256)
        throw MaiPortraitFailure("too_many_face_candidates", "Face detection exceeded the candidate budget");
    auto ordered = faces;
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.width * a.height > b.width * b.height; });
    std::vector<MaiPortraitFaceHint> result;
    for (const auto& candidate : ordered) {
        if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) || !std::isfinite(candidate.width) ||
            !std::isfinite(candidate.height) || !std::isfinite(candidate.confidence) || candidate.confidence < 0 ||
            candidate.confidence > 1 || candidate.width < 8 || candidate.height < 8)
            throw MaiPortraitFailure("invalid_face_bounds",
                                     "Host face bounds must be finite positive pixel coordinates");
        bool duplicate = false;
        for (const auto& previous : result) {
            const float width = std::max(0.0f, std::min(candidate.x + candidate.width, previous.x + previous.width) -
                                                   std::max(candidate.x, previous.x));
            const float height = std::max(0.0f, std::min(candidate.y + candidate.height, previous.y + previous.height) -
                                                    std::max(candidate.y, previous.y));
            const float intersection = width * height;
            const float smaller = std::min(candidate.width * candidate.height, previous.width * previous.height);
            const float unionArea =
                candidate.width * candidate.height + previous.width * previous.height - intersection;
            if (intersection / std::max(1.0f, unionArea) > 0.3f || intersection / std::max(1.0f, smaller) > 0.72f) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) result.push_back(candidate);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    return result;
}

MaiPortraitRegionPlan MaiPortraitRegionPlanner::faceRegion(int width, int height,
                                                           const std::vector<MaiPortraitFaceHint>& hints,
                                                           int subjectIndex) {
    if (width <= 0 || height <= 0 || width > 20'000 || height > 20'000 ||
        static_cast<int64_t>(width) * height > 100'000'000)
        throw MaiPortraitFailure("invalid_source_dimensions", "Source dimensions exceed the metadata budget");
    const auto faces = mergeFaces(hints);
    if (faces.empty())
        throw MaiPortraitFailure("face_detection_failed", "No reliable face was located in the source image");
    if (subjectIndex < 0 && faces.size() != 1)
        throw MaiPortraitFailure("subject_required",
                                 "Multiple faces were found; choose subject_index from left to right");
    const int selected = subjectIndex < 0 ? 0 : subjectIndex;
    if (selected < 0 || selected >= static_cast<int>(faces.size()))
        throw MaiPortraitFailure("invalid_subject_index", "subject_index does not identify a detected face");
    const auto& face = faces[selected];
    if (face.x < 0 || face.y < 0 || face.x + face.width > width + 1 || face.y + face.height > height + 1)
        throw MaiPortraitFailure("face_outside_source", "Detected face bounds must be inside the oriented source");
    const int side = std::max(128, static_cast<int>(std::ceil(std::max(face.width, face.height) * 2.5f)));
    const int regionWidth = std::min(width, side), regionHeight = std::min(height, side);
    if (regionWidth > 2048 || regionHeight > 2048)
        throw MaiPortraitFailure(
            "roi_budget_exceeded",
            "The native face region exceeds 2048 pixels per side; use a smaller source for this close-up");
    MaiPortraitRegionPlan plan;
    plan.sourceWidth = width;
    plan.sourceHeight = height;
    plan.faceCount = static_cast<int>(faces.size());
    plan.subjectIndex = selected;
    plan.selectedFace = face;
    plan.region = {std::clamp(static_cast<int>(std::floor(face.x + face.width * 0.5f - regionWidth * 0.5f)), 0,
                              width - regionWidth),
                   std::clamp(static_cast<int>(std::floor(face.y + face.height * 0.5f - regionHeight * 0.5f)), 0,
                              height - regionHeight),
                   regionWidth, regionHeight};
    return plan;
}

std::array<float, 8> MaiPortraitRegionPlanner::globalFeatures(const unsigned char* rgba, int width, int height,
                                                              int stride, const MaiPortraitRegionPlan& plan) {
    if (!rgba || width < 1 || height < 1 || width > 256 || height > 256 || stride < width * 4)
        throw MaiPortraitFailure("invalid_analysis_proxy", "A bounded oriented RGBA analysis proxy is required");
    double sum[3]{}, square[3]{};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel) {
                const double value = rgba[y * stride + x * 4 + channel];
                sum[channel] += value;
                square[channel] += value * value;
            }
    const double count = static_cast<double>(width) * height;
    double deviation[3]{};
    for (int channel = 0; channel < 3; ++channel) {
        sum[channel] /= count;
        deviation[channel] = std::sqrt(std::max(0.0, square[channel] / count - sum[channel] * sum[channel]));
    }
    return {1,
            static_cast<float>((sum[0] + sum[1] + sum[2]) / 765),
            static_cast<float>((deviation[0] + deviation[1] + deviation[2]) / 384),
            static_cast<float>(sum[0] / 255),
            static_cast<float>(sum[2] / 255),
            std::min(2.0f, static_cast<float>(plan.sourceWidth) / plan.sourceHeight) * 0.5f,
            std::min(1.0f, plan.selectedFace.width * plan.selectedFace.height /
                               (static_cast<float>(plan.sourceWidth) * plan.sourceHeight)),
            0};
}

bool MaiPortraitRegionPlanner::samePersonCrop(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    for (float value : a)
        if (!std::isfinite(value)) return false;
    for (float value : b)
        if (!std::isfinite(value)) return false;
    const float ax = a[2] - a[0], ay = a[3] - a[1], bx = b[2] - b[0], by = b[3] - b[1];
    const float first = std::hypot(ax, ay), second = std::hypot(bx, by), radius = std::min(first, second);
    if (radius < 1 || std::max(first, second) / radius > 1.15f) return false;
    return std::hypot(a[0] - b[0], a[1] - b[1]) < .08f * radius &&
           std::hypot(a[2] - b[2], a[3] - b[3]) < .08f * radius && (ax * bx + ay * by) / (first * second) > .997f;
}

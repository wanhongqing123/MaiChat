#include "MaiBodySkinRetouch.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

// 保留私有 v2 参考的四向 9px black-hat 和连通组件几何阈值。它是身体内区
// 暗细结构探测，不是通用毛发/纹身分割；调用方必须先完成可信人物与脸部保护。
cv::Mat repairSupport(const cv::Mat& source, const cv::Mat& scope, MaiBodySkinRetouchOutput& result) {
    cv::Mat gray;
    cv::cvtColor(source, gray, cv::COLOR_RGB2GRAY);
    cv::Mat response = cv::Mat::zeros(gray.size(), CV_8UC1);
    const cv::Point endpoints[4][2] = {{{0, 4}, {8, 4}}, {{4, 0}, {4, 8}},
                                      {{0, 0}, {8, 8}}, {{0, 8}, {8, 0}}};
    for (const auto& ends : endpoints) {
        cv::Mat kernel = cv::Mat::zeros(9, 9, CV_8UC1), blackhat;
        cv::line(kernel, ends[0], ends[1], cv::Scalar(1), 1, cv::LINE_8);
        cv::morphologyEx(gray, blackhat, cv::MORPH_BLACKHAT, kernel);
        cv::max(response, blackhat, response);
    }
    cv::Mat candidates;
    cv::threshold(response, candidates, 8, 255, cv::THRESH_BINARY);
    cv::bitwise_and(candidates, scope, candidates);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(candidates, labels, stats, centroids, 8, CV_32S);
    result.candidateComponents = count - 1;
    cv::Mat hair = cv::Mat::zeros(gray.size(), CV_8UC1);
    for (int label = 1; label < count; ++label) {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area < 2 || area > 100) continue;
        // 分量面积已受限，在其包围框内计算同一中心矩，避免每根毛都重扫完整图。
        // 两像素零区覆盖参考中最大膨胀半径；图边界裁剪与整图运算一致。
        const int left = stats.at<int>(label, cv::CC_STAT_LEFT);
        const int top = stats.at<int>(label, cv::CC_STAT_TOP);
        const cv::Rect bounds = cv::Rect(left - 2, top - 2, stats.at<int>(label, cv::CC_STAT_WIDTH) + 4,
                                         stats.at<int>(label, cv::CC_STAT_HEIGHT) + 4) &
                                cv::Rect(0, 0, labels.cols, labels.rows);
        const cv::Mat component = labels(bounds) == label;
        const cv::Moments moments = cv::moments(component, true);
        const double a = moments.mu20 / moments.m00, c = moments.mu02 / moments.m00;
        const double b = moments.mu11 / moments.m00;
        const double discriminant = std::sqrt((a - c) * (a - c) + 4 * b * b);
        const double major = (a + c + discriminant) / 2, minor = (a + c - discriminant) / 2;
        const double length = 4 * std::sqrt(std::max(major, 0.0));
        const double width = 4 * std::sqrt(std::max(minor, 0.0));
        const bool elongated = length >= 4 && length <= 40 && width <= 7 &&
                               major >= 3 * std::max(minor, 0.25);
        // 下采样/JPEG 可把短毛和毛囊变成紧凑暗点；保留参考中的紧凑分量通路。
        const bool compact = length >= 2 && length <= 14 && width <= 7 && area <= 65;
        if (!elongated && !compact) continue;
        const int radius = std::min(2, std::max(1, static_cast<int>(std::ceil(width * 0.5))));
        cv::Mat expanded;
        cv::dilate(component, expanded,
                   cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(radius * 2 + 1, radius * 2 + 1)));
        cv::Mat destination = hair(bounds);
        cv::bitwise_or(destination, expanded, destination);
        ++result.acceptedComponents;
    }
    cv::bitwise_and(hair, scope, hair);
    return hair;
}

cv::Mat cleanSkin(const cv::Mat& repaired, const cv::Mat& scope, MaiBodySkinMaterial* material = nullptr) {
    cv::Mat rgbFloat, lab;
    repaired.convertTo(rgbFloat, CV_32FC3, 1.0 / 255.0);
    cv::cvtColor(rgbFloat, lab, cv::COLOR_RGB2Lab);
    std::vector<cv::Mat> labChannels;
    cv::split(lab, labChannels);
    cv::Mat support;
    scope.convertTo(support, CV_32F, 1.0 / 255.0);
    cv::Mat numerator, denominator, guide;
    cv::GaussianBlur(labChannels[0].mul(support), numerator, cv::Size(19, 19), 3, 3, cv::BORDER_REPLICATE);
    cv::GaussianBlur(support, denominator, cv::Size(19, 19), 3, 3, cv::BORDER_REPLICATE);
    cv::divide(numerator, cv::max(denominator, 1e-6), guide);
    cv::Mat distance;
    cv::distanceTransform(scope, distance, cv::DIST_L2, cv::DIST_MASK_PRECISE);
    cv::Mat cleanLab = lab.clone();
    cv::Mat targetLab;
    if (material) {
        targetLab = lab.clone();
        material->alpha.assign(scope.total(), 0);
    }
    constexpr int radius = 9;
    constexpr float spatialSigma = 3.0f, rangeSigma = 6.0f, detailRetention = 0.20f;
    for (int y = 0; y < repaired.rows; ++y) {
        for (int x = 0; x < repaired.cols; ++x) {
            if (scope.at<unsigned char>(y, x) == 0) continue;
            const float centerL = guide.at<float>(y, x);
            float weightSum = 0;
            cv::Vec3f sum(0, 0, 0);
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= repaired.cols || yy >= repaired.rows ||
                        scope.at<unsigned char>(yy, xx) == 0) continue;
                    const float dL = guide.at<float>(yy, xx) - centerL;
                    const float weight = std::exp(-(dx * dx + dy * dy) / (2 * spatialSigma * spatialSigma) -
                                                  dL * dL / (2 * rangeSigma * rangeSigma));
                    sum += lab.at<cv::Vec3f>(yy, xx) * weight;
                    weightSum += weight;
                }
            }
            const cv::Vec3f smoothed = sum / std::max(weightSum, 1e-6f);
            // 低通 L 引导故意让高频体毛/毛囊暗点衰减；保留 20% 细节及大范围明暗。
            // 六像素内侧渐变只约束净肤阶段，Telea 的实际支持域另由 repairMask 诊断。
            const float t = std::min(distance.at<float>(y, x) / 6.0f, 1.0f);
            const float alpha = (1 - detailRetention) * t * t * (3 - 2 * t);
            cleanLab.at<cv::Vec3f>(y, x) = lab.at<cv::Vec3f>(y, x) * (1 - alpha) + smoothed * alpha;
            if (material) {
                targetLab.at<cv::Vec3f>(y, x) = lab.at<cv::Vec3f>(y, x) * detailRetention +
                                               smoothed * (1 - detailRetention);
                material->alpha[static_cast<std::size_t>(y) * scope.cols + x] =
                    cv::saturate_cast<unsigned char>(255 * t * t * (3 - 2 * t));
            }
        }
    }
    cv::Mat cleanFloat, converted;
    cv::cvtColor(cleanLab, cleanFloat, cv::COLOR_Lab2RGB);
    cleanFloat.convertTo(converted, CV_8UC3, 255);
    cv::Mat result = repaired.clone();
    converted.copyTo(result, scope);
    if (material) {
        cv::Mat targetRgb;
        cv::cvtColor(targetLab, targetRgb, cv::COLOR_Lab2RGB);
        material->target.resize(scope.total());
        for (std::size_t i = 0; i < scope.total(); ++i) {
            const auto value = targetRgb.ptr<cv::Vec3f>()[i];
            material->target[i] = {255 * value[0], 255 * value[1], 255 * value[2]};
        }
    }
    return result;
}

} // namespace

MaiBodySkinMaterial MaiBodySkinRetouch::buildMaterial(const unsigned char* rgba, int width, int height,
                                                      int rgbaStride, const unsigned char* bodyScope, int scopeStride) {
    if (!rgba || !bodyScope || width < 1 || height < 1 || width > 1024 || height > 1024 || rgbaStride < width * 4 ||
        scopeStride < width)
        throw std::invalid_argument("Body material requires one bounded complete source ROI");
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const unsigned char value = bodyScope[static_cast<std::size_t>(y) * scopeStride + x];
            if (value != 0 && value != 255) throw std::invalid_argument("Body material scope must be binary");
        }
    cv::Mat input(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), rgbaStride), source;
    cv::cvtColor(input, source, cv::COLOR_RGBA2RGB);
    const cv::Mat scope(height, width, CV_8UC1, const_cast<unsigned char*>(bodyScope), scopeStride);
    MaiBodySkinRetouchOutput diagnostics;
    const cv::Mat hair = repairSupport(source, scope, diagnostics);
    cv::Mat repaired;
    const int count = cv::countNonZero(hair);
    if (count > 0) cv::inpaint(source, hair, repaired, 3, cv::INPAINT_TELEA);
    else repaired = source;
    MaiBodySkinMaterial material;
    material.repairPixels = count;
    cleanSkin(repaired, scope, &material);
    return material;
}

MaiBodySkinRetouchOutput MaiBodySkinRetouch::apply(const unsigned char* rgba, int width, int height, int rgbaStride,
                                                 const unsigned char* bodyScope, int scopeStride, float strength) {
    if (!rgba || !bodyScope || width <= 0 || height <= 0 || width > 20'000 || height > 20'000 ||
        static_cast<std::int64_t>(width) * height > 32'000'000 || rgbaStride < width * 4 || scopeStride < width ||
        !std::isfinite(strength) || strength < 0 || strength > 1)
        throw std::invalid_argument("Body clean-skin requires bounded RGBA, a binary body scope, and strength in [0,1]");
    const std::size_t count = static_cast<std::size_t>(width) * height;
    MaiBodySkinRetouchOutput result;
    result.rgba.resize(count * 4);
    result.mask.assign(count, 0);
    result.repairMask.assign(count, 0);
    for (int y = 0; y < height; ++y) {
        std::memcpy(result.rgba.data() + static_cast<std::size_t>(y) * width * 4,
                    rgba + static_cast<std::size_t>(y) * rgbaStride, static_cast<std::size_t>(width) * 4);
        for (int x = 0; x < width; ++x) {
            const unsigned char value = bodyScope[static_cast<std::size_t>(y) * scopeStride + x];
            if (value != 0 && value != 255)
                throw std::invalid_argument("Body clean-skin scope must contain only 0 and 255");
            result.scopePixels += value != 0;
        }
    }
    // 必须在任何颜色转换前退出，避免零强度发生 RGB/Lab 往返量化变化。
    if (strength == 0 || result.scopePixels == 0) return result;
    cv::Mat input(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), rgbaStride), source;
    cv::cvtColor(input, source, cv::COLOR_RGBA2RGB);
    const cv::Mat scope(height, width, CV_8UC1, const_cast<unsigned char*>(bodyScope), scopeStride);
    const cv::Mat hair = repairSupport(source, scope, result);
    result.repairPixels = cv::countNonZero(hair);
    cv::Mat repaired;
    if (result.repairPixels != 0) cv::inpaint(source, hair, repaired, 3, cv::INPAINT_TELEA);
    else repaired = source.clone();
    const cv::Mat cleaned = cleanSkin(repaired, scope);
    const unsigned char coverage = cv::saturate_cast<unsigned char>(255 * strength);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            if (bodyScope[static_cast<std::size_t>(y) * scopeStride + x] == 0) continue;
            result.mask[pixel] = coverage;
            result.repairMask[pixel] = hair.at<unsigned char>(y, x);
            bool changed = false;
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned char before = result.rgba[pixel * 4 + channel];
                const unsigned char after = cv::saturate_cast<unsigned char>(
                    before * (1 - strength) + cleaned.at<cv::Vec3b>(y, x)[channel] * strength);
                result.rgba[pixel * 4 + channel] = after;
                changed |= before != after;
            }
            result.changedPixels += changed;
        }
    }
    return result;
}

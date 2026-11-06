// 文件作用：运行固定版本的 MediaPipe ONNX 感知模型并执行局部照片修饰。
// 设计边界：学习偏好不改变感知模型；低置信度定位不能替代为猜测的人体比例。
#include "MaiPortraitEngine.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "MaiFilePath.h"
#include "MaiPortraitRegion.h"
#include "MaiBodySkinRetouch.h"
#include "MaiPortraitWhiteningLut.h"

namespace {
constexpr float kPi = 3.14159265359f;
float sigmoid(float value) {
    return 1.0f / (1.0f + std::exp(-std::clamp(value, -60.0f, 60.0f)));
}
float smooth(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}
cv::Point2f mapped(const cv::Mat& matrix, cv::Point2f point) {
    return {static_cast<float>(matrix.at<double>(0, 0) * point.x + matrix.at<double>(0, 1) * point.y +
                               matrix.at<double>(0, 2)),
            static_cast<float>(matrix.at<double>(1, 0) * point.x + matrix.at<double>(1, 1) * point.y +
                               matrix.at<double>(1, 2))};
}
cv::Mat cropTransform(cv::Point2f center, float side, float degrees, int size) {
    cv::Mat matrix = cv::getRotationMatrix2D(center, degrees, size / static_cast<double>(side));
    matrix.at<double>(0, 2) += size * 0.5 - center.x;
    matrix.at<double>(1, 2) += size * 0.5 - center.y;
    return matrix;
}
cv::Mat rgbInput(const cv::Mat& rgba, const cv::Mat& transform, int size) {
    cv::Mat crop, rgb, floats;
    cv::warpAffine(rgba, crop, transform, {size, size}, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    cv::cvtColor(crop, rgb, cv::COLOR_RGBA2RGB);
    rgb.convertTo(floats, CV_32FC3, 1.0 / 255.0);
    return floats;
}
cv::Rect bounded(cv::Rect rectangle, int width, int height) {
    return rectangle & cv::Rect(0, 0, width, height);
}
struct Detection {
    cv::Rect2f box;
    cv::Point2f hip, extent;
    float score;
};
float overlap(const cv::Rect2f& first, const cv::Rect2f& second) {
    const float area = (first & second).area();
    return area / std::max(1.0f, first.area() + second.area() - area);
}

std::array<float, 3> sampleSkinField(const std::vector<std::array<float, 3>>& field,
                                  const MaiPortraitSkinPlan& plan, float gx, float gy) {
    const auto& domain = plan.analysisSourceRegion;
    const float px = (gx + .5f - domain[0]) * plan.proxyWidth / domain[2] - .5f,
                py = (gy + .5f - domain[1]) * plan.proxyHeight / domain[3] - .5f;
    const int left = cvFloor(px), top = cvFloor(py);
    std::array<float, 3> value{};
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int sx = std::clamp(left + dx, 0, plan.proxyWidth - 1),
                      sy = std::clamp(top + dy, 0, plan.proxyHeight - 1);
            const float w = (dx ? px - left : 1 - (px - left)) * (dy ? py - top : 1 - (py - top));
            const auto& sample = field[static_cast<size_t>(sy) * plan.proxyWidth + sx];
            for (int c = 0; c < 3; ++c) value[c] += w * sample[c];
        }
    return value;
}

// 工作场先在全 ROI 上求解；原生 tile 仅重建同一个场。局部双边半径3，8px halo
// 仅供这一步和已有几何采样使用，不能用于独立求解 Telea 或全局连通组件。
float sampleBodyAlpha(const MaiPortraitSkinPlan& plan, float gx, float gy) {
    if (plan.bodyAlpha.empty()) return 0;
    const auto& domain = plan.analysisSourceRegion;
    const float px = (gx + .5f - domain[0]) * plan.proxyWidth / domain[2] - .5f;
    const float py = (gy + .5f - domain[1]) * plan.proxyHeight / domain[3] - .5f;
    const int left = cvFloor(px), top = cvFloor(py);
    float alpha = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int sx = std::clamp(left + dx, 0, plan.proxyWidth - 1);
            const int sy = std::clamp(top + dy, 0, plan.proxyHeight - 1);
            const float w = (dx ? px - left : 1 - (px - left)) * (dy ? py - top : 1 - (py - top));
            alpha += w * plan.bodyAlpha[static_cast<size_t>(sy) * plan.proxyWidth + sx] / 255.f;
        }
    return alpha;
}

// D65 Lab 到尚未裁剪的线性sRGB，用于沿同色相压缩超色域目标；不以RGB硬截断改变肤色方向。
std::array<float, 3> whiteningLinearRgb(float L, float a, float b) {
    auto inverse = [](float value) {
        constexpr float d = 6.f / 29.f;
        return value > d ? value * value * value : 3 * d * d * (value - 4.f / 29.f);
    };
    const float fy = (L + 16) / 116;
    const float X = .950456f * inverse(fy + a / 500), Y = inverse(fy), Z = 1.088754f * inverse(fy - b / 200);
    return {3.240479f * X - 1.53715f * Y - .498535f * Z,
            -.969256f * X + 1.875991f * Y + .041556f * Z,
            .055648f * X - .204043f * Y + 1.057311f * Z};
}

bool whiteningInGamut(const std::array<float, 3>& rgb) {
    return rgb[0] >= 0 && rgb[0] <= 1 && rgb[1] >= 0 && rgb[1] <= 1 && rgb[2] >= 0 && rgb[2] <= 1;
}

void skinColor(const cv::Mat& source, cv::Mat& destination, const cv::Mat& mask, const cv::Rect& area,
               const MaiPortraitParameters::Values& values, const MaiPortraitSkinPlan* plan,
               int sourceX = 0, int sourceY = 0) {
    const bool clean = plan && plan->cleanApplied;
    if (!clean && values[0] == 0 && values[1] == 0 && values[2] == 0 && values[7] == 0) return;
    if (clean) {
        for (int y = area.y; y < area.y + area.height; ++y)
            for (int x = area.x; x < area.x + area.width; ++x) {
                const float alpha = plan->cleanStrength * std::min(mask.at<unsigned char>(y, x) / 255.f,
                    sampleBodyAlpha(*plan, x + sourceX, y + sourceY));
                if (alpha == 0) continue;
                const auto work = sampleSkinField(plan->materialOriginal, *plan, x + sourceX, y + sourceY);
                const auto target = sampleSkinField(plan->materialTarget, *plan, x + sourceX, y + sourceY);
                const auto original = source.at<cv::Vec4b>(y, x);
                auto& output = destination.at<cv::Vec4b>(y, x);
                for (int c = 0; c < 3; ++c) {
                    // 只加回20%原生高频，不能把原生细毛完整叠回已净肤的新源。
                    const float reconstructed = target[c] + .2f * (original[c] - work[c]);
                    output[c] = cv::saturate_cast<unsigned char>(original[c] + alpha * (reconstructed - original[c]));
                }
            }
    }
    if (values[0] > 0 || values[1] > 0 || values[2] != 0) {
        const cv::Mat base = destination(area).clone();
        cv::Mat rgb, blurred;
        if (values[0] > 0) {
            cv::cvtColor(base, rgb, cv::COLOR_RGBA2RGB);
            cv::bilateralFilter(rgb, blurred, 7, 22, 5);
        }
        for (int y = 0; y < area.height; ++y)
            for (int x = 0; x < area.width; ++x) {
                const float alpha = mask.at<unsigned char>(y + area.y, x + area.x) / 255.f;
                if (alpha == 0) continue;
                const auto original = base.at<cv::Vec4b>(y, x);
                const auto softened = values[0] > 0 ? blurred.at<cv::Vec3b>(y, x) :
                    cv::Vec3b(original[0], original[1], original[2]);
                auto& output = destination.at<cv::Vec4b>(y + area.y, x + area.x);
                for (int c = 0; c < 3; ++c) {
                    float value = original[c] + (softened[c] - original[c]) * values[0] * .65f;
                    value *= 1 + values[1] * .22f;
                    value += (c == 0 ? 1 : c == 2 ? -1 : 0) * values[2] * 8;
                    output[c] = cv::saturate_cast<unsigned char>(original[c] + (value - original[c]) * alpha);
                }
            }
    }
    if (values[7] > 0) {
        const float shader = MaiPortraitEngine::whiteningShaderStrength(values[7]);
        const float gate = MaiPortraitEngine::whiteningBlendGate(values[7]);
        // LUT只处理当前净肤新源；完整固定stages一起渐入，解决原shader的0->tiny跳变。
        std::vector<unsigned char> row(static_cast<size_t>(area.width) * 4);
        for (int y = area.y; y < area.y + area.height; ++y) {
            auto* pixels = destination.ptr<unsigned char>(y) + area.x * 4;
            MaiPortraitWhiteningLut::apply(pixels, row.data(), area.width, shader);
            cv::Mat inputRow(1, area.width, CV_8UC4, pixels), lutRow(1, area.width, CV_8UC4, row.data());
            cv::Mat inputRgb, lutRgb, inputLab, lutLab;
            cv::cvtColor(inputRow, inputRgb, cv::COLOR_RGBA2RGB);
            inputRgb.convertTo(inputRgb, CV_32F, 1. / 255);
            cv::cvtColor(lutRow, lutRgb, cv::COLOR_RGBA2RGB);
            lutRgb.convertTo(lutRgb, CV_32F, 1. / 255);
            cv::cvtColor(inputRgb, inputLab, cv::COLOR_RGB2Lab);
            cv::cvtColor(lutRgb, lutLab, cv::COLOR_RGB2Lab);
            for (int x = 0; x < area.width; ++x) {
                const float alpha = gate * mask.at<unsigned char>(y, x + area.x) / 255.f;
                if (alpha == 0) continue;
                const auto input = inputLab.at<cv::Vec3f>(0, x), target = lutLab.at<cv::Vec3f>(0, x);
                // 显式暖度/提亮已在pixels中生效。保护的是该阶段输入，不回看原片a/b抵消用户意图。
                // 原版LUT保留为成熟明度参考；暗皮肤不再被其固定skin stages额外染红。
                const auto guarded = MaiPortraitEngine::whiteningTargetRgb(
                    {input[0], input[1], input[2]}, {target[0], target[1], target[2]}, values[7]);
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = cv::saturate_cast<unsigned char>(255 * guarded[c]);
                for (int c = 0; c < 3; ++c)
                    pixels[x * 4 + c] = cv::saturate_cast<unsigned char>(
                        pixels[x * 4 + c] + (row[x * 4 + c] - pixels[x * 4 + c]) * alpha);
            }
        }
    }
}

void skinStatistics(const cv::Mat& source, const cv::Mat& edited, const cv::Mat& mask, const cv::Rect& area,
                    MaiPortraitOutput& result) {
    for (int top = area.y; top < area.y + area.height; top += 64) {
        const cv::Rect band(area.x, top, area.width, std::min(64, area.y + area.height - top));
        const auto active = cv::boundingRect(mask(band));
        if (active.empty()) continue;
        const cv::Rect tile(band.x + active.x, band.y + active.y, active.width, active.height);
        cv::Mat first, second, before, after;
        cv::cvtColor(source(tile), first, cv::COLOR_RGBA2RGB);
        cv::cvtColor(edited(tile), second, cv::COLOR_RGBA2RGB);
        first.convertTo(first, CV_32F, 1.0 / 255);
        second.convertTo(second, CV_32F, 1.0 / 255);
        cv::cvtColor(first, before, cv::COLOR_RGB2Lab);
        cv::cvtColor(second, after, cv::COLOR_RGB2Lab);
        for (int y = 0; y < tile.height; ++y)
            for (int x = 0; x < tile.width; ++x) {
                const double weight = mask.at<unsigned char>(y + tile.y, x + tile.x) / 255.0;
                if (weight <= 0) continue;
                ++result.skinPixels;
                result.skinWeight += weight;
                const auto firstPixel = before.at<cv::Vec3f>(y, x), secondPixel = after.at<cv::Vec3f>(y, x);
                result.skinLightness[0] += firstPixel[0] * weight;
                result.skinLightness[1] += secondPixel[0] * weight;
                result.skinChroma[0] += std::hypot(firstPixel[1], firstPixel[2]) * weight;
                result.skinChroma[1] += std::hypot(secondPixel[1], secondPixel[2]) * weight;
            }
    }
    if (result.skinWeight > 0)
        for (int index = 0; index < 2; ++index) {
            result.skinLightness[index] /= result.skinWeight;
            result.skinChroma[index] /= result.skinWeight;
        }
    for (int y = 0; y < source.rows; ++y)
        for (int x = 0; x < source.cols; ++x) {
            const auto first = source.at<cv::Vec4b>(y, x), second = edited.at<cv::Vec4b>(y, x);
            if (first[0] != second[0] || first[1] != second[1] || first[2] != second[2]) ++result.changedPixels;
        }
}
}  // namespace

MaiPortraitParameters::MaiPortraitParameters() : values{0.28f, 0.12f, 0, 0, 0, 0, 0, 0} {}

class MaiPortraitEngine::Implementation {
public:
    Implementation(const std::string& directory, const std::string& runtimePath, const void* supplied) {
        const OrtApiBase* base = static_cast<const OrtApiBase*>(supplied);
        if (!base) {
#if defined(_WIN32)
            library = LoadLibraryW(MaiFilePath::fromUtf8(runtimePath).value().c_str());
            auto symbol = library ? GetProcAddress(library, "OrtGetApiBase") : nullptr;
#else
            library = runtimePath.empty() ? nullptr : dlopen(runtimePath.c_str(), RTLD_NOW | RTLD_LOCAL);
            auto symbol = dlsym(library ? library : RTLD_DEFAULT, "OrtGetApiBase");
#endif
            using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
            if (symbol) base = reinterpret_cast<GetApiBase>(symbol)();
        }
        if (!base || !base->GetApi(ORT_API_VERSION)) throw std::runtime_error("Bundled ONNX Runtime is unavailable");
        Ort::InitApi(base->GetApi(ORT_API_VERSION));
        environment = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "MaiPortrait");
        options = Ort::SessionOptions();
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        modelDirectory = directory;
    }
    ~Implementation() {
        sessions.clear();
        environment.reset();
        // ORT C++ 的静态 API 指针及其他会话仍可能借用库；库映射保留至进程结束。
    }
    std::vector<Ort::Value> run(const char* file, cv::Mat& data, bool channelsFirst = false) {
        std::size_t index = 0;
        if (std::string(file) == "selfie_multiclass.onnx")
            index = 1;
        else if (std::string(file) == "person_detection.onnx")
            index = 2;
        else if (std::string(file) == "pose_estimation.onnx")
            index = 3;
        if (sessions.empty()) sessions.resize(4);
        if (!sessions[index]) {
            const auto path = MaiFilePath::fromUtf8(modelDirectory).append(MaiFilePath::fromUtf8(file));
            sessions[index] = std::make_unique<Ort::Session>(*environment, path.value().c_str(), options);
        }
        auto& session = *sessions[index];
        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = session.GetInputNameAllocated(0, allocator);
        const char* input = inputName.get();
        std::array<int64_t, 4> dimensions{1, data.rows, data.cols, 3};
        std::vector<float> planar;
        float* values = data.ptr<float>();
        if (channelsFirst) {
            dimensions = {1, 3, data.rows, data.cols};
            planar.resize(data.total() * 3);
            for (std::size_t pixel = 0; pixel < data.total(); ++pixel)
                for (int channel = 0; channel < 3; ++channel)
                    planar[channel * data.total() + pixel] = values[pixel * 3 + channel];
            values = planar.data();
        }
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor =
            Ort::Value::CreateTensor<float>(memory, values, data.total() * 3, dimensions.data(), dimensions.size());
        std::vector<Ort::AllocatedStringPtr> names;
        std::vector<const char*> pointers;
        for (std::size_t output = 0; output < session.GetOutputCount(); ++output) {
            names.push_back(session.GetOutputNameAllocated(output, allocator));
            pointers.push_back(names.back().get());
        }
        return session.Run(Ort::RunOptions{nullptr}, &input, &tensor, 1, pointers.data(), pointers.size());
    }
    cv::Mat skinMask(const cv::Mat& image, bool includeBody = false, cv::Mat* bodyOnly = nullptr) {
        cv::Mat small, rgb, input;
        cv::resize(image, small, {256, 256});
        cv::cvtColor(small, rgb, cv::COLOR_RGBA2RGB);
        rgb.convertTo(input, CV_32FC3, 1.0 / 255.0);
        auto outputs = run("selfie_multiclass.onnx", input);
        if (outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() != 256 * 256 * 6)
            throw std::runtime_error("Skin model output contract mismatch");
        const float* logits = outputs[0].GetTensorData<float>();
        cv::Mat mask(256, 256, CV_8U);
        cv::Mat body;
        if (bodyOnly) body.create(256, 256, CV_8U);
        for (int pixel = 0; pixel < 256 * 256; ++pixel) {
            const float* scores = logits + pixel * 6;
            const float maximum = *std::max_element(scores, scores + 6);
            float total = 0;
            for (int category = 0; category < 6; ++category) total += std::exp(scores[category] - maximum);
            const float probability =
                (std::exp(scores[3] - maximum) + (includeBody ? std::exp(scores[2] - maximum) : 0.f)) / total;
            mask.data[pixel] = static_cast<unsigned char>(255 * smooth((probability - 0.4f) / 0.45f));
            if (bodyOnly) {
                const float bodyProbability = std::exp(scores[2] - maximum) / total;
                body.data[pixel] = static_cast<unsigned char>(255 * smooth((bodyProbability - .4f) / .45f));
            }
        }
        cv::Mat resized;
        cv::resize(mask, resized, image.size(), 0, 0, cv::INTER_LINEAR);
        if (bodyOnly) cv::resize(body, *bodyOnly, image.size(), 0, 0, cv::INTER_LINEAR);
        return resized;
    }
    std::vector<cv::Point2f> mesh(const cv::Mat& image, const MaiPortraitFaceHint& face) {
        const cv::Point2f center(face.x + face.width * 0.5f, face.y + face.height * 0.5f);
        float degrees = 0;
        if (std::hypot(face.rightEyeX - face.leftEyeX, face.rightEyeY - face.leftEyeY) > face.width * 0.1f) {
            degrees = std::atan2(face.rightEyeY - face.leftEyeY, face.rightEyeX - face.leftEyeX) * 180 / kPi;
            if (degrees > 90) degrees -= 180;
            if (degrees < -90) degrees += 180;
        }
        auto transform = cropTransform(center, std::max(face.width, face.height) * 1.5f, degrees, 256);
        auto input = rgbInput(image, transform, 256);
        auto outputs = run("face_landmarks.onnx", input);
        if (outputs.size() < 2 || outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() != 1434 ||
            sigmoid(outputs[1].GetTensorData<float>()[0]) < 0.5f)
            throw MaiPortraitFailure("face_landmarks_failed",
                                     "Dense face landmarks are not reliable for the selected region");
        cv::Mat inverse;
        cv::invertAffineTransform(transform, inverse);
        const float* points = outputs[0].GetTensorData<float>();
        std::vector<cv::Point2f> result;
        for (int point = 0; point < 478; ++point)
            result.push_back(mapped(inverse, {points[point * 3], points[point * 3 + 1]}));
        return result;
    }
    std::vector<Detection> detectPeople(const cv::Mat& image, bool multiScale = false, int* windowCount = nullptr) {
        if (windowCount) *windowCount = 1;
        const float scale = 224.0f / std::max(image.cols, image.rows);
        const int resizedWidth = std::max(1, static_cast<int>(image.cols * scale));
        const int resizedHeight = std::max(1, static_cast<int>(image.rows * scale));
        cv::Mat small, rgb, normalized;
        cv::resize(image, small, {resizedWidth, resizedHeight});
        cv::cvtColor(small, rgb, cv::COLOR_RGBA2RGB);
        rgb.convertTo(normalized, CV_32FC3, 2.0 / 255.0, -1.0);
        const int left = (224 - resizedWidth) / 2, top = (224 - resizedHeight) / 2;
        cv::Mat input;
        cv::copyMakeBorder(normalized, input, top, 224 - resizedHeight - top, left, 224 - resizedWidth - left,
                           cv::BORDER_CONSTANT, cv::Scalar());
        auto outputs = run("person_detection.onnx", input, true);
        const float* boxes = nullptr;
        const float* scores = nullptr;
        for (auto& output : outputs) {
            const auto count = output.GetTensorTypeAndShapeInfo().GetElementCount();
            if (count == 2254 * 12) boxes = output.GetTensorData<float>();
            if (count == 2254) scores = output.GetTensorData<float>();
        }
        if (!boxes || !scores) throw std::runtime_error("Person detector output contract mismatch");
        std::vector<Detection> candidates;
        int anchor = 0;
        for (const auto grid : {28, 14, 7}) {
            const int repeats = grid == 7 ? 6 : 2;
            for (int y = 0; y < grid; ++y)
                for (int x = 0; x < grid; ++x)
                    for (int repeated = 0; repeated < repeats; ++repeated, ++anchor) {
                        const float confidence = sigmoid(scores[anchor]);
                        if (confidence < 0.6f) continue;
                        const float* box = boxes + anchor * 12;
                        const cv::Point2f origin((x + 0.5f) / grid * 224 - left, (y + 0.5f) / grid * 224 - top);
                        auto point = [&](int offset) {
                            return (origin + cv::Point2f(box[offset], box[offset + 1])) / scale;
                        };
                        const cv::Point2f center = point(0);
                        candidates.push_back({{center.x - box[2] / scale * 0.5f, center.y - box[3] / scale * 0.5f,
                                               box[2] / scale, box[3] / scale},
                                              point(4),
                                              point(6),
                                              confidence});
                    }
        }
        if (multiScale) {
            // 整幅 proxy 漏掉远处人物时做固定两层真实窗口分析，最多 1+9+49 次检测。
            // 模型阈值不变；所有虚拟点平移回同一 proxy，不能用预设身体比例制造检测。
            auto positions = [](int length, int size, int steps) {
                std::vector<int> values;
                for (int index = 0; index <= steps; ++index) {
                    const int value = (index * (length - size) + steps / 2) / steps;
                    if (values.empty() || values.back() != value) values.push_back(value);
                }
                return values;
            };
            for (int divisor : {2, 4}) {
                const int w = std::max(1, image.cols / divisor), h = std::max(1, image.rows / divisor);
                for (int y : positions(image.rows, h, divisor * 2 - 2))
                    for (int x : positions(image.cols, w, divisor * 2 - 2)) {
                        if (windowCount) ++*windowCount;
                        auto found = detectPeople(image(cv::Rect(x, y, w, h)));
                        for (auto person : found) {
                            person.box.x += x;
                            person.box.y += y;
                            person.hip += cv::Point2f(x, y);
                            person.extent += cv::Point2f(x, y);
                            candidates.push_back(person);
                        }
                    }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        std::vector<Detection> selected;
        for (const auto& candidate : candidates) {
            bool duplicate = false;
            for (const auto& previous : selected)
                if (overlap(candidate.box, previous.box) > 0.3f ||
                    MaiPortraitRegionPlanner::samePersonCrop(
                        {candidate.hip.x, candidate.hip.y, candidate.extent.x, candidate.extent.y},
                        {previous.hip.x, previous.hip.y, previous.extent.x, previous.extent.y}))
                    duplicate = true;
            if (!duplicate && selected.size() < 8) selected.push_back(candidate);
        }
        std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.hip.x < b.hip.x; });
        return selected;
    }
    std::pair<std::array<cv::Point2f, 33>, cv::Mat> pose(const cv::Mat& image, const Detection& person,
                                                         bool requireJoints = true) {
        const float side = cv::norm(person.extent - person.hip) * 2;
        if (side < 32 || side > std::max(image.cols, image.rows) * 4)
            throw std::runtime_error("Person crop is invalid");
        float radians = kPi * 0.5f - std::atan2(-(person.extent.y - person.hip.y), person.extent.x - person.hip.x);
        radians = std::remainder(radians, 2 * kPi);
        auto transform = cropTransform(person.hip, side, radians * 180 / kPi, 256);
        auto input = rgbInput(image, transform, 256);
        auto outputs = run("pose_estimation.onnx", input);
        if (outputs.size() < 3 || outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() != 195 ||
            outputs[1].GetTensorData<float>()[0] < 0.6f)
            throw std::runtime_error("Body pose could not be located reliably");
        const float* coordinates = outputs[0].GetTensorData<float>();
        cv::Mat inverse;
        cv::invertAffineTransform(transform, inverse);
        std::array<cv::Point2f, 33> points{};
        for (int index = 0; index < 33; ++index) {
            points[index] = mapped(inverse, {coordinates[index * 5], coordinates[index * 5 + 1]});
            if (requireJoints &&
                (index == 11 || index == 12 || index == 23 || index == 24 || index == 25 || index == 26 ||
                 index == 27 || index == 28) &&
                std::min(sigmoid(coordinates[index * 5 + 3]), sigmoid(coordinates[index * 5 + 4])) < 0.55f)
                throw std::runtime_error(
                    "Body joints are occluded or outside the photo; use a clear standing full-body photo");
        }
        const float* logits = outputs[2].GetTensorData<float>();
        cv::Mat small(256, 256, CV_8U);
        for (int index = 0; index < 256 * 256; ++index)
            small.data[index] = static_cast<unsigned char>(255 * sigmoid(logits[index]));
        cv::Mat mask;
        cv::warpAffine(small, mask, inverse, image.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT);
        return {points, mask};
    }

    void buildSkinMasks(MaiPortraitSkinPlan& plan, const cv::Mat& source, const std::vector<cv::Mat>& owners,
                        int selectedPerson, bool retainGlobalForeign) {
        const int width = source.cols, height = source.rows;
        const auto& domain = plan.analysisSourceRegion;
        cv::Mat semantic = cv::Mat::zeros(height, width, CV_8U), body = semantic.clone();
        cv::Mat foreign(height, width, CV_8U, cv::Scalar(0));
        cv::Rect semanticRegion(0, 0, width, height);
        if (selectedPerson >= 0) {
            cv::Mat binary;
            cv::threshold(owners[selectedPerson], binary, 127, 255, cv::THRESH_BINARY);
            const auto region = cv::boundingRect(binary);
            if (region.empty())
                throw MaiPortraitFailure("person_ownership_unreliable", "Selected person support is empty");
            semanticRegion = bounded({region.x - 2, region.y - 2, region.width + 4, region.height + 4}, width, height);
            const bool left = cv::countNonZero(binary.col(0)) > 0, right = cv::countNonZero(binary.col(width - 1)) > 0;
            const bool top = cv::countNonZero(binary.row(0)) > 0, bottom = cv::countNonZero(binary.row(height - 1)) > 0;
            if ((left && domain[0] > 0) || (right && domain[0] + domain[2] < plan.sourceWidth) ||
                (top && domain[1] > 0) || (bottom && domain[1] + domain[3] < plan.sourceHeight))
                throw MaiPortraitFailure("skin_refinement_incomplete",
                                         "Reliable person support reaches an artificial analysis edge");
            plan.visibleSourcePartial = plan.visibleSourcePartial || left || right || top || bottom;
        }
        cv::Mat bodySemantic;
        skinMask(source(semanticRegion), true, &bodySemantic).copyTo(semantic(semanticRegion));
        bodySemantic.copyTo(body(semanticRegion));
        cv::Mat ownership(height, width, CV_8U, cv::Scalar(0));
        plan.bodyConfidencePixels = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float gx = domain[0] + (x + .5f) * domain[2] / width,
                            gy = domain[1] + (y + .5f) * domain[3] / height;
                const auto& face = plan.selectedFace;
                float owner =
                    selectedPerson >= 0
                        ? smooth((owners[selectedPerson].at<unsigned char>(y, x) / 255.f - .4f) / .4f)
                        : (gx >= face.x && gx < face.x + face.width && gy >= face.y && gy < face.y + face.height ? 1.f
                                                                                                                 : 0.f);
                for (int index = 0; index < static_cast<int>(owners.size()); ++index)
                    if (index != selectedPerson && !owners[index].empty() &&
                        owners[index].at<unsigned char>(y, x) > 96) {
                        owner = 0;
                        foreign.at<unsigned char>(y, x) = 255;
                    }
                for (const auto& excluded : plan.excludedFaces)
                    if (gx >= excluded[0] && gx < excluded[0] + excluded[2] && gy >= excluded[1] &&
                        gy < excluded[1] + excluded[3])
                        owner = 0;
                ownership.at<unsigned char>(y, x) = cv::saturate_cast<unsigned char>(255 * owner);
                semantic.at<unsigned char>(y, x) =
                    cv::saturate_cast<unsigned char>(semantic.at<unsigned char>(y, x) * owner);
                const bool onFace = plan.faceCount > 0 && gx >= face.x - .15f * face.width &&
                                    gx < face.x + 1.15f * face.width && gy >= face.y - .15f * face.height &&
                                    gy < face.y + 1.15f * face.height;
                // 净肤只接受真实class2 + 可靠人物归属，绝不以combined-face mask去除纹理。
                body.at<unsigned char>(y, x) = selectedPerson >= 0 && !onFace && owner >= .75f
                    ? cv::saturate_cast<unsigned char>(body.at<unsigned char>(y, x) * owner) : 0;
                if (body.at<unsigned char>(y, x) >= 128) ++plan.bodyConfidencePixels;
            }
        plan.skinProxyRegion = {semanticRegion.x, semanticRegion.y, semanticRegion.width, semanticRegion.height};
        plan.alpha.assign(semantic.data, semantic.data + semantic.total());
        plan.bodyAlpha.assign(body.data, body.data + body.total());
        plan.ownership.assign(ownership.data, ownership.data + ownership.total());
        const auto pixels = source.clone();
        plan.proxyRgba.assign(pixels.data, pixels.data + pixels.total() * 4);
        plan.proxyWidth = width;
        plan.proxyHeight = height;
        if (retainGlobalForeign) {
            plan.globalForeignVeto.assign(foreign.data, foreign.data + foreign.total());
            plan.foreignProxyWidth = width;
            plan.foreignProxyHeight = height;
        }
    }

#if defined(_WIN32)
    HMODULE library = nullptr;
#else
    void* library = nullptr;
#endif
    std::unique_ptr<Ort::Env> environment;
    Ort::SessionOptions options{nullptr};
    std::string modelDirectory;
    std::vector<std::unique_ptr<Ort::Session>> sessions;
};

MaiPortraitEngine::MaiPortraitEngine(const std::string& modelDirectory, const std::string& runtimePath,
                                     const void* apiBase)
    : mImplementation(std::make_unique<Implementation>(modelDirectory, runtimePath, apiBase)) {}
MaiPortraitEngine::~MaiPortraitEngine() = default;

float MaiPortraitEngine::whiteningResponse(float strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw MaiPortraitFailure("invalid_parameters", "Whitening strength must be finite and bounded");
    return 1 - (1 - strength) * (1 - strength);
}

float MaiPortraitEngine::whiteningShaderStrength(float strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw MaiPortraitFailure("invalid_parameters", "Whitening strength must be finite and bounded");
    return strength * (5.f / 12.f);
}

float MaiPortraitEngine::whiteningBlendGate(float strength) {
    whiteningShaderStrength(strength);
    return smooth(strength / .05f);
}

float MaiPortraitEngine::whiteningShadowAmount(float strength) {
    whiteningShaderStrength(strength);
    return 25 * strength * (2 - strength);
}

float MaiPortraitEngine::whiteningShadowLightness(float lightness, float strength) {
    if (!std::isfinite(lightness) || lightness < 0 || lightness > 100)
        throw MaiPortraitFailure("invalid_parameters", "Whitening lightness must be finite and in [0,100]");
    const float amount = whiteningShadowAmount(strength);
    const float shoulder = std::max(0.f, 1 - lightness / 50.f);
    // 请求内固定D，去掉源均值反馈。固定强度下F'(L)>=1-.028D>=.3，3/15/50处C1。
    return lightness + amount * smooth((lightness - 3.f) / 12.f) * shoulder * shoulder;
}

std::array<float, 3> MaiPortraitEngine::whiteningTargetLab(const std::array<float, 3>& inputLab,
                                                         const std::array<float, 3>& lutLab, float strength) {
    whiteningShaderStrength(strength);
    for (const auto* lab : {&inputLab, &lutLab}) {
        for (float value : *lab)
            if (!std::isfinite(value)) throw MaiPortraitFailure("invalid_parameters", "Whitening Lab must be finite");
        if ((*lab)[0] < 0 || (*lab)[0] > 100 || std::abs((*lab)[1]) > 160 || std::abs((*lab)[2]) > 160)
            throw MaiPortraitFailure("invalid_parameters", "Whitening Lab is outside its bounded domain");
    }
    if (strength == 0) return inputLab;
    const float inputChroma = std::hypot(inputLab[1], inputLab[2]);
    const float lutChroma = std::hypot(lutLab[1], lutLab[2]);
    const float scale = inputChroma > 1e-4f ? std::min(1.f, lutChroma / inputChroma) : 0;
    return {whiteningShadowLightness(lutLab[0], strength), inputLab[1] * scale, inputLab[2] * scale};
}

std::array<float, 3> MaiPortraitEngine::whiteningTargetRgb(const std::array<float, 3>& inputLab,
                                                         const std::array<float, 3>& lutLab, float strength) {
    const auto target = whiteningTargetLab(inputLab, lutLab, strength);
    auto rgb = whiteningLinearRgb(target[0], target[1], target[2]);
    if (!whiteningInGamut(rgb)) {
        const float inputChroma = std::hypot(inputLab[1], inputLab[2]);
        const float lutChroma = std::hypot(lutLab[1], lutLab[2]);
        float low = 0, high = strength == 0 ? 1.f :
            (inputChroma > 1e-4f ? std::min(1.f, lutChroma / inputChroma) : 0);
        // 只沿目标Lab的a/b方向减色度；L和色相不变，20次覆盖远低于RGB8量化格的边界。
        for (int i = 0; i < 20; ++i) {
            const float middle = (low + high) * .5f;
            if (whiteningInGamut(whiteningLinearRgb(target[0], inputLab[1] * middle, inputLab[2] * middle))) low = middle;
            else high = middle;
        }
        rgb = whiteningLinearRgb(target[0], inputLab[1] * low, inputLab[2] * low);
    }
    for (float& value : rgb) {
        value = std::clamp(value, 0.f, 1.f);
        value = value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
    }
    return rgb;
}

// 色域边缘采用连续肩部，不把一段梯度硬钉在 254/255；仅临界颜色收缩，灰轴亮度保持。
std::array<float, 3> MaiPortraitEngine::skinRgbFromLab(const std::array<float, 3>& lab, float amount) {
    if (!std::isfinite(amount) || amount < 0 || amount > 1)
        throw MaiPortraitFailure("invalid_parameters", "Gamut amount must be finite and bounded");
    for (float value : lab)
        if (!std::isfinite(value)) throw MaiPortraitFailure("invalid_parameters", "Lab must be finite");
    if (lab[0] < 0 || lab[0] > 100 || std::abs(lab[1]) > 160 || std::abs(lab[2]) > 160)
        throw MaiPortraitFailure("invalid_parameters", "Lab is outside the supported color domain");
    if (lab[0] == 0) return {0, 0, 0};
    if (lab[0] == 100) return {1, 1, 1};
    auto inv = [](double f) {
        constexpr double d = 6. / 29;
        return f > d ? f * f * f : 3 * d * d * (f - 4. / 29);
    };
    const double fy = (lab[0] + 16.) / 116.;
    const double X = .950456 * inv(fy + lab[1] / 500.), Y = inv(fy), Z = 1.088754 * inv(fy - lab[2] / 200.);
    double r = 3.240479 * X - 1.537150 * Y - .498535 * Z;
    double g = -.969256 * X + 1.875991 * Y + .041556 * Z;
    double b = .055648 * X - .204043 * Y + 1.057311 * Z;
    const double y = .212671 * r + .715160 * g + .072169 * b;
    double q = 0;
    for (double value : {r - y, g - y, b - y}) q = std::max(q, value >= 0 ? value / (1 - y) : -value / y);
    double scale = 1;
    // 有效量包含真实软 mask；量趋零时肩部也趋零，避免在边缘产生固定色度跳变。
    // 越界目标仍映回色域，不依赖后续 RGB clamp。峰值肩部宽度保持 0.2。
    const double width = .2 * amount, knee = 1 - width;
    if (q > knee) {
        const double over = q - knee;
        const double mapped = width > 0 ? knee + width * over / (width + over) : 1.;
        scale = mapped / q;
    }
    r = y + (r - y) * scale;
    g = y + (g - y) * scale;
    b = y + (b - y) * scale;
    if (std::min({r, g, b}) < -1e-6 || std::max({r, g, b}) > 1 + 1e-6 || !std::isfinite(r + g + b))
        throw MaiPortraitFailure("invalid_color", "Continuous skin gamut mapping failed");
    auto gamma = [](double value) {
        return value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1. / 2.4) - .055;
    };
    return {static_cast<float>(gamma(r)), static_cast<float>(gamma(g)), static_cast<float>(gamma(b))};
}

std::array<float, 3> MaiPortraitEngine::toneSkinLab(const std::array<float, 3>& lab, float amount) {
    if (!std::isfinite(amount) || amount < 0 || amount > 1 || !std::isfinite(lab[0]) || !std::isfinite(lab[1]) ||
        !std::isfinite(lab[2]))
        throw MaiPortraitFailure("invalid_parameters", "Skin tone inputs must be finite and amount bounded");
    if (amount == 0) return lab;
    const float lightness = std::clamp(lab[0], 0.f, 100.f);
    const float luminance = lightness > 8.f ? std::pow((lightness + 16.f) / 116.f, 3.f) : lightness / 903.3f;
    const float gain = std::exp2(0.65f * amount * smooth((lightness - 3.f) / 15.f));
    const float exposed = gain * luminance / (1.f + (gain - 1.f) * luminance);
    const float edited = exposed > 0.008856f ? 116.f * std::cbrt(exposed) - 16.f : 903.3f * exposed;
    const float chromaGain = 1.f - .06f * amount * smooth((std::hypot(lab[1], lab[2]) - 10.f) / 25.f);
    return {edited, lab[1] * chromaGain, lab[2] * chromaGain};
}

MaiPortraitOutput MaiPortraitEngine::apply(const unsigned char* rgba, int width, int height, int stride,
                                           const std::vector<MaiPortraitFaceHint>& faceHints,
                                           const MaiPortraitParameters& parameters, bool body, int subjectIndex,
                                           const MaiPortraitRegionContext* context) {
    if (!rgba || width < 1 || height < 1 || stride < static_cast<int64_t>(width) * 4)
        throw MaiPortraitFailure("invalid_pixels", "Portrait pixels and row stride must be valid");
    if (static_cast<int64_t>(width) * height > 12'000'000)
        throw MaiPortraitFailure("image_too_large",
                                 "Image exceeds the 12 MP full-raster budget; use a bounded face region");
    if (context && (body || width > 2048 || height > 2048 || context->plan.region.width != width ||
                    context->plan.region.height != height))
        throw MaiPortraitFailure("roi_budget_exceeded", "Region pixels must match the bounded native face region");
    for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
        if (!std::isfinite(parameters.values[index]) || parameters.values[index] > 1 ||
            parameters.values[index] < (index == 2 ? -1 : 0))
            throw MaiPortraitFailure("invalid_parameters", "Portrait parameters are outside their allowed ranges");
    if (body && parameters.removeBodyHair.has_value())
        throw MaiPortraitFailure("invalid_parameters", "Body-hair removal is a beautify option, not body geometry");
    if (context && parameters.removeBodyHair.value_or(false))
        throw MaiPortraitFailure("body_skin_unavailable", "A face-only native ROI cannot establish body skin for explicit removal");
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), stride);
    cv::Mat destination = source.clone();
    MaiPortraitOutput result;
    result.whiteningShaderStrength = whiteningShaderStrength(parameters.values[7]);
    result.whiteningBlendGate = whiteningBlendGate(parameters.values[7]);
    result.whiteningShadowAmount = whiteningShadowAmount(parameters.values[7]);
    result.cleanSkipReason = context ? "face_region_only" : body ? "body_geometry_only" : "";
    result.cleanScope = "none";
    result.analysisVersion = "mediapipe-onnx-20261102-v2";
    cv::Mat thumbnail, rgb;
    cv::resize(source, thumbnail, {64, 64});
    cv::cvtColor(thumbnail, rgb, cv::COLOR_RGBA2RGB);
    cv::Scalar mean, deviation;
    cv::meanStdDev(rgb, mean, deviation);
    result.features = {1,
                       static_cast<float>((mean[0] + mean[1] + mean[2]) / 765),
                       static_cast<float>((deviation[0] + deviation[1] + deviation[2]) / 384),
                       static_cast<float>(mean[0] / 255),
                       static_cast<float>(mean[2] / 255),
                       std::min(2.0f, static_cast<float>(width) / height) * 0.5f,
                       0,
                       body ? 1.0f : 0.0f};
    if (context) result.features = context->globalFeatures;
    const auto& values = parameters.values;
    if (!body && !context) {
        // 普通完整光栅入口与移动端分块入口共用同一个人物/皮肤计划和颜色算子。
        // 颜色不依赖密集人脸或站姿；仅显式几何阶段继续要求可靠人脸 mesh。
        cv::Mat proxy;
        const double factor = std::min(1., 512. / std::max(width, height));
        cv::resize(source, proxy, {}, factor, factor);
        auto plan = analyzeSkin(proxy.data, proxy.cols, proxy.rows, static_cast<int>(proxy.step), width, height,
                                faceHints, subjectIndex);

        if (plan.refinementRequired) {
            const auto& r = plan.refinementSourceRegion;
            cv::Mat refined;
            cv::resize(source(cv::Rect(r[0], r[1], r[2], r[3])), refined,
                       {plan.refinementProxyWidth, plan.refinementProxyHeight});
            refineSkin(plan, refined.data, refined.cols, refined.rows, static_cast<int>(refined.step));
        }
        const auto& faceArea = plan.faceRegion;
        if (plan.faceCount > 0) {
            const auto facePixels = source(cv::Rect(faceArea[0], faceArea[1], faceArea[2], faceArea[3]));
            protectSkinFace(plan, facePixels.data, facePixels.cols, facePixels.rows, static_cast<int>(facePixels.step));
        } else
            protectSkinFace(plan, nullptr, 0, 0, 0);
        prepareSkinMaterial(plan, parameters);
        cv::Mat mask = cv::Mat::zeros(height, width, CV_8U);
        for (size_t index = 0; index < plan.tiles.size(); ++index) {
            const auto& tile = plan.tiles[index];
            const auto sample = source(cv::Rect(tile.sample[0], tile.sample[1], tile.sample[2], tile.sample[3]));
            const auto output = applySkinTile(sample.data, sample.cols, sample.rows, static_cast<int>(sample.step),
                                              plan, static_cast<int>(index), parameters);
            cv::Mat edited(sample.rows, sample.cols, CV_8UC4, const_cast<unsigned char*>(output.rgba.data()));
            cv::Mat alpha(sample.rows, sample.cols, CV_8U, const_cast<unsigned char*>(output.skinMask.data()));
            const cv::Rect local(tile.write[0] - tile.sample[0], tile.write[1] - tile.sample[1], tile.write[2],
                                 tile.write[3]);
            const cv::Rect write(tile.write[0], tile.write[1], tile.write[2], tile.write[3]);
            edited(local).copyTo(destination(write));
            alpha(local).copyTo(mask(write));
        }
        result.faceAnalysisPerformed = result.personAnalysisPerformed = result.skinAnalysisPerformed = true;
        result.faceCount = plan.faceCount;
        result.personCount = plan.personCount;
        result.personOwnershipReliable = plan.hasPersonOwnership;
        result.faceColorSkipped = plan.faceColorSkipped;
        result.unreliablePersonMasks = plan.unreliablePersonMasks;
        result.skinRefinementApplied = plan.refinementApplied;
        result.skinAnalysisSourceRegion = plan.analysisSourceRegion;
        result.skinAnalysisProxyWidth = plan.proxyWidth;
        result.skinAnalysisProxyHeight = plan.proxyHeight;
        result.cleanApplied = plan.cleanApplied;
        result.cleanStrength = plan.cleanStrength;
        result.cleanSkipReason = plan.cleanSkipReason;
        result.cleanScope = plan.cleanScope;
        result.bodyOwnershipSkipReason = plan.bodyOwnershipSkipReason;
        result.whiteningShaderStrength = plan.whiteningShaderStrength;
        result.whiteningBlendGate = plan.whiteningBlendGate;
        result.whiteningShadowAmount = plan.whiteningShadowAmount;
        result.skinScope = plan.hasPersonOwnership ? "selected_person_visible_skin" : "face_skin_only";
        result.analysisVersion = "mediapipe-onnx-skin-20261104-v3";
        if (plan.faceCount > 0)
            result.features[6] =
                plan.selectedFace.width * plan.selectedFace.height / (width * static_cast<float>(height));
        const auto active = cv::boundingRect(mask);
        result.processingRegion = {active.x, active.y, active.width, active.height};
        skinStatistics(source, destination, mask, {0, 0, width, height}, result);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto& face = plan.selectedFace;
                const bool onFace = plan.faceCount > 0 && x >= face.x - .15f * face.width &&
                                    x < face.x + 1.15f * face.width && y >= face.y - .15f * face.height &&
                                    y < face.y + 1.15f * face.height;
                if (!onFace && mask.at<unsigned char>(y, x) > 0) ++result.bodySkinPixels;
            }
        const bool color = values[0] > 0 || values[1] > 0 || values[2] != 0 || values[7] > 0 || plan.cleanApplied;
        if (color && (result.skinPixels == 0 ||
                      (plan.faceColorSkipped && (result.bodySkinPixels < 64 || plan.bodyConfidencePixels < 8))))
            throw MaiPortraitFailure("skin_mask_empty", "No sufficient reliably protected skin was found");
        if (color && result.changedPixels == 0 && values[3] == 0 && values[4] == 0 &&
            (parameters.removeBodyHair.value_or(false) ||
             std::max({values[0], values[1], std::abs(values[2]), values[7]}) > .05f))
            throw MaiPortraitFailure("no_visible_effect", "Skin processing did not change any RGB pixels");
        if (values[3] > 0 || values[4] > 0) {
            if (plan.faceCount == 0)
                throw MaiPortraitFailure("face_detection_failed", "Face geometry requires a reliable face");
            MaiPortraitRegionContext geometryContext;
            geometryContext.plan.sourceWidth = width;
            geometryContext.plan.sourceHeight = height;
            geometryContext.plan.faceCount = plan.faceCount;
            geometryContext.plan.subjectIndex = plan.subjectIndex;
            geometryContext.plan.selectedFace = plan.selectedFace;
            geometryContext.plan.region = {faceArea[0], faceArea[1], faceArea[2], faceArea[3]};
            geometryContext.globalFeatures = result.features;
            auto face = plan.selectedFace;
            face.x -= faceArea[0];
            face.y -= faceArea[1];
            face.leftEyeX -= faceArea[0];
            face.rightEyeX -= faceArea[0];
            face.leftEyeY -= faceArea[1];
            face.rightEyeY -= faceArea[1];
            auto geometry = parameters;
            geometry.removeBodyHair = false;
            for (int index : {0, 1, 2, 7}) geometry.values[index] = 0;
            const cv::Rect faceRect(faceArea[0], faceArea[1], faceArea[2], faceArea[3]);
            const auto facePixels = destination(faceRect);
            const auto output = apply(facePixels.data, facePixels.cols, facePixels.rows,
                                      static_cast<int>(facePixels.step), {face}, geometry, false, 0, &geometryContext);
            cv::Mat warped(facePixels.rows, facePixels.cols, CV_8UC4, const_cast<unsigned char*>(output.rgba.data()));
            warped.copyTo(destination(faceRect));
        }
        result.rgba.assign(destination.data, destination.data + destination.total() * 4);
        result.skinMask.assign(mask.data, mask.data + mask.total());
        return result;
    }
    if (!body) {
        auto faces = faceHints;
        std::sort(faces.begin(), faces.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
        result.faceAnalysisPerformed = true;
        result.faceCount = context ? context->plan.faceCount : static_cast<int>(faces.size());
        if (faces.empty())
            throw MaiPortraitFailure(
                "face_detection_failed",
                "No reliable face was detected; use automatic tiled detection or choose a clearer portrait");
        if (subjectIndex < 0 && faces.size() > 1)
            throw std::runtime_error(
                "Multiple faces detected; specify subject_index from left to right, starting at 0");
        const int selected = subjectIndex < 0 ? 0 : subjectIndex;
        if (selected >= static_cast<int>(faces.size()))
            throw std::runtime_error("subject_index does not identify a detected face");
        const auto& face = faces[selected];
        const auto points = mImplementation->mesh(source, face);
        if (!context)
            result.features[6] = std::min(1.0f, face.width * face.height / (width * static_cast<float>(height)));
        // 感知模型只看选中脸及上下文，避免大构图中的小脸在整图 256 输入中消失。
        const auto contextArea = bounded({cvFloor(face.x - face.width * 0.75f), cvFloor(face.y - face.height * 0.75f),
                                          cvCeil(face.width * 2.5f), cvCeil(face.height * 2.5f)},
                                         width, height);
        if (contextArea.empty()) throw MaiPortraitFailure("face_outside_source", "Face is outside the image");
        cv::Mat mask = cv::Mat::zeros(height, width, CV_8U);
        mImplementation->skinMask(source(contextArea)).copyTo(mask(contextArea));
        // 外轮廓也需要软边；不能让矩形 ROI 的边界成为贴回原图时的可见接缝。
        cv::Mat oval = cv::Mat::zeros(height, width, CV_8U);
        std::vector<cv::Point> faceOval;
        for (int index : {10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377,
                          152, 148, 176, 149, 150, 136, 172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109})
            faceOval.emplace_back(cvRound(points[index].x), cvRound(points[index].y));
        cv::fillConvexPoly(oval, faceOval, cv::Scalar(255));
        cv::GaussianBlur(oval, oval, {0, 0}, std::clamp(face.width * 0.015f, 1.0f, 12.0f));
        cv::multiply(mask, oval, mask, 1.0 / 255.0);
        // 眼、眉、唇区域即使被低分辨率皮肤分类覆盖，也不能进入平滑／调色。
        for (const auto& indices : std::vector<std::vector<int>>{{33, 160, 158, 133, 153, 144},
                                                                 {362, 385, 387, 263, 373, 380},
                                                                 {61, 40, 0, 270, 291, 321, 17, 91},
                                                                 {70, 63, 105, 66, 107},
                                                                 {336, 296, 334, 293, 300}}) {
            std::vector<cv::Point> polygon;
            for (int index : indices) polygon.emplace_back(cvRound(points[index].x), cvRound(points[index].y));
            cv::fillConvexPoly(mask, polygon, cv::Scalar(0));
            cv::polylines(mask, polygon, true, cv::Scalar(0), std::max(3, cvRound(face.width * 0.025f)));
        }
        const auto area = bounded({cvFloor(face.x - face.width * 0.15f), cvFloor(face.y - face.height * 0.15f),
                                   cvCeil(face.width * 1.3f), cvCeil(face.height * 1.3f)},
                                  width, height);
        if (area.empty()) throw std::runtime_error("Face is outside the image");
        result.processingRegion = {area.x, area.y, area.width, area.height};
        const float feather = std::clamp(face.width * 0.025f, 2.0f, 16.0f);
        for (int y = 0; y < mask.rows; ++y)
            for (int x = 0; x < mask.cols; ++x) {
                auto& value = mask.at<unsigned char>(y, x);
                if (!area.contains(cv::Point(x, y))) {
                    value = 0;
                    continue;
                }
                const float edge = std::min({static_cast<float>(x - area.x), static_cast<float>(y - area.y),
                                             static_cast<float>(area.x + area.width - 1 - x),
                                             static_cast<float>(area.y + area.height - 1 - y)});
                value = cv::saturate_cast<unsigned char>(value * smooth(edge / feather));
            }
        const bool colorRequested =
            values[0] != 0 || values[1] != 0 || values[2] != 0 || values[MaiPortraitParameters::kWhiteningIndex] != 0;
        if (colorRequested && cv::countNonZero(mask) == 0)
            throw MaiPortraitFailure(
                "skin_mask_empty",
                "A face was found but skin segmentation was not reliable; no retouched result was created");
        // 显式face-ROI也使用同一LUT和渐入映射，不创建第二条Lab美白路径。
        skinColor(source, destination, mask, area, values, nullptr);
        auto warpRegion = [&](cv::Point2f center, float radiusX, float radiusY, float amount, bool eye) {
            if (amount == 0 || radiusX < 2 || radiusY < 2) return;
            auto rectangle = bounded({cvFloor(center.x - radiusX), cvFloor(center.y - radiusY), cvCeil(radiusX * 2 + 1),
                                      cvCeil(radiusY * 2 + 1)},
                                     width, height);
            cv::Mat mapX(rectangle.size(), CV_32F), mapY(rectangle.size(), CV_32F);
            for (int y = 0; y < rectangle.height; ++y)
                for (int x = 0; x < rectangle.width; ++x) {
                    float dx = x + rectangle.x - center.x, dy = y + rectangle.y - center.y;
                    const float distance = std::sqrt(dx * dx / (radiusX * radiusX) + dy * dy / (radiusY * radiusY));
                    const float weight = smooth(1 - distance);
                    const float scale = eye ? 1 - amount * 0.16f * weight : 1 + amount * 0.16f * weight;
                    mapX.at<float>(y, x) = x + dx * (scale - 1);
                    mapY.at<float>(y, x) = eye ? y + dy * (scale - 1) : static_cast<float>(y);
                }
            cv::Mat patch;
            cv::remap(destination(rectangle).clone(), patch, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
            patch.copyTo(destination(rectangle));
        };
        warpRegion((points[234] + points[454]) * 0.5f + (points[152] - (points[234] + points[454]) * 0.5f) * 0.3f,
                   face.width * 0.64f, face.height * 0.55f, values[3], false);
        warpRegion((points[33] + points[133]) * 0.5f, face.width * 0.16f, face.width * 0.12f, values[4], true);
        warpRegion((points[362] + points[263]) * 0.5f, face.width * 0.16f, face.width * 0.12f, values[4], true);
        skinStatistics(source, destination, mask, area, result);
        if (colorRequested && result.changedPixels == 0 && values[3] == 0 && values[4] == 0)
            throw MaiPortraitFailure("no_visible_effect",
                                     "The requested edit produced no byte-level RGB change; inspect the skin mask "
                                     "instead of retrying stronger values blindly");
        result.skinMask.assign(mask.datastart, mask.dataend);
    } else {
        result.personAnalysisPerformed = true;
        auto people = mImplementation->detectPeople(source);
        result.personCount = static_cast<int>(people.size());
        if (people.empty()) throw std::runtime_error("No person was detected");
        if (subjectIndex < 0 && people.size() > 1)
            throw std::runtime_error(
                "Multiple people detected; specify subject_index from left to right, starting at 0");
        int selected = subjectIndex < 0 ? 0 : subjectIndex;
        if (selected >= static_cast<int>(people.size()))
            throw std::runtime_error("subject_index does not identify a detected person");
        auto analysis = mImplementation->pose(source, people[selected]);
        const auto& points = analysis.first;
        const auto hip = (points[23] + points[24]) * 0.5f;
        const auto shoulder = (points[11] + points[12]) * 0.5f;
        const float ankle = std::max(points[27].y, points[28].y), foot = std::max(points[29].y, points[30].y);
        if (hip.y <= shoulder.y + 20 || ankle <= hip.y + 30 ||
            std::abs(hip.x - shoulder.x) > (hip.y - shoulder.y) * 0.45f)
            throw std::runtime_error("Use an upright standing photo for body reshaping");
        const float extension = std::min((ankle - hip.y) * 0.08f * values[6], std::max(0.0f, height - 2.0f - foot));
        if (values[6] > 0 && extension < 1)
            throw std::runtime_error("Not enough space below the feet to extend the legs naturally");
        const float radiusX = std::max(static_cast<float>(cv::norm(points[23] - points[24])) * 1.6f, 30.0f);
        const float radiusY = (hip.y - shoulder.y) * 0.7f;
        const cv::Point2f waist = shoulder * 0.35f + hip * 0.65f;
        const auto rectangle = bounded({cvFloor(hip.x - radiusX * 1.8f), cvFloor(shoulder.y), cvCeil(radiusX * 3.6f),
                                        cvCeil(foot + extension - shoulder.y + 20)},
                                       width, height);
        if (rectangle.empty()) throw std::runtime_error("Body region is outside the image");
        cv::Mat mapX(rectangle.size(), CV_32F), mapY(rectangle.size(), CV_32F);
        for (int y = 0; y < rectangle.height; ++y)
            for (int x = 0; x < rectangle.width; ++x) {
                const float targetY = y + rectangle.y;
                float sourceY = targetY;
                if (targetY > hip.y)
                    sourceY =
                        targetY - extension * std::clamp((targetY - hip.y) / (ankle + extension - hip.y), 0.0f, 1.0f);
                const float dx = x + rectangle.x - waist.x, dy = sourceY - waist.y;
                const float weight =
                    smooth(1 - std::sqrt(dx * dx / (radiusX * radiusX) + dy * dy / (radiusY * radiusY)));
                mapX.at<float>(y, x) = x + dx * values[5] * 0.12f * weight;
                mapY.at<float>(y, x) = sourceY - rectangle.y;
            }
        cv::Mat warped, warpedMask;
        cv::remap(source(rectangle), warped, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
        cv::remap(analysis.second(rectangle), warpedMask, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
        cv::Mat holes = (analysis.second(rectangle) > 150) & (warpedMask < 100);
        if (cv::countNonZero(holes) > 0) {
            cv::Mat rgbPatch, filled;
            cv::cvtColor(source(rectangle), rgbPatch, cv::COLOR_RGBA2RGB);
            cv::inpaint(rgbPatch, holes, filled, 3, cv::INPAINT_TELEA);
            for (int y = 0; y < rectangle.height; ++y)
                for (int x = 0; x < rectangle.width; ++x)
                    if (holes.at<unsigned char>(y, x)) {
                        auto& pixel = destination.at<cv::Vec4b>(rectangle.y + y, rectangle.x + x);
                        const auto fill = filled.at<cv::Vec3b>(y, x);
                        for (int channel = 0; channel < 3; ++channel) pixel[channel] = fill[channel];
                    }
        }
        for (int y = 0; y < rectangle.height; ++y)
            for (int x = 0; x < rectangle.width; ++x) {
                const float alpha = smooth((warpedMask.at<unsigned char>(y, x) / 255.0f - 0.25f) / 0.5f);
                auto& target = destination.at<cv::Vec4b>(rectangle.y + y, rectangle.x + x);
                const auto edited = warped.at<cv::Vec4b>(y, x);
                for (int channel = 0; channel < 3; ++channel)
                    target[channel] =
                        cv::saturate_cast<unsigned char>(target[channel] + (edited[channel] - target[channel]) * alpha);
            }
    }
    result.rgba.assign(destination.datastart, destination.dataend);
    return result;
}

MaiPortraitSkinPlan MaiPortraitEngine::analyzeSkin(const unsigned char* rgba, int width, int height, int stride,
                                                   int sourceWidth, int sourceHeight,
                                                   const std::vector<MaiPortraitFaceHint>& faceHints,
                                                   int subjectIndex) {
    if (!rgba || width < 1 || height < 1 || width > 512 || height > 512 || stride < width * 4 || sourceWidth < 1 ||
        sourceHeight < 1 || sourceWidth > 20000 || sourceHeight > 20000 ||
        static_cast<int64_t>(sourceWidth) * sourceHeight > 32'000'000 ||
        std::abs(width / static_cast<double>(height) - sourceWidth / static_cast<double>(sourceHeight)) > .03)
        throw MaiPortraitFailure("invalid_analysis_proxy", "A proportional bounded whole-source proxy is required");
    MaiPortraitSkinPlan plan;
    plan.sourceWidth = sourceWidth;
    plan.sourceHeight = sourceHeight;
    plan.proxyWidth = width;
    plan.proxyHeight = height;
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), stride);
    const auto copied = source.clone();
    plan.proxyRgba.assign(copied.data, copied.data + copied.total() * 4);
    const auto faces = MaiPortraitRegionPlanner::mergeFaces(faceHints);
    plan.faceCount = static_cast<int>(faces.size());
    if (!faces.empty()) {
        const auto facePlan = MaiPortraitRegionPlanner::faceRegion(sourceWidth, sourceHeight, faces, subjectIndex);
        plan.subjectIndex = facePlan.subjectIndex;
        plan.selectedFace = facePlan.selectedFace;
        plan.faceRegion = {facePlan.region.x, facePlan.region.y, facePlan.region.width, facePlan.region.height};
    }
    auto people = mImplementation->detectPeople(source);
    std::vector<cv::Mat> owners;
    auto estimateOwners = [&] {
        owners.clear();
        plan.unreliablePersonMasks = 0;
        for (const auto& person : people) {
            try {
                owners.push_back(mImplementation->pose(source, person, false).second);
            } catch (const std::exception&) {
                owners.emplace_back();
                ++plan.unreliablePersonMasks;
            }
        }
    };
    bool ambiguousFaceOnly = false;
    std::vector<double> faceCoverages;
    auto choosePerson = [&] {
        faceCoverages.assign(owners.size(), 0);
        if (faces.empty()) {
            const int index = subjectIndex < 0 ? 0 : subjectIndex;
            if ((subjectIndex < 0 && people.size() != 1) || index < 0 || index >= static_cast<int>(owners.size()) ||
                owners[index].empty())
                return -1;
            return index;
        }
        const auto& face = plan.selectedFace;
        const auto region = bounded({cvFloor(face.x * width / sourceWidth), cvFloor(face.y * height / sourceHeight),
                                     std::max(1, cvCeil(face.width * width / sourceWidth)),
                                     std::max(1, cvCeil(face.height * height / sourceHeight))},
                                    width, height);
        double best = .2, second = 0;
        int selected = -1;
        for (int i = 0; i < static_cast<int>(owners.size()); ++i) {
            if (owners[i].empty() || region.empty()) continue;
            const double coverage = cv::mean(owners[i](region))[0] / 255;
            faceCoverages[i] = coverage;
            if (coverage > best) {
                second = best;
                best = coverage;
                selected = i;
            } else
                second = std::max(second, coverage);
        }
        if (selected >= 0 && second > .35 && best - second < .15) {
            // 唯一宿主脸已经通过原图边界校验；身体仍拒绝归属，不能任挑一个竞争seed。
            // 继续到原生mesh保护后才允许face-only，mesh失败仍由既有流程拒绝发布。
            if (faces.size() == 1) {
                ambiguousFaceOnly = true;
                plan.bodyOwnershipSkipReason = "overlapping_person_seeds";
                return -1;
            }
            throw MaiPortraitFailure("person_ownership_ambiguous", "Overlapping people cannot be separated reliably");
        }
        return selected;
    };
    estimateOwners();
    int selectedPerson = choosePerson();
    if (selectedPerson < 0 && !ambiguousFaceOnly) {
        int windows = 0;
        people = mImplementation->detectPeople(source, true, &windows);
        plan.personAnalysisWindows += windows;
        estimateOwners();
        selectedPerson = choosePerson();
    }
    plan.personCount = static_cast<int>(people.size());
    if (faces.empty()) {
        if (subjectIndex < 0 && people.size() != 1)
            throw MaiPortraitFailure("subject_required", "Choose one reliably detected person from left to right");
        if (selectedPerson < 0)
            throw MaiPortraitFailure("person_ownership_unreliable",
                                     "The selected person has no reliable ownership mask");
        plan.subjectIndex = selectedPerson;
    }
    // 一个实例遮罩不能同时把另一个明确检测的人脸认作同一目标；此时宁可明确失败。
    if (selectedPerson >= 0)
        for (int i = 0; i < static_cast<int>(faces.size()); ++i)
            if (i != plan.subjectIndex) {
                const auto& face = faces[i];
                const auto box =
                    bounded({cvFloor(face.x * width / sourceWidth), cvFloor(face.y * height / sourceHeight),
                             std::max(1, cvCeil(face.width * width / sourceWidth)),
                             std::max(1, cvCeil(face.height * height / sourceHeight))},
                            width, height);
                if (!box.empty() && cv::mean(owners[selectedPerson](box))[0] > 102)
                    throw MaiPortraitFailure("person_ownership_ambiguous",
                                             "The selected ownership mask also covers another person's face");
            }

    for (int index = 0; index < static_cast<int>(faces.size()); ++index)
        if (index != plan.subjectIndex) {
            const auto& face = faces[index];
            plan.excludedFaces.push_back(
                {face.x - .15f * face.width, face.y - .15f * face.height, face.width * 1.3f, face.height * 1.3f});
        }
    plan.hasPersonOwnership = selectedPerson >= 0;
    plan.selectedPerson = selectedPerson;
    plan.analysisSourceRegion = {0, 0, sourceWidth, sourceHeight};
    auto maskOwners = owners;
    if (ambiguousFaceOnly)
        for (size_t i = 0; i < maskOwners.size(); ++i)
            // 这些seed同时覆盖同一唯一脸，身份未定，不能当作该脸的可靠foreign证据。
            // 其余候选及其他脸的硬排除保持；所有候选都不获得身体归属。
            if (faceCoverages[i] > .35) maskOwners[i].release();
    mImplementation->buildSkinMasks(plan, source, maskOwners, selectedPerson, true);
    const float scaleX = sourceWidth / static_cast<float>(width), scaleY = sourceHeight / static_cast<float>(height);
    for (const auto& person : people)
        plan.people.push_back(
            {{person.box.x * scaleX, person.box.y * scaleY, person.box.width * scaleX, person.box.height * scaleY},
             {person.hip.x * scaleX, person.hip.y * scaleY},
             {person.extent.x * scaleX, person.extent.y * scaleY},
             person.score});
    if (selectedPerson >= 0) {
        const auto& person = people[selectedPerson];
        const float radius = cv::norm(person.extent - person.hip);
        cv::Mat binary;
        cv::threshold(owners[selectedPerson], binary, 96, 255, cv::THRESH_BINARY);
        const auto support = cv::boundingRect(binary);
        const auto circle = bounded(
            {cvFloor(person.hip.x - radius), cvFloor(person.hip.y - radius), cvCeil(radius * 2), cvCeil(radius * 2)},
            width, height);
        const auto extent = support | circle;
        cv::Rect original(cvFloor(extent.x * scaleX), cvFloor(extent.y * scaleY), cvCeil(extent.width * scaleX),
                          cvCeil(extent.height * scaleY));
        if (plan.faceCount > 0)
            original |= cv::Rect(cvFloor(plan.selectedFace.x), cvFloor(plan.selectedFace.y),
                                 cvCeil(plan.selectedFace.width), cvCeil(plan.selectedFace.height));
        const int halo = std::clamp(cvCeil(std::max(scaleX, scaleY) * 4), 16, 96);
        original =
            bounded({original.x - halo, original.y - halo, original.width + halo * 2, original.height + halo * 2},
                    sourceWidth, sourceHeight);
        // 从真实原图区域请求工作平面；不能将先前512全图proxy放大冒充新细节。
        const double scale = std::min(1., 1024. / std::max(original.width, original.height));
        const int refinedWidth = std::max(1, cvRound(original.width * scale)),
                  refinedHeight = std::max(1, cvRound(original.height * scale));
        const double oldSamples = std::max(original.width / scaleX, original.height / scaleY);
        if (std::max(refinedWidth, refinedHeight) > oldSamples * 1.01) {
            plan.refinementRequired = true;
            plan.refinementSourceRegion = {original.x, original.y, original.width, original.height};
            plan.refinementProxyWidth = refinedWidth;
            plan.refinementProxyHeight = refinedHeight;
        }
    }
    // 全图分成有界写入块，halo 只参与采样；统计和最终合成不重复处理重叠像素。
    for (int y = 0; y < sourceHeight; y += 1024)
        for (int x = 0; x < sourceWidth; x += 1024) {
            const cv::Rect write(x, y, std::min(1024, sourceWidth - x), std::min(1024, sourceHeight - y));
            const auto sample = bounded({x - 8, y - 8, write.width + 16, write.height + 16}, sourceWidth, sourceHeight);
            plan.tiles.push_back(
                {{sample.x, sample.y, sample.width, sample.height}, {write.x, write.y, write.width, write.height}});
        }
    return plan;
}

void MaiPortraitEngine::protectSkinFace(MaiPortraitSkinPlan& plan, const unsigned char* rgba, int width, int height,
                                        int stride) {
    if (plan.refinementRequired && !plan.refinementApplied)
        throw MaiPortraitFailure("skin_refinement_required",
                                 "Complete the bound regional analysis before face protection");
    if (plan.faceProtected) throw MaiPortraitFailure("invalid_skin_sequence", "Face protection was already prepared");
    plan.faceProtected = true;
    if (plan.faceCount == 0) return;
    const auto& area = plan.faceRegion;
    if (!rgba || width != area[2] || height != area[3] || stride < width * 4)
        throw MaiPortraitFailure("invalid_region_context", "Native face pixels must match the advertised face region");
    plan.faceProtection.assign(static_cast<size_t>(width) * height, 0);
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), stride);
    auto face = plan.selectedFace;
    face.x -= area[0];
    face.y -= area[1];
    face.leftEyeX -= area[0];
    face.rightEyeX -= area[0];
    face.leftEyeY -= area[1];
    face.rightEyeY -= area[1];
    try {
        const auto points = mImplementation->mesh(source, face);
        cv::Mat mask = mImplementation->skinMask(source, false), oval = cv::Mat::zeros(height, width, CV_8U);
        std::vector<cv::Point> polygon;
        for (int index : {10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377,
                          152, 148, 176, 149, 150, 136, 172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109})
            polygon.emplace_back(cvRound(points[index].x), cvRound(points[index].y));
        cv::fillPoly(oval, std::vector<std::vector<cv::Point>>{polygon}, cv::Scalar(255));
        cv::GaussianBlur(oval, oval, {}, std::clamp(face.width * .015f, 1.f, 12.f));
        plan.faceBlend.assign(oval.data, oval.data + oval.total());
        cv::multiply(mask, oval, mask, 1.0 / 255);
        cv::Mat exclusions(height, width, CV_8U, cv::Scalar(255));
        const std::vector<std::vector<int>> holes{{33, 160, 158, 133, 153, 144},
                                                  {362, 385, 387, 263, 373, 380},
                                                  {61, 40, 37, 0, 267, 270, 291, 321, 314, 17, 84, 91},
                                                  {70, 63, 105, 66, 107, 55, 65, 52, 53, 46},
                                                  {336, 296, 334, 293, 300, 276, 283, 282, 295, 285}};
        for (const auto& indices : holes) {
            std::vector<cv::Point> hole;
            for (int index : indices) hole.emplace_back(cvRound(points[index].x), cvRound(points[index].y));
            cv::fillPoly(exclusions, std::vector<std::vector<cv::Point>>{hole}, cv::Scalar(0));
            cv::polylines(exclusions, std::vector<std::vector<cv::Point>>{hole}, true, cv::Scalar(0),
                          std::max(3, cvRound(face.width * .025f)));
        }
        // 全图和原生上下文可能给出不同眼点；与 mesh 保护取保守并集，不推断嘴部位置。
        // coarse 采样尺度只界定邻域，不代表 Vision 实测误差；半径受脸及真实眼距约束。
        const float sampling = std::max(plan.sourceWidth / static_cast<float>(std::max(1, plan.foreignProxyWidth)),
                                        plan.sourceHeight / static_cast<float>(std::max(1, plan.foreignProxyHeight)));
        const float eyeDistance = std::hypot(face.leftEyeX - face.rightEyeX, face.leftEyeY - face.rightEyeY);
        const float capX =
            std::min(face.width * .14f, eyeDistance > face.width * .05f ? eyeDistance * .45f : face.width * .14f);
        const float capY =
            std::min(face.height * .16f, eyeDistance > face.width * .05f ? eyeDistance * .65f : face.height * .16f);
        const float rx = std::max(1.f, std::min(std::max(face.width * .065f, sampling), capX));
        const float ry = std::max(1.f, std::min(std::max(face.height * .045f, sampling), capY));
        for (const auto& eye :
             std::array<cv::Point2f, 2>{{{face.leftEyeX, face.leftEyeY}, {face.rightEyeX, face.rightEyeY}}}) {
            if (eye.x < face.x || eye.x > face.x + face.width || eye.y < face.y || eye.y > face.y + face.height)
                continue;
            const auto bounds = bounded({cvFloor(eye.x - rx * 1.25f), cvFloor(eye.y - ry * 1.25f),
                                         cvCeil(rx * 2.5f) + 1, cvCeil(ry * 2.5f) + 1},
                                        width, height);
            for (int y = bounds.y; y < bounds.y + bounds.height; ++y)
                for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
                    const float distance = std::hypot((x - eye.x) / rx, (y - eye.y) / ry);
                    const auto alpha = cv::saturate_cast<unsigned char>(255 * smooth((distance - 1) / .25f));
                    exclusions.at<unsigned char>(y, x) = std::min(exclusions.at<unsigned char>(y, x), alpha);
                }
        }
        cv::multiply(mask, exclusions, mask, 1. / 255);
        plan.faceExclusion.assign(exclusions.data, exclusions.data + exclusions.total());
        plan.faceProtection.assign(mask.data, mask.data + mask.total());
        if (cv::countNonZero(mask) == 0) plan.faceColorSkipped = true;
    } catch (const std::exception&) {
        plan.faceColorSkipped = true;
    }
}

void MaiPortraitEngine::prepareSkinMaterial(MaiPortraitSkinPlan& plan, const MaiPortraitParameters& parameters) {
    if (!plan.faceProtected || plan.materialPrepared)
        throw MaiPortraitFailure("invalid_skin_sequence", "Prepare the shared body material once after face protection");
    const size_t count = static_cast<size_t>(plan.proxyWidth) * plan.proxyHeight;
    const auto& domain = plan.analysisSourceRegion;
    if (plan.proxyWidth < 1 || plan.proxyHeight < 1 || plan.proxyWidth > 1024 || plan.proxyHeight > 1024 ||
        plan.proxyRgba.size() != count * 4 || plan.bodyAlpha.size() != count ||
        (plan.hasPersonOwnership && plan.ownership.size() != count) || plan.sourceWidth < 1 || plan.sourceHeight < 1 ||
        domain[0] < 0 || domain[1] < 0 || domain[2] < 1 || domain[3] < 1 ||
        static_cast<int64_t>(domain[0]) + domain[2] > plan.sourceWidth ||
        static_cast<int64_t>(domain[1]) + domain[3] > plan.sourceHeight ||
        (!plan.globalForeignVeto.empty() && (plan.foreignProxyWidth < 1 || plan.foreignProxyHeight < 1 ||
         plan.globalForeignVeto.size() != static_cast<size_t>(plan.foreignProxyWidth) * plan.foreignProxyHeight)))
        throw MaiPortraitFailure("invalid_region_context", "Body material requires its owned source workplane");
    for (int i = 0; i < 8; ++i)
        if (!std::isfinite(parameters.values[i]) || parameters.values[i] > 1 ||
            parameters.values[i] < (i == 2 ? -1 : 0))
            throw MaiPortraitFailure("invalid_parameters", "Body material parameters must be finite and bounded");
    plan.whiteningShaderStrength = whiteningShaderStrength(parameters.values[7]);
    plan.whiteningBlendGate = whiteningBlendGate(parameters.values[7]);
    plan.whiteningShadowAmount = whiteningShadowAmount(parameters.values[7]);
    plan.cleanStrength = parameters.removeBodyHair ? (*parameters.removeBodyHair ? 1.f : 0.f) :
        smooth(std::max(parameters.values[0], parameters.values[7]) / .2f);
    plan.cleanScope = "none";
    cv::Mat scope(plan.proxyHeight, plan.proxyWidth, CV_8U);
    for (size_t i = 0; i < count; ++i)
        scope.data[i] = plan.hasPersonOwnership && plan.bodyAlpha[i] > 96 ? 255 : 0;
    // 与已看原型一致的内区成熟过程；只修补小孔，腐蚀保护衣服/身体轮廓边缘。
    cv::morphologyEx(scope, scope, cv::MORPH_CLOSE,
                    cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(7, 7)));
    cv::erode(scope, scope, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(9, 9)));
    for (int y = 0; y < plan.proxyHeight; ++y)
        for (int x = 0; x < plan.proxyWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * plan.proxyWidth + x;
            const float gx = domain[0] + (x + .5f) * domain[2] / plan.proxyWidth;
            const float gy = domain[1] + (y + .5f) * domain[3] / plan.proxyHeight;
            bool forbidden = !plan.hasPersonOwnership || plan.ownership[i] < 192;
            const auto& face = plan.selectedFace;
            if (plan.faceCount > 0 && gx >= face.x - .15f * face.width && gx < face.x + 1.15f * face.width &&
                gy >= face.y - .15f * face.height && gy < face.y + 1.15f * face.height) forbidden = true;
            for (const auto& excluded : plan.excludedFaces)
                if (gx >= excluded[0] && gx < excluded[0] + excluded[2] && gy >= excluded[1] &&
                    gy < excluded[1] + excluded[3]) forbidden = true;
            if (!plan.globalForeignVeto.empty()) {
                const int vx = std::clamp(static_cast<int>(gx * plan.foreignProxyWidth / plan.sourceWidth), 0,
                                           plan.foreignProxyWidth - 1);
                const int vy = std::clamp(static_cast<int>(gy * plan.foreignProxyHeight / plan.sourceHeight), 0,
                                           plan.foreignProxyHeight - 1);
                if (plan.globalForeignVeto[static_cast<size_t>(vy) * plan.foreignProxyWidth + vx]) forbidden = true;
            }
            if (forbidden) scope.data[i] = 0;
        }
    const int bodyPixels = cv::countNonZero(scope);
    plan.cleanApplied = plan.cleanStrength > 0 && bodyPixels >= 64;
    if (plan.cleanApplied) {
        // 全 ROI 一次 Telea/连通组件/低通引导，峰值场大小受1024²约束，与32MP源大小无关。
        const auto material = MaiBodySkinRetouch::buildMaterial(plan.proxyRgba.data(), plan.proxyWidth,
            plan.proxyHeight, plan.proxyWidth * 4, scope.data, plan.proxyWidth);
        plan.materialTarget = material.target;
        plan.bodyAlpha = material.alpha;
        plan.materialOriginal.resize(count);
        for (size_t i = 0; i < count; ++i)
            plan.materialOriginal[i] = {static_cast<float>(plan.proxyRgba[i * 4]),
                static_cast<float>(plan.proxyRgba[i * 4 + 1]), static_cast<float>(plan.proxyRgba[i * 4 + 2])};
        plan.cleanScope = "selected_person_class2_body_interior";
    } else {
        cv::Mat distance;
        cv::distanceTransform(scope, distance, cv::DIST_L2, cv::DIST_MASK_PRECISE);
        for (size_t i = 0; i < count; ++i)
            plan.bodyAlpha[i] = bodyPixels >= 64 && scope.data[i] ? cv::saturate_cast<unsigned char>(
                255 * smooth(distance.ptr<float>()[i] / 6.f)) : 0;
        plan.cleanSkipReason = plan.cleanStrength == 0 ?
            (parameters.removeBodyHair ? "disabled_by_parameter" : "zero_auto_strength") : "body_skin_unavailable";
        if (parameters.removeBodyHair.value_or(false))
            throw MaiPortraitFailure("body_skin_unavailable", "Explicit body-hair removal requires reliable class2 body skin");
    }
    plan.materialPrepared = true;
}

MaiPortraitOutput MaiPortraitEngine::applySkinTile(const unsigned char* rgba, int width, int height, int stride,
                                                   const MaiPortraitSkinPlan& plan, int tileIndex,
                                                   const MaiPortraitParameters& parameters) {
    if (!plan.faceProtected || !plan.materialPrepared || tileIndex < 0 || tileIndex >= static_cast<int>(plan.tiles.size()))
        throw MaiPortraitFailure("invalid_skin_sequence", "Prepare face protection before processing planned tiles");
    const auto& tile = plan.tiles[tileIndex];
    if (!rgba || width != tile.sample[2] || height != tile.sample[3] || stride < width * 4 || width > 1040 ||
        height > 1040)
        throw MaiPortraitFailure("invalid_region_context",
                                 "Native pixels must match the planned bounded sample rectangle");
    const auto& sample = tile.sample;
    const auto& writeBounds = tile.write;
    if (sample[0] < 0 || sample[1] < 0 || static_cast<int64_t>(sample[0]) + width > plan.sourceWidth ||
        static_cast<int64_t>(sample[1]) + height > plan.sourceHeight || writeBounds[0] < sample[0] ||
        writeBounds[1] < sample[1] || writeBounds[2] < 1 || writeBounds[3] < 1 ||
        static_cast<int64_t>(writeBounds[0]) + writeBounds[2] > sample[0] + width ||
        static_cast<int64_t>(writeBounds[1]) + writeBounds[3] > sample[1] + height)
        throw MaiPortraitFailure("invalid_region_context",
                                 "Skin write rectangle must be contained in its source sample");

    const auto& domain = plan.analysisSourceRegion;
    if (domain[0] < 0 || domain[1] < 0 || domain[2] < 1 || domain[3] < 1 ||
        static_cast<int64_t>(domain[0]) + domain[2] > plan.sourceWidth ||
        static_cast<int64_t>(domain[1]) + domain[3] > plan.sourceHeight ||
        (!plan.globalForeignVeto.empty() &&
         (plan.foreignProxyWidth < 1 || plan.foreignProxyHeight < 1 ||
          plan.globalForeignVeto.size() != static_cast<size_t>(plan.foreignProxyWidth) * plan.foreignProxyHeight)))
        throw MaiPortraitFailure("invalid_region_context", "Skin analysis domain or foreign veto is invalid");
    const size_t proxyPixels = static_cast<size_t>(plan.proxyWidth) * plan.proxyHeight;
    const size_t facePixels = static_cast<size_t>(plan.faceRegion[2]) * plan.faceRegion[3];
    if (plan.proxyWidth < 1 || plan.proxyWidth > 1024 || plan.proxyHeight < 1 || plan.proxyHeight > 1024 ||
        plan.sourceWidth < 1 || plan.sourceHeight < 1 || plan.alpha.size() != proxyPixels ||
        plan.proxyRgba.size() != proxyPixels * 4 || plan.bodyAlpha.size() != proxyPixels ||
        (plan.cleanApplied && (plan.materialOriginal.size() != proxyPixels || plan.materialTarget.size() != proxyPixels)) ||
        (plan.hasPersonOwnership && plan.ownership.size() != proxyPixels) ||
        (plan.faceCount > 0 && !plan.faceColorSkipped &&
         (plan.faceProtection.size() != facePixels || plan.faceBlend.size() != facePixels ||
          plan.faceExclusion.size() != facePixels)))
        throw MaiPortraitFailure("invalid_region_context", "Skin plan masks do not match their owned dimensions");
    for (int index = 0; index < 8; ++index)
        if (!std::isfinite(parameters.values[index]) || parameters.values[index] > 1 ||
            parameters.values[index] < (index == 2 ? -1 : 0))
            throw MaiPortraitFailure("invalid_parameters", "Skin parameters must be finite and in range");
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), stride), destination = source.clone();
    cv::Mat mask = cv::Mat::zeros(height, width, CV_8U);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int gx = x + tile.sample[0], gy = y + tile.sample[1];

            const auto& domain = plan.analysisSourceRegion;
            if (gx < domain[0] || gy < domain[1] || gx >= domain[0] + domain[2] || gy >= domain[1] + domain[3])
                continue;
            if (!plan.globalForeignVeto.empty()) {
                const int vx = std::clamp(static_cast<int>((gx + .5) * plan.foreignProxyWidth / plan.sourceWidth), 0,
                                          plan.foreignProxyWidth - 1);
                const int vy = std::clamp(static_cast<int>((gy + .5) * plan.foreignProxyHeight / plan.sourceHeight), 0,
                                          plan.foreignProxyHeight - 1);
                if (plan.globalForeignVeto[static_cast<size_t>(vy) * plan.foreignProxyWidth + vx]) continue;
            }
            bool forbidden = false;
            for (const auto& excluded : plan.excludedFaces)
                if (gx >= excluded[0] && gx < excluded[0] + excluded[2] && gy >= excluded[1] &&
                    gy < excluded[1] + excluded[3]) {
                    forbidden = true;
                    break;
                }
            if (forbidden) continue;
            const auto& face = plan.selectedFace;
            if (plan.faceColorSkipped && plan.faceCount > 0 && gx >= face.x - .15f * face.width &&
                gx < face.x + 1.15f * face.width && gy >= face.y - .15f * face.height &&
                gy < face.y + 1.15f * face.height)
                continue;
            if (!plan.bodyOwnershipSkipReason.empty() && !plan.hasPersonOwnership) {
                // 歧义退至唯一脸时只接受实际mesh保护后的原生范围。2.5倍采样ROI和
                // coarse combined alpha均不是授权皮肤域，不能留下框边上采样残量。
                const int nativeX = gx - plan.faceRegion[0], nativeY = gy - plan.faceRegion[1];
                if (!plan.faceColorSkipped && nativeX >= 0 && nativeY >= 0 && nativeX < plan.faceRegion[2] &&
                    nativeY < plan.faceRegion[3]) {
                    const size_t i = static_cast<size_t>(nativeY) * plan.faceRegion[2] + nativeX;
                    mask.at<unsigned char>(y, x) = cv::saturate_cast<unsigned char>(
                        plan.faceProtection[i] * (plan.faceExclusion[i] / 255.f));
                }
                continue;
            }
            // 原生颜色引导双线性上采样，不让低分辨率皮肤边界均匀铺到高对比衣服/背景。
            const float px = (gx + .5f - domain[0]) * plan.proxyWidth / domain[2] - .5f;
            const float py = (gy + .5f - domain[1]) * plan.proxyHeight / domain[3] - .5f;
            const int left = cvFloor(px), top = cvFloor(py);
            float sum = 0, ownerWeight = 0;
            const auto original = source.at<cv::Vec4b>(y, x);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::clamp(left + dx, 0, plan.proxyWidth - 1),
                              sy = std::clamp(top + dy, 0, plan.proxyHeight - 1);
                    const size_t index = static_cast<size_t>(sy) * plan.proxyWidth + sx;
                    float distance = 0;
                    for (int c = 0; c < 3; ++c) {
                        const float d = original[c] - plan.proxyRgba[index * 4 + c];
                        distance += d * d;
                    }
                    const float spatial = (dx ? px - left : 1 - (px - left)) * (dy ? py - top : 1 - (py - top));
                    const float originalSupport = std::exp(-distance / 1800.f);
                    const float w = spatial * originalSupport;
                    sum += w * plan.alpha[index];
                    if (plan.hasPersonOwnership) ownerWeight += spatial * plan.ownership[index] / 255.f;
                }
            // 保留绝对颜色支持度：不能除以 weight，否则四个邻居均为皮肤时远色衣服也会得到满 alpha。
            // 成熟class2身体内区不再因原生黑毛与工作平面的RGB差异被挖回棋盘。
            // 边缘仍用绝对颜色支持；人物/其他脸/五官的硬排除始终在此之前或之后执行。
            float alpha = std::max(sum, 255 * sampleBodyAlpha(plan, gx, gy));
            const int fx = gx - plan.faceRegion[0], fy = gy - plan.faceRegion[1];
            if (!plan.faceColorSkipped && plan.faceCount > 0 && fx >= 0 && fy >= 0 && fx < plan.faceRegion[2] &&
                fy < plan.faceRegion[3]) {
                const size_t i = static_cast<size_t>(fy) * plan.faceRegion[2] + fx;
                alpha = (alpha * (1 - plan.faceBlend[i] / 255.f) +
                         plan.faceProtection[i] * (plan.hasPersonOwnership ? ownerWeight : 1.f)) *
                        plan.faceExclusion[i] / 255.f;
            }
            mask.at<unsigned char>(y, x) = cv::saturate_cast<unsigned char>(alpha);
        }
    const bool hasSkin = cv::countNonZero(mask) > 0;
    if (hasSkin) skinColor(source, destination, mask, {0, 0, width, height}, parameters.values,
                           &plan, tile.sample[0], tile.sample[1]);
    MaiPortraitOutput result;
    result.analysisVersion = "mediapipe-onnx-skin-20261104-v3";
    result.cleanApplied = plan.cleanApplied;
    result.cleanStrength = plan.cleanStrength;
    result.cleanSkipReason = plan.cleanSkipReason;
    result.cleanScope = plan.cleanScope;
    result.bodyOwnershipSkipReason = plan.bodyOwnershipSkipReason;
    result.whiteningShaderStrength = plan.whiteningShaderStrength;
    result.whiteningBlendGate = plan.whiteningBlendGate;
    result.whiteningShadowAmount = whiteningShadowAmount(parameters.values[7]);
    result.faceAnalysisPerformed = true;
    result.faceCount = plan.faceCount;
    result.personAnalysisPerformed = true;
    result.personCount = plan.personCount;
    const cv::Rect write(tile.write[0] - tile.sample[0], tile.write[1] - tile.sample[1], tile.write[2], tile.write[3]);
    if (hasSkin)
        skinStatistics(source(write), destination(write), mask(write), {0, 0, write.width, write.height}, result);
    const auto active = cv::boundingRect(mask(write));
    result.processingRegion = {tile.write[0] + active.x, tile.write[1] + active.y, active.width, active.height};
    result.rgba.assign(destination.data, destination.data + destination.total() * 4);
    result.skinMask.assign(mask.data, mask.data + mask.total());
    return result;
}

void MaiPortraitEngine::refineSkin(MaiPortraitSkinPlan& plan, const unsigned char* rgba, int width, int height,
                                   int stride) {
    if (!plan.refinementRequired || plan.refinementApplied || plan.faceProtected || plan.selectedPerson < 0 ||
        plan.selectedPerson >= static_cast<int>(plan.people.size()))
        throw MaiPortraitFailure("invalid_skin_sequence", "Regional refinement is not available in this phase");
    if (!rgba || width != plan.refinementProxyWidth || height != plan.refinementProxyHeight || stride < width * 4 ||
        width < 1 || height < 1 || width > 1024 || height > 1024)
        throw MaiPortraitFailure("invalid_region_context", "Regional proxy must match the bound request exactly");
    auto refined = plan;
    refined.analysisSourceRegion = plan.refinementSourceRegion;
    const auto& region = refined.analysisSourceRegion;
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), stride);
    std::vector<cv::Mat> owners;
    for (int index = 0; index < static_cast<int>(plan.people.size()); ++index) {
        const auto& original = plan.people[index];
        Detection seed{
            {(original.box[0] - region[0]) * width / region[2], (original.box[1] - region[1]) * height / region[3],
             original.box[2] * width / region[2], original.box[3] * height / region[3]},
            {(original.hip[0] - region[0]) * width / region[2], (original.hip[1] - region[1]) * height / region[3]},
            {(original.extent[0] - region[0]) * width / region[2],
             (original.extent[1] - region[1]) * height / region[3]},
            original.confidence};
        try {
            owners.push_back(mImplementation->pose(source, seed, false).second);
        } catch (const std::exception&) {
            if (index == plan.selectedPerson)
                throw MaiPortraitFailure("person_ownership_unreliable",
                                         "Selected regional person could not be re-estimated reliably");
            owners.emplace_back();
        }
    }
    // 身份使用原图中的真实 seed，不重新排序或猜局部 subject_index=0。
    const auto& face = plan.selectedFace;
    auto localBox = [&](float x, float y, float w, float h) {
        return bounded({cvFloor((x - region[0]) * width / region[2]), cvFloor((y - region[1]) * height / region[3]),
                        std::max(1, cvCeil(w * width / region[2])), std::max(1, cvCeil(h * height / region[3]))},
                       width, height);
    };
    if (plan.faceCount > 0) {
        const auto box = localBox(face.x, face.y, face.width, face.height);
        if (box.empty() || cv::mean(owners[plan.selectedPerson](box))[0] < 51)
            throw MaiPortraitFailure("person_ownership_unreliable",
                                     "Regional ownership no longer contains the originally selected face");
    }
    for (const auto& foreign : plan.excludedFaces) {
        const auto box = localBox(foreign[0] + foreign[2] * (.15f / 1.3f), foreign[1] + foreign[3] * (.15f / 1.3f),
                                  foreign[2] / 1.3f, foreign[3] / 1.3f);
        if (!box.empty() && cv::mean(owners[plan.selectedPerson](box))[0] > 102)
            throw MaiPortraitFailure("person_ownership_ambiguous",
                                     "Regional selected ownership also contains another person's face");
    }
    mImplementation->buildSkinMasks(refined, source, owners, plan.selectedPerson, false);
    // 只替换成功的同主体区域数据；原全图竞争人物硬排除保留，粗 selected 软 alpha 不再参与。
    refined.refinementApplied = true;
    plan = std::move(refined);
}

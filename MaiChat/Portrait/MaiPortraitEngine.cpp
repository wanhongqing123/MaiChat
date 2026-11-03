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
    cv::Mat skinMask(const cv::Mat& image) {
        cv::Mat small, rgb, input;
        cv::resize(image, small, {256, 256});
        cv::cvtColor(small, rgb, cv::COLOR_RGBA2RGB);
        rgb.convertTo(input, CV_32FC3, 1.0 / 255.0);
        auto outputs = run("selfie_multiclass.onnx", input);
        if (outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() != 256 * 256 * 6)
            throw std::runtime_error("Skin model output contract mismatch");
        const float* logits = outputs[0].GetTensorData<float>();
        cv::Mat mask(256, 256, CV_8U);
        for (int pixel = 0; pixel < 256 * 256; ++pixel) {
            const float* scores = logits + pixel * 6;
            const float maximum = *std::max_element(scores, scores + 6);
            float total = 0;
            for (int category = 0; category < 6; ++category) total += std::exp(scores[category] - maximum);
            const float probability = std::exp(scores[3] - maximum) / total;
            mask.data[pixel] = static_cast<unsigned char>(255 * smooth((probability - 0.4f) / 0.45f));
        }
        cv::Mat resized;
        cv::resize(mask, resized, image.size(), 0, 0, cv::INTER_LINEAR);
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
            throw std::runtime_error("Dense face landmarks are not reliable for this image");
        cv::Mat inverse;
        cv::invertAffineTransform(transform, inverse);
        const float* points = outputs[0].GetTensorData<float>();
        std::vector<cv::Point2f> result;
        for (int point = 0; point < 478; ++point)
            result.push_back(mapped(inverse, {points[point * 3], points[point * 3 + 1]}));
        return result;
    }
    std::vector<Detection> detectPeople(const cv::Mat& image) {
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
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        std::vector<Detection> selected;
        for (const auto& candidate : candidates) {
            bool duplicate = false;
            for (const auto& previous : selected)
                if (overlap(candidate.box, previous.box) > 0.3f) duplicate = true;
            if (!duplicate && selected.size() < 8) selected.push_back(candidate);
        }
        std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.hip.x < b.hip.x; });
        return selected;
    }
    std::pair<std::array<cv::Point2f, 33>, cv::Mat> pose(const cv::Mat& image, const Detection& person) {
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
            if ((index == 11 || index == 12 || index == 23 || index == 24 || index == 25 || index == 26 ||
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

MaiPortraitOutput MaiPortraitEngine::apply(const unsigned char* rgba, int width, int height, int stride,
                                           const std::vector<MaiPortraitFaceHint>& faceHints,
                                           const MaiPortraitParameters& parameters, bool body, int subjectIndex) {
    if (!rgba || width < 1 || height < 1 || static_cast<int64_t>(width) * height > 12'000'000 ||
        stride < static_cast<int64_t>(width) * 4)
        throw std::runtime_error("Invalid portrait pixels or image exceeds 12 megapixels");
    for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
        if (!std::isfinite(parameters.values[index]) || parameters.values[index] > 1 ||
            parameters.values[index] < (index == 2 ? -1 : 0))
            throw std::runtime_error("Portrait parameters are outside their allowed ranges");
    cv::Mat source(height, width, CV_8UC4, const_cast<unsigned char*>(rgba), stride);
    cv::Mat destination = source.clone();
    MaiPortraitOutput result;
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
    const auto& values = parameters.values;
    if (!body) {
        auto faces = faceHints;
        std::sort(faces.begin(), faces.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
        result.faceCount = static_cast<int>(faces.size());
        if (faces.empty()) throw std::runtime_error("No face was detected; choose a clear portrait");
        if (subjectIndex < 0 && faces.size() > 1)
            throw std::runtime_error(
                "Multiple faces detected; specify subject_index from left to right, starting at 0");
        const int selected = subjectIndex < 0 ? 0 : subjectIndex;
        if (selected >= static_cast<int>(faces.size()))
            throw std::runtime_error("subject_index does not identify a detected face");
        const auto& face = faces[selected];
        const auto points = mImplementation->mesh(source, face);
        result.features[6] = std::min(1.0f, face.width * face.height / (width * static_cast<float>(height)));
        auto mask = mImplementation->skinMask(source);
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
        cv::Mat faceRgb, reduced, filtered, resized;
        cv::cvtColor(source(area), faceRgb, cv::COLOR_RGBA2RGB);
        const double factor = std::min(1.0, 640.0 / std::max(area.width, area.height));
        cv::resize(faceRgb, reduced, {}, factor, factor, cv::INTER_AREA);
        cv::bilateralFilter(reduced, filtered, 7, 22, 5);
        cv::resize(filtered, resized, area.size(), 0, 0, cv::INTER_LINEAR);
        for (int y = 0; y < area.height; ++y)
            for (int x = 0; x < area.width; ++x) {
                const float alpha = mask.at<unsigned char>(y + area.y, x + area.x) / 255.0f;
                auto& target = destination.at<cv::Vec4b>(y + area.y, x + area.x);
                const auto original = source.at<cv::Vec4b>(y + area.y, x + area.x);
                const auto blurred = resized.at<cv::Vec3b>(y, x);
                for (int channel = 0; channel < 3; ++channel) {
                    float value = original[channel] + (blurred[channel] - original[channel]) * values[0] * 0.65f;
                    // 小幅按亮度增益提亮，避免统一加常数使暗部和肤色泛白。
                    value *= 1 + values[1] * 0.22f;
                    value += (channel == 0 ? 1 : channel == 2 ? -1 : 0) * values[2] * 8;
                    target[channel] =
                        cv::saturate_cast<unsigned char>(original[channel] + (value - original[channel]) * alpha);
                }
            }
        if (values[MaiPortraitParameters::kWhiteningIndex] > 0) {
            // 使用 float Lab 分离明度和色度；a*/b* 同比例收缩，避免用降暖制造偏蓝。
            // 亮部提升随原 L* 收敛，色度最多降低 30%，保留自然肤色和局部纹理。
            // 复用排除眼眉唇后的皮肤遮罩；遮罩外逐字节不变，零强度完全绕过色彩转换。
            // 调色为逐像素运算，分 64 行处理不产生接缝，也避免为 1200 万像素保留多份 float 图。
            for (int top = area.y; top < area.y + area.height; top += 64) {
                const cv::Rect tile(area.x, top, area.width, std::min(64, area.y + area.height - top));
                cv::Mat rgbFloat, lab, whitened;
                cv::cvtColor(destination(tile), rgbFloat, cv::COLOR_RGBA2RGB);
                rgbFloat.convertTo(rgbFloat, CV_32F, 1.0 / 255);
                cv::cvtColor(rgbFloat, lab, cv::COLOR_RGB2Lab);
                for (int y = 0; y < tile.height; ++y)
                    for (int x = 0; x < tile.width; ++x) {
                        const float amount = values[MaiPortraitParameters::kWhiteningIndex] *
                                             mask.at<unsigned char>(y + tile.y, x + tile.x) / 255.0f;
                        auto& pixel = lab.at<cv::Vec3f>(y, x);
                        pixel[0] += 10 * amount * (1 - pixel[0] / 100);
                        pixel[1] *= 1 - 0.3f * amount;
                        pixel[2] *= 1 - 0.3f * amount;
                    }
                cv::cvtColor(lab, whitened, cv::COLOR_Lab2RGB);
                whitened.convertTo(whitened, CV_8U, 255);
                for (int y = 0; y < tile.height; ++y)
                    for (int x = 0; x < tile.width; ++x)
                        if (mask.at<unsigned char>(y + tile.y, x + tile.x) > 0) {
                            auto& target = destination.at<cv::Vec4b>(y + tile.y, x + tile.x);
                            const auto edited = whitened.at<cv::Vec3b>(y, x);
                            for (int channel = 0; channel < 3; ++channel) target[channel] = edited[channel];
                        }
            }
        }
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
    } else {
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

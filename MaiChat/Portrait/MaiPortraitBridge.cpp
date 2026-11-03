// 文件作用：将平台参数转换为普通 C++ 调用，并拦住跨语言异常和内存所有权。
#include "MaiPortraitBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <json.hpp>
#include <openssl/evp.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPortraitEngine.h"
#include "MaiPortraitPersonalization.h"

namespace {
using Json = nlohmann::json;
std::mutex configurationMutex;
std::string modelDirectory, runtimePath;
const void* apiBase = nullptr;
std::unique_ptr<MaiPortraitEngine> engine;
constexpr std::array<const char*, MaiPortraitParameters::kCount> parameterNames{
    "smoothing", "brightening", "warmth", "slim_face", "enlarge_eyes", "slim_waist", "leg_length", "whitening"};
char* copied(const std::string& value) {
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (result) std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}
char* success(const Json& output) {
    return copied(Json{{"ok", true}, {"output", output}}.dump());
}
char* failure(const std::string& message) {
    return copied(Json{{"ok", false}, {"errorCode", "invalid_input"}, {"error", message}}.dump());
}
std::string fingerprint(const unsigned char* pixels, int width, int height, int stride) {
    auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot fingerprint source image");
    const std::string dimensions = std::to_string(width) + "x" + std::to_string(height) + ":";
    EVP_DigestUpdate(context.get(), dimensions.data(), dimensions.size());
    for (int row = 0; row < height; ++row)
        EVP_DigestUpdate(context.get(), pixels + static_cast<size_t>(row) * stride, static_cast<size_t>(width) * 4);
    std::array<unsigned char, 32> digest{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 || length != digest.size())
        throw std::runtime_error("Cannot fingerprint source image");
    std::ostringstream text;
    for (unsigned char value : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return text.str();
}
std::vector<MaiPortraitFaceHint> faceHints(const Json& arguments) {
    std::vector<MaiPortraitFaceHint> result;
    if (!arguments.contains("_face_hints")) return result;
    const auto& hints = arguments.at("_face_hints");
    if (!hints.is_array() || hints.size() > 32) throw std::runtime_error("Invalid host face hints");
    for (const auto& value : hints) {
        const auto& box = value.at("box");
        MaiPortraitFaceHint face;
        face.x = box.at("x");
        face.y = box.at("y");
        face.width = box.at("width");
        face.height = box.at("height");
        if (value.contains("landmarks")) {
            const auto& points = value.at("landmarks");
            if (points.contains("left_eye")) {
                face.leftEyeX = points.at("left_eye").at("x");
                face.leftEyeY = points.at("left_eye").at("y");
            }
            if (points.contains("right_eye")) {
                face.rightEyeX = points.at("right_eye").at("x");
                face.rightEyeY = points.at("right_eye").at("y");
            }
        }
        if (!std::isfinite(face.x) || !std::isfinite(face.y) || !std::isfinite(face.width) ||
            !std::isfinite(face.height) || face.width < 8 || face.height < 8)
            throw std::runtime_error("Invalid host face bounds");
        result.push_back(face);
    }
    return result;
}
}  // namespace

extern "C" void maiPortraitConfigure(const char* directory, const char* runtime, const void* supplied) {
    std::lock_guard<std::mutex> lock(configurationMutex);
    const std::string next = directory ? directory : "", nextRuntime = runtime ? runtime : "";
    if (next != modelDirectory || nextRuntime != runtimePath || supplied != apiBase) {
        engine.reset();
        modelDirectory = next;
        runtimePath = nextRuntime;
        apiBase = supplied;
    }
}
extern "C" MaiPortraitImageResult maiPortraitProcessRgba(const unsigned char* pixels, int width, int height, int stride,
                                                         const char* encoded, const char* profileDirectory,
                                                         const char* sourcePath) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!encoded || !profileDirectory || !*profileDirectory || !sourcePath || !*sourcePath)
            throw std::runtime_error("Current account portrait workspace is unavailable");
        const Json arguments = Json::parse(encoded);
        const std::string operation = arguments.value("operation", "");
        const bool body = operation == "reshape_body";
        if (operation != "beautify" && !body) throw std::runtime_error("Unsupported portrait operation");
        if (modelDirectory.empty()) throw std::runtime_error("Portrait models have not been configured");
        if (!engine) engine = std::make_unique<MaiPortraitEngine>(modelDirectory, runtimePath, apiBase);
        MaiPortraitPersonalization profile(profileDirectory);
        MaiPortraitParameters parameters;
        if (arguments.contains("strength")) {
            const float strength = arguments.at("strength").get<float>();
            if (!std::isfinite(strength) || strength < 0 || strength > 1)
                throw std::runtime_error("strength must be 0..1");
            parameters.values[0] = strength;
            parameters.values[1] = strength * 0.35f;
        }
        if (body) parameters.values = {0, 0, 0, 0, 0, 0.2f, 0};
        for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
            if (arguments.contains(parameterNames[index]))
                parameters.values[index] = arguments.at(parameterNames[index]).get<float>();
        const auto hints = faceHints(arguments);
        auto output =
            engine->apply(pixels, width, height, stride, hints, parameters, body, arguments.value("subject_index", -1));
        const auto status = profile.status();
        if (status.activeVersion > 0 && arguments.value("use_personal_model", true)) {
            auto learned = profile.recommend(output.features);
            for (int index = 0; index < MaiPortraitParameters::kCount; ++index) {
                const bool relevant =
                    body ? (index == 5 || index == 6) : (index < 5 || index == MaiPortraitParameters::kWhiteningIndex);
                if (relevant && !arguments.contains(parameterNames[index]) &&
                    !(arguments.contains("strength") && index < 2))
                    parameters.values[index] = learned.values[index];
            }
            output = engine->apply(pixels, width, height, stride, hints, parameters, body,
                                   arguments.value("subject_index", -1));
        }
        const Json metadata = {{"source_fingerprint", fingerprint(pixels, width, height, stride)},
                               {"source_path", sourcePath},
                               {"parameters", parameters.values},
                               {"features", output.features},
                               {"face_count", output.faceCount},
                               {"person_count", output.personCount},
                               {"analysis_version", output.analysisVersion},
                               {"personal_model_version", status.activeVersion}};
        auto* bytes = static_cast<unsigned char*>(std::malloc(output.rgba.size()));
        if (!bytes) throw std::runtime_error("Cannot allocate portrait output");
        std::memcpy(bytes, output.rgba.data(), output.rgba.size());
        return {bytes, width, height, copied(metadata.dump()), nullptr};
    } catch (const std::exception& error) {
        return {nullptr, 0, 0, nullptr, copied(error.what())};
    }
}
extern "C" char* maiPortraitOriginalSource(const char* directory, const char* path) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!directory || !path) return nullptr;
        MaiPortraitPersonalization profile(directory);
        return copied(profile.originalSource(path));
    } catch (...) {
        return nullptr;
    }
}
extern "C" char* maiPortraitCommitResult(const char* directory, const char* encoded, const char* path) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!directory || !encoded || !path || !MaiFileSystem::exists(MaiFilePath::fromUtf8(path)))
            throw std::runtime_error("Portrait output has not been saved completely");
        const Json data = Json::parse(encoded);
        MaiPortraitPersonalization profile(directory);
        MaiPortraitParameters parameters;
        parameters.values = data.at("parameters").get<MaiPortraitParameters::Values>();
        auto identifier = profile.recordResult(data.at("source_fingerprint"), data.at("source_path"), path, parameters,
                                               data.at("features").get<std::array<float, 8>>());
        Json effective = Json::object();
        for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
            effective[parameterNames[index]] = parameters.values[index];
        return success({{"result_id", identifier},
                        {"original_path", data.at("source_path")},
                        {"parameters", effective},
                        {"analysis_version", data.at("analysis_version")},
                        {"personal_model_version", data.at("personal_model_version")},
                        {"face_count", data.at("face_count")},
                        {"person_count", data.at("person_count")}});
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}
extern "C" char* maiPortraitPersonalizationRequest(const char* directory, const char* encoded) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!directory || !encoded) throw std::runtime_error("Personal portrait profile is unavailable");
        const Json arguments = Json::parse(encoded);
        MaiPortraitPersonalization profile(directory);
        const auto action = arguments.value("action", "");
        if (action == "record_feedback") {
            std::vector<int> attributes;
            for (const auto& name : arguments.at("attributes")) {
                const auto found = std::find(parameterNames.begin(), parameterNames.end(), name.get<std::string>());
                if (found == parameterNames.end()) throw std::runtime_error("Unknown portrait feedback attribute");
                attributes.push_back(static_cast<int>(found - parameterNames.begin()));
            }
            int inserted = profile.recordPreference(arguments.at("preferred_result_id"),
                                                    arguments.at("rejected_result_id"), attributes);
            return success({{"recorded", inserted}, {"feedback_count", profile.status().feedbackCount}});
        }
        if (action == "train") {
            const auto trained = profile.train();
            return success(
                {{"status", trained.activated ? "activated" : "candidate_only"},
                 {"version", trained.version},
                 {"active_version", trained.activeVersion},
                 {"feedback_count", trained.feedbackCount},
                 {"independent_photos", trained.sourceCount},
                 {"training_loss", trained.trainingLoss},
                 {"validation_loss", trained.validationLoss < 0 ? Json(nullptr) : Json(trained.validationLoss)},
                 {"validation_note",
                  "Each evaluated attribute needs at least three independent original photos and improvement on "
                  "held-out photos before activation"},
                 {"base_model_updated", false},
                 {"personal_weights_updated", true}});
        }
        if (action == "rollback")
            profile.rollback(arguments.at("version").get<int>());
        else if (action != "status" && action != "versions")
            throw std::runtime_error("Unsupported portrait personalization action");
        const auto status = profile.status();
        return success({{"active_version", status.activeVersion},
                        {"feedback_count", status.feedbackCount},
                        {"versions", status.versions},
                        {"baseline_version", 0}});
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}
extern "C" void maiPortraitFree(void* pointer) {
    std::free(pointer);
}

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
#include <set>
#include <sstream>
#include <stdexcept>

#include <json.hpp>
#include <openssl/evp.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPortraitEngine.h"
#include "MaiPortraitPersonalization.h"
#include "MaiPortraitRegion.h"

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
    if (!hints.is_array() || hints.size() > 256)
        throw MaiPortraitFailure("invalid_face_bounds", "Invalid host face hints");
    for (const auto& value : hints) {
        const auto& box = value.at("box");
        MaiPortraitFaceHint face;
        face.x = box.at("x");
        face.y = box.at("y");
        face.width = box.at("width");
        face.height = box.at("height");
        face.confidence = value.value("confidence", 0.0f);
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

Json diagnostic(const std::string& code, const std::string& message, int width = 0, int height = 0) {
    const std::string stage = code == "face_detection_failed"                               ? "face_detection"
                              : code == "skin_mask_empty"                                   ? "skin_segmentation"
                              : code == "no_visible_effect"                                 ? "output_metrics"
                              : code == "image_too_large" || code == "roi_budget_exceeded"  ? "budget"
                              : code == "unknown_parameter" || code == "invalid_parameters" ? "validation"
                                                                                            : "portrait";
    std::string hint = "Inspect the reported stage and actual parameters; do not retry identical inputs blindly";
    if (code == "image_too_large")
        hint = "Use the bounded face-region path; keep the original dimensions and background";
    else if (code == "face_detection_failed")
        hint = "Run tiled face detection on the original; do not merely shrink the whole photo";
    else if (code == "skin_mask_empty" || code == "no_visible_effect")
        hint = "Inspect mask coverage and skin ROI L*/C*; full-image threshold fractions are not whitening strength";
    else if (code == "invalid_parameters" || code == "unknown_parameter")
        hint = "Use only the documented operation fields; action:delegate is not supported";
    return {{"code", code},
            {"stage", stage},
            {"message", message},
            {"hint", hint},
            {"width", width},
            {"height", height},
            {"full_raster_max_pixels", 12'000'000},
            {"roi_max_side", 2048}};
}

void validateArguments(const std::string& operation, const Json& arguments, bool hostFields) {
    if (!arguments.is_object() || (operation != "beautify" && operation != "reshape_body"))
        throw MaiPortraitFailure("invalid_parameters", "A documented portrait operation object is required");
    std::set<std::string> allowed{"path", "use_personal_model", "subject_index"};
    if (operation == "beautify") {
        allowed.insert("strength");
        for (int index : {0, 1, 2, 3, 4, 7}) allowed.insert(parameterNames[index]);
    } else {
        allowed.insert("slim_waist");
        allowed.insert("leg_length");
    }
    if (hostFields) {
        allowed.insert("operation");
        allowed.insert("_face_hints");
        allowed.insert("workspace_path");
        allowed.insert("session_id");
    }
    for (auto field = arguments.begin(); field != arguments.end(); ++field) {
        if (!allowed.count(field.key()))
            throw MaiPortraitFailure("unknown_parameter",
                                     "Unknown portrait parameter; action/delegate is not a supported edit operation");
        if (field.key() == "path" || field.key() == "workspace_path" || field.key() == "session_id") {
            if (!field.value().is_string() || field.value().get_ref<const std::string&>().empty() ||
                field.value().get_ref<const std::string&>().size() > 4096)
                throw MaiPortraitFailure("invalid_parameters", "path must be a nonempty bounded string");
        } else if (field.key() == "subject_index") {
            if (!field.value().is_number_integer() || field.value().get<int64_t>() < 0 ||
                field.value().get<int64_t>() > 255)
                throw MaiPortraitFailure("invalid_parameters", "subject_index must be a nonnegative bounded integer");
        } else if (field.key() == "use_personal_model") {
            if (!field.value().is_boolean())
                throw MaiPortraitFailure("invalid_parameters", "use_personal_model must be boolean");
        } else if (field.key() == "operation") {
            if (!field.value().is_string() || field.value() != operation)
                throw MaiPortraitFailure("invalid_parameters", "Host operation does not match the requested edit");
        } else if (field.key() == "_face_hints") {
            (void)faceHints(arguments);
        } else {
            if (!field.value().is_number())
                throw MaiPortraitFailure("invalid_parameters", "Portrait strengths must be finite numbers");
            const double value = field.value().get<double>();
            if (!std::isfinite(value) || value > 1 || value < (field.key() == "warmth" ? -1 : 0))
                throw MaiPortraitFailure("invalid_parameters", "Portrait strengths exceed their documented ranges");
        }
    }
}

Json faceJson(const MaiPortraitFaceHint& face) {
    return {{"box", {{"x", face.x}, {"y", face.y}, {"width", face.width}, {"height", face.height}}},
            {"confidence", face.confidence},
            {"landmarks",
             {{"left_eye", {{"x", face.leftEyeX}, {"y", face.leftEyeY}}},
              {"right_eye", {{"x", face.rightEyeX}, {"y", face.rightEyeY}}}}}};
}

MaiPortraitRegionResult processImage(const unsigned char* pixels, int width, int height, int stride,
                                     const char* encoded, const char* profileDirectory, const char* sourcePath,
                                     const MaiPortraitRegionContext* context) {
    Json effective = Json::object();
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!encoded || !profileDirectory || !*profileDirectory || !sourcePath || !*sourcePath)
            throw MaiPortraitFailure("portrait_not_configured", "Current account portrait workspace is unavailable");
        const Json arguments = Json::parse(encoded);
        const std::string operation = arguments.value("operation", "");
        validateArguments(operation, arguments, true);
        const bool body = operation == "reshape_body";
        if (modelDirectory.empty())
            throw MaiPortraitFailure("portrait_not_configured", "Portrait models have not been configured");
        if (!engine) engine = std::make_unique<MaiPortraitEngine>(modelDirectory, runtimePath, apiBase);
        MaiPortraitPersonalization profile(profileDirectory);
        MaiPortraitParameters parameters;
        if (arguments.contains("strength")) {
            parameters.values[0] = arguments.at("strength").get<float>();
            parameters.values[1] = parameters.values[0] * 0.35f;
        }
        if (body) parameters.values = {0, 0, 0, 0, 0, 0.2f, 0, 0};
        for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
            if (arguments.contains(parameterNames[index]))
                parameters.values[index] = arguments.at(parameterNames[index]).get<float>();
        auto hints = faceHints(arguments);
        int subject = arguments.value("subject_index", -1);
        if (context) {
            auto face = context->plan.selectedFace;
            face.x -= context->plan.region.x;
            face.y -= context->plan.region.y;
            face.leftEyeX -= context->plan.region.x;
            face.leftEyeY -= context->plan.region.y;
            face.rightEyeX -= context->plan.region.x;
            face.rightEyeY -= context->plan.region.y;
            hints = {face};
            subject = 0;
        }
        for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
            effective[parameterNames[index]] = parameters.values[index];
        auto output = engine->apply(pixels, width, height, stride, hints, parameters, body, subject, context);
        const auto status = profile.status();
        if (status.activeVersion > 0 && arguments.value("use_personal_model", true)) {
            const auto learned = profile.recommend(output.features);
            for (int index = 0; index < MaiPortraitParameters::kCount; ++index) {
                const bool relevant = body ? (index == 5 || index == 6) : (index < 5 || index == 7);
                if (relevant && !arguments.contains(parameterNames[index]) &&
                    !(arguments.contains("strength") && index < 2))
                    parameters.values[index] = learned.values[index];
            }
            output = engine->apply(pixels, width, height, stride, hints, parameters, body, subject, context);
        }
        const int x = context ? context->plan.region.x : 0, y = context ? context->plan.region.y : 0;
        auto processing = output.processingRegion;
        processing[0] += x;
        processing[1] += y;
        const int sourceWidth = context ? context->plan.sourceWidth : width;
        const int sourceHeight = context ? context->plan.sourceHeight : height;
        for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
            effective[parameterNames[index]] = parameters.values[index];
        const Json metadata = {
            {"source_fingerprint", context ? context->sourceFingerprint : fingerprint(pixels, width, height, stride)},
            {"source_path", sourcePath},
            {"parameters", parameters.values},
            {"effective_parameters", effective},
            {"features", output.features},
            {"face_count", output.faceAnalysisPerformed ? Json(output.faceCount) : Json(nullptr)},
            {"person_count", output.personAnalysisPerformed ? Json(output.personCount) : Json(nullptr)},
            {"face_analysis_performed", output.faceAnalysisPerformed},
            {"person_analysis_performed", output.personAnalysisPerformed},
            {"analysis_version", output.analysisVersion},
            {"processing_version", "native-face-roi-20261103-v3"},
            {"personal_model_version", status.activeVersion},
            {"source_width", sourceWidth},
            {"source_height", sourceHeight},
            {"processing_bbox", processing},
            {"roi", {{"x", x}, {"y", y}, {"width", width}, {"height", height}}},
            {"mask_semantics", "diagnostic_skin_alpha; rgba_is_already_composited; do_not_apply_mask_again"},
            {"skin_pixel_count", output.skinPixels},
            {"skin_weight", output.skinWeight},
            {"mask_coverage_in_roi", output.skinPixels / static_cast<double>(width * height)},
            {"changed_pixel_count", output.changedPixels},
            {"changed_rgb_fraction_in_roi", output.changedPixels / static_cast<double>(width * height)},
            {"skin_L_before", output.skinWeight > 0 ? Json(output.skinLightness[0]) : Json(nullptr)},
            {"skin_L_after", output.skinWeight > 0 ? Json(output.skinLightness[1]) : Json(nullptr)},
            {"skin_C_before", output.skinWeight > 0 ? Json(output.skinChroma[0]) : Json(nullptr)},
            {"skin_C_after", output.skinWeight > 0 ? Json(output.skinChroma[1]) : Json(nullptr)}};
        auto bytes = std::unique_ptr<unsigned char, decltype(&std::free)>(
            static_cast<unsigned char*>(std::malloc(output.rgba.size())), &std::free);
        auto mask = std::unique_ptr<unsigned char, decltype(&std::free)>(
            output.skinMask.empty() ? nullptr : static_cast<unsigned char*>(std::malloc(output.skinMask.size())),
            &std::free);
        if (!bytes || (!output.skinMask.empty() && !mask))
            throw MaiPortraitFailure("allocation_failed", "Cannot allocate the bounded portrait output");
        std::memcpy(bytes.get(), output.rgba.data(), output.rgba.size());
        if (mask) std::memcpy(mask.get(), output.skinMask.data(), output.skinMask.size());
        return {bytes.release(), mask.release(), width, height, copied(metadata.dump()), nullptr};
    } catch (const MaiPortraitFailure& error) {
        auto detail = diagnostic(error.code(), error.what(), width, height);
        detail["effective_parameters"] = effective;
        return {nullptr, nullptr, 0, 0, nullptr, copied(detail.dump())};
    } catch (const std::exception&) {
        return {nullptr,
                nullptr,
                0,
                0,
                nullptr,
                copied(diagnostic("portrait_processing_failed", "Portrait processing failed; no output was committed",
                                  width, height)
                           .dump())};
    }
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
extern "C" char* maiPortraitValidateArguments(const char* operation, const char* encoded, int hostFields) {
    try {
        if (!operation || !encoded) throw MaiPortraitFailure("invalid_parameters", "Portrait arguments are required");
        validateArguments(operation, Json::parse(encoded), hostFields != 0);
        return nullptr;
    } catch (const MaiPortraitFailure& error) {
        return copied(diagnostic(error.code(), error.what()).dump());
    } catch (const std::exception&) {
        return copied(
            diagnostic("invalid_parameters", "Portrait arguments must be a valid object with documented field types")
                .dump());
    }
}

extern "C" char* maiPortraitMergeFaceHints(const char* encoded) {
    try {
        if (!encoded) throw MaiPortraitFailure("invalid_face_bounds", "Host face hints are required");
        const auto merged = MaiPortraitRegionPlanner::mergeFaces(faceHints(Json::parse(encoded)));
        Json faces = Json::array();
        for (const auto& face : merged) faces.push_back(faceJson(face));
        return success({{"faces", faces}});
    } catch (const std::exception&) {
        return copied(Json{{"ok", false},
                           {"errorCode", "invalid_input"},
                           {"error", diagnostic("invalid_face_bounds", "Host face hints could not be merged")}}
                          .dump());
    }
}

extern "C" char* maiPortraitPlanFaceRegion(int width, int height, const char* encodedHints, int subject,
                                           const unsigned char* rgba, int proxyWidth, int proxyHeight,
                                           int proxyStride) {
    try {
        if (!encodedHints) throw MaiPortraitFailure("face_detection_failed", "Host face hints are required");
        const auto plan =
            MaiPortraitRegionPlanner::faceRegion(width, height, faceHints(Json::parse(encodedHints)), subject);
        const auto features =
            MaiPortraitRegionPlanner::globalFeatures(rgba, proxyWidth, proxyHeight, proxyStride, plan);
        return success({{"source_width", width},
                        {"source_height", height},
                        {"face_count", plan.faceCount},
                        {"subject_index", plan.subjectIndex},
                        {"roi",
                         {{"x", plan.region.x},
                          {"y", plan.region.y},
                          {"width", plan.region.width},
                          {"height", plan.region.height}}},
                        {"selected_face", faceJson(plan.selectedFace)},
                        {"global_features", features},
                        {"coordinate_origin", "top_left_oriented_source_pixels"}});
    } catch (const MaiPortraitFailure& error) {
        return copied(Json{{"ok", false},
                           {"errorCode", "invalid_input"},
                           {"error", diagnostic(error.code(), error.what(), width, height)}}
                          .dump());
    } catch (const std::exception&) {
        return copied(
            Json{{"ok", false},
                 {"errorCode", "invalid_input"},
                 {"error", diagnostic("invalid_parameters", "Face region planning inputs are invalid", width, height)}}
                .dump());
    }
}

extern "C" MaiPortraitImageResult maiPortraitProcessRgba(const unsigned char* pixels, int width, int height, int stride,
                                                         const char* encoded, const char* profileDirectory,
                                                         const char* sourcePath) {
    auto result = processImage(pixels, width, height, stride, encoded, profileDirectory, sourcePath, nullptr);
    std::free(result.mask);
    return {result.rgba, result.width, result.height, result.metadata, result.error};
}

extern "C" MaiPortraitRegionResult maiPortraitProcessRegionRgba(const unsigned char* pixels, int width, int height,
                                                                int stride, const char* encoded,
                                                                const char* profileDirectory, const char* sourcePath,
                                                                const char* encodedContext) {
    try {
        if (!encodedContext) throw MaiPortraitFailure("invalid_region_context", "Host region context is required");
        const Json input = Json::parse(encodedContext);
        MaiPortraitRegionContext context;
        context.plan.sourceWidth = input.at("source_width");
        context.plan.sourceHeight = input.at("source_height");
        context.plan.faceCount = input.at("face_count");
        context.plan.subjectIndex = input.at("subject_index");
        const auto& roi = input.at("roi");
        context.plan.region = {roi.at("x"), roi.at("y"), roi.at("width"), roi.at("height")};
        const auto selected = faceHints(Json{{"_face_hints", Json::array({input.at("selected_face")})}});
        context.plan.selectedFace = selected.front();
        context.globalFeatures = input.at("global_features").get<std::array<float, 8>>();
        context.sourceFingerprint = input.at("source_fingerprint");
        const auto& region = context.plan.region;
        if (context.sourceFingerprint.empty() || context.sourceFingerprint.size() > 128 ||
            context.plan.sourceWidth < 1 || context.plan.sourceHeight < 1 || context.plan.faceCount < 1 ||
            region.x < 0 || region.y < 0 || region.width != width || region.height != height ||
            static_cast<int64_t>(region.x) + width > context.plan.sourceWidth ||
            static_cast<int64_t>(region.y) + height > context.plan.sourceHeight)
            throw MaiPortraitFailure("invalid_region_context", "Host region bounds or source fingerprint are invalid");
        for (float feature : context.globalFeatures)
            if (!std::isfinite(feature))
                throw MaiPortraitFailure("invalid_region_context", "Global image features must be finite");
        return processImage(pixels, width, height, stride, encoded, profileDirectory, sourcePath, &context);
    } catch (const MaiPortraitFailure& error) {
        return {nullptr, nullptr, 0, 0, nullptr, copied(diagnostic(error.code(), error.what(), width, height).dump())};
    } catch (const std::exception&) {
        return {
            nullptr,
            nullptr,
            0,
            0,
            nullptr,
            copied(diagnostic("invalid_region_context", "Host face-region context is invalid", width, height).dump())};
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
extern "C" char* maiPortraitPublishOutput(const char* temporaryPath, const char* outputPath) {
    try {
        if (!temporaryPath || !outputPath)
            throw MaiPortraitFailure("output_commit_failed", "Prepared output paths are required");
        const auto source = MaiFilePath::fromUtf8(temporaryPath), target = MaiFilePath::fromUtf8(outputPath);
        uint64_t size = 0;
        if (source == target || source.dirName() != target.dirName() || MaiFileSystem::isDirectory(source) ||
            !MaiFileSystem::fileSize(source, size) || size == 0)
            throw MaiPortraitFailure("output_commit_failed", "A complete nonempty sibling temporary file is required");
        if (MaiFileSystem::publishNewFile(source, target))
            throw MaiPortraitFailure("output_commit_failed",
                                     "Cannot atomically publish portrait output without replacing an existing file");
        return success({{"published", true}});
    } catch (const MaiPortraitFailure& error) {
        return copied(
            Json{{"ok", false}, {"errorCode", "internal"}, {"error", diagnostic(error.code(), error.what())}}.dump());
    } catch (const std::exception&) {
        return copied(Json{{"ok", false},
                           {"errorCode", "internal"},
                           {"error", diagnostic("output_commit_failed", "Cannot publish portrait output")}}
                          .dump());
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
        Json output = {{"result_id", identifier},
                       {"original_path", data.at("source_path")},
                       {"parameters", effective},
                       {"analysis_version", data.at("analysis_version")},
                       {"personal_model_version", data.at("personal_model_version")},
                       {"face_count", data.at("face_count")},
                       {"person_count", data.at("person_count")}};
        for (const char* key :
             {"processing_version", "face_analysis_performed", "person_analysis_performed", "source_width",
              "source_height", "roi", "processing_bbox", "mask_semantics", "skin_pixel_count", "skin_weight",
              "mask_coverage_in_roi", "changed_pixel_count", "changed_rgb_fraction_in_roi", "skin_L_before",
              "skin_L_after", "skin_C_before", "skin_C_after"})
            if (data.contains(key)) output[key] = data[key];
        return success(output);
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

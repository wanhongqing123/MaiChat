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

// 计划只借用共享引擎到面部保护完成；之后仅使用拥有的遮罩与参数，配置切换不会悬空。
struct MaiPortraitSkinContext {
    MaiPortraitSkinPlan plan;
    MaiPortraitParameters parameters;
    std::shared_ptr<MaiPortraitEngine> faceEngine;
    nlohmann::json metadata;
    int nextTile = 0;
    bool failed = false;
    double weight = 0;
    std::array<double, 2> lightness{}, chroma{};
    uint64_t skinPixels = 0, changedPixels = 0, bodyPixels = 0;
    std::array<int, 4> processing{};
};

namespace {
using Json = nlohmann::json;
std::mutex configurationMutex;
std::string modelDirectory, runtimePath;
const void* apiBase = nullptr;
std::shared_ptr<MaiPortraitEngine> engine;
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
        hint = "Inspect owned skin coverage, clean scope and actual LUT shader/blend strengths";
    else if (code == "body_skin_unavailable" || code == "cannot_remove_body_hair")
        hint = "Explicit body-hair removal requires reliably owned body skin; face-only coverage is insufficient";
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
        allowed.insert("remove_body_hair");
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
        } else if (field.key() == "use_personal_model" || field.key() == "remove_body_hair") {
            if (!field.value().is_boolean())
                throw MaiPortraitFailure("invalid_parameters", "Portrait switches must be boolean");
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

void effectiveHairParameter(Json& effective, const MaiPortraitParameters& parameters) {
    if (parameters.removeBodyHair)
        effective["remove_body_hair"] = *parameters.removeBodyHair;
    else
        effective.erase("remove_body_hair");
}

const char* bodyCleanupMode(const MaiPortraitParameters& parameters) {
    return !parameters.removeBodyHair ? "auto" : *parameters.removeBodyHair ? "enabled" : "disabled";
}

template <class Material>
void materialMetadata(Json& metadata, const Material& material) {
    metadata["clean_applied"] = material.cleanApplied;
    metadata["clean_strength"] = material.cleanStrength;
    metadata["clean_skip_reason"] = material.cleanSkipReason;
    metadata["clean_scope"] = material.cleanScope;
    metadata["body_ownership_skip_reason"] = material.bodyOwnershipSkipReason;
    metadata["whitening_shader_strength"] = material.whiteningShaderStrength;
    metadata["whitening_blend_gate"] = material.whiteningBlendGate;
    metadata["whitening_color_policy"] = "source_hue_chroma_cap";
    // D 是请求固定的 L* 曲线幅度，不是每个像素最终得到的亮度差。
    metadata["whitening_shadow_amount"] = material.whiteningShadowAmount;
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
        if (!engine) engine = std::make_shared<MaiPortraitEngine>(modelDirectory, runtimePath, apiBase);
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
        if (arguments.contains("remove_body_hair"))
            parameters.removeBodyHair = arguments.at("remove_body_hair").get<bool>();
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
        if (!body) effectiveHairParameter(effective, parameters);
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
        Json metadata = {
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
        if (!body) metadata["body_cleanup_request_mode"] = bodyCleanupMode(parameters);
        if (context && parameters.values[MaiPortraitParameters::kWhiteningIndex] > 0) {
            // 只有实际运行美白的新颜色目标才标 v8；纯几何 ROI 仍沿用其原有版本。
            metadata["processing_version"] = "native-face-roi-20261105-v8";
            materialMetadata(metadata, output);
        }
        if (output.skinAnalysisPerformed) {
            metadata["processing_version"] = "native-owned-skin-full-raster-20261105-v8";
            metadata["skin_scope"] = output.skinScope;
            metadata["skin_refinement_applied"] = output.skinRefinementApplied;
            metadata["skin_analysis_source_rect"] = output.skinAnalysisSourceRegion;
            metadata["skin_analysis_proxy_width"] = output.skinAnalysisProxyWidth;
            metadata["skin_analysis_proxy_height"] = output.skinAnalysisProxyHeight;
            materialMetadata(metadata, output);
            metadata["face_color_skipped"] = output.faceColorSkipped;
            metadata["person_ownership_reliable"] = output.personOwnershipReliable;
            metadata["body_skin_pixel_count"] = output.bodySkinPixels;
            metadata["unreliable_person_masks"] = output.unreliablePersonMasks;
            metadata["mask_edge_refinement"] = "body_class2_mature_matte_and_native_face_rgb";
            metadata["metrics_scope"] = "skin_color_and_smoothing_before_optional_face_geometry";
            metadata["geometry_applied"] = parameters.values[3] > 0 || parameters.values[4] > 0;
            if (parameters.values[3] > 0 || parameters.values[4] > 0) {
                const auto region =
                    MaiPortraitRegionPlanner::faceRegion(width, height, faceHints(arguments), subject).region;
                metadata["geometry_processing_bbox"] = {region.x, region.y, region.width, region.height};
            }
        }
        auto bytes = std::unique_ptr<unsigned char, decltype(&std::free)>(
            static_cast<unsigned char*>(std::malloc(output.rgba.size())), &std::free);
        auto mask = std::unique_ptr<unsigned char, decltype(&std::free)>(
            output.skinMask.empty() ? nullptr : static_cast<unsigned char*>(std::malloc(output.skinMask.size())),
            &std::free);
        if (!bytes || (!output.skinMask.empty() && !mask))
            throw MaiPortraitFailure("allocation_failed", "Cannot allocate the bounded portrait output");
        std::memcpy(bytes.get(), output.rgba.data(), output.rgba.size());
        if (mask) std::memcpy(mask.get(), output.skinMask.data(), output.skinMask.size());
        auto response = std::unique_ptr<char, decltype(&std::free)>(copied(metadata.dump()), &std::free);
        if (!response) throw MaiPortraitFailure("allocation_failed", "Cannot allocate portrait metadata");
        return {bytes.release(), mask.release(), width, height, response.release(), nullptr};
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
        if (data.contains("effective_parameters") && data.at("effective_parameters").contains("remove_body_hair") &&
            data.at("effective_parameters").at("remove_body_hair").is_boolean())
            parameters.removeBodyHair = data.at("effective_parameters").at("remove_body_hair").get<bool>();
        auto identifier = profile.recordResult(data.at("source_fingerprint"), data.at("source_path"), path, parameters,
                                               data.at("features").get<std::array<float, 8>>());
        Json effective = Json::object();
        for (int index = 0; index < MaiPortraitParameters::kCount; ++index)
            effective[parameterNames[index]] = parameters.values[index];
        effectiveHairParameter(effective, parameters);
        Json output = {{"result_id", identifier},
                       {"original_path", data.at("source_path")},
                       {"parameters", effective},
                       {"analysis_version", data.at("analysis_version")},
                       {"personal_model_version", data.at("personal_model_version")},
                       {"face_count", data.at("face_count")},
                       {"person_count", data.at("person_count")}};
        for (const char* key : {"processing_version",
                                "face_analysis_performed",
                                "person_analysis_performed",
                                "source_width",
                                "source_height",
                                "roi",
                                "processing_bbox",
                                "mask_semantics",
                                "skin_pixel_count",
                                "skin_weight",
                                "mask_coverage_in_roi",
                                "changed_pixel_count",
                                "changed_rgb_fraction_in_roi",
                                "skin_L_before",
                                "skin_L_after",
                                "skin_C_before",
                                "skin_C_after",
                                "skin_refinement_applied",
                                "skin_analysis_source_rect",
                                "skin_analysis_proxy_width",
                                "skin_analysis_proxy_height",
                                "skin_visible_source_partial",
                                "whitening_shader_strength",
                                "whitening_blend_gate",
                                "whitening_color_policy",
                                "whitening_shadow_amount",
                                "clean_applied",
                                "clean_strength",
                                "clean_skip_reason",
                                "clean_scope",
                                "body_ownership_skip_reason",
                                "body_cleanup_request_mode",
                                "skin_scope",
                                "face_color_skipped",
                                "person_ownership_reliable",
                                "body_skin_pixel_count",
                                "tile_count",
                                "mask_edge_refinement",
                                "unreliable_person_masks",
                                "owned_body_proxy_pixels",
                                "person_analysis_windows",
                                "person_analysis_policy",
                                "skin_analysis_proxy_rect",
                                "geometry_applied",
                                "geometry_processing_bbox",
                                "metrics_scope"})
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

extern "C" MaiPortraitSkinAnalysisResult maiPortraitAnalyzeSkinRgba(const unsigned char* pixels, int width, int height,
                                                                    int stride, int sourceWidth, int sourceHeight,
                                                                    const char* encodedHints, const char* encoded,
                                                                    const char* directory, const char* sourcePath,
                                                                    const char* sourceFingerprint,
                                                                    const unsigned char* globalRgba, int globalStride) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!encodedHints || !encoded || !directory || !*directory || !sourcePath || !*sourcePath ||
            !sourceFingerprint || std::strlen(sourceFingerprint) != 64 || modelDirectory.empty())
            throw MaiPortraitFailure("portrait_not_configured",
                                     "A current-account source and complete fingerprint are required");
        const Json arguments = Json::parse(encoded);
        validateArguments("beautify", arguments, true);
        if (!engine) engine = std::make_shared<MaiPortraitEngine>(modelDirectory, runtimePath, apiBase);
        auto context = std::make_unique<MaiPortraitSkinContext>();
        context->faceEngine = engine;
        context->plan = engine->analyzeSkin(pixels, width, height, stride, sourceWidth, sourceHeight,
                                            faceHints(Json::parse(encodedHints)), arguments.value("subject_index", -1));
        const auto& plan = context->plan;
        MaiPortraitRegionPlan facePlan;
        facePlan.sourceWidth = sourceWidth;
        facePlan.sourceHeight = sourceHeight;
        facePlan.faceCount = plan.faceCount;
        facePlan.subjectIndex = plan.subjectIndex;
        facePlan.selectedFace = plan.selectedFace;
        facePlan.region = {plan.faceRegion[0], plan.faceRegion[1], plan.faceRegion[2], plan.faceRegion[3]};
        const auto features = MaiPortraitRegionPlanner::globalFeatures(globalRgba, 64, 64, globalStride, facePlan);
        auto& parameters = context->parameters;
        if (arguments.contains("strength")) {
            parameters.values[0] = arguments.at("strength");
            parameters.values[1] = parameters.values[0] * .35f;
        }
        for (int index : {0, 1, 2, 3, 4, 7})
            if (arguments.contains(parameterNames[index]))
                parameters.values[index] = arguments.at(parameterNames[index]);
        if (arguments.contains("remove_body_hair"))
            parameters.removeBodyHair = arguments.at("remove_body_hair").get<bool>();
        MaiPortraitPersonalization profile(directory);
        const auto status = profile.status();
        if (status.activeVersion > 0 && arguments.value("use_personal_model", true)) {
            const auto learned = profile.recommend(features);
            for (int index : {0, 1, 2, 3, 4, 7})
                if (!arguments.contains(parameterNames[index]) && !(arguments.contains("strength") && index < 2))
                    parameters.values[index] = learned.values[index];
        }
        if (plan.faceCount == 0 && (parameters.values[3] > 0 || parameters.values[4] > 0))
            throw MaiPortraitFailure("face_detection_failed", "Face geometry requires a reliably located face");
        Json effective = Json::object(), geometry = {{"operation", "beautify"}, {"use_personal_model", false}};
        for (int index = 0; index < 8; ++index) effective[parameterNames[index]] = parameters.values[index];
        effectiveHairParameter(effective, parameters);
        // Geometry is a second face-only pass; it must never repeat body cleaning.
        geometry["remove_body_hair"] = false;
        for (int index : {0, 1, 2, 3, 4, 7})
            geometry[parameterNames[index]] = index == 3 || index == 4 ? parameters.values[index] : 0.f;
        Json tiles = Json::array();
        for (const auto& tile : plan.tiles) tiles.push_back({{"sample_rect", tile.sample}, {"write_rect", tile.write}});
        Json faceContext = nullptr, faceRoi = nullptr;
        if (plan.faceCount > 0) {
            faceRoi = {{"x", plan.faceRegion[0]},
                       {"y", plan.faceRegion[1]},
                       {"width", plan.faceRegion[2]},
                       {"height", plan.faceRegion[3]}};
            faceContext = {{"source_width", sourceWidth},
                           {"source_height", sourceHeight},
                           {"face_count", plan.faceCount},
                           {"subject_index", plan.subjectIndex},
                           {"roi", faceRoi},
                           {"selected_face", faceJson(plan.selectedFace)},
                           {"global_features", features},
                           {"source_fingerprint", sourceFingerprint}};
        }
        context->metadata = {
            {"source_fingerprint", sourceFingerprint},
            {"source_path", sourcePath},
            {"parameters", parameters.values},
            {"effective_parameters", effective},
            {"features", features},
            {"face_count", plan.faceCount},
            {"person_count", plan.personCount},
            {"face_analysis_performed", true},
            {"person_analysis_performed", true},
            {"analysis_version", "mediapipe-onnx-skin-20261104-v3"},
            {"processing_version", "native-owned-skin-tiles-20261105-v8"},
            {"personal_model_version", status.activeVersion},
            {"source_width", sourceWidth},
            {"source_height", sourceHeight},
            {"roi", {{"x", 0}, {"y", 0}, {"width", sourceWidth}, {"height", sourceHeight}}},
            {"skin_scope", plan.hasPersonOwnership ? "selected_person_visible_skin" : "face_skin_only"},
            {"person_ownership_reliable", plan.hasPersonOwnership},
            {"body_ownership_skip_reason", plan.bodyOwnershipSkipReason},
            {"face_color_skipped", false},
            {"unreliable_person_masks", plan.unreliablePersonMasks},
            {"owned_body_proxy_pixels", plan.bodyConfidencePixels},
            {"person_analysis_windows", plan.personAnalysisWindows},
            {"person_analysis_policy",
             plan.personAnalysisWindows > 1 ? "whole_proxy_plus_overlapping_half_quarter_windows" : "whole_proxy"},
            {"skin_analysis_proxy_rect", plan.skinProxyRegion},
            {"tile_count", plan.tiles.size()},
            {"mask_edge_refinement", "body_class2_mature_matte_and_native_face_rgb"},
            {"whitening_shader_strength", MaiPortraitEngine::whiteningShaderStrength(parameters.values[7])},
            {"whitening_blend_gate", MaiPortraitEngine::whiteningBlendGate(parameters.values[7])},
            {"whitening_color_policy", "source_hue_chroma_cap"},
            {"whitening_shadow_amount", MaiPortraitEngine::whiteningShadowAmount(parameters.values[7])},
            {"clean_applied", false},
            {"clean_strength", 0},
            {"clean_skip_reason", "material_not_prepared"},
            {"clean_scope", "none"},
            {"body_cleanup_request_mode", bodyCleanupMode(parameters)},
            {"mask_semantics", "diagnostic_skin_alpha; rgba_is_already_composited; do_not_apply_mask_again"}};
        Json analysis = context->metadata;
        analysis["skin_refinement_request"] = plan.refinementRequired
                                                  ? Json{{"source_rect", plan.refinementSourceRegion},
                                                         {"proxy_width", plan.refinementProxyWidth},
                                                         {"proxy_height", plan.refinementProxyHeight}}
                                                  : Json(nullptr);
        analysis["tiles"] = tiles;
        analysis["face_roi"] = faceRoi;
        analysis["face_plan"] = faceContext;
        analysis["geometry_parameters"] = geometry;
        auto response = std::unique_ptr<char, decltype(&std::free)>(success(analysis), &std::free);
        if (!response) throw MaiPortraitFailure("allocation_failed", "Cannot allocate skin analysis metadata");
        return {context.release(), response.release(), nullptr};
    } catch (const MaiPortraitFailure& error) {
        return {nullptr, nullptr, copied(diagnostic(error.code(), error.what(), sourceWidth, sourceHeight).dump())};
    } catch (const std::exception&) {
        return {nullptr, nullptr,
                copied(diagnostic("skin_analysis_failed", "Skin analysis failed; no output was committed", sourceWidth,
                                  sourceHeight)
                           .dump())};
    }
}

extern "C" char* maiPortraitProtectSkinFaceRegion(MaiPortraitSkinContext* context, const unsigned char* rgba, int width,
                                                  int height, int stride) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!context || context->failed || !context->faceEngine)
            throw MaiPortraitFailure("invalid_skin_sequence", "Skin context is not ready for face protection");
        context->faceEngine->protectSkinFace(context->plan, rgba, width, height, stride);
        context->metadata["face_color_skipped"] = context->plan.faceColorSkipped;
        MaiPortraitEngine::prepareSkinMaterial(context->plan, context->parameters);
        if (context->plan.faceColorSkipped &&
            (!context->plan.hasPersonOwnership || context->plan.bodyConfidencePixels < 8))
            throw MaiPortraitFailure("skin_mask_empty",
                                     "Face protection is unreliable and no owned body skin is available");
        context->faceEngine.reset();
        materialMetadata(context->metadata, context->plan);
        context->metadata["body_cleanup_request_mode"] = bodyCleanupMode(context->parameters);
        Json protection = {{"face_color_skipped", context->plan.faceColorSkipped}};
        materialMetadata(protection, context->plan);
        auto* response = success(protection);
        if (!response) throw MaiPortraitFailure("allocation_failed", "Cannot allocate face protection metadata");
        return response;
    } catch (const MaiPortraitFailure& e) {
        if (context) context->failed = true;
        return copied(Json{{"ok", false}, {"error", diagnostic(e.code(), e.what())}}.dump());
    } catch (const std::exception&) {
        if (context) context->failed = true;
        return copied(
            Json{{"ok", false}, {"error", diagnostic("skin_analysis_failed", "Face protection failed")}}.dump());
    }
}

extern "C" MaiPortraitRegionResult maiPortraitProcessSkinTile(MaiPortraitSkinContext* context, int index,
                                                              const unsigned char* rgba, int width, int height,
                                                              int stride) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!context || context->failed || index != context->nextTile)
            throw MaiPortraitFailure("invalid_skin_sequence", "Process each planned skin tile once in order");
        const auto output =
            MaiPortraitEngine::applySkinTile(rgba, width, height, stride, context->plan, index, context->parameters);
        auto bytes = std::unique_ptr<unsigned char, decltype(&std::free)>(
            static_cast<unsigned char*>(std::malloc(output.rgba.size())), &std::free);
        auto mask = std::unique_ptr<unsigned char, decltype(&std::free)>(
            static_cast<unsigned char*>(std::malloc(output.skinMask.size())), &std::free);
        if (!bytes || !mask) throw MaiPortraitFailure("allocation_failed", "Cannot allocate bounded skin output");
        std::memcpy(bytes.get(), output.rgba.data(), output.rgba.size());
        std::memcpy(mask.get(), output.skinMask.data(), output.skinMask.size());
        context->skinPixels += output.skinPixels;
        context->changedPixels += output.changedPixels;
        for (int k = 0; k < 2; ++k) {
            context->lightness[k] += output.skinLightness[k] * output.skinWeight;
            context->chroma[k] += output.skinChroma[k] * output.skinWeight;
        }
        context->weight += output.skinWeight;
        const auto& tile = context->plan.tiles[index];
        const auto& face = context->plan.selectedFace;
        for (int y = tile.write[1]; y < tile.write[1] + tile.write[3]; ++y)
            for (int x = tile.write[0]; x < tile.write[0] + tile.write[2]; ++x) {
                if (context->plan.faceCount > 0 && x >= face.x - .15f * face.width && x < face.x + 1.15f * face.width &&
                    y >= face.y - .15f * face.height && y < face.y + 1.15f * face.height)
                    continue;
                if (output.skinMask[static_cast<size_t>(y - tile.sample[1]) * width + x - tile.sample[0]] > 0)
                    ++context->bodyPixels;
            }
        const auto& box = output.processingRegion;
        if (box[2] > 0 && box[3] > 0) {
            auto& all = context->processing;
            if (all[2] == 0)
                all = box;
            else {
                const int right = std::max(all[0] + all[2], box[0] + box[2]),
                          bottom = std::max(all[1] + all[3], box[1] + box[3]);
                all[0] = std::min(all[0], box[0]);
                all[1] = std::min(all[1], box[1]);
                all[2] = right - all[0];
                all[3] = bottom - all[1];
            }
        }
        ++context->nextTile;
        auto response =
            std::unique_ptr<char, decltype(&std::free)>(copied(Json{{"tile_index", index}}.dump()), &std::free);
        if (!response) throw MaiPortraitFailure("allocation_failed", "Cannot allocate skin tile metadata");
        return {bytes.release(), mask.release(), width, height, response.release(), nullptr};
    } catch (const MaiPortraitFailure& e) {
        if (context) context->failed = true;
        return {nullptr, nullptr, 0, 0, nullptr, copied(diagnostic(e.code(), e.what(), width, height).dump())};
    } catch (const std::exception&) {
        if (context) context->failed = true;
        return {
            nullptr, nullptr,
            0,       0,
            nullptr, copied(diagnostic("skin_processing_failed", "Skin tile processing failed", width, height).dump())};
    }
}

extern "C" char* maiPortraitFinishSkin(MaiPortraitSkinContext* context) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!context || context->failed || context->nextTile != static_cast<int>(context->plan.tiles.size()))
            throw MaiPortraitFailure("incomplete_skin_tiles",
                                     "All planned skin tiles must complete before publication");
        const auto& values = context->parameters.values;
        const bool color =
            values[0] > 0 || values[1] > 0 || values[2] != 0 || values[7] > 0 || context->plan.cleanApplied;
        if (color && (context->skinPixels == 0 || (context->plan.faceColorSkipped && context->bodyPixels < 64)))
            throw MaiPortraitFailure("skin_mask_empty",
                                     "No sufficient reliably protected skin was found; no result was committed");
        // A C1 gate near zero may round to an exact RGBA8 identity. This remains a
        // valid edit; it must not be reported as a processing failure.
        const bool substantial = values[0] > .05f || values[1] > .05f || std::abs(values[2]) > .05f ||
                                 values[7] > .05f || context->parameters.removeBodyHair.value_or(false);
        if (color && substantial && context->changedPixels == 0)
            throw MaiPortraitFailure("no_visible_effect", "Skin processing did not change any RGB pixels");
        context->metadata["skin_analysis_proxy_rect"] = context->plan.skinProxyRegion;
        context->metadata["owned_body_proxy_pixels"] = context->plan.bodyConfidencePixels;
        context->metadata["skin_refinement_applied"] = context->plan.refinementApplied;
        context->metadata["skin_analysis_source_rect"] = context->plan.analysisSourceRegion;
        context->metadata["skin_analysis_proxy_width"] = context->plan.proxyWidth;
        context->metadata["skin_analysis_proxy_height"] = context->plan.proxyHeight;
        context->metadata["skin_visible_source_partial"] = context->plan.visibleSourcePartial;
        materialMetadata(context->metadata, context->plan);
        auto metadata = context->metadata;
        metadata["processing_bbox"] = context->processing;
        metadata["skin_pixel_count"] = context->skinPixels;
        metadata["body_skin_pixel_count"] = context->bodyPixels;
        metadata["skin_weight"] = context->weight;
        metadata["mask_coverage_in_roi"] =
            context->skinPixels / static_cast<double>(context->plan.sourceWidth * context->plan.sourceHeight);
        metadata["changed_pixel_count"] = context->changedPixels;
        metadata["changed_rgb_fraction_in_roi"] =
            context->changedPixels / static_cast<double>(context->plan.sourceWidth * context->plan.sourceHeight);
        metadata["skin_L_before"] = context->weight > 0 ? Json(context->lightness[0] / context->weight) : Json(nullptr);
        metadata["skin_L_after"] = context->weight > 0 ? Json(context->lightness[1] / context->weight) : Json(nullptr);
        metadata["skin_C_before"] = context->weight > 0 ? Json(context->chroma[0] / context->weight) : Json(nullptr);
        metadata["skin_C_after"] = context->weight > 0 ? Json(context->chroma[1] / context->weight) : Json(nullptr);
        auto* response = success(metadata);
        if (!response) throw MaiPortraitFailure("allocation_failed", "Cannot allocate finished skin metadata");
        return response;
    } catch (const MaiPortraitFailure& e) {
        if (context) context->failed = true;
        return copied(Json{{"ok", false}, {"error", diagnostic(e.code(), e.what())}}.dump());
    } catch (const std::exception&) {
        if (context) context->failed = true;
        return copied(
            Json{{"ok", false}, {"error", diagnostic("skin_processing_failed", "Cannot finish skin processing")}}
                .dump());
    }
}
extern "C" void maiPortraitFreeSkinContext(MaiPortraitSkinContext* context) {
    delete context;
}

extern "C" char* maiPortraitRefineSkinRegionRgba(MaiPortraitSkinContext* context, const unsigned char* rgba, int width,
                                                 int height, int stride) {
    try {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!context || context->failed || !context->faceEngine || context->nextTile != 0)
            throw MaiPortraitFailure("invalid_skin_sequence", "Regional refinement is unavailable in this phase");
        context->faceEngine->refineSkin(context->plan, rgba, width, height, stride);
        const auto& p = context->plan;
        auto* response = success({{"applied", true},
                                  {"source_rect", p.analysisSourceRegion},
                                  {"proxy_width", p.proxyWidth},
                                  {"proxy_height", p.proxyHeight}});
        if (!response) throw MaiPortraitFailure("allocation_failed", "Cannot allocate refinement metadata");
        return response;
    } catch (const MaiPortraitFailure& e) {
        if (context) context->failed = true;
        return copied(Json{{"ok", false}, {"error", diagnostic(e.code(), e.what(), width, height)}}.dump());
    } catch (const std::exception&) {
        if (context) context->failed = true;
        return copied(
            Json{{"ok", false},
                 {"error", diagnostic("skin_refinement_failed",
                                      "Regional skin analysis failed; no result was committed", width, height)}}
                .dump());
    }
}

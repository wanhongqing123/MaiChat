// 文件作用：移动端与桌面共用的人像工具定义；普通服务仍可直接调用 C++ 核心。
#include "MaiPortraitHostTools.h"

#include <utility>
#include "MaiPortraitBridge.h"

namespace {
class MaiPortraitHostTool final : public MaiTool {
public:
    MaiPortraitHostTool(const char* name, const char* description, const char* schema, MaiPortraitHostHandler handler)
        : mName(name), mDescription(description), mSchema(schema), mHandler(std::move(handler)) {}
    std::string name() const override {
        return mName;
    }
    std::string description() const override {
        return mDescription;
    }
    std::string parametersSchema() const override {
        return mSchema;
    }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Portrait operation was canceled");
        if (mName == "mobile_beautify_image" || mName == "mobile_reshape_body") {
            const char* operation = mName == "mobile_beautify_image" ? "beautify" : "reshape_body";
            char* invalid = maiPortraitValidateArguments(operation, arguments.c_str(), 0);
            if (invalid) {
                const std::string detail(invalid);
                maiPortraitFree(invalid);
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, detail);
            }
        }
        if (!mHandler) return MaiToolResult::failure(MaiErrorCode::NotConfigured, "Portrait host is unavailable");
        return mHandler(mName, arguments, context);
    }

private:
    std::string mName, mDescription, mSchema;
    MaiPortraitHostHandler mHandler;
};
}  // namespace

void registerMaiPortraitHostTools(MaiToolRegistry& tools, const MaiPortraitHostHandler& handler) {
    tools.add(std::make_unique<MaiPortraitHostTool>(
        "mobile_beautify_image",
        // 调色覆盖可靠归属的同一人物皮肤；原生脸部关键点只负责保护五官与几何修饰。
        // 必须按返回范围说明部分处理，不能把脸部跳过或仅处理脸部说成全身处理成功。
        "Retouch an existing workspace portrait into a NEW file using local skin and person masks. "
        "path is the existing file path; do not export it again as a photo-library asset ID. "
        "Smoothing, brightening, whitening, warmth, slimming and eye enlargement are independent. "
        "whitening (default 0) uses a local LUT with input-hue/chroma protection and bounded shadow compensation. "
        "It affects reliably identified visible skin "
        "of the selected person, excluding protected eyes, brows, lips, clothing and background. "
        "brightening is RGB gain, not whitening. For whitening-only use explicit smoothing=0, "
        "brightening=0 and warmth=0; strength only controls smoothing and brightening. "
        "Use original_path from prior results when refining; prior generated paths are also "
        "resolved to their original. Explicit parameters override the validated personal model. "
        "remove_body_hair=true explicitly cleans fine body hair and small dark spots; false preserves body hair. "
        "Omit it for automatic gentle body cleansing with skin retouch strength. This only affects trusted "
        "body skin and does not remove face, beard or scalp hair. Unspecified shape changes default to zero. "
        "For multiple faces set subject_index "
        "from left to right starting at 0. Return result_id for user feedback and deliver the "
        "returned path with agent_send_media (file_path=path, type=image) in this AI conversation. "
        "Do not open a full-screen preview automatically; the user can tap the chat image. "
        "Use mobile_preview_image only when explicitly requested. Hosts process original pixels "
        "within their documented size limits and preserve dimensions. "
        "Read skin_scope and face_color_skipped: report partial coverage instead of claiming full "
        "skin retouching. The returned mask is diagnostic; pixels are already composited. "
        "Use masked skin L*/C* and coverage to assess changes, not full-image threshold fractions. "
        "Unknown fields including action/delegate are rejected. Never infer user preference from your own judgment.",
        // 所有处理复用普通 C++ 核心；单独的几何参数默认关闭，strength 保留为旧调用映射。
        R"({"type":"object","properties":{"path":{"type":"string"},"strength":{"type":"number","minimum":0,"maximum":1},)"
        R"("smoothing":{"type":"number","minimum":0,"maximum":1},"brightening":{"type":"number","minimum":0,"maximum":1},)"
        R"("whitening":{"type":"number","minimum":0,"maximum":1,"default":0,"description":"Independent skin whitening with LUT, hue/chroma protection and shadow compensation; brightening is separate RGB gain."},)"
        R"("warmth":{"type":"number","minimum":-1,"maximum":1},"slim_face":{"type":"number","minimum":0,"maximum":1},)"
        R"("enlarge_eyes":{"type":"number","minimum":0,"maximum":1},"subject_index":{"type":"integer","minimum":0},)"
        R"("use_personal_model":{"type":"boolean"},"remove_body_hair":{"type":"boolean",)"
        R"("description":"Optional body skin cleansing: true cleans fine hair and small dark spots; false preserves hair;)"
        R"( omit for automatic gentle cleansing with skin strength."}},"required":["path"],"additionalProperties":false})",
        handler));
    tools.add(std::make_unique<MaiPortraitHostTool>(
        "mobile_reshape_body",
        "Create a NEW locally reshaped photo using body pose and person segmentation. "
        "Supports mild waist slimming and leg proportion extension on clear upright standing "
        "photos. Occluded joints, uncertain poses or no room below the feet return an error. "
        "slim_waist and leg_length are bounded normalized strengths, not literal percentages. "
        "For multiple people specify subject_index from left to right starting at 0. "
        "Keep the original and deliver the new path with agent_send_media (file_path=path, "
        "type=image) in this AI conversation. Do not open a full-screen preview automatically; "
        "the user can tap the chat image. Use mobile_preview_image only when explicitly "
        "requested, and use result_id for explicit user feedback.",
        R"({"type":"object","properties":{"path":{"type":"string"},"slim_waist":{"type":"number","minimum":0,"maximum":1},)"
        R"("leg_length":{"type":"number","minimum":0,"maximum":1},"subject_index":{"type":"integer","minimum":0},)"
        R"("use_personal_model":{"type":"boolean"}},"required":["path"],"additionalProperties":false})",
        handler));
    tools.add(std::make_unique<MaiPortraitHostTool>(
        "mobile_portrait_feedback",
        "Record the user's explicit preference between two portrait results from the SAME "
        "original photo. Use returned result_id values. Include ONLY the attributes the user "
        "actually evaluated; unmentioned attributes remain unknown. This stores feedback, "
        "not model training. Use model_continue_training afterward when requested.",
        R"({"type":"object","properties":{"preferred_result_id":{"type":"string"},"rejected_result_id":{"type":"string"},)"
        R"("attributes":{"type":"array","minItems":1,"maxItems":8,"uniqueItems":true,"items":{"type":"string",)"
        R"("enum":["smoothing","brightening","warmth","slim_face","enlarge_eyes","slim_waist","leg_length","whitening"]}}},)"
        R"("required":["preferred_result_id","rejected_result_id","attributes"],"additionalProperties":false})",
        handler));
    tools.add(std::make_unique<MaiPortraitHostTool>(
        "model_continue_training",
        "Train or inspect the CURRENT user's local portrait preference model using saved "
        "explicit result-pair feedback. This updates small ranking weights; it does not train "
        "MediaPipe base networks or implement album semantic search. train saves a checkpoint. "
        "Fewer than three independent original photos produce candidate_only, not an activated "
        "model. Activation requires held-out-photo validation improvement. status/versions "
        "show active and saved versions. rollback with version=0 restores the baseline. "
        "Report the actual returned activation and validation status to the user.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["train","status","versions","rollback"]},)"
        R"("version":{"type":"integer","minimum":0}},"required":["action"],"additionalProperties":false})",
        handler));
}

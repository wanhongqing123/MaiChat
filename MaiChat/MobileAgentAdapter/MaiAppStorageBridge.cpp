// 文件作用：只适配 C JSON、句柄生命周期和原生页的清理范围，共享核心决定候选与执行。
#include "MaiAppStorageBridge.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <json.hpp>

#include "MaiAppStorage.h"
#include "MaiFilePath.h"
#include "MaiIdGenerator.h"

namespace {
using Json = nlohmann::json;
constexpr std::size_t maximumRequestBytes = 64 * 1024;
constexpr std::size_t maximumRootBytes = 16 * 1024;

struct StorageState {
    explicit StorageState(MaiAppStoragePaths paths)
        : workspace(paths.workspace), storage(std::move(paths)), sessionId(MaiIdGenerator::generate("storage_ui_")) {}

    std::string workspace;
    MaiAppStorage storage;
    std::string sessionId;
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    bool closed = false;
    bool cleanupAllowed = false;
    std::string previewId;
};

// 不解引用 ABI 句柄，也不复用标识，避免迟到请求访问已释放对象或误中后来的账号实例。
std::mutex registryMutex;
std::unordered_map<std::uintptr_t, std::shared_ptr<StorageState>> registry;
std::uintptr_t nextHandle = 1;

std::size_t boundedLength(const char* text, std::size_t maximum) {
    if (!text) return 0;
    std::size_t length = 0;
    while (length <= maximum && text[length]) ++length;
    return length;
}

bool validRoot(const char* root) {
    const auto length = boundedLength(root, maximumRootBytes);
    return length > 0 && length <= maximumRootBytes && MaiFilePath::fromUtf8(std::string(root, length)).isAbsolute();
}

Json failure(MaiErrorCode code, const std::string& message) {
    return {{"ok", false}, {"errorCode", maiErrorCodeToString(code)}, {"error", message}};
}

char* ownedJson(const Json& value) {
    const auto serialized = value.dump();
    auto* response = static_cast<char*>(std::malloc(serialized.size() + 1));
    if (response) std::memcpy(response, serialized.c_str(), serialized.size() + 1);
    return response;
}

std::shared_ptr<StorageState> findState(void* handle) {
    std::lock_guard<std::mutex> lock(registryMutex);
    const auto found = registry.find(reinterpret_cast<std::uintptr_t>(handle));
    return found == registry.end() ? nullptr : found->second;
}

bool allowedFields(const Json& request, std::initializer_list<const char*> fields) {
    for (const auto& item : request.items()) {
        bool allowed = false;
        for (const auto* field : fields) allowed = allowed || item.key() == field;
        if (!allowed) return false;
    }
    return true;
}

Json previewOutput(const MaiAppStoragePreview& preview, bool cleanupAllowed) {
    auto summaries = Json::array();
    for (const auto& summary : preview.summaries) {
        summaries.push_back({{"target", summary.target},
                             {"bytes", summary.bytes},
                             {"files", summary.files},
                             {"eligible_bytes", summary.eligibleBytes},
                             {"eligible_files", summary.eligibleFiles},
                             {"excluded_recent_files", summary.excludedRecentFiles},
                             {"excluded_recent_bytes", summary.excludedRecentBytes},
                             {"excluded_protected_files", summary.excludedProtectedFiles},
                             {"excluded_policy_files", summary.excludedPolicyFiles},
                             {"truncated", summary.truncated}});
    }
    return {{"preview_id", preview.id},
            {"summaries", std::move(summaries)},
            {"candidate_bytes", preview.candidateBytes},
            {"candidate_files", preview.candidates.size()},
            {"limited", preview.limited},
            {"min_age_hours", preview.minimumAgeHours},
            {"cleanup_allowed", cleanupAllowed},
            {"workspace_cleanup_allowed", false}};
}

Json requestStorage(StorageState& state, const char* text) {
    const auto length = boundedLength(text, maximumRequestBytes);
    if (!length || length > maximumRequestBytes)
        return failure(MaiErrorCode::InvalidInput, "Storage request must be 1..65536 bytes");
    bool duplicateField = false;
    std::unordered_set<std::string> keys;
    const auto request = Json::parse(text, text + length, [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::key && depth == 1)
            duplicateField = !keys.insert(parsed.get<std::string>()).second || duplicateField;
        return true;
    });
    if (duplicateField || !request.is_object() || !request.contains("action") || !request["action"].is_string())
        return failure(MaiErrorCode::InvalidInput, "Expected a unique string action in an object");

    MaiAppStorageContext context;
    context.sessionId = state.sessionId;
    context.workspace = state.workspace;
    context.cancel = &state.cancel;
    // 未收集跨会话交付收据；本入口永远不声明完整保护，也绝不请求 Workspace 清理。
    context.protectionComplete = false;
    const auto action = request["action"].get<std::string>();
    if (action == "preview") {
        if (!allowedFields(request, {"action", "target"}) ||
            (request.contains("target") && !request["target"].is_string()))
            return failure(MaiErrorCode::InvalidInput, "Preview accepts only action and target");
        const auto target = request.value("target", "app");
        MaiAppStorageSelection selection;
        if (target == "temporary")
            selection.target = MaiAppStorageTarget::Temporary;
        else if (target == "cache")
            selection.target = MaiAppStorageTarget::Cache;
        else if (target != "app")
            return failure(MaiErrorCode::InvalidInput, "Only app, temporary, or cache is allowed");

        state.previewId.clear();
        state.cleanupAllowed = false;
        const auto preview = state.storage.preview(selection, context);
        if (!preview) return failure(preview.error().code(), preview.error().message());
        state.previewId = preview.value().id;
        state.cleanupAllowed = selection.target != MaiAppStorageTarget::App;
        return {{"ok", true}, {"output", previewOutput(preview.value(), state.cleanupAllowed)}};
    }
    if (action == "cleanup") {
        if (!allowedFields(request, {"action", "preview_id"}) || !request.contains("preview_id") ||
            !request["preview_id"].is_string())
            return failure(MaiErrorCode::InvalidInput, "Cleanup accepts only action and preview_id");
        const auto previewId = request["preview_id"].get<std::string>();
        if (!state.cleanupAllowed || previewId.empty() || previewId != state.previewId)
            return failure(MaiErrorCode::InvalidInput, "Preview temporary or cache on this handle before cleanup");
        // 核心管理单次/过期及精确文件快照；适配层也撤销 UI 能力，失败后必须重新预览。
        state.previewId.clear();
        state.cleanupAllowed = false;
        const auto cleanup = state.storage.cleanup(previewId, context);
        if (!cleanup) return failure(cleanup.error().code(), cleanup.error().message());
        return {{"ok", true},
                {"output",
                 {{"removed_files", cleanup.value().removedFiles},
                  {"reclaimed_bytes", cleanup.value().reclaimedBytes},
                  {"skipped_files", cleanup.value().skippedFiles}}}};
    }
    return failure(MaiErrorCode::InvalidInput, "Unknown storage action");
}
}  // namespace

extern "C" void* maiMobileAppStorageCreate(const char* temporary, const char* cache, const char* workspace) {
    try {
        if (!validRoot(temporary) || !validRoot(cache) || !validRoot(workspace)) return nullptr;
        auto state = std::make_shared<StorageState>(MaiAppStoragePaths{temporary, cache, workspace});
        std::lock_guard<std::mutex> lock(registryMutex);
        if (nextHandle == std::numeric_limits<std::uintptr_t>::max()) return nullptr;
        const auto id = nextHandle++;
        registry.emplace(id, std::move(state));
        return reinterpret_cast<void*>(id);
    } catch (...) {
        return nullptr;
    }
}

extern "C" char* maiMobileAppStorageRequest(void* handle, const char* request) {
    try {
        auto state = findState(handle);
        if (!state) return ownedJson(failure(MaiErrorCode::NotConfigured, "Storage handle is closed"));
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed || state->cancel.load())
            return ownedJson(failure(MaiErrorCode::NotConfigured, "Storage handle is closed"));
        return ownedJson(requestStorage(*state, request));
    } catch (const Json::exception&) {
        try {
            return ownedJson(failure(MaiErrorCode::InvalidInput, "Invalid storage request JSON"));
        } catch (...) {
            return nullptr;
        }
    } catch (...) {
        try {
            return ownedJson(failure(MaiErrorCode::Internal, "Storage request failed"));
        } catch (...) {
            return nullptr;
        }
    }
}

extern "C" void maiMobileAppStorageDestroy(void* handle) {
    try {
        std::shared_ptr<StorageState> state;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            const auto found = registry.find(reinterpret_cast<std::uintptr_t>(handle));
            if (found == registry.end()) return;
            state = found->second;
            registry.erase(found);
            state->cancel.store(true);
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed = true;
    } catch (...) {
        // C ABI 的销毁不传播异常；已撤销的句柄不可再访问共享服务。
    }
}

extern "C" void maiMobileAppStorageFree(char* response) {
    std::free(response);
}

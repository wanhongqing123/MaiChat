// 文件作用：桥接 SQLite 生成列与受限的会话原始日志。
#include "MaiRolloutProjection.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <string>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"
#include "MaiRolloutLog.h"

namespace {
struct ProjectionState {
    MaiFilePath root;
    bool itemOnly = false;
};

MaiResult<MaiFilePath> resolveSource(const ProjectionState& state, const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos || value.find(':') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "Invalid rollout source path."};
    auto relative = value;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    if (relative.compare(0, 9, "sessions/") != 0 || relative.back() == '/')
        return {MaiErrorCode::InvalidInput, "Source is outside the session rollout directory."};
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto end = relative.find('/', start);
        const auto component = relative.substr(start, end - start);
        if (component.empty() || component == "." || component == "..")
            return {MaiErrorCode::InvalidInput, "Rollout source contains an invalid path component."};
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const auto path = MaiFilePath::fromUtf8(relative);
    if (path.isAbsolute())
        return {MaiErrorCode::InvalidInput, "Rollout source must be relative to its storage root."};
    const auto combined = state.root.append(path);
    if (MaiFileSystem::isSymbolicLink(combined))
        return {MaiErrorCode::InvalidInput, "Rollout source cannot be a symbolic link."};
    const auto resolved = MaiFileSystem::resolve(combined);
    if (resolved.isEmpty() || !state.root.isParentOf(resolved))
        return {MaiErrorCode::InvalidInput, "Resolved rollout source is outside its storage root."};
    return resolved;
}

void projectionValue(sqlite3_context* context, int count, sqlite3_value** values) {
    try {
        if (count != 3 || sqlite3_value_type(values[0]) != SQLITE_TEXT ||
            sqlite3_value_type(values[1]) != SQLITE_INTEGER ||
            sqlite3_value_type(values[2]) != SQLITE_INTEGER || sqlite3_value_int64(values[1]) < 0 ||
            sqlite3_value_int64(values[2]) <= 0) {
            sqlite3_result_error(context, "Invalid rollout source location.", -1);
            return;
        }
        const auto* state = static_cast<const ProjectionState*>(sqlite3_user_data(context));
        const auto* text = reinterpret_cast<const char*>(sqlite3_value_text(values[0]));
        if (!text) {
            sqlite3_result_error_nomem(context);
            return;
        }
        const std::string pathValue(text, static_cast<std::size_t>(sqlite3_value_bytes(values[0])));
        const auto path = resolveSource(*state, pathValue);
        if (!path) {
            sqlite3_result_error(context, path.error().message().c_str(), -1);
            return;
        }
        const auto record = MaiRolloutLog::readAt(
            path.value(), static_cast<std::uint64_t>(sqlite3_value_int64(values[1])),
            static_cast<std::uint64_t>(sqlite3_value_int64(values[2])));
        if (!record) {
            sqlite3_result_error(context, "Rollout source is missing, incomplete or invalid.", -1);
            sqlite3_result_error_code(context, SQLITE_IOERR_READ);
            return;
        }
        auto payload = nlohmann::ordered_json::parse(record.value().payload, nullptr, false);
        if (state->itemOnly) {
            // thread_items 指向应用历史投影完成事件；不能把供应商 response_item
            // 或任意合法对象混充成同一种数据结构。
            if (record.value().type != "event_msg" || !payload.is_object() ||
                payload.value("type", std::string{}) != "item_completed" ||
                !payload.contains("item") || !payload["item"].is_object()) {
                sqlite3_result_error(context, "Rollout location does not contain a completed item.", -1);
                return;
            }
            payload = payload["item"];
        }
        const auto result = payload.dump();
        sqlite3_result_text64(context, result.data(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    } catch (const std::bad_alloc&) {
        sqlite3_result_error_nomem(context);
    } catch (const std::exception&) {
        sqlite3_result_error(context, "Invalid rollout projection source.", -1);
    }
}

void destroyState(void* state) {
    delete static_cast<ProjectionState*>(state);
}
}  // namespace

MaiError maiRegisterRolloutProjection(sqlite3* database, const MaiFilePath& storageRoot) {
    maiAssertBlockingAllowed("maiRegisterRolloutProjection");
    if (!database || storageRoot.isEmpty() || !MaiFileSystem::isDirectory(storageRoot))
        return {MaiErrorCode::InvalidInput, "Projection requires a database and an existing storage root."};
    const auto root = MaiFileSystem::resolve(storageRoot);
    if (root.isEmpty() || !root.isAbsolute())
        return {MaiErrorCode::InvalidInput, "Projection storage root cannot be resolved."};
    for (const auto& entry : {std::make_pair("mai_rollout_item_json", true),
                              std::make_pair("mai_rollout_payload", false)}) {
        auto state = std::make_unique<ProjectionState>();
        state->root = root;
        state->itemOnly = entry.second;
        // 来源区间在追加日志中不可变；函数没有写副作用且拒绝根外访问。
        // 这两个标记使可信根内的生成列在 trusted_schema=OFF 下仍可安全使用。
        // create_function_v2 在注册失败时也负责调用提供的状态析构器。
        const int result = sqlite3_create_function_v2(
            database, entry.first, 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
            state.release(), projectionValue, nullptr, nullptr, destroyState);
        if (result != SQLITE_OK)
            return {MaiErrorCode::Internal, "Cannot register rollout projection functions."};
    }
    return {};
}

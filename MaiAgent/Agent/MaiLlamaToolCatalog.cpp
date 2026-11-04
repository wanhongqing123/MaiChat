#include "MaiLlamaToolCatalog.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include <json.hpp>

namespace {

constexpr std::size_t kMaximumCatalogSize = 512;
constexpr std::size_t kMaximumDiscoveryNames = 8;
constexpr std::size_t kMaximumCatalogBytes = 2 * 1024 * 1024;
constexpr const char* kDiscoveryName = "tool_search";

bool isValidCatalogText(const std::string& value, std::size_t& bytes) {
    if (value.size() > kMaximumCatalogBytes - bytes || value.find('\0') != std::string::npos)
        return false;
    // 严格编码只验证 UTF-8；验证结果不替换原字段，否则会改变后续实际工具声明。
    try {
        nlohmann::json(value).dump();
    } catch (const nlohmann::json::type_error&) {
        return false;
    }
    bytes += value.size();
    return true;
}

}  // namespace

MaiLlamaToolCatalog::MaiLlamaToolCatalog(std::vector<MaiToolSpec> specifications)
    : mSpecifications(std::move(specifications)) {
    if (mSpecifications.empty() || mSpecifications.size() > kMaximumCatalogSize) {
        mError = {MaiErrorCode::InvalidInput, "Tool catalog must contain 1 to 512 tools."};
        return;
    }
    std::unordered_set<std::string> names;
    std::size_t bytes = 0;
    for (const auto& specification : mSpecifications) {
        if (specification.name.empty() || specification.name == kDiscoveryName ||
            !names.insert(specification.name).second) {
            mError = {
                MaiErrorCode::InvalidInput,
                "Tool catalog names must be nonempty, unique, and different from tool_search."};
            return;
        }
        if (!isValidCatalogText(specification.name, bytes) ||
            !isValidCatalogText(specification.description, bytes) ||
            !isValidCatalogText(specification.parametersJson, bytes) ||
            !nlohmann::json::parse(specification.parametersJson, nullptr, false).is_object()) {
            mError = {MaiErrorCode::InvalidInput,
                      "Tool catalog requires bounded UTF-8 metadata and object parameter schemas."};
            return;
        }
    }
}

const MaiError& MaiLlamaToolCatalog::error() const {
    return mError;
}

MaiToolSpec MaiLlamaToolCatalog::specification() const {
    if (mError.hasError()) return {};

    nlohmann::json names = nlohmann::json::array();
    for (const auto& specification : mSpecifications) names.push_back(specification.name);
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties",
                                    {{"names",
                                      {{"type", "array"},
                                       {"items", {{"type", "string"}, {"enum", std::move(names)}}},
                                       {"minItems", 1},
                                       {"maxItems", kMaximumDiscoveryNames},
                                       {"uniqueItems", true}}}}},
                                   {"required", {"names"}},
                                   {"additionalProperties", false}};

    MaiToolSpec result;
    result.name = kDiscoveryName;
    // 先发现当前任务所需的工具，names 必须选枚举中的原名且每次最多八个。
    // 返回值只有完整声明，绝不会执行用户请求；要完成任务，还必须调用发现后的具体工具。
    result.description =
        "Discover the full definitions of tools needed for the current user task before using "
        "them. Select only needed exact names from the names enum, at most 8 per call. This "
        "read-only lookup returns tool metadata only; it never executes the requested actions. "
        "After discovery, call the discovered tool to perform the user task.";
    result.parametersJson = schema.dump();
    return result;
}

MaiResult<std::vector<MaiToolSpec>> MaiLlamaToolCatalog::resolveNames(
    const std::vector<std::string>& names) const {
    if (mError.hasError()) return mError;
    if (names.empty() || names.size() > kMaximumDiscoveryNames)
        return {MaiErrorCode::InvalidInput, "Discover 1 to 8 distinct tool names per call."};

    std::unordered_set<std::string> requestedNames;
    std::vector<MaiToolSpec> result;
    result.reserve(names.size());
    for (const auto& name : names) {
        if (!requestedNames.insert(name).second)
            return {MaiErrorCode::InvalidInput, "Duplicate tool names are not allowed."};
        const auto specification =
            std::find_if(mSpecifications.begin(), mSpecifications.end(),
                         [&name](const MaiToolSpec& entry) { return entry.name == name; });
        if (specification == mSpecifications.end())
            return {MaiErrorCode::InvalidInput, "Unknown tool name in discovery request."};
        result.push_back(*specification);
    }
    return result;
}

MaiResult<std::vector<MaiToolSpec>> MaiLlamaToolCatalog::resolve(
    const std::string& argumentsJson) const {
    if (mError.hasError()) return mError;

    // 默认对象解析会静默覆盖同名键；独立计数根对象的键，防止重复 names 被当成合法单字段。
    std::size_t rootFieldCount = 0;
    const auto countRootFields = [&rootFieldCount](int depth, nlohmann::json::parse_event_t event,
                                                   nlohmann::json&) {
        if (depth == 1 && event == nlohmann::json::parse_event_t::key) ++rootFieldCount;
        return true;
    };
    const auto arguments = nlohmann::json::parse(argumentsJson, countRootFields, false);
    if (!arguments.is_object() || rootFieldCount != 1 || arguments.size() != 1 ||
        !arguments.contains("names") || !arguments["names"].is_array())
        return {MaiErrorCode::InvalidInput,
                "Tool discovery requires an object containing only a names array."};

    const auto& requestedNames = arguments["names"];
    if (requestedNames.empty() || requestedNames.size() > kMaximumDiscoveryNames)
        return {MaiErrorCode::InvalidInput, "Discover 1 to 8 distinct tool names per call."};
    std::vector<std::string> names;
    names.reserve(requestedNames.size());
    for (const auto& name : requestedNames) {
        if (!name.is_string())
            return {MaiErrorCode::InvalidInput, "Every discovered tool name must be a string."};
        names.push_back(name.get<std::string>());
    }
    return resolveNames(names);
}

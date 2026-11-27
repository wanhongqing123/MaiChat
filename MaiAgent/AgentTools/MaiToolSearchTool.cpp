// 文件作用：校验 tool_search 的有界参数，并复用普通目录搜索/加载核心。
#include "MaiToolSearchTool.h"

#include <cstdint>
#include <limits>
#include <utility>

#include <json.hpp>

namespace {
class MaiToolSearchTool final : public MaiTool {
public:
    explicit MaiToolSearchTool(std::shared_ptr<MaiModelToolCatalog> catalog)
        : mCatalog(std::move(catalog)) {}
    std::string name() const override {
        return "tool_search";
    }
    // 查询只返回公开名称/描述；选择至多 5 项后，下次请求才追加完整声明。
    // 不执行所选工具，也不改变权限；来源历史和输出补读工具已自举，无需先搜索。
    std::string description() const override {
        return "Find tools by name or description using query/offset/limit (metadata only). "
               "Use select with up to 5 exact names to load their schemas for the NEXT model "
               "request before calling them. Empty query browses names; limit is at most 10. "
               "Selection does not execute tools or grant permissions.";
    }
    // 未知字段拒绝；query 最多 4096 字节、offset 非负、limit 1..10、select 至多 5 项。
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"query":{"type":"string","maxLength":4096},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":10},"select":{"type":"array","maxItems":5,"uniqueItems":true,"items":{"type":"string","minLength":1,"maxLength":256}}},"additionalProperties":false})";
    }
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Tool discovery canceled.");
        if (!mCatalog)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "Tool discovery is unavailable.");
        const auto arguments = nlohmann::json::parse(argumentsJson, nullptr, false);
        const auto invalid = [] {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "Expected {query?,offset?,limit:1..10?,select:[up to 5 names]?}.");
        };
        if (!arguments.is_object()) return invalid();
        for (const auto& item : arguments.items())
            if (item.key() != "query" && item.key() != "offset" && item.key() != "limit" &&
                item.key() != "select")
                return invalid();
        MaiToolCatalogQuery query;
        if (arguments.contains("query")) {
            if (!arguments["query"].is_string()) return invalid();
            query.query = arguments["query"].get<std::string>();
        }
        for (const auto* field : {"offset", "limit"}) {
            if (!arguments.contains(field)) continue;
            const auto& number = arguments[field];
            if (!number.is_number_integer() ||
                (!number.is_number_unsigned() && number.get<std::int64_t>() < 0) ||
                number.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max())
                return invalid();
            if (std::string(field) == "offset")
                query.offset = number.get<std::size_t>();
            else
                query.limit = number.get<std::size_t>();
        }
        if (arguments.contains("select")) {
            if (!arguments["select"].is_array() || arguments["select"].size() > 5) return invalid();
            for (const auto& selected : arguments["select"]) {
                if (!selected.is_string()) return invalid();
                query.select.push_back(selected.get<std::string>());
            }
        }
        MaiTurnContext owner{context.sessionId, context.turnId, context.model, {}, context.root};
        const auto result = mCatalog->search(owner, query);
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        nlohmann::json output{{"type", "tool_metadata"},
                              {"total", result.value().total},
                              {"next_offset", result.value().nextOffset},
                              {"has_more", result.value().nextOffset < result.value().total},
                              {"selected", result.value().selected},
                              {"tools", nlohmann::json::array()}};
        for (const auto& metadata : result.value().tools)
            output["tools"].push_back({{"name", metadata.name},
                                       {"description", metadata.description},
                                       {"loaded", metadata.loaded}});
        const auto bytes = output.dump();
        if (bytes.size() > 64 * 1024)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "Tool metadata exceeds its byte limit.");
        return MaiToolResult::success(bytes);
    }

private:
    std::shared_ptr<MaiModelToolCatalog> mCatalog;
};
}  // namespace

std::unique_ptr<MaiTool> makeMaiToolSearchTool(std::shared_ptr<MaiModelToolCatalog> catalog) {
    return std::make_unique<MaiToolSearchTool>(std::move(catalog));
}

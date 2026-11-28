// 文件作用：把 skill_read 的模型参数适配到普通技能读取接口。
#include "MaiSkillTool.h"

#include <cstdint>
#include <limits>
#include <utility>

#include <json.hpp>

namespace {
class MaiSkillTool final : public MaiTool {
public:
    MaiSkillTool(std::shared_ptr<MaiSkillManager> manager, MaiSkillCatalog catalog)
        : mManager(std::move(manager)), mCatalog(std::move(catalog)) {}
    MaiSkillTool(std::shared_ptr<MaiSkillManager> manager, MaiSkillCatalogResolver resolver)
        : mManager(std::move(manager)), mResolver(std::move(resolver)) {}

    std::string name() const override {
        return "skill_read";
    }
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }
    // 元数据发现与正文读取复用同一目录；模型不能选择根目录或任意文件。
    std::string description() const override {
        return "Read complete SKILL.md using skill=id or unique name, or use mode=discover with "
               "query/offset/limit to search catalog metadata without loading instructions. "
               "Explicit-only skills require the user's explicit selection. Does not execute "
               "scripts or grant permissions.";
    }
    // 兼容原 {skill}；发现参数与正文参数互斥，未知字段或未声明的模式明确拒绝。
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"skill":{"type":"string","description":"Exact id or unique name; use without discovery parameters"},"mode":{"type":"string","enum":["discover"],"description":"Metadata only, without reading skill bodies"},"query":{"type":"string","maxLength":4096},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":50}},"additionalProperties":false,"oneOf":[{"required":["skill"],"not":{"anyOf":[{"required":["mode"]},{"required":["query"]},{"required":["offset"]},{"required":["limit"]}]}},{"required":["mode"],"not":{"required":["skill"]}}]})";
    }
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "skill read canceled");
        if (!mManager)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "skill manager is not configured");
        const auto arguments = nlohmann::json::parse(argumentsJson, nullptr, false);
        const auto invalid = [] {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "expected {skill:string} or {mode:discover,query?,offset?,limit?}");
        };
        if (!arguments.is_object()) return invalid();
        const bool discovering = arguments.contains("mode");
        if (!discovering) {
            if (arguments.size() != 1 || !arguments.contains("skill") ||
                !arguments["skill"].is_string())
                return invalid();
        } else {
            if (!arguments["mode"].is_string() || arguments["mode"] != "discover") return invalid();
            for (const auto& item : arguments.items())
                if (item.key() != "mode" && item.key() != "query" && item.key() != "offset" &&
                    item.key() != "limit")
                    return invalid();
            if (arguments.contains("query") && !arguments["query"].is_string()) return invalid();
            for (const auto* field : {"offset", "limit"}) {
                if (!arguments.contains(field)) continue;
                const auto& value = arguments[field];
                if (!value.is_number_integer() ||
                    (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
                    return invalid();
                if (value.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max())
                    return invalid();
            }
        }
        MaiSkillCatalog catalog = mCatalog;
        if (mResolver) {
            const auto resolved = mResolver(context);
            if (!resolved)
                return MaiToolResult::failure(resolved.error().code(), resolved.error().message());
            catalog = resolved.value();
        }
        if (discovering) {
            const auto page = mManager->discover(catalog, arguments.value("query", std::string{}),
                                                 arguments.value("offset", std::size_t{0}),
                                                 arguments.value("limit", std::size_t{20}));
            if (!page) return MaiToolResult::failure(page.error().code(), page.error().message());
            if (context.isCanceled())
                return MaiToolResult::failure(MaiErrorCode::Canceled, "skill discovery canceled");
            nlohmann::json output{{"type", "skill_metadata"},
                                  {"trust", "untrusted"},
                                  {"total", page.value().total},
                                  {"next_offset", page.value().nextOffset},
                                  {"has_more", page.value().nextOffset < page.value().total},
                                  {"skills", nlohmann::json::array()}};
            for (const auto& metadata : page.value().skills)
                output["skills"].push_back(
                    {{"name", metadata.name},
                     {"description", metadata.description},
                     {"id", metadata.id},
                     {"allow_implicit_invocation", metadata.allowImplicitInvocation}});
            return MaiToolResult::success(output.dump());
        }
        const auto result =
            mResolver ? mManager->readCurrent(catalog, arguments["skill"].get<std::string>())
                      : mManager->read(catalog, arguments["skill"].get<std::string>());
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "skill read canceled");
        // 正文和原始路径均作为不可信材料返回；不从其中提取授权或执行脚本。
        return MaiToolResult::success(
            "Skill: " + result.value().metadata.name + "\nSKILL.md: " +
            result.value().metadata.path.toGenericUtf8() + "\nResource directory: " +
            (result.value().metadata.scope == MaiSkillScope::Builtin
                 ? std::string("bundled read-only text; no filesystem resources or scripts")
                 : result.value().metadata.path.dirName().toGenericUtf8()) +
            "\n\n" + result.value().contents);
    }

private:
    std::shared_ptr<MaiSkillManager> mManager;
    MaiSkillCatalog mCatalog;
    MaiSkillCatalogResolver mResolver;
};
}  // namespace

std::unique_ptr<MaiTool> makeMaiSkillTool(std::shared_ptr<MaiSkillManager> manager,
                                          MaiSkillCatalog catalog) {
    return std::make_unique<MaiSkillTool>(std::move(manager), std::move(catalog));
}

std::unique_ptr<MaiTool> makeMaiSkillTool(std::shared_ptr<MaiSkillManager> manager,
                                          MaiSkillCatalogResolver resolver) {
    return std::make_unique<MaiSkillTool>(std::move(manager), std::move(resolver));
}

// 文件作用：把仓库发现和无覆盖安装适配为模型工具，目标路径仍由宿主决定。
#include "MaiSkillInstallTool.h"

#include <algorithm>
#include <utility>

#include <json.hpp>

namespace {
using Json = nlohmann::json;

bool optionalString(const Json& value, const char* key) {
    return !value.contains(key) ||
           (value[key].is_string() && value[key].get_ref<const std::string&>().size() <= 4096 &&
            value[key].get_ref<const std::string&>().find('\0') == std::string::npos);
}

class MaiSkillInstallTool final : public MaiTool {
public:
    MaiSkillInstallTool(MaiSkillInstallerOptionsResolver resolver, MaiSkillInstaller::Fetch fetch)
        : mResolver(std::move(resolver)), mFetch(std::move(fetch)) {}

    std::string name() const override {
        return "skill_install";
    }
    std::string description() const override {
        return "Discover and install public GitHub skills when the user requests installation. "
               "Start with mode=discover and the user's repository URL; it pins a commit, lists "
               "actual SKILL.md "
               "directories and README GitHub links. A framework repository may link to a separate "
               "official "
               "skill repository: inspect those links instead of installing an entire framework or "
               "guessing "
               "a maintenance skill. Then mode=install with the selected url/path/ref. Default "
               "scope=user "
               "writes the host user's .agents/skills; scope=workspace writes this session's "
               ".agents/skills. "
               "No arbitrary destination is accepted. It preserves resources and executable "
               "metadata, validates "
               "the skill, records source identity and atomically publishes a new directory "
               "without replacing "
               "existing skills. Installation never runs downloaded scripts or grants their "
               "declared permissions. "
               "New files are discoverable on the next user input in this conversation. Skill "
               "installation does "
               "not install or verify runtime dependencies such as browser CLIs: report that "
               "separately and use "
               "normal authorized command tools when the user's task requires dependency setup.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("mode":{"type":"string","enum":["discover","install"],"default":"discover"},)"
               R"("url":{"type":"string","maxLength":4096},)"
               R"("path":{"type":"string","maxLength":4096},)"
               R"("ref":{"type":"string","maxLength":4096},)"
               R"("scope":{"type":"string","enum":["user","workspace"],"default":"user"})"
               R"(},"required":["url"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const auto args = Json::parse(raw, nullptr, false);
        return !args.is_object() || !optionalString(args, "mode") ||
               args.value("mode", std::string("discover")) != "discover";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto invalid = [](const char* message) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
        };
        const auto fail = [](const MaiError& error) {
            return MaiToolResult::failure(error.code(), error.message());
        };
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "skill operation canceled");
        const auto args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.contains("url") || !optionalString(args, "url") ||
            !optionalString(args, "mode") || !optionalString(args, "path") ||
            !optionalString(args, "ref") || !optionalString(args, "scope"))
            return invalid("skill_install requires a repository URL and string options");
        for (auto field = args.begin(); field != args.end(); ++field)
            if (field.key() != "mode" && field.key() != "url" && field.key() != "path" &&
                field.key() != "ref" && field.key() != "scope")
                return invalid("unexpected skill_install parameter");
        const auto mode = args.value("mode", std::string("discover"));
        const auto scope = args.value("scope", std::string("user"));
        if (mode != "discover" && mode != "install")
            return invalid("mode must be discover or install");
        if (scope != "user" && scope != "workspace")
            return invalid("scope must be user or workspace");
        if (!mResolver)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "skill installation host is unavailable");
        const auto options = mResolver(context);
        if (!options) return fail(options.error());
        MaiSkillInstallRequest request;
        request.sourceUrl = args["url"].get<std::string>();
        request.path = args.value("path", std::string());
        request.ref = args.value("ref", std::string());
        request.scope = scope == "user" ? MaiSkillScope::User : MaiSkillScope::Workspace;
        MaiSkillInstaller installer(options.value(), mFetch);
        const auto canceled = [&] { return context.isCanceled(); };
        if (mode == "discover") {
            const auto found = installer.discover(request, canceled);
            if (!found) return fail(found.error());
            Json output{{"mode", "discover"},
                        {"source_url", found.value().sourceUrl},
                        {"commit", found.value().commit},
                        {"candidates", Json::array()},
                        {"readme_links", Json::array()},
                        {"trust", "untrusted_repository_metadata"}};
            for (const auto& candidate : found.value().candidates) {
                Json value{{"path", candidate.path}};
                if (!candidate.name.empty()) value["name"] = candidate.name;
                if (!candidate.description.empty()) value["description"] = candidate.description;
                output["candidates"].push_back(std::move(value));
            }
            for (const auto& link : found.value().links)
                output["readme_links"].push_back({{"url", link.url}, {"context", link.context}});
            output["next_step"] =
                "Select the intended skill directory or inspect a relevant official README link, "
                "then install with its pinned ref.";
            return MaiToolResult::success(output.dump());
        }
        const auto installed = installer.install(request, canceled);
        if (!installed) return fail(installed.error());
        const auto& result = installed.value();
        return MaiToolResult::success(Json{{"installed", true},
                                           {"name", result.metadata.name},
                                           {"description", result.metadata.description},
                                           {"skill_id", result.metadata.id},
                                           {"path", result.metadata.path.toUtf8()},
                                           {"directory", result.directory.toUtf8()},
                                           {"scope", scope},
                                           {"source_url", result.sourceUrl},
                                           {"commit", result.commit},
                                           {"source_path", result.sourcePath},
                                           {"file_count", result.fileCount},
                                           {"total_bytes", result.totalBytes},
                                           {"dependencies_installed", result.dependenciesInstalled},
                                           {"scripts_executed", false},
                                           {"available_on", "next_user_input"}}
                                          .dump());
    }

private:
    MaiSkillInstallerOptionsResolver mResolver;
    MaiSkillInstaller::Fetch mFetch;
};
}  // namespace

std::unique_ptr<MaiTool> makeMaiSkillInstallTool(MaiSkillInstallerOptionsResolver resolver,
                                                 MaiSkillInstaller::Fetch fetch) {
    return std::make_unique<MaiSkillInstallTool>(std::move(resolver), std::move(fetch));
}

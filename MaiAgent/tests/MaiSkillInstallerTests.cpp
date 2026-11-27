// 文件作用：以确定性仓库响应验证技能安装边界、资源及来源记录。
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiSkillInstaller.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {
const std::string commit(40, 'a');
const std::string skill = "---\nname: real-name\ndescription: Browser work\n---\nUse resources.\n";
MaiFilePath child(const MaiFilePath& root, const std::string& path) {
    return root.append(MaiFilePath::fromUtf8(path));
}
struct Repository {
    std::map<std::string, std::string> responses;
    std::vector<std::string> requested;
    Repository() {
        responses["https://api.github.com/repos/acme/tools/commits/HEAD"] =
            "{\"sha\":\"" + commit + "\",\"commit\":{\"tree\":{\"sha\":\"" + commit + "\"}}}";
        responses["https://api.github.com/repos/acme/tools/commits/" + commit] =
            responses.begin()->second;
        setTree(
            "{\"path\":\"skills/browser/"
            "SKILL.md\",\"type\":\"blob\",\"mode\":\"100644\",\"size\":64},"
            "{\"path\":\"skills/browser/scripts/"
            "run.sh\",\"type\":\"blob\",\"mode\":\"100755\",\"size\":12},"
            "{\"path\":\"README.md\",\"type\":\"blob\",\"mode\":\"100644\",\"size\":100},"
            "{\"path\":\"unrelated.txt\",\"type\":\"blob\",\"mode\":\"100644\",\"size\":50}");
        responses[raw("skills/browser/SKILL.md")] = skill;
        responses[raw("skills/browser/scripts/run.sh")] = "#!/bin/sh\n";
        responses[raw("README.md")] =
            "Browser CLI skill: "
            "[official](https://github.com/acme/browser-cli/tree/main/skills/browser).\n";
    }
    std::string raw(const std::string& path) const {
        return "https://raw.githubusercontent.com/acme/tools/" + commit + "/" + path;
    }
    void setTree(const std::string& entries, bool truncated = false) {
        responses["https://api.github.com/repos/acme/tools/git/trees/" + commit + "?recursive=1"] =
            "{\"truncated\":" + std::string(truncated ? "true" : "false") + ",\"tree\":[" +
            entries + "]}";
    }
    MaiSkillInstaller::Fetch fetch() {
        return [this](const std::string& url, std::size_t maxBytes,
                      const std::function<bool()>& canceled) -> MaiResult<std::string> {
            requested.push_back(url);
            if (canceled && canceled()) return {MaiErrorCode::Canceled, "canceled"};
            const auto found = responses.find(url);
            if (found == responses.end())
                return {MaiErrorCode::NotFound, "fixture URL not found: " + url};
            if (found->second.size() > maxBytes)
                return {MaiErrorCode::InvalidInput, "response exceeds budget"};
            return found->second;
        };
    }
};
void testInstallAndDiscovery(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "home");
    options.workspaceDirectory = child(root, "workspace");
    MaiSkillInstaller installer(options, repository.fetch());
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools";
    auto discovery = installer.discover(request);
    CHECK(discovery.isOk());
    if (!discovery) std::printf("%s\n", discovery.error().message().c_str());
    if (discovery) {
        CHECK(discovery.value().commit == commit);
        CHECK(discovery.value().candidates.size() == 1);
        CHECK(discovery.value().candidates[0].path == "skills/browser");
        CHECK(discovery.value().links.size() == 1);
        CHECK(discovery.value().links[0].context.find("Browser CLI") != std::string::npos);
    }
    CHECK(!MaiFileSystem::exists(options.homeDirectory));
    repository.requested.clear();
    request.ref = commit;
    request.path = "skills/browser";
    auto installed = installer.install(request);
    CHECK(installed.isOk());
    if (!installed) std::printf("%s\n", installed.error().message().c_str());
    const auto target = child(options.homeDirectory, ".agents/skills/browser");
    if (installed) {
        CHECK(installed.value().metadata.name == "real-name");
        CHECK(installed.value().directory == MaiFileSystem::resolve(target));
        CHECK(installed.value().fileCount == 2);
        CHECK(!installed.value().dependenciesInstalled);
        CHECK(installed.value().commit == commit);
        std::string contents;
        CHECK(!MaiFileSystem::readFile(child(target, "scripts/run.sh"), contents));
        CHECK(contents == "#!/bin/sh\n");
        CHECK(!MaiFileSystem::readFile(child(target, ".mai-skill-source.json"), contents));
        CHECK(contents.find(commit) != std::string::npos);
        CHECK(contents.find("sha256") != std::string::npos);
        CHECK(contents.find("skills/browser") != std::string::npos);
    }
    for (const auto& url : repository.requested)
        CHECK(url.find("unrelated.txt") == std::string::npos);
    CHECK(!installer.install(request));
    std::string contents;
    CHECK(!MaiFileSystem::readFile(child(target, "SKILL.md"), contents));
    CHECK(contents == skill);
    request.scope = MaiSkillScope::Workspace;
    CHECK(installer.install(request).isOk());
}
void testInvalidSources(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "invalid");
    MaiSkillInstaller installer(options, repository.fetch());
    for (const std::string& url :
         {"http://github.com/acme/tools", "https://evil.example/acme/tools",
          "https://user@github.com/acme/tools", "https://github.com/acme/tools?token=x",
          "https://github.com/acme/tools#fragment", "https://github.com/acme/../tools",
          "https://github.com/acme/tools/tree/main/../secret"}) {
        MaiSkillInstallRequest request;
        request.sourceUrl = url;
        CHECK(!installer.discover(request));
    }
    CHECK(repository.requested.empty());
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools";
    request.path = "../escape";
    CHECK(!installer.install(request));
    request.path = "skills/browser";
    CHECK(!installer.install(request, [] { return true; }));
    CHECK(!MaiFileSystem::exists(options.homeDirectory));
}
void testAmbiguityAndMaliciousTree(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "malicious");
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools";
    repository.setTree(
        "{\"path\":\"one/SKILL.md\",\"type\":\"blob\",\"mode\":\"100644\"},"
        "{\"path\":\"two/SKILL.md\",\"type\":\"blob\",\"mode\":\"100644\"}");
    MaiSkillInstaller installer(options, repository.fetch());
    CHECK(!installer.install(request));
    request.path = "one";
    for (const std::string& entry :
         {"{\"path\":\"one/link\",\"type\":\"blob\",\"mode\":\"120000\"}",
          "{\"path\":\"one/submodule\",\"type\":\"commit\",\"mode\":\"160000\"}",
          "{\"path\":\"one/../escape\",\"type\":\"blob\",\"mode\":\"100644\"}",
          "{\"path\":\"one/CON.txt\",\"type\":\"blob\",\"mode\":\"100644\"}",
          "{\"path\":\"one/a\",\"type\":\"blob\",\"mode\":\"100644\"},"
          "{\"path\":\"one/A\",\"type\":\"blob\",\"mode\":\"100644\"}"}) {
        repository.setTree("{\"path\":\"one/SKILL.md\",\"type\":\"blob\",\"mode\":\"100644\"}," +
                           entry);
        CHECK(!installer.install(request));
    }
    CHECK(!MaiFileSystem::exists(child(options.homeDirectory, ".agents/skills/one")));
    repository.setTree("", true);
    CHECK(!installer.discover(request));
}
void testLimitsAndCleanup(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "limits");
    options.maxBodyBytes = 16;
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools";
    request.path = "skills/browser";
    CHECK(!MaiSkillInstaller(options, repository.fetch()).install(request));
    options.maxBodyBytes = 256 * 1024;
    options.maxFiles = 1;
    CHECK(!MaiSkillInstaller(options, repository.fetch()).install(request));
    options.maxFiles = 512;
    options.maxTotalBytes = 8;
    CHECK(!MaiSkillInstaller(options, repository.fetch()).install(request));
    options.maxTotalBytes = 16 * 1024 * 1024;
    repository.responses[repository.raw("skills/browser/SKILL.md")] = "---\nname: bad\n---\n";
    CHECK(!MaiSkillInstaller(options, repository.fetch()).install(request));
    repository.responses[repository.raw("skills/browser/SKILL.md")] = skill + '\xFF';
    CHECK(!MaiSkillInstaller(options, repository.fetch()).install(request));
    CHECK(!MaiFileSystem::exists(child(options.homeDirectory, ".agents/skills/browser")));
}
void testUrlSelectionAndRootSkill(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "url-selection");
    MaiSkillInstaller installer(options, repository.fetch());
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools/blob/main/skills/browser/SKILL.md";
    request.ref = commit;
    CHECK(installer.install(request).isOk());
    options.homeDirectory = child(root, "tree-selection");
    request.sourceUrl = "https://github.com/acme/tools/tree/main/skills/browser";
    CHECK(MaiSkillInstaller(options, repository.fetch()).install(request).isOk());
    options.homeDirectory = child(root, "root-skill");
    request.sourceUrl = "https://github.com/acme/tools";
    request.path = ".";
    repository.setTree("{\"path\":\"SKILL.md\",\"type\":\"blob\",\"mode\":\"100644\"}");
    repository.responses[repository.raw("SKILL.md")] = skill;
    auto installed = MaiSkillInstaller(options, repository.fetch()).install(request);
    CHECK(installed.isOk());
    if (installed) CHECK(installed.value().directory.baseName().toUtf8() == "tools");
}
void testStagingCancellationAndIsolation(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "cancel-staging");
    const auto other = child(options.homeDirectory, ".agents/.skill-stage-unrelated");
    CHECK(!MaiFileSystem::createDirectories(other));
    CHECK(!MaiFileSystem::writeFile(child(other, "keep.txt"), "unrelated"));
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools";
    request.path = "skills/browser";
    bool sawStagedSkill = false;
    const auto cancellation = [&] {
        MaiFileSystem::walk(child(options.homeDirectory, ".agents"),
                            [&](const MaiFileEntry& entry) {
                                if (entry.nameUtf8 == "SKILL.md") sawStagedSkill = true;
                                return MaiWalkAction::Continue;
                            });
        return sawStagedSkill;
    };
    const auto result =
        MaiSkillInstaller(options, repository.fetch()).install(request, cancellation);
    CHECK(!result);
    CHECK(result.error().code() == MaiErrorCode::Canceled);
    CHECK(sawStagedSkill);
    CHECK(!MaiFileSystem::exists(child(options.homeDirectory, ".agents/skills/browser")));
    std::size_t stages = 0;
    MaiFileSystem::walk(child(options.homeDirectory, ".agents"), [&](const MaiFileEntry& entry) {
        if (entry.isDirectory && entry.nameUtf8.find(".skill-stage-") == 0) ++stages;
        return MaiWalkAction::Continue;
    });
    CHECK(stages == 1);
    std::string contents;
    CHECK(!MaiFileSystem::readFile(child(other, "keep.txt"), contents));
    CHECK(contents == "unrelated");
}
void testHostFrontmatterBudget(const MaiFilePath& root) {
    Repository repository;
    MaiSkillInstallRequest request;
    request.sourceUrl = "https://github.com/acme/tools";
    request.path = "skills/browser";
    MaiSkillInstallerOptions options;
    options.homeDirectory = child(root, "small-frontmatter");
    options.maxFrontmatterBytes = 8 * 1024;
    repository.responses[repository.raw("skills/browser/SKILL.md")] =
        "---\nname: constrained\ndescription: Budget test\nmetadata: " +
        std::string(9 * 1024, 'x') + "\n---\nBody\n";
    CHECK(!MaiSkillInstaller(options, repository.fetch()).install(request));
    CHECK(!MaiFileSystem::exists(child(options.homeDirectory, ".agents/skills/browser")));
    options.homeDirectory = child(root, "large-frontmatter");
    options.maxFrontmatterBytes = 128 * 1024;
    repository.responses[repository.raw("skills/browser/SKILL.md")] =
        "---\nname: expanded\ndescription: Budget test\nmetadata: " + std::string(70 * 1024, 'x') +
        "\n---\nBody\n";
    const auto installed = MaiSkillInstaller(options, repository.fetch()).install(request);
    CHECK(installed.isOk());
    if (installed) CHECK(installed.value().metadata.name == "expanded");
}
}  // namespace

int main() {
    const auto root =
        child(MaiFileSystem::temporaryDirectory(), MaiIdGenerator::generate("skill-install-test_"));
    testInstallAndDiscovery(root);
    testInvalidSources(root);
    testAmbiguityAndMaliciousTree(root);
    testLimitsAndCleanup(root);
    testUrlSelectionAndRootSkill(root);
    testStagingCancellationAndIsolation(root);
    testHostFrontmatterBudget(root);
    MaiFileSystem::removeRecursively(root);
    std::printf("MaiSkillInstallerTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// 文件作用：验证技能发现、显式选择、延迟读取与模型入口的共同契约。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "MaiFileSystem.h"
#include "MaiModelClient.h"
#include "MaiSkillManager.h"
#include "MaiSkillTool.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {
MaiFilePath child(const MaiFilePath& path, const std::string& name) {
    return path.append(MaiFilePath::fromUtf8(name));
}

void writeSkill(const MaiFilePath& directory, const std::string& text) {
    CHECK(!MaiFileSystem::createDirectories(directory));
    CHECK(!MaiFileSystem::writeFile(child(directory, "SKILL.md"), text));
}

void testDiscoveryAndRead(const MaiFilePath& root) {
    auto manager = std::make_shared<MaiSkillManager>();
    const auto workspace = child(root, "repo");
    const auto nested = child(workspace, "nested");
    const auto home = child(root, "home");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    CHECK(!MaiFileSystem::createDirectories(nested));
    writeSkill(child(workspace, ".agents/skills/first"),
               "---\nname: first\ndescription: 'Use for tests'\n---\nFIRST BODY\n");
    writeSkill(
        child(nested, ".agents/skills/second"),
        "---\nname: second\ndescription: >-\n  Second skill\n  across lines\n---\nSECOND BODY\n");
    writeSkill(child(home, ".agents/skills/first"),
               "---\nname: first\ndescription: Personal copy\n---\nPERSONAL BODY\n");
    writeSkill(child(root, ".agents/skills/escape"),
               "---\nname: escape\ndescription: Above repository\n---\nOUTSIDE\n");
    MaiSkillManagerOptions options;
    options.workspaceDirectory = nested;
    options.homeDirectory = home;
    auto discovered = manager->snapshot(options);
    CHECK(discovered.isOk());
    if (!discovered) return;
    const auto catalog = discovered.value();
    CHECK(catalog.skills().size() == 3);
    CHECK(catalog.errors().empty());
    CHECK(!manager->find(catalog, "first"));
    CHECK(!manager->find(catalog, "escape"));
    auto second = manager->read(catalog, "second");
    CHECK(second.isOk());
    if (second) {
        CHECK(second.value().metadata.description == "Second skill across lines");
        CHECK(second.value().contents.find("SECOND BODY") != std::string::npos);
    }
    CHECK(MaiSkillManager::formatCatalog(catalog).find("SECOND BODY") == std::string::npos);
    const auto selected = MaiSkillManager::explicitMentions(catalog, "$first $second $second");
    CHECK(selected.size() == 1);
    if (!selected.empty()) CHECK(selected[0].name == "second");
    CHECK(MaiSkillManager::explicitMentions(catalog, "$second-extra").empty());
    CHECK(MaiSkillManager::explicitMentions(catalog, "$second.").size() == 1);
    const auto explicitSecond = manager->explicitContext(catalog, "Use $second.");
    CHECK(explicitSecond.isOk());
    if (explicitSecond) {
        CHECK(explicitSecond.value().find("SECOND BODY") != std::string::npos);
        CHECK(explicitSecond.value().find("untrusted_user_context") != std::string::npos);
        CHECK(explicitSecond.value().find("does_not_grant_permissions") != std::string::npos);
    }
    const auto ambiguous = manager->explicitContext(catalog, "Use $first");
    CHECK(ambiguous.isOk());
    if (ambiguous) CHECK(ambiguous.value().empty());
    CHECK(!manager->explicitContext(catalog, "$second", 8));
    CHECK(MaiSkillManager::explicitMentions(catalog, "[$second](/outside/SKILL.md)").empty());
    const auto exactMention = MaiSkillManager::explicitMentions(
        catalog, "[$first](skill://" + catalog.skills()[0].id + ")");
    CHECK(exactMention.size() == 1);
    CHECK(!manager->read(catalog, child(root, "secret.txt").toUtf8()));
    auto tool = makeMaiSkillTool(manager, catalog);
    MaiToolContext context;
    const auto toolRead = tool->execute("{\"skill\":\"second\"}", context);
    CHECK(!toolRead.hasError());
    CHECK(toolRead.output().find("SECOND BODY") != std::string::npos);
    CHECK(tool->execute("{\"skill\":\"../../secret.txt\"}", context).hasError());
    CHECK(tool->execute("{\"skill\":1}", context).hasError());
    CHECK(tool->execute("{", context).hasError());
    CHECK(!tool->requiresWorkingDirectory("{}"));
    MaiSkillManager anotherManager;
    CHECK(!anotherManager.read(catalog, "second"));
    writeSkill(child(nested, ".agents/skills/second"),
               "---\nname: second\ndescription: Updated description\n---\nNEW BODY\n");
    CHECK(!manager->read(catalog, "second"));
    const auto currentContents = manager->readCurrent(catalog, "second");
    CHECK(currentContents.isOk());
    if (currentContents)
        CHECK(currentContents.value().contents.find("NEW BODY") != std::string::npos);
    CHECK(catalog.skills()[1].description == "Second skill across lines");
    const auto updated = manager->snapshot(options);
    CHECK(updated.isOk());
    if (updated) {
        auto contents = manager->read(updated.value(), "second");
        CHECK(contents.isOk());
        if (contents) CHECK(contents.value().contents.find("NEW BODY") != std::string::npos);
    }
    auto dynamicTool = makeMaiSkillTool(
        manager, MaiSkillCatalogResolver{
                     [catalog](const MaiToolContext& toolContext) -> MaiResult<MaiSkillCatalog> {
                         if (toolContext.sessionId != "owned" || toolContext.root != "owned-root")
                             return {MaiErrorCode::NotFound,
                                     "The context does not own this skill catalog."};
                         return catalog;
                     }});
    CHECK(dynamicTool->execute("{\"skill\":\"second\"}", context).hasError());
    context.sessionId = "owned";
    context.root = "owned-root";
    const auto freshToolResult = dynamicTool->execute("{\"skill\":\"second\"}", context);
    CHECK(!freshToolResult.hasError());
    CHECK(freshToolResult.output().find("NEW BODY") != std::string::npos);
}

void testMetadataCompatibilityAndBudgets(const MaiFilePath& root) {
    auto manager = std::make_shared<MaiSkillManager>();
    const auto workspace = child(root, "metadata");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    const auto skills = child(workspace, ".agents/skills");
    writeSkill(child(skills, "unicode-\u4e2d\u6587"),
               "---\nname: unicode\ndescription: \"Use for \\u4e2d\\u6587\"\n"
               "metadata:\n  short-description: Summary\n  tags:\n    - data\n    - files\n"
               "allowed-tools: [read, exec]\n---\nUNICODE BODY\n");
    writeSkill(child(skills, "fallback"),
               "---\ndescription: Build for AWS: ECS\n  and keep a multiline "
               "continuation\n---\nFALLBACK\n");
    writeSkill(
        child(skills, "single"),
        "---\nname: single\ndescription: 'It''s a quoted # description' # comment\n---\nONE\n");
    writeSkill(child(skills, "literal"),
               "---\nname: literal\ndescription: |\n  First line\n  Next line\n---\nLITERAL\n");
    writeSkill(child(skills, "missing"), "---\nname: missing\n---\nMISSING\n");
    writeSkill(child(skills, "duplicate-key"),
               "---\nname: duplicate\ndescription: first\ndescription: second\n---\nBAD\n");
    writeSkill(child(skills, "container"),
               "---\nname: container\ndescription: [not, a, string]\n---\nBAD\n");
    writeSkill(child(skills, "unclosed"), "---\nname: unclosed\ndescription: no end\n");
    writeSkill(child(workspace, ".agent/skills/unsupported-alias"),
               "---\nname: unsupported-alias\ndescription: Wrong singular root\n---\nBAD\n");
    writeSkill(child(skills, ".hidden/hidden"),
               "---\nname: hidden\ndescription: Hidden directory\n---\nBAD\n");
    MaiSkillManagerOptions options;
    options.workspaceDirectory = workspace;
    const auto discovered = manager->snapshot(options);
    CHECK(discovered.isOk());
    if (!discovered) return;
    CHECK(discovered.value().skills().size() == 4);
    CHECK(discovered.value().errors().size() == 4);
    const auto unicode = manager->read(discovered.value(), "unicode");
    CHECK(unicode.isOk());
    if (unicode) CHECK(unicode.value().metadata.description == "Use for \u4e2d\u6587");
    const auto fallback = manager->find(discovered.value(), "fallback");
    CHECK(fallback.isOk());
    if (fallback)
        CHECK(fallback.value().description ==
              "Build for AWS: ECS and keep a multiline continuation");
    const auto single = manager->find(discovered.value(), "single");
    CHECK(single.isOk());
    if (single) CHECK(single.value().description == "It's a quoted # description");
    CHECK(!manager->find(discovered.value(), "unsupported-alias"));
    CHECK(!manager->find(discovered.value(), "hidden"));
    const auto formatted = MaiSkillManager::formatCatalog(discovered.value());
    CHECK(formatted.find("catalog may be incomplete") != std::string::npos);
    CHECK(formatted.find("allowed-tools") == std::string::npos);
    options.maxBodyBytes = 16;
    const auto smallBudgetCatalog = manager->snapshot(options);
    CHECK(smallBudgetCatalog.isOk());
    if (smallBudgetCatalog) CHECK(!manager->read(smallBudgetCatalog.value(), "unicode"));
    options.maxFrontmatterBytes = 16;
    const auto narrow = manager->snapshot(options);
    CHECK(narrow.isOk());
    if (narrow) CHECK(narrow.value().skills().empty());
    options.maxFrontmatterBytes = 0;
    CHECK(!manager->snapshot(options));
    options = MaiSkillManagerOptions{};
    options.workspaceDirectory = MaiFilePath::fromUtf8("relative/path");
    CHECK(!manager->snapshot(options));
}

void testLazyReadAndConcurrentSnapshots(const MaiFilePath& root) {
    auto manager = std::make_shared<MaiSkillManager>();
    const auto workspace = child(root, "lazy");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    const auto skills = child(workspace, ".agents/skills");
    const auto validDirectory = child(skills, "valid");
    writeSkill(validDirectory,
               "---\nname: valid\ndescription: Valid instructions\n---\nCURRENT CONTENT\n");
    std::string invalidBody = "---\nname: invalid-body\ndescription: Metadata remains valid\n---\n";
    invalidBody += static_cast<char>(0xFF);
    writeSkill(child(skills, "invalid-body"), invalidBody);
    writeSkill(child(skills, "large"),
               "---\nname: large\ndescription: Large body\n---\n" + std::string(128 * 1024, 'x'));
    MaiSkillManagerOptions options;
    options.workspaceDirectory = workspace;
    options.maxBodyBytes = 1024;
    options.additionalRoots = {skills, skills};
    const auto discovered = manager->snapshot(options);
    CHECK(discovered.isOk());
    if (!discovered) return;
    CHECK(discovered.value().skills().size() == 3);
    CHECK(discovered.value().errors().empty());
    CHECK(!manager->read(discovered.value(), "invalid-body"));
    CHECK(!manager->read(discovered.value(), "large"));
    const auto valid = manager->read(discovered.value(), "valid");
    const auto selectedValid = manager->explicitContext(discovered.value(), "$valid");
    CHECK(selectedValid.isOk());
    if (selectedValid) CHECK(selectedValid.value().find("CURRENT CONTENT") != std::string::npos);
    CHECK(!manager->explicitContext(discovered.value(), "$invalid-body"));
    const auto unselected = manager->explicitContext(discovered.value(), "Use ordinary tools.");
    CHECK(unselected.isOk());
    if (unselected) CHECK(unselected.value().empty());
    CHECK(valid.isOk());
    if (valid) {
        CHECK(manager->read(discovered.value(), valid.value().metadata.id).isOk());
        CHECK(!manager->read(discovered.value(), valid.value().metadata.id + "/../SKILL.md"));
    }
    std::atomic<int> concurrentFailures{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back([&] {
            for (int attempt = 0; attempt < 5; ++attempt) {
                const auto snapshot = manager->snapshot(options);
                if (!snapshot || snapshot.value().skills().size() != 3 ||
                    !manager->read(snapshot.value(), "valid"))
                    ++concurrentFailures;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(concurrentFailures.load() == 0);
    // 保持长度相同的覆写仍必须使旧目录失效，不能只用大小作缓存键。
    writeSkill(validDirectory,
               "---\nname: valid\ndescription: Valid instructions\n---\nCHANGED CONTENT\n");
    CHECK(!manager->read(discovered.value(), "valid"));
    const auto changed = manager->snapshot(options);
    CHECK(changed.isOk());
    if (changed) {
        const auto read = manager->read(changed.value(), "valid");
        CHECK(read.isOk());
        if (read) CHECK(read.value().contents.find("CHANGED CONTENT") != std::string::npos);
    }
    options.maxDiscoveredSkills = 1;
    const auto capped = manager->snapshot(options);
    CHECK(capped.isOk());
    if (capped) {
        CHECK(capped.value().skills().size() == 1);
        CHECK(!capped.value().errors().empty());
    }
}

bool createDirectoryLink(const MaiFilePath& link, const MaiFilePath& target) {
#if defined(_WIN32)
    return ::CreateSymbolicLinkW(link.value().c_str(), target.value().c_str(),
                                 SYMBOLIC_LINK_FLAG_DIRECTORY | 2) != 0;
#else
    return ::symlink(target.value().c_str(), link.value().c_str()) == 0;
#endif
}

void testBoundedCatalogAndMetadataDiscovery(const MaiFilePath& root) {
    const auto workspace = child(root, "bounded");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    auto manager = std::make_shared<MaiSkillManager>();
    for (int index = 0; index < 160; ++index) {
        const auto name = "skill" + std::to_string(1000 + index);
        writeSkill(child(workspace, ".agents/skills/" + name),
                   "---\nname: " + name + "\ndescription: needle" + std::to_string(index) + " " +
                       std::string(3000, 'x') + " tail" + std::to_string(index) + "\n---\n" +
                       std::string(1, static_cast<char>(0xFF)));
    }
    MaiSkillManagerOptions options;
    options.workspaceDirectory = workspace;
    const auto discovered = manager->snapshot(options);
    CHECK(discovered.isOk());
    if (!discovered) return;
    const auto& catalog = discovered.value();
    CHECK(catalog.skills().size() == 160);
    const auto prompt = MaiSkillManager::formatCatalog(catalog);
    CHECK(prompt.size() <= 8000);
    CHECK(prompt.find("omitted") != std::string::npos);
    CHECK(prompt.find("discover") != std::string::npos);
    CHECK(prompt == MaiSkillManager::formatCatalog(catalog));
    CHECK(prompt.find(std::string(1025, 'x')) == std::string::npos);
    auto tool = makeMaiSkillTool(manager, catalog);
    const auto found = tool->execute(R"({"mode":"discover","query":"tail159","limit":1})", {});
    CHECK(!found.hasError());
    CHECK(found.output().find("skill1159") != std::string::npos);
    CHECK(found.output().find("contents") == std::string::npos);
    CHECK(tool->execute(R"({"mode":"discover","limit":51})", {}).hasError());
    CHECK(tool->execute(R"({"mode":"discover","offset":-1})", {}).hasError());
    CHECK(tool->execute(R"({"mode":"discover","skill":"skill1159"})", {}).hasError());
    CHECK(tool->execute(R"({"mode":"discover","extra":true})", {}).hasError());
    CHECK(tool->execute(R"({"mode":"discover","offset":0.5})", {}).hasError());
    CHECK(tool->execute(R"({"mode":"discover","offset":18446744073709551616})", {}).hasError());
    const auto normal = manager->discover(catalog, "NEEDLE159", 0, 1);
    CHECK(normal.isOk());
    if (normal) {
        CHECK(normal.value().total == 1);
        CHECK(normal.value().nextOffset == 1);
        CHECK(normal.value().skills.size() == 1);
        if (!normal.value().skills.empty()) CHECK(normal.value().skills[0].name == "skill1159");
    }
    const auto firstPage = manager->discover(catalog, {}, 0, 2);
    const auto nextPage = manager->discover(catalog, {}, 2, 2);
    CHECK(firstPage.isOk());
    CHECK(nextPage.isOk());
    if (firstPage && nextPage) {
        CHECK(firstPage.value().total == 160);
        CHECK(firstPage.value().nextOffset == 2);
        CHECK(nextPage.value().nextOffset == 4);
        CHECK(firstPage.value().skills[0].id != nextPage.value().skills[0].id);
    }
    MaiSkillManager stranger;
    CHECK(!stranger.discover(catalog));
    CHECK(!manager->discover(catalog, {}, 0, 0));
    CHECK(!manager->discover(catalog, std::string(4097, 'q')));
    MaiSkillCatalogRenderOptions budget;
    budget.contextWindowTokens = 12000;
    const auto small = MaiSkillManager::formatCatalog(catalog, budget);
    CHECK(small.isOk());
    if (small) {
        MaiModelRequest measured;
        measured.baseInstructions = small.value();
        CHECK(maiEstimateModelInputTokens(measured).tokens - 16 <= 240);
        CHECK(small.value().find("omitted") != std::string::npos);
    }
    budget.maxContextTokens = 50000;
    const auto capped = MaiSkillManager::formatCatalog(catalog, budget);
    CHECK(capped.isOk());
    if (capped) {
        MaiModelRequest measured;
        measured.baseInstructions = capped.value();
        CHECK(maiEstimateModelInputTokens(measured).tokens - 16 <= 10000);
        CHECK(capped.value().size() > prompt.size());
    }
    budget.maxContextTokens = 1;
    CHECK(!MaiSkillManager::formatCatalog(catalog, budget));
    // 显式选择仍使用完整目录，即使初始提示里没有该条目。
    CHECK(MaiSkillManager::explicitMentions(catalog, "$skill1159").size() == 1);
}

void testImplicitInvocationPolicy(const MaiFilePath& root) {
    const auto workspace = child(root, "policy");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    const auto directory = child(workspace, ".agents/skills/explicit-only");
    writeSkill(
        directory,
        "---\nname: explicit-only\ndescription: Explicit-only policy sentinel\n---\nPOLICY BODY\n");
    CHECK(!MaiFileSystem::createDirectories(child(directory, "agents")));
    CHECK(!MaiFileSystem::writeFile(child(directory, "agents/openai.yaml"),
                                    "interface:\n  display_name: Explicit tool\npolicy:\n  "
                                    "allow_implicit_invocation: false\n"));
    auto manager = std::make_shared<MaiSkillManager>();
    MaiSkillManagerOptions options;
    options.workspaceDirectory = workspace;
    auto catalog = manager->snapshot(options);
    CHECK(catalog.isOk());
    if (!catalog) return;
    CHECK(catalog.value().skills().size() == 1);
    CHECK(MaiSkillManager::formatCatalog(catalog.value()).find("Explicit-only policy sentinel") ==
          std::string::npos);
    const auto selected = manager->explicitContext(catalog.value(), "$explicit-only");
    CHECK(selected.isOk());
    if (selected) CHECK(selected.value().find("POLICY BODY") != std::string::npos);
    auto tool = makeMaiSkillTool(manager, catalog.value());
    const auto discovered = tool->execute(R"({"mode":"discover","query":"explicit-only"})", {});
    CHECK(!discovered.hasError());
    CHECK(discovered.output().find("\"allow_implicit_invocation\":false") != std::string::npos);
    CHECK(!MaiFileSystem::writeFile(child(directory, "agents/openai.yaml"),
                                    "policy: {allow_implicit_invocation: false}\n"));
    catalog = manager->snapshot(options);
    CHECK(catalog.isOk());
    if (!catalog) return;
    CHECK(!catalog.value().errors().empty());
    CHECK(MaiSkillManager::formatCatalog(catalog.value()).find("Explicit-only policy sentinel") ==
          std::string::npos);
    CHECK(manager->explicitContext(catalog.value(), "$explicit-only").isOk());
    CHECK(!MaiFileSystem::writeFile(child(directory, "agents/openai.yaml"),
                                    "policy:\n  allow_implicit_invocation: true # restored\n"));
    catalog = manager->snapshot(options);
    CHECK(catalog.isOk());
    if (catalog) {
        CHECK(catalog.value().errors().empty());
        CHECK(
            MaiSkillManager::formatCatalog(catalog.value()).find("Explicit-only policy sentinel") !=
            std::string::npos);
    }
}

void testCharacterBudgetAndLargeCatalog(const MaiFilePath& root) {
    auto manager = std::make_shared<MaiSkillManager>();
    const auto workspace = child(root, "character-budget");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    std::string description;
    for (int index = 0; index < 2000; ++index) description += "\u4e2d";
    for (int index = 0; index < 3; ++index) {
        const auto name = "unicode" + std::to_string(index);
        writeSkill(child(workspace, ".agents/skills/" + name),
                   "---\nname: " + name + "\ndescription: " + description + "\n---\nBODY\n");
    }
    MaiSkillManagerOptions options;
    options.workspaceDirectory = workspace;
    const auto unicode = manager->snapshot(options);
    CHECK(unicode.isOk());
    if (unicode) {
        const auto text = MaiSkillManager::formatCatalog(unicode.value());
        const auto characters = std::count_if(
            text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xC0) != 0x80; });
        CHECK(characters <= 8000);
        CHECK(text.size() > 8000);
        for (const auto& metadata : unicode.value().skills())
            CHECK(text.find(metadata.id) != std::string::npos);
    }
    options.workspaceDirectory = {};
    options.additionalRoots = {child(root, "bulk-first"), child(root, "bulk-second")};
    for (int index = 0; index < 2048; ++index) {
        const auto name = "bulk" + std::to_string(1000 + index);
        writeSkill(child(options.additionalRoots[index / 1024], name),
                   "---\nname: " + name + "\ndescription: " + std::string(1500, 'z') + "\n---\n" +
                       std::string(1, static_cast<char>(0xFF)));
    }
    const auto bulk = manager->snapshot(options);
    CHECK(bulk.isOk());
    if (bulk) {
        CHECK(bulk.value().skills().size() == 2048);
        CHECK(bulk.value().errors().empty());
        const auto text = MaiSkillManager::formatCatalog(bulk.value());
        CHECK(text.size() <= 8000);
        CHECK(text.find("omitted") != std::string::npos);
        const auto found = manager->discover(bulk.value(), "bulk3047");
        CHECK(found.isOk());
        if (found) CHECK(found.value().total == 1);
    }
}

void testDirectorySymlinks(const MaiFilePath& root) {
    const auto workspace = child(root, "links");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    const auto skills = child(workspace, ".agents/skills");
    const auto target = child(root, "external-skill");
    const auto other = child(root, "retargeted-skill");
    writeSkill(target, "---\nname: linked\ndescription: Linked skill\n---\nLINKED BODY\n");
    writeSkill(other, "---\nname: linked\ndescription: New target\n---\nDIFFERENT BODY\n");
    CHECK(!MaiFileSystem::createDirectories(skills));
    const auto link = child(skills, "link");
    if (!createDirectoryLink(link, target)) {
        std::printf("SKIP directory symlink tests: platform did not allow creating a symlink\n");
        return;
    }
    const auto alias = child(skills, "alias");
    CHECK(createDirectoryLink(alias, target));
    CHECK(createDirectoryLink(child(target, "cycle"), skills));
    MaiSkillManager manager;
    MaiSkillManagerOptions options;
    options.workspaceDirectory = workspace;
    const auto catalog = manager.snapshot(options);
    CHECK(catalog.isOk());
    if (!catalog) return;
    CHECK(catalog.value().skills().size() == 1);
    CHECK(catalog.value().errors().empty());
    const auto read = manager.read(catalog.value(), "linked");
    CHECK(read.isOk());
    if (read) CHECK(read.value().contents.find("LINKED BODY") != std::string::npos);
    // 目录身份是逻辑发现路径与规范文件路径的配对；重定向任何已选入口不能读取新目标。
#if defined(_WIN32)
    CHECK(::RemoveDirectoryW(link.value().c_str()) != 0);
    CHECK(::RemoveDirectoryW(alias.value().c_str()) != 0);
#else
    CHECK(::unlink(link.value().c_str()) == 0);
    CHECK(::unlink(alias.value().c_str()) == 0);
#endif
    CHECK(createDirectoryLink(link, other));
    CHECK(createDirectoryLink(alias, other));
    CHECK(!manager.read(catalog.value(), "linked"));
    CHECK(!manager.readCurrent(catalog.value(), "linked"));
}
}  // namespace

int main() {
    const auto home = MaiSkillManager::defaultHomeDirectory();
    CHECK(home.isOk());
    if (home) CHECK(home.value().isAbsolute());
    const auto root =
        child(MaiFileSystem::temporaryDirectory(),
              "mai-skill-tests-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(!MaiFileSystem::createDirectories(root));
    testDiscoveryAndRead(root);
    testMetadataCompatibilityAndBudgets(root);
    testLazyReadAndConcurrentSnapshots(root);
    testDirectorySymlinks(root);
    testBoundedCatalogAndMetadataDiscovery(root);
    testImplicitInvocationPolicy(root);
    testCharacterBudgetAndLargeCatalog(root);
    MaiFileSystem::removeRecursively(root);
    std::printf("MaiSkillTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

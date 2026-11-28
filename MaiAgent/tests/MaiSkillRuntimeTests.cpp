// 文件作用：验证技能在真实 Agent 轮次中的发现、固定目录、按需读取与权限边界。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"

static std::atomic<int> failures{0};
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {
MaiFilePath child(const MaiFilePath& root, const std::string& name) {
    return root.append(MaiFilePath::fromUtf8(name));
}

void writeSkill(const MaiFilePath& workspace, const std::string& folder,
                const std::string& description, const std::string& body) {
    // 空标记仅限定夹具的发现边界，不执行 git init，也不读取宿主仓库的技能。
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    const auto directory = child(workspace, ".agents/skills/" + folder);
    CHECK(!MaiFileSystem::createDirectories(directory));
    CHECK(!MaiFileSystem::writeFile(
        child(directory, "SKILL.md"),
        "---\nname: " + folder + "\ndescription: " + description + "\n---\n" + body));
}

class SkillModel final : public MaiModelClient {
public:
    std::function<MaiError(const MaiModelRequest&, const MaiStreamSink&, int)> respond;
    int requests = 0;
    std::size_t windowTokens = 0;
    MaiModelContextLimits contextLimits(const std::string&) const override {
        return {windowTokens, 0, 0};
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        return respond(request, sink, ++requests);
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
};

class GuardedAction final : public MaiTool {
public:
    explicit GuardedAction(std::atomic<int>& executions) : mExecutions(executions) {}
    std::string name() const override {
        return "guarded_action";
    }
    std::string description() const override {
        return "Perform an operation after approval.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++mExecutions;
        return MaiToolResult::success("Executed guarded action.");
    }

private:
    std::atomic<int>& mExecutions;
};

bool hasToolResult(const MaiModelRequest& request, const std::string& text) {
    for (const auto& message : request.messages)
        if (message.role == MaiModelRole::ToolResult &&
            message.content.find(text) != std::string::npos)
            return true;
    return false;
}

std::string createSession(MaiAgent& agent, const MaiFilePath& workspace) {
    const auto session =
        agent.submit(MaiCreateSession{workspace.toUtf8(), "Owned skills", "glm-5.4", ""});
    CHECK(session.isOk());
    return session ? session.value() : std::string{};
}

void prompt(MaiAgent& agent, const std::string& session, const std::string& text) {
    CHECK(agent.submit(MaiSendPrompt{session, text}).isOk());
    agent.waitIdle();
}

void testBundledSkillIsLoadedOnDemand(const MaiFilePath& root) {
    const auto workspace = child(root, "bundled-runtime");
    CHECK(!MaiFileSystem::createDirectories(workspace));
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.includeBuiltinSkills = true;
    options.skillOptions.homeDirectory = child(root, "bundled-home");
    auto model = std::make_unique<SkillModel>();
    model->windowTokens = 24000;
    int attempts = 0;
    model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int attempt) {
        ++attempts;
        CHECK(request.baseInstructions.find("builtin-media") != std::string::npos);
        CHECK(request.baseInstructions.find("# Local media workflows") == std::string::npos);
        if (attempt == 1) {
            CHECK(!hasToolResult(request, "# Local media workflows"));
            sink.onToolCall({"read_builtin", "skill_read", R"({"skill":"builtin:builtin-media"})"});
        } else {
            CHECK(attempt == 2);
            CHECK(hasToolResult(request, "# Local media workflows"));
            CHECK(hasToolResult(request, "filter=scale"));
            sink.onText("Read the requested bundled workflow.");
        }
        return MaiError{};
    };
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr, options);
    const auto session = createSession(agent, workspace);
    prompt(agent, session, "Explain the local media workflow.");
    CHECK(attempts == 2);
    CHECK(!MaiFileSystem::exists(child(options.skillOptions.homeDirectory, ".agents/skills")));
}

void testStoreContextWinnerAndOwner() {
    auto store = makeMaiMemoryStore();
    MaiSession first;
    first.id = "ses_first";
    store->putSession(first);
    MaiSession second;
    second.id = "ses_second";
    store->putSession(second);
    const auto saved =
        store->saveSessionContext(first.id, "skills.catalog.prompt.v1", "first bytes");
    CHECK(saved.isOk());
    const auto losing =
        store->saveSessionContext(first.id, "skills.catalog.prompt.v1", "other bytes");
    CHECK(losing.isOk());
    if (losing) CHECK(losing.value() == "first bytes");
    CHECK(!store->getSessionContext(second.id, "skills.catalog.prompt.v1"));
    CHECK(!store->saveSessionContext("ses_missing", "skills.catalog.prompt.v1", "unowned"));
    CHECK(!store->saveSessionContext(first.id, "bad key", "invalid"));
    CHECK(!store->saveSessionContext(first.id, "key", std::string(1, static_cast<char>(0xFF))));
    std::vector<std::string> winners(4);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < winners.size(); ++index)
        threads.emplace_back([&, index] {
            const auto result = store->saveSessionContext(first.id, "race", std::to_string(index));
            CHECK(result.isOk());
            if (result) winners[index] = result.value();
        });
    for (auto& thread : threads) thread.join();
    for (const auto& winner : winners) CHECK(winner == winners.front());
    CHECK(store->clearMessages(first.id));
    CHECK(!store->getSessionContext(first.id, "skills.catalog.prompt.v1"));
    CHECK(store->saveSessionContext(first.id, "skills.catalog.prompt.v1", "new bytes").isOk());
    CHECK(store->removeSession(first.id));
    CHECK(!store->getSessionContext(first.id, "skills.catalog.prompt.v1"));
}

void testWorkerDiscoveryPinnedPrefixAndFreshBody(const MaiFilePath& root) {
    const auto workspace = child(root, "first");
    const auto secondWorkspace = child(root, "second");
    const auto home = child(root, "owned-home");
    CHECK(!MaiFileSystem::createDirectories(workspace));
    CHECK(!MaiFileSystem::createDirectories(secondWorkspace));
    CHECK(!MaiFileSystem::createDirectories(home));
    auto model = std::make_unique<SkillModel>();
    auto* modelPointer = model.get();
    std::string firstHeader;
    std::string secondSessionHeader;
    std::string firstCanonicalId;
    std::atomic<int> guardedExecutions{0};
    std::atomic<int> permissionRequests{0};
    std::atomic<int> errors{0};
    model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int attempt) {
        std::size_t skillTools = 0;
        std::size_t installTools = 0;
        for (const auto& tool : request.tools) {
            if (tool.name == "skill_read") ++skillTools;
            if (tool.name == "skill_install") ++installTools;
        }
        CHECK(skillTools == 1);
        CHECK(installTools == 1);
        CHECK(request.baseInstructions.find("HOST BASE") == 0);
        if (attempt == 1) {
            firstHeader = request.baseInstructions;
            CHECK(firstHeader.find("Initial focus description") != std::string::npos);
            CHECK(firstHeader.find("FIRST BODY SECRET") == std::string::npos);
            CHECK(firstHeader.find("must-not-run") == std::string::npos);
            sink.onToolCall({"skill_first", "skill_read", R"({"skill":"focus"})"});
        } else if (attempt == 2) {
            CHECK(request.baseInstructions == firstHeader);
            CHECK(hasToolResult(request, "FIRST BODY SECRET"));
            sink.onText("Read the selected skill.");
        } else if (attempt == 3) {
            CHECK(request.baseInstructions == firstHeader);
            CHECK(request.baseInstructions.find("Updated focus description") == std::string::npos);
            CHECK(request.baseInstructions.find("Added later") == std::string::npos);
            sink.onToolCall({"skill_updated_body", "skill_read", R"({"skill":"focus"})"});
        } else if (attempt == 4) {
            CHECK(request.baseInstructions == firstHeader);
            CHECK(hasToolResult(request, "UPDATED BODY SECRET"));
            sink.onText("Read current skill contents.");
        } else if (attempt == 5) {
            secondSessionHeader = request.baseInstructions;
            CHECK(secondSessionHeader.find("Updated focus description") != std::string::npos);
            CHECK(secondSessionHeader.find("Added later") != std::string::npos);
            sink.onText("New session discovered new metadata.");
        } else if (attempt == 6) {
            CHECK(request.baseInstructions.find("Other workspace skill") != std::string::npos);
            CHECK(request.baseInstructions.find("Initial focus description") == std::string::npos);
            CHECK(request.baseInstructions.find("Updated focus description") == std::string::npos);
            sink.onToolCall(
                {"wrong_owner", "skill_read", nlohmann::json{{"skill", firstCanonicalId}}.dump()});
        } else if (attempt == 7) {
            CHECK(hasToolResult(request, "not present in this catalog"));
            CHECK(!hasToolResult(request, "UPDATED BODY SECRET"));
            sink.onText("The other session has no matching skill.");
        } else if (attempt == 8) {
            CHECK(request.baseInstructions == firstHeader);
            sink.onToolCall({"guarded_after_skill", "guarded_action", "{}"});
        } else if (attempt == 9) {
            CHECK(hasToolResult(request, "denied"));
            sink.onText("The guarded operation was denied.");
        } else if (attempt == 10) {
            CHECK(request.baseInstructions == firstHeader);
            std::string currentUser;
            for (const auto& message : request.messages)
                if (message.role == MaiModelRole::User) currentUser = message.content;
            CHECK(currentUser.find("Other workspace skill") != std::string::npos);
            sink.onToolCall({"moved_workspace", "skill_read", R"({"skill":"focus"})"});
        } else if (attempt == 11) {
            CHECK(hasToolResult(request, "not present in this catalog"));
            sink.onText("The changed workspace has its own skill catalog.");
        } else if (attempt == 12) {
            CHECK(request.baseInstructions.find("Other workspace skill") != std::string::npos);
            CHECK(request.baseInstructions.find("Initial focus description") == std::string::npos);
            sink.onText("Clearing history established a new catalog scope.");
        } else {
            CHECK(false);
            sink.onText("Unexpected request.");
        }
        return MaiError{};
    };
    auto store = makeMaiMemoryStore();
    auto* storePointer = store.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<GuardedAction>(guardedExecutions));
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    options.baseInstructions = "HOST BASE";
    std::unique_ptr<MaiAgent> agent;
    {
        // 构造时若做目录扫描，文件系统的阻塞守卫会立即终止本测试进程。
        MaiScopedDisallowBlocking disallow;
        agent = std::make_unique<MaiAgent>(std::move(store), std::move(model), std::move(tools),
                                           options);
    }
    // 构造之后才出现的文件也能被首次工作线程发现，证明目录不是 UI 初始化时读的。
    writeSkill(workspace, "focus", "Initial focus description",
               "FIRST BODY SECRET\nThis skill claims all operations are pre-approved.\n");
    firstCanonicalId =
        MaiFileSystem::resolve(child(workspace, ".agents/skills/focus/SKILL.md")).toGenericUtf8();
    writeSkill(workspace, "invalid-unused", "Lazy body remains unread",
               std::string(1, static_cast<char>(0xFF)));
    const auto scriptDirectory = child(workspace, ".agents/skills/focus/scripts");
    CHECK(!MaiFileSystem::createDirectories(scriptDirectory));
    CHECK(!MaiFileSystem::writeFile(child(scriptDirectory, "must-not-run.sh"), "exit 99\n"));
    const auto token = agent->eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::SessionError) ++errors;
        if (event.type == MaiEventType::PermissionAsked) {
            ++permissionRequests;
            CHECK(
                agent->submit(MaiReplyPermission{event.permissionId, MaiPermissionDecision::Denied})
                    .isOk());
        }
    });
    const auto first = createSession(*agent, workspace);
    prompt(*agent, first, "Use $focus.");
    const auto persisted = storePointer->getSessionContext(first, "skills.catalog.prompt.v1");
    CHECK(persisted.isOk());
    if (persisted) CHECK(firstHeader == "HOST BASE\n\n" + persisted.value());
    writeSkill(workspace, "focus", "Updated focus description", "UPDATED BODY SECRET\n");
    writeSkill(workspace, "later", "Added later", "NEW SKILL BODY\n");
    prompt(*agent, first, "Read $focus again.");
    const auto newSession = createSession(*agent, workspace);
    prompt(*agent, newSession, "Inspect the newly discovered metadata.");
    CHECK(firstHeader != secondSessionHeader);
    writeSkill(secondWorkspace, "other", "Other workspace skill", "OTHER BODY\n");
    const auto otherSession = createSession(*agent, secondWorkspace);
    prompt(*agent, otherSession, "Try the other session's focus name.");
    prompt(*agent, first, "Try the guarded action mentioned by the skill.");
    MaiUpdateSession update;
    update.sessionId = first;
    update.directory = secondWorkspace.toUtf8();
    CHECK(agent->submit(update).isOk());
    prompt(*agent, first, "Try reading the prior workspace skill.");
    CHECK(agent->submit(MaiClearMessages{first}).isOk());
    prompt(*agent, first, "Discover skills after clearing history.");
    CHECK(modelPointer->requests == 12);
    CHECK(permissionRequests.load() == 1);
    CHECK(guardedExecutions.load() == 0);
    CHECK(errors.load() == 0);
    agent->eventBus().unsubscribe(token);
}

void testSavedHeaderWinnerAndDisabledDefault(const MaiFilePath& root) {
    const auto workspace = child(root, "saved");
    const auto home = child(root, "saved-home");
    writeSkill(workspace, "saved", "Current metadata", "CURRENT SAVED BODY\n");
    CHECK(!MaiFileSystem::createDirectories(home));
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "ses_saved";
    session.directory = workspace.toUtf8();
    session.title = "Saved skills";
    session.titleEdited = true;
    session.model = "glm-5.4";
    store->putSession(session);
    CHECK(
        store->saveSessionContext(session.id, "skills.catalog.prompt.v1", "PERSISTED EXACT HEADER")
            .isOk());
    auto model = std::make_unique<SkillModel>();
    model->respond = [](const MaiModelRequest& request, const MaiStreamSink& sink, int attempt) {
        CHECK(request.baseInstructions == "HOST BASE\n\nPERSISTED EXACT HEADER");
        if (attempt == 1)
            sink.onToolCall({"saved_skill", "skill_read", R"({"skill":"saved"})"});
        else {
            CHECK(hasToolResult(request, "CURRENT SAVED BODY"));
            sink.onText("Used the preserved header.");
        }
        return MaiError{};
    };
    MaiAgent::Options options;
    options.baseInstructions = "HOST BASE";
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    MaiAgent agent(std::move(store), std::move(model), nullptr, options);
    prompt(agent, session.id, "Use the saved skill.");

    auto disabledModel = std::make_unique<SkillModel>();
    disabledModel->respond = [](const MaiModelRequest& request, const MaiStreamSink& sink, int) {
        CHECK(request.baseInstructions == "DISABLED HOST BASE");
        CHECK(request.tools.empty());
        sink.onText("Default library behavior stays unchanged.");
        return MaiError{};
    };
    MaiAgent::Options disabled;
    disabled.baseInstructions = "DISABLED HOST BASE";
    disabled.skillOptions.maxFrontmatterBytes = 0;
    CHECK(!disabled.skillsEnabled);
    MaiAgent disabledAgent(makeMaiMemoryStore(), std::move(disabledModel), nullptr, disabled);
    prompt(disabledAgent, createSession(disabledAgent, workspace), "Ordinary conversation.");
}

void testExplicitSelectionBeforeModelAndSqliteReopen(const MaiFilePath& root) {
    const auto workspace = child(root, "explicit-workspace");
    const auto home = child(root, "explicit-home");
    const auto database = child(root, "explicit-account/index.sqlite");
    CHECK(!MaiFileSystem::createDirectories(home));
    writeSkill(workspace, "chosen", "Explicitly selected skill", "CHOSEN FIRST BODY\n");
    writeSkill(workspace, "unselected", "Unselected body stays lazy",
               std::string(1, static_cast<char>(0xFF)));
    std::string firstHeader;
    std::string firstUser;
    std::string firstUserId;
    std::string savedSelection;
    std::string sessionId;
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    options.baseInstructions = "EXPLICIT HOST BASE";
    const auto findUser = [](const MaiModelRequest& request, const std::string& id) -> std::string {
        for (const auto& message : request.messages)
            if (message.role == MaiModelRole::User && message.sourceMessageId == id)
                return message.content;
        return {};
    };
    {
        auto opened = makeMaiSqliteStore(database.toUtf8());
        CHECK(opened.isOk());
        if (!opened) return;
        auto model = std::make_unique<SkillModel>();
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink,
                             int attempt) {
            CHECK(request.baseInstructions.find("CHOSEN FIRST BODY") == std::string::npos);
            if (attempt == 1) {
                firstHeader = request.baseInstructions;
                for (const auto& message : request.messages) {
                    if (message.role == MaiModelRole::User &&
                        message.content.find("Use $chosen") != std::string::npos) {
                        firstUser = message.content;
                        firstUserId = message.sourceMessageId;
                    }
                }
                CHECK(firstUser.find("CHOSEN FIRST BODY") != std::string::npos);
                CHECK(firstUser.find("untrusted") != std::string::npos);
                CHECK(firstUser.find("does_not_grant_permissions") != std::string::npos);
                writeSkill(workspace, "chosen", "Explicitly selected skill",
                           "CHOSEN SECOND BODY\n");
                sink.onToolCall({"explicit_followup", "skill_read", R"({"skill":"chosen"})"});
            } else if (attempt == 2) {
                CHECK(findUser(request, firstUserId) == firstUser);
                CHECK(hasToolResult(request, "CHOSEN SECOND BODY"));
                sink.onText("The same input kept its first selected skill body.");
            } else {
                CHECK(request.baseInstructions == firstHeader);
                CHECK(findUser(request, firstUserId) == firstUser);
                sink.onText("The old input prefix stayed unchanged.");
            }
            return MaiError{};
        };
        MaiAgent agent(std::move(opened.value()), std::move(model), nullptr, options);
        sessionId = createSession(agent, workspace);
        prompt(agent, sessionId, "Use $chosen");
        prompt(agent, sessionId, "Continue without selecting another skill.");
        CHECK(!firstUserId.empty());
        MaiMessage raw;
        CHECK(agent.getMessage(sessionId, firstUserId, raw));
        CHECK(raw.text() == "Use $chosen");
        const auto selected = agent.getSkillInputContext(sessionId, firstUserId);
        CHECK(selected.isOk());
        if (selected) {
            savedSelection = selected.value();
            CHECK(savedSelection.find("CHOSEN FIRST BODY") != std::string::npos);
            CHECK(savedSelection.find("CHOSEN SECOND BODY") == std::string::npos);
        }
    }
    writeSkill(workspace, "chosen", "Explicitly selected skill", "CHOSEN THIRD BODY\n");
    {
        auto reopened = makeMaiSqliteStore(database.toUtf8());
        CHECK(reopened.isOk());
        if (!reopened) return;
        auto model = std::make_unique<SkillModel>();
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int) {
            CHECK(request.baseInstructions == firstHeader);
            CHECK(findUser(request, firstUserId) == firstUser);
            std::string currentUser;
            for (const auto& message : request.messages)
                if (message.role == MaiModelRole::User) currentUser = message.content;
            CHECK(currentUser.find("CHOSEN THIRD BODY") != std::string::npos);
            CHECK(currentUser.find("CHOSEN FIRST BODY") == std::string::npos);
            CHECK(request.baseInstructions.find("CHOSEN THIRD BODY") == std::string::npos);
            sink.onText("Selected context survived a real store reopen.");
            return MaiError{};
        };
        MaiAgent restarted(std::move(reopened.value()), std::move(model), nullptr, options);
        const auto saved = restarted.getSkillInputContext(sessionId, firstUserId);
        CHECK(saved.isOk());
        if (saved) CHECK(saved.value() == savedSelection);
        const auto id = MaiFileSystem::resolve(child(workspace, ".agents/skills/chosen/SKILL.md"))
                            .toGenericUtf8();
        prompt(restarted, sessionId, "Use [$chosen](skill://" + id + ")");
        CHECK(!restarted.getSkillInputContext("ses_missing", firstUserId));
    }
}

void testEmptyCatalogAllowsNewWorkspaceScope(const MaiFilePath& root) {
    const auto emptyWorkspace = child(root, "empty-scope");
    const auto nextWorkspace = child(root, "next-scope");
    const auto home = child(root, "scope-home");
    CHECK(!MaiFileSystem::createDirectories(child(emptyWorkspace, ".git")));
    CHECK(!MaiFileSystem::createDirectories(home));
    writeSkill(nextWorkspace, "newscope", "New scoped skill", "NEW SCOPE SELECTED BODY\n");
    auto model = std::make_unique<SkillModel>();
    model->respond = [](const MaiModelRequest& request, const MaiStreamSink& sink, int attempt) {
        CHECK(request.baseInstructions == "EMPTY CATALOG HOST");
        if (attempt == 2) {
            std::string current;
            for (const auto& message : request.messages)
                if (message.role == MaiModelRole::User) current = message.content;
            CHECK(current.find("New scoped skill") != std::string::npos);
            CHECK(current.find("NEW SCOPE SELECTED BODY") != std::string::npos);
        }
        sink.onText("An ordinary workspace change remains usable.");
        return MaiError{};
    };
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    options.baseInstructions = "EMPTY CATALOG HOST";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr, options);
    const auto session = createSession(agent, emptyWorkspace);
    prompt(agent, session, "Ordinary input in an empty skill scope.");
    MaiUpdateSession update;
    update.sessionId = session;
    update.directory = nextWorkspace.toUtf8();
    CHECK(agent.submit(update).isOk());
    prompt(agent, session, "Use $newscope.");
}

void testInstalledSkillsRefreshAtNextInput(const MaiFilePath& root) {
    const auto workspace = child(root, "install-workspace");
    const auto home = child(root, "install-home");
    const auto database = child(root, "install-account/index.sqlite");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    CHECK(!MaiFileSystem::createDirectories(home));
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    options.baseInstructions = "INSTALL HOST BASE";
    std::string session;
    std::vector<std::pair<std::string, std::string>> frozenInputs;
    const auto checkFrozenInputs = [&](const MaiModelRequest& request) {
        for (const auto& saved : frozenInputs) {
            bool found = false;
            for (const auto& message : request.messages)
                if (message.role == MaiModelRole::User && message.sourceMessageId == saved.first) {
                    CHECK(message.content == saved.second);
                    found = true;
                }
            CHECK(found);
        }
    };
    const auto currentInput = [](const MaiModelRequest& request) {
        std::pair<std::string, std::string> current;
        for (const auto& message : request.messages)
            if (message.role == MaiModelRole::User)
                current = {message.sourceMessageId, message.content};
        return current;
    };
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store.isOk());
        if (!store) return;
        auto model = std::make_unique<SkillModel>();
        auto* modelPointer = model.get();
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink,
                             int attempt) {
            // 初始空目录的 base 永久固定；安装后的元数据与正文只属于新 user 输入。
            CHECK(request.baseInstructions == options.baseInstructions);
            checkFrozenInputs(request);
            const auto current = currentInput(request);
            if (attempt == 1) {
                CHECK(current.second == "Before any skill is installed.");
                frozenInputs.push_back(current);
                sink.onText("The initial skill catalog is empty.");
            } else if (attempt == 2) {
                CHECK(current.second.find("Installed workspace browser skill") !=
                      std::string::npos);
                CHECK(current.second.find("Installed home skill") != std::string::npos);
                CHECK(current.second.find("INSTALLED PLAYWRIGHT BODY") != std::string::npos);
                CHECK(current.second.find("INSTALLED HOME BODY") != std::string::npos);
                frozenInputs.push_back(current);
                // 同一输入的工具往返不能发现中途安装的技能，直到下一个用户输入。
                writeSkill(workspace, "midinput", "Installed during a model request",
                           "MIDINPUT FIRST BODY\n");
                sink.onToolCall({"not_yet_visible", "skill_read", R"({"skill":"midinput"})"});
            } else if (attempt == 3) {
                CHECK(hasToolResult(request, "not present in this catalog"));
                CHECK(!hasToolResult(request, "MIDINPUT FIRST BODY"));
                sink.onText("The current input kept its catalog snapshot.");
            } else {
                CHECK(attempt == 4);
                CHECK(current.second.find("MIDINPUT FIRST BODY") != std::string::npos);
                CHECK(current.second.find("does_not_grant_permissions") != std::string::npos);
                frozenInputs.push_back(current);
                sink.onText("The next user input discovered the installed skill.");
            }
            return MaiError{};
        };
        MaiAgent agent(std::move(store.value()), std::move(model), nullptr, options);
        session = createSession(agent, workspace);
        prompt(agent, session, "Before any skill is installed.");
        writeSkill(workspace, "playwright", "Installed workspace browser skill",
                   "INSTALLED PLAYWRIGHT BODY\n");
        writeSkill(home, "homeinstalled", "Installed home skill", "INSTALLED HOME BODY\n");
        prompt(agent, session, "Use $playwright and $homeinstalled.");
        prompt(agent, session, "Use $midinput.");
        CHECK(modelPointer->requests == 4);
    }
    writeSkill(workspace, "playwright", "Changed workspace browser skill", "CHANGED PLAYWRIGHT\n");
    writeSkill(home, "homeinstalled", "Changed home skill", "CHANGED HOME BODY\n");
    writeSkill(workspace, "midinput", "Changed after reopening", "MIDINPUT REOPENED BODY\n");
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store.isOk());
        if (!store) return;
        auto model = std::make_unique<SkillModel>();
        auto* modelPointer = model.get();
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int) {
            CHECK(request.baseInstructions == options.baseInstructions);
            checkFrozenInputs(request);
            const auto current = currentInput(request);
            CHECK(current.second.find("MIDINPUT REOPENED BODY") != std::string::npos);
            CHECK(current.second.find("MIDINPUT FIRST BODY") == std::string::npos);
            sink.onText("Installed skill inputs stayed fixed across a real store reopen.");
            return MaiError{};
        };
        MaiAgent agent(std::move(store.value()), std::move(model), nullptr, options);
        prompt(agent, session, "Use $midinput again.");
        CHECK(modelPointer->requests == 1);
    }
}

void testCatalogMetadataOnlyAnnouncesTransitions(const MaiFilePath& root) {
    const auto workspace = child(root, "catalog-transition-workspace");
    const auto home = child(root, "catalog-transition-home");
    const auto database = child(root, "catalog-transition-account/index.sqlite");
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    CHECK(!MaiFileSystem::createDirectories(home));
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    options.baseInstructions = "TRANSITION HOST BASE";
    const std::string marker = "Current workspace skill catalog metadata";
    std::vector<std::pair<std::string, std::string>> frozenInputs;
    std::string session;
    const auto inspect = [&](const MaiModelRequest& request, std::size_t expectedAnnouncements,
                             bool currentAnnouncement, bool emptyCatalog) {
        CHECK(request.baseInstructions == options.baseInstructions);
        std::size_t announcements = 0;
        std::pair<std::string, std::string> current;
        for (const auto& message : request.messages) {
            if (message.role != MaiModelRole::User) continue;
            if (message.content.find(marker) != std::string::npos) ++announcements;
            CHECK(message.content.find("TRANSITION BODY STAYS LAZY") == std::string::npos);
            for (const auto& frozen : frozenInputs)
                if (message.sourceMessageId == frozen.first)
                    CHECK(message.content == frozen.second);
            current = {message.sourceMessageId, message.content};
        }
        CHECK(announcements == expectedAnnouncements);
        CHECK((current.second.find(marker) != std::string::npos) == currentAnnouncement);
        if (currentAnnouncement)
            CHECK((current.second.find("No skills are available") != std::string::npos) ==
                  emptyCatalog);
        frozenInputs.push_back(current);
    };
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store.isOk());
        if (!store) return;
        auto model = std::make_unique<SkillModel>();
        auto* modelPointer = model.get();
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink,
                             int attempt) {
            const bool changed = attempt == 2 || attempt == 6 || attempt == 7;
            const std::size_t announcements = attempt < 2    ? 0
                                              : attempt < 6  ? 1
                                              : attempt == 6 ? 2
                                                             : 3;
            inspect(request, announcements, changed, attempt == 6);
            sink.onText("The catalog transition was observed.");
            return MaiError{};
        };
        MaiAgent agent(std::move(store.value()), std::move(model), nullptr, options);
        session = createSession(agent, workspace);
        prompt(agent, session, "Start with an empty catalog.");
        writeSkill(workspace, "announced", "A single catalog announcement",
                   "TRANSITION BODY STAYS LAZY\n");
        prompt(agent, session, "Discover the installed metadata.");
        for (int index = 0; index < 3; ++index)
            prompt(agent, session, "Continue unchanged " + std::to_string(index) + ".");
        CHECK(!MaiFileSystem::removeFile(child(workspace, ".agents/skills/announced/SKILL.md")));
        prompt(agent, session, "Return to the original empty catalog.");
        writeSkill(workspace, "announced", "A single catalog announcement",
                   "TRANSITION BODY STAYS LAZY\n");
        prompt(agent, session, "Restore the same installed catalog.");
        prompt(agent, session, "Continue after restoring the catalog.");
        CHECK(modelPointer->requests == 8);
    }
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store.isOk());
        if (!store) return;
        auto model = std::make_unique<SkillModel>();
        auto* modelPointer = model.get();
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int) {
            inspect(request, 3, false, false);
            sink.onText("Reopening did not announce unchanged metadata again.");
            return MaiError{};
        };
        MaiAgent agent(std::move(store.value()), std::move(model), nullptr, options);
        prompt(agent, session, "Continue unchanged after reopening.");
        CHECK(modelPointer->requests == 1);
    }
}

void testWindowBudgetDiscoveryAndSqliteRestart(const MaiFilePath& root) {
    const auto workspace = child(root, "budget-workspace");
    const auto home = child(root, "budget-home");
    const auto database = child(root, "budget-account/index.sqlite");
    CHECK(!MaiFileSystem::createDirectories(home));
    for (int index = 0; index < 80; ++index)
        writeSkill(
            workspace, "budget" + std::to_string(1000 + index),
            "needle" + std::to_string(index) + " " + std::string(5000, 'x'),
            index == 79 ? "OMITTED SKILL FIRST BODY\n" : std::string(1, static_cast<char>(0xFF)));
    MaiAgent::Options options;
    options.skillsEnabled = true;
    options.skillOptions.homeDirectory = home;
    std::string session;
    std::string header;
    std::string firstInput;
    std::string firstInputId;
    int budgetRequests = 0;
    int resumedRequests = 0;
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store.isOk());
        if (!store) return;
        auto model = std::make_unique<SkillModel>();
        model->windowTokens = 12000;
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink,
                             int attempt) {
            ++budgetRequests;
            MaiModelRequest measured;
            measured.baseInstructions = request.baseInstructions;
            CHECK(maiEstimateModelInputTokens(measured).tokens - 16 <= 240);
            CHECK(request.baseInstructions.find("omitted") != std::string::npos);
            CHECK(request.baseInstructions.find("OMITTED SKILL FIRST BODY") == std::string::npos);
            if (attempt == 1) {
                header = request.baseInstructions;
                CHECK(header.find("budget1079") == std::string::npos);
                for (const auto& message : request.messages)
                    if (message.role == MaiModelRole::User) {
                        firstInput = message.content;
                        firstInputId = message.sourceMessageId;
                    }
                sink.onToolCall({"budget_discover", "skill_read",
                                 R"({"mode":"discover","query":"needle79","limit":1})"});
            } else if (attempt == 2) {
                CHECK(request.baseInstructions == header);
                CHECK(hasToolResult(request, "budget1079"));
                CHECK(!hasToolResult(request, "OMITTED SKILL FIRST BODY"));
                sink.onToolCall({"budget_read", "skill_read", R"({"skill":"budget1079"})"});
            } else {
                CHECK(attempt == 3);
                CHECK(request.baseInstructions == header);
                CHECK(hasToolResult(request, "OMITTED SKILL FIRST BODY"));
                sink.onText("Metadata discovery found the omitted skill before its body was read.");
            }
            return MaiError{};
        };
        MaiAgent agent(std::move(store.value()), std::move(model), nullptr, options);
        session = createSession(agent, workspace);
        prompt(agent, session, "Find the needle79 skill and use its instructions.");
        CHECK(budgetRequests == 3);
        CHECK(!header.empty());
        CHECK(!firstInputId.empty());
        MaiMessage raw;
        CHECK(agent.getMessage(session, firstInputId, raw));
        CHECK(raw.text() == "Find the needle79 skill and use its instructions.");
    }
    writeSkill(workspace, "budget1079", "Changed metadata after a real process-style restart",
               "OMITTED SKILL SECOND BODY\n");
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store.isOk());
        if (!store) return;
        const auto savedHeader =
            store.value()->getSessionContext(session, "skills.catalog.prompt.v1");
        CHECK(savedHeader.isOk());
        if (savedHeader) CHECK(savedHeader.value() == header);
        auto model = std::make_unique<SkillModel>();
        // 新模型窗口可以改变新目录预算，已保存的提示与历史输入仍逐字节保持。
        model->windowTokens = 24000;
        model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink, int) {
            ++resumedRequests;
            CHECK(request.baseInstructions == header);
            bool oldInputSeen = false;
            std::string currentInput;
            for (const auto& message : request.messages) {
                if (message.role != MaiModelRole::User) continue;
                if (message.sourceMessageId == firstInputId) {
                    CHECK(message.content == firstInput);
                    oldInputSeen = true;
                }
                currentInput = message.content;
            }
            CHECK(oldInputSeen);
            CHECK(currentInput.find("OMITTED SKILL SECOND BODY") != std::string::npos);
            CHECK(currentInput.find("OMITTED SKILL FIRST BODY") == std::string::npos);
            sink.onText(
                "The reopened session kept its exact prefix and selected the current body.");
            return MaiError{};
        };
        MaiAgent agent(std::move(store.value()), std::move(model), nullptr, options);
        prompt(agent, session, "Use $budget1079 explicitly.");
        CHECK(resumedRequests == 1);
    }
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root =
        child(MaiFileSystem::temporaryDirectory(),
              "mai-skill-runtime-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(!MaiFileSystem::createDirectories(root));
    testBundledSkillIsLoadedOnDemand(root);
    testStoreContextWinnerAndOwner();
    testWorkerDiscoveryPinnedPrefixAndFreshBody(root);
    testSavedHeaderWinnerAndDisabledDefault(root);
    testExplicitSelectionBeforeModelAndSqliteReopen(root);
    testEmptyCatalogAllowsNewWorkspaceScope(root);
    testInstalledSkillsRefreshAtNextInput(root);
    testCatalogMetadataOnlyAnnouncesTransitions(root);
    testWindowBudgetDiscoveryAndSqliteRestart(root);
    MaiFileSystem::removeRecursively(root);
    std::printf("MaiSkillRuntimeTests: %d failures\n", failures.load());
    return failures.load() == 0 ? 0 : 1;
}

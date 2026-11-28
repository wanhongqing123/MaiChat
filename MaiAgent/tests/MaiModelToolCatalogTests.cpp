// 文件作用：验证目录搜索、按需声明和跨会话恢复的前缀稳定性。
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <json.hpp>
#include <openssl/evp.h>

#include "MaiMemoryStore.h"
#include "MaiArkMediaTools.h"
#include "MaiFileSystem.h"
#include "MaiSqliteStore.h"
#include "MaiModelToolCatalog.h"
#include "MaiTool.h"
#include "MaiToolSearchTool.h"

static int failures = 0;
#define CHECK(value)                                       \
    do {                                                   \
        if (!(value)) {                                    \
            std::printf("FAIL %d %s\n", __LINE__, #value); \
            ++failures;                                    \
        }                                                  \
    } while (0)

namespace {
class MaiCatalogFaultStore final : public MaiSessionStore {
public:
    std::unique_ptr<MaiSessionStore> inner = makeMaiMemoryStore();
    bool failed = false;
    bool readFailed = false;
    bool contextUnsupported = false;
    MaiError lastReadError() const override {
        return readFailed ? MaiError{MaiErrorCode::Internal, "Owned storage read failed."}
                          : inner->lastReadError();
    }
    void putSession(const MaiSession& value) override {
        inner->putSession(value);
    }
    bool getSession(const std::string& id, MaiSession& out) const override {
        return inner->getSession(id, out);
    }
    std::vector<MaiSession> listSessions() const override {
        return inner->listSessions();
    }
    bool removeSession(const std::string& id) override {
        return inner->removeSession(id);
    }
    bool clearMessages(const std::string& id) override {
        return inner->clearMessages(id);
    }
    void putMessage(const std::string& id, const MaiMessage& value) override {
        inner->putMessage(id, value);
    }
    std::vector<MaiMessage> listMessages(const std::string& id) const override {
        return inner->listMessages(id);
    }
    std::vector<MaiMessage> listMessagesPage(const std::string& id, const std::string& before,
                                             std::size_t limit) const override {
        return inner->listMessagesPage(id, before, limit);
    }
    bool getMessage(const std::string& id, const std::string& message,
                    MaiMessage& out) const override {
        return inner->getMessage(id, message, out);
    }
    void recoverInterruptedTools(const std::string& error, std::int64_t completed) override {
        inner->recoverInterruptedTools(error, completed);
    }
    MaiError lastWriteError() const override {
        return failed ? MaiError{MaiErrorCode::Internal, "Owned storage write failed."}
                      : inner->lastWriteError();
    }
    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutation) override {
        return inner->mutateSession(id, mutation);
    }
    MaiResult<std::string> getSessionContext(const std::string& id,
                                             const std::string& key) const override {
        if (contextUnsupported) return {MaiErrorCode::NotSupported, "Owned nonpersistent backend."};
        return inner->getSessionContext(id, key);
    }
    MaiResult<std::string> saveSessionContext(const std::string& id, const std::string& key,
                                              const std::string& value) override {
        if (failed) return {MaiErrorCode::Internal, "Injected session context write failure."};
        if (contextUnsupported) return {MaiErrorCode::NotSupported, "Owned nonpersistent backend."};
        return inner->saveSessionContext(id, key, value);
    }
    MaiError insertSpecialistTask(const MaiSpecialistTask& value) override {
        return inner->insertSpecialistTask(value);
    }
    bool getSpecialistTask(const std::string& id, const std::string& owner,
                           MaiSpecialistTask& out) const override {
        return inner->getSpecialistTask(id, owner, out);
    }
    MaiError deleteCompletedSpecialistTask(const std::string& id,
                                           const std::string& owner) override {
        return inner->deleteCompletedSpecialistTask(id, owner);
    }
    MaiError appendSpecialistText(const std::string& id, const std::string& owner,
                                  const std::string& text, MaiMillis checked) override {
        return inner->appendSpecialistText(id, owner, text, checked);
    }
    MaiError updateSpecialistProgress(const std::string& id, const std::string& owner,
                                      MaiMillis checked) override {
        return inner->updateSpecialistProgress(id, owner, checked);
    }
    MaiError recoverStatusCheckFailures() override {
        return inner->recoverStatusCheckFailures();
    }
    MaiError finishSpecialistTask(const std::string& id, const std::string& owner,
                                  MaiSpecialistTaskStatus status, const std::string& text,
                                  const std::string& path, MaiMillis finished) override {
        return inner->finishSpecialistTask(id, owner, status, text, path, finished);
    }
    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const override {
        return inner->listActiveSpecialistTasks(limit);
    }
    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t limit) const override {
        return inner->listUnnotifiedSpecialistTasks(limit);
    }
    MaiResult<std::string> reserveSpecialistNotification(const std::string& id,
                                                         const std::string& owner,
                                                         const std::string& message,
                                                         MaiMillis attempted) override {
        return inner->reserveSpecialistNotification(id, owner, message, attempted);
    }
    MaiError markSpecialistNotified(const std::string& id, const std::string& owner,
                                    const std::string& message, MaiMillis notified) override {
        return inner->markSpecialistNotified(id, owner, message, notified);
    }
    MaiError markSpecialistResultAlreadyDelivered(const std::string& id, const std::string& owner,
                                                  MaiMillis notified) override {
        return inner->markSpecialistResultAlreadyDelivered(id, owner, notified);
    }
};
class MaiCatalogTestTool final : public MaiTool {
public:
    MaiCatalogTestTool(std::string toolName, std::string detail = "metadata",
                       int* schemaCalls = nullptr)
        : mName(std::move(toolName)), mDetail(std::move(detail)), mSchemaCalls(schemaCalls) {}
    std::string name() const override {
        return mName;
    }
    std::string description() const override {
        return mDetail;
    }
    std::string parametersSchema() const override {
        if (mSchemaCalls) ++*mSchemaCalls;
        if (mDetail == "CHANGED" || mDetail == "MODIFIED")
            return R"({"type":"object","properties":{"changed":{"type":"integer"}}})";
        return R"({"type":"object","properties":{"schema_secret":{"type":"string"}}})";
    }
    bool isAvailableForModel(const std::string& model) const override {
        return mName != "unsupported" || model == "other";
    }
    MaiToolSpec specForModel(const std::string&) const override {
        MaiToolSpec result{mName, mDetail, parametersSchema()};
        if (mName == "apply_patch" || mName == "alpha")
            result.inputGrammar = "start: \"" + mDetail + "\"\n";
        return result;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success("called");
    }

private:
    std::string mName;
    std::string mDetail;
    int* mSchemaCalls = nullptr;
};
bool has(const std::vector<MaiToolSpec>& tools, const std::string& name) {
    return std::any_of(tools.begin(), tools.end(),
                       [&](const auto& tool) { return tool.name == name; });
}
const MaiToolSpec* findSpec(const std::vector<MaiToolSpec>& tools, const std::string& name) {
    const auto found = std::find_if(tools.begin(), tools.end(),
                                    [&](const auto& tool) { return tool.name == name; });
    return found == tools.end() ? nullptr : &*found;
}
void create(MaiSessionStore& store, const std::string& id) {
    MaiSession session;
    session.id = id;
    store.putSession(session);
}
void run() {
    auto store = makeMaiMemoryStore();
    create(*store, "first");
    create(*store, "second");
    create(*store, "legacy");
    MaiToolRegistry registry;
    int schemaCalls = 0;
    for (const auto* name :
         {"exec_command", "tool_search", "session_history", "tool_output_read", "skill_read",
          "view_image", "apply_patch", "write_stdin", "alpha", "beta", "unsupported"})
        registry.add(std::make_unique<MaiCatalogTestTool>(name, "metadata", &schemaCalls));
    registry.addInternal(std::make_unique<MaiCatalogTestTool>("internal"));
    MaiTurnContext context{"first", "current", "model", {}, {}};
    MaiMessage input;
    input.id = "input";
    input.role = MaiRole::User;
    MaiMessage current;
    current.id = "current";
    current.role = MaiRole::Assistant;
    store->putMessage("first", input);
    store->putMessage("first", current);
    MaiModelToolCatalog catalog(*store, registry);
    auto initial = catalog.exposedSpecs(context);
    CHECK(initial && initial.value().size() == 8);
    CHECK(initial && has(initial.value(), "session_history") &&
          has(initial.value(), "tool_output_read"));
    CHECK(initial && !has(initial.value(), "alpha") && !has(initial.value(), "internal"));
    CHECK(schemaCalls == 8);
    MaiToolCatalogQuery query;
    query.query = "ALPHA";
    auto page = catalog.search(context, query);
    CHECK(page && page.value().total == 1 && page.value().tools.size() == 1);
    CHECK(page && page.value().tools.size() == 1 && page.value().tools[0].name == "alpha");
    CHECK(catalog.exposedSpecs(context).value().size() == 8);
    CHECK(schemaCalls == 8);
    query.select = {"alpha"};
    CHECK(catalog.search(context, query));
    auto appended = catalog.exposedSpecs(context);
    CHECK(appended && appended.value().size() == 9 && appended.value().back().name == "alpha");
    if (initial && appended && appended.value().size() >= initial.value().size())
        for (std::size_t index = 0; index < initial.value().size(); ++index)
            CHECK(appended.value()[index].parametersJson == initial.value()[index].parametersJson &&
                  appended.value()[index].name == initial.value()[index].name);
    context.sessionId = "second";
    CHECK(catalog.exposedSpecs(context).value().size() == 8);
    context.sessionId = "first";
    context.model = "other";
    CHECK(!has(catalog.exposedSpecs(context).value(), "alpha"));
    context.model = "model";
    for (const auto* name : {"unknown", "internal", "unsupported"}) {
        query.select = {name};
        CHECK(!catalog.search(context, query));
    }
    query.select = {"alpha", "beta", "exec_command", "tool_search", "skill_read", "view_image"};
    CHECK(!catalog.search(context, query));
    query.select.clear();
    query.limit = 11;
    CHECK(!catalog.search(context, query));
    query.limit = 1;
    query.offset = 1000;
    CHECK(catalog.search(context, query).value().tools.empty());
    registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "CHANGED"));
    MaiModelToolCatalog reopened(*store, registry);
    auto restored = reopened.exposedSpecs(context);
    CHECK(restored && restored.value().size() == 9 &&
          restored.value().back().description == "metadata");
    CHECK(restored && appended &&
          restored.value().back().parametersJson == appended.value().back().parametersJson);
    MaiMessage old;
    old.id = "old";
    old.role = MaiRole::Assistant;
    store->putMessage("legacy", old);
    context.sessionId = "legacy";
    auto legacy = reopened.exposedSpecs(context);
    CHECK(legacy && has(legacy.value(), "alpha") && has(legacy.value(), "beta"));
    registry.add(std::make_unique<MaiCatalogTestTool>("gamma"));
    CHECK(!has(reopened.exposedSpecs(context).value(), "gamma"));
    const auto legacyBeforeSummary = reopened.exposedSpecs(context);
    CHECK(reopened.capabilitySummary(context));
    const auto legacyAfterSummary = reopened.exposedSpecs(context);
    CHECK(legacyAfterSummary.value().size() == legacyBeforeSummary.value().size());
    for (std::size_t i = 0; i < legacyBeforeSummary.value().size(); ++i) {
        CHECK(legacyAfterSummary.value()[i].name == legacyBeforeSummary.value()[i].name);
        CHECK(legacyAfterSummary.value()[i].description ==
              legacyBeforeSummary.value()[i].description);
        CHECK(legacyAfterSummary.value()[i].parametersJson ==
              legacyBeforeSummary.value()[i].parametersJson);
    }

    // 保存候选超过后端内容上限时必须失败，不能激活未保存的声明。
    registry.add(
        std::make_unique<MaiCatalogTestTool>("oversized", std::string(4 * 1024 * 1024, 'x')));
    query = {};
    query.select = {"oversized"};
    const auto beforeFailure = reopened.exposedSpecs(context).value().size();
    CHECK(!reopened.search(context, query));
    CHECK(reopened.exposedSpecs(context).value().size() == beforeFailure);
}

void testNewSessionMarkerBeforePendingInputs() {
    MaiCatalogFaultStore store;
    MaiToolRegistry registry;
    registry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    registry.add(std::make_unique<MaiCatalogTestTool>("alpha"));
    create(store, "new-pending");
    MaiModelToolCatalog catalog(store, registry);
    CHECK(!catalog.initializeNewSession("new-pending"));
    MaiMessage user;
    user.id = "001";
    user.role = MaiRole::User;
    MaiMessage current;
    current.id = "002";
    current.role = MaiRole::Assistant;
    MaiMessage pending = user;
    pending.id = "003";
    pending.pendingInput = true;
    store.putMessage("new-pending", user);
    store.putMessage("new-pending", current);
    store.putMessage("new-pending", pending);
    const auto initial = catalog.exposedSpecs({"new-pending", "002", "model", {}, {}});
    CHECK(initial && initial.value().size() == 1 && !has(initial.value(), "alpha"));
    MaiModelToolCatalog reopened(store, registry);
    const auto restored = reopened.exposedSpecs({"new-pending", "002", "model", {}, {}});
    CHECK(restored && restored.value().size() == 1);
    create(store, "existing");
    user.id = "old-user";
    current.id = "old-assistant";
    store.putMessage("existing", user);
    store.putMessage("existing", current);
    CHECK(has(catalog.exposedSpecs({"existing", "next-turn", "model", {}, {}}).value(), "alpha"));
    CHECK(!catalog.initializeNewSession("existing"));
    MaiModelToolCatalog oldReopened(store, registry);
    CHECK(
        has(oldReopened.exposedSpecs({"existing", "next-turn", "model", {}, {}}).value(), "alpha"));
    create(store, "failed-save");
    store.failed = true;
    CHECK(catalog.initializeNewSession("failed-save").code() == MaiErrorCode::Internal);
    store.failed = false;
    CHECK(!store.getSessionContext("failed-save", "model-tool-catalog-mode-v1"));
    store.putMessage("failed-save", user);
    store.putMessage("failed-save", current);
    CHECK(
        has(catalog.exposedSpecs({"failed-save", "next-turn", "model", {}, {}}).value(), "alpha"));
}

void testBuiltinDeclarationBudget() {
    auto store = makeMaiMemoryStore();
    create(*store, "builtin-budget");
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto core = std::make_shared<MaiModelToolCatalog>(*store, registry);
    registry.add(makeMaiToolSearchTool(core));
    MaiTurnContext owner{"builtin-budget", {}, "glm-5.4", {}, {}};
    const auto initial = core->exposedSpecs(owner);
    CHECK(initial);
    if (!initial) return;
    const auto all = registry.specs(owner.model);
    const auto summary = core->capabilitySummary(owner);
    CHECK(summary && summary.value().size() <= 8192);
    CHECK(summary && summary.value().find("omitted") == std::string::npos);
    if (summary) {
        MaiModelRequest request;
        request.model = owner.model;
        request.baseInstructions = summary.value();
        std::printf("Builtin capability summary: %zu bytes/%zu estimated tokens\n",
                    summary.value().size(), maiEstimateModelInputTokens(request).tokens);
    }
    const auto bytes = [](const std::vector<MaiToolSpec>& specs) {
        std::size_t result = 0;
        for (const auto& spec : specs)
            result += spec.name.size() + spec.description.size() + spec.parametersJson.size();
        return result;
    };
    CHECK(initial.value().size() < all.size());
    CHECK(bytes(initial.value()) < bytes(all));
    CHECK(has(initial.value(), "session_history") && has(initial.value(), "tool_output_read"));
    std::printf(
        "Builtin declaration payload fields: all=%zu tools/%zu bytes; initial=%zu tools/%zu "
        "bytes\n",
        all.size(), bytes(all), initial.value().size(), bytes(initial.value()));
}

void testMetadataDoesNotReadCredentials() {
    int credentialReads = 0;
    MaiToolRegistry registry;
    registry.add(makeMaiSeedanceVideoTool([&] {
        ++credentialReads;
        return std::string{};
    }));
    const auto metadata = registry.metadata("glm-5.4");
    CHECK(metadata.size() == 1);
    CHECK(credentialReads == 0);
    CHECK(!metadata.empty() && metadata[0].description.find("paid") != std::string::npos);
    auto store = makeMaiMemoryStore();
    create(*store, "no-credential-summary");
    registry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    MaiModelToolCatalog catalog(*store, registry);
    CHECK(catalog.capabilitySummary({"no-credential-summary", {}, "glm-5.4", {}, {}}));
    CHECK(credentialReads == 0);
}

void testSummarySnapshot(MaiSessionStore& store) {
    create(store, "summary");
    create(store, "summary-other");
    MaiToolRegistry registry;
    int schemaCalls = 0;
    registry.add(std::make_unique<MaiCatalogTestTool>("tool_search", "Search", &schemaCalls));
    registry.add(std::make_unique<MaiCatalogTestTool>("beta", "Second purpose", &schemaCalls));
    registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "Read\nlocal \u6587\u4ef6 safely",
                                                      &schemaCalls));
    registry.add(std::make_unique<MaiCatalogTestTool>("unsupported", "Other model", &schemaCalls));
    registry.addInternal(
        std::make_unique<MaiCatalogTestTool>("hidden", "Hidden purpose", &schemaCalls));
    MaiModelToolCatalog catalog(store, registry);
    MaiTurnContext context{"summary", {}, "model", {}, {}};
    const auto summary = catalog.capabilitySummary(context);
    CHECK(summary &&
          summary.value().find("alpha: Read local \u6587\u4ef6 safely") != std::string::npos);
    CHECK(summary && summary.value().find("alpha") < summary.value().find("beta"));
    CHECK(summary && summary.value().find("unsupported") == std::string::npos);
    CHECK(summary && summary.value().find("hidden") == std::string::npos);
    CHECK(summary && summary.value().find("schema_secret") == std::string::npos);
    CHECK(summary && summary.value().find("next request") != std::string::npos);
    CHECK(schemaCalls == 0);
    const auto initial = catalog.exposedSpecs(context);
    CHECK(initial && initial.value().size() == 1 && schemaCalls == 1);
    MaiToolCatalogQuery query;
    query.select = {"alpha"};
    CHECK(catalog.search(context, query));
    CHECK(catalog.capabilitySummary(context).value() == summary.value());
    registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "Changed purpose"));
    MaiModelToolCatalog reopened(store, registry);
    CHECK(reopened.capabilitySummary(context).value() == summary.value());
    CHECK(findSpec(reopened.exposedSpecs(context).value(), "alpha")->description ==
          "Read\nlocal \u6587\u4ef6 safely");
    context.sessionId = "summary-other";
    CHECK(reopened.capabilitySummary(context).value().find("Changed purpose") != std::string::npos);
    context.sessionId = "summary";
    context.model = "other";
    CHECK(reopened.capabilitySummary(context).value().find("unsupported") != std::string::npos);
    context.model.clear();
    CHECK(!reopened.capabilitySummary(context));
}

void testSummaryBoundsFailureAndRace() {
    MaiCatalogFaultStore store;
    create(store, "bounds");
    MaiToolRegistry registry;
    registry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    std::string detail;
    for (int i = 0; i < 300; ++i) detail += "\u6587";
    for (int i = 0; i < 600; ++i)
        registry.add(
            std::make_unique<MaiCatalogTestTool>("capability_" + std::to_string(1000 + i), detail));
    MaiModelToolCatalog catalog(store, registry);
    MaiTurnContext context{"bounds", {}, "model", {}, {}};
    store.failed = true;
    CHECK(!catalog.capabilitySummary(context));
    store.failed = false;
    registry.add(std::make_unique<MaiCatalogTestTool>("aaa_saved_after_failure", "Fresh purpose"));
    const auto summary = catalog.capabilitySummary(context);
    CHECK(summary && summary.value().size() <= 8192);
    CHECK(summary && !maiValidateSessionContextContent(summary.value()));
    CHECK(summary && summary.value().find("aaa_saved_after_failure") != std::string::npos);
    CHECK(summary && summary.value().find("omitted") != std::string::npos);
    CHECK(summary && summary.value().find("tool_search") != std::string::npos);
    create(store, "race");
    context.sessionId = "race";
    MaiToolRegistry leftRegistry, rightRegistry;
    leftRegistry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    rightRegistry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    leftRegistry.add(std::make_unique<MaiCatalogTestTool>("left_candidate", "Left purpose"));
    rightRegistry.add(std::make_unique<MaiCatalogTestTool>("right_candidate", "Right purpose"));
    MaiModelToolCatalog leftCatalog(store, leftRegistry), rightCatalog(store, rightRegistry);
    std::string left, right;
    std::thread first([&] { left = leftCatalog.capabilitySummary(context).value(); });
    std::thread second([&] { right = rightCatalog.capabilitySummary(context).value(); });
    first.join();
    second.join();
    CHECK(!left.empty() && left == right);
    CHECK((left.find("left_candidate") != std::string::npos) !=
          (left.find("right_candidate") != std::string::npos));
    create(store, "read-failure");
    context.sessionId = "read-failure";
    store.readFailed = true;
    CHECK(!catalog.capabilitySummary(context));
    store.readFailed = false;
    CHECK(catalog.capabilitySummary(context));
    create(store, "empty");
    MaiToolRegistry empty;
    MaiModelToolCatalog emptyCatalog(store, empty);
    CHECK(emptyCatalog.capabilitySummary({"empty", {}, "model", {}, {}}).value().empty());
    create(store, "missing-route");
    empty.add(std::make_unique<MaiCatalogTestTool>("unreachable"));
    CHECK(emptyCatalog.capabilitySummary({"missing-route", {}, "model", {}, {}}).value().empty());
    create(store, "utf8-boundary");
    MaiToolRegistry unicode;
    unicode.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    unicode.add(
        std::make_unique<MaiCatalogTestTool>("unicode", std::string(124, 'a') + "\u6587\u4ef6"));
    MaiModelToolCatalog unicodeCatalog(store, unicode);
    const auto bounded = unicodeCatalog.capabilitySummary({"utf8-boundary", {}, "model", {}, {}});
    CHECK(bounded && !maiValidateSessionContextContent(bounded.value()));
    CHECK(bounded && bounded.value().find(std::string(124, 'a') + "...") != std::string::npos);
}

void testSummaryUnsupportedPersistenceBoundary() {
    MaiCatalogFaultStore store;
    store.contextUnsupported = true;
    create(store, "volatile-summary");
    MaiToolRegistry registry;
    registry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
    registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "First purpose"));
    MaiModelToolCatalog catalog(store, registry);
    const MaiTurnContext context{"volatile-summary", {}, "model", {}, {}};
    const auto first = catalog.capabilitySummary(context);
    CHECK(first && first.value().find("First purpose") != std::string::npos);
    registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "New instance purpose"));
    CHECK(catalog.capabilitySummary(context).value() == first.value());
    MaiModelToolCatalog reopened(store, registry);
    const auto next = reopened.capabilitySummary(context);
    CHECK(next && next.value() != first.value());
    CHECK(next && next.value().find("New instance purpose") != std::string::npos);
}

void testSummaryFirstBudgetFreezesTheWinner() {
    auto store = makeMaiMemoryStore();
    create(*store, "small-first-summary");
    create(*store, "large-first-summary");
    MaiToolRegistry registry;
    int schemaCalls = 0;
    registry.add(std::make_unique<MaiCatalogTestTool>("tool_search", "Search", &schemaCalls));
    for (int i = 0; i < 40; ++i)
        registry.add(std::make_unique<MaiCatalogTestTool>("capability_" + std::to_string(i),
                                                          std::string(200, 'p'), &schemaCalls));
    MaiModelToolCatalog catalog(*store, registry);
    MaiTurnContext context{"small-first-summary", {}, "model", {}, {}};
    const auto small = catalog.capabilitySummary(context, 1536);
    CHECK(small && small.value().size() <= 1536);
    CHECK(small && small.value().find("omitted") == std::string::npos);
    CHECK(catalog.capabilitySummary(context, 8192).value() == small.value());
    MaiModelToolCatalog reopened(*store, registry);
    CHECK(reopened.capabilitySummary(context, 512).value() == small.value());
    context.sessionId = "large-first-summary";
    const auto large = catalog.capabilitySummary(context);
    CHECK(large && large.value().size() > 1536 && large.value().size() <= 8192);
    CHECK(catalog.capabilitySummary(context, 512).value() == large.value());
    CHECK(reopened.capabilitySummary(context, 512).value() == large.value());
    CHECK(schemaCalls == 0);
}

void testPersistentReopenAndToolAdapter() {
    const auto directory = MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
        "mai-tool-catalog-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    CHECK(!MaiFileSystem::createDirectories(directory));
    const auto database = directory.append(MaiFilePath::fromUtf8("sessions.db"));
    MaiTurnContext owner{"persisted", {}, "model", {}, {}};
    {
        auto opened = makeMaiSqliteStore(database.toUtf8());
        CHECK(opened);
        if (!opened) return;
        create(*opened.value(), owner.sessionId);
        testSummarySnapshot(*opened.value());
        MaiToolRegistry registry;
        registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "ORIGINAL"));
        registry.add(std::make_unique<MaiCatalogTestTool>("apply_patch", "PATCH ORIGINAL"));
        auto core = std::make_shared<MaiModelToolCatalog>(*opened.value(), registry);
        auto tool = makeMaiToolSearchTool(core);
        registry.add(makeMaiToolSearchTool(core));
        CHECK(core->exposedSpecs(owner).value().size() == 2);
        const auto summary = core->capabilitySummary(owner);
        CHECK(summary && summary.value().find("alpha: ORIGINAL") != std::string::npos);
        MaiToolContext context;
        context.sessionId = owner.sessionId;
        context.model = owner.model;
        for (const auto* invalid :
             {"[]", R"({"unknown":1})", R"({"limit":0})", R"({"limit":11})", R"({"offset":-1})",
              R"({"offset":1.5})", R"({"select":[1]})", R"({"select":["alpha","alpha"]})"})
            CHECK(tool->execute(invalid, context).hasError());
        const auto selected = tool->execute(R"({"query":"alpha","select":["alpha"]})", context);
        CHECK(!selected.hasError());
        CHECK(selected.output().find("schema_secret") == std::string::npos);
        CHECK(selected.output().find("parameters") == std::string::npos);
        CHECK(core->exposedSpecs(owner).value().size() == 3);
    }
    {
        auto opened = makeMaiSqliteStore(database.toUtf8());
        CHECK(opened);
        if (!opened) return;
        MaiToolRegistry registry;
        registry.add(std::make_unique<MaiCatalogTestTool>("alpha", "MODIFIED"));
        registry.add(std::make_unique<MaiCatalogTestTool>("tool_search", "MODIFIED SEARCH"));
        registry.add(std::make_unique<MaiCatalogTestTool>("apply_patch", "PATCH MODIFIED"));
        MaiModelToolCatalog reopened(*opened.value(), registry);
        const auto summary = reopened.capabilitySummary(owner);
        CHECK(summary && summary.value().find("alpha: ORIGINAL") != std::string::npos);
        CHECK(summary && summary.value().find("MODIFIED") == std::string::npos);
        const auto specs = reopened.exposedSpecs(owner);
        CHECK(specs && specs.value().size() == 3 && specs.value().back().description == "ORIGINAL");
        const auto* patch = specs ? findSpec(specs.value(), "apply_patch") : nullptr;
        CHECK(patch && patch->inputGrammar == "start: \"PATCH ORIGINAL\"\n");
        CHECK(specs && specs.value().back().inputGrammar == "start: \"ORIGINAL\"\n");
    }
    MaiFileSystem::removeRecursively(directory);
}

void testExplicitProtocolUpgradeKeepsOldSnapshot(int previousVersion) {
    const auto directory = MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
        "mai-tool-upgrade-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    CHECK(!MaiFileSystem::createDirectories(directory));
    {
        auto opened =
            makeMaiSqliteStore(directory.append(MaiFilePath::fromUtf8("sessions.db")).toUtf8());
        CHECK(opened);
        if (!opened) return;
        auto store = std::move(opened.value());
        create(*store, "upgrade");
        const std::string model = "model";
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int length = 0;
        CHECK(EVP_Digest(model.data(), model.size(), digest, &length, EVP_sha256(), nullptr) == 1);
        std::string suffix;
        for (unsigned int index = 0; index < length; ++index) {
            char bytes[3]{};
            std::snprintf(bytes, sizeof(bytes), "%02x", digest[index]);
            suffix += bytes;
        }
        const auto oldKey = "model-tools-v" + std::to_string(previousVersion) + "-" + suffix;
        const std::string old =
            previousVersion == 1
                ? R"({"model":"model","specs":[{"name":"file_patch","description":"old","parameters":"{}"}]})"
            : previousVersion == 4
                ? R"({"model":"model","specs":[{"name":"file_read","description":"old loaded tool","parameters":"{}","input_grammar":""}]})"
            : previousVersion == 2
                ? R"({"model":"model","specs":[{"name":"desktop_computer","description":"old","parameters":"{}","input_grammar":""}]})"
                : R"({"model":"model","specs":[{"name":"computer","description":"old","parameters":"{\"properties\":{\"action\":{\"type\":\"string\"}}}","input_grammar":""}]})";
        CHECK(store->saveSessionContext("upgrade", "model-tool-catalog-mode-v1", "bounded"));
        CHECK(store->saveSessionContext("upgrade", oldKey, old));
        MaiToolRegistry registry;
        registry.add(std::make_unique<MaiCatalogTestTool>("apply_patch", "CURRENT"));
        registry.add(std::make_unique<MaiCatalogTestTool>("tool_search"));
        registry.add(std::make_unique<MaiCatalogTestTool>("alpha"));
        registry.add(std::make_unique<MaiCatalogTestTool>("computer", "SKY-WINDOW2"));
        MaiModelToolCatalog catalog(*store, registry);
        const MaiTurnContext context{"upgrade", {}, model, {}, {}};
        const auto upgraded = catalog.exposedSpecs(context);
        CHECK(upgraded && has(upgraded.value(), "apply_patch") &&
              !has(upgraded.value(), "file_patch"));
        CHECK(upgraded && !has(upgraded.value(), "desktop_computer"));
        CHECK(upgraded && !has(upgraded.value(), "alpha"));
        CHECK(upgraded && !has(upgraded.value(), "file_read"));
        CHECK(store->getSessionContext("upgrade", oldKey).value() == old);
        const auto current = store->getSessionContext("upgrade", "model-tools-v5-" + suffix);
        CHECK(current && current.value().find("input_grammar") != std::string::npos);
        MaiToolCatalogQuery computerQuery;
        computerQuery.select = {"computer"};
        CHECK(catalog.search(context, computerQuery));
        const auto withComputer = catalog.exposedSpecs(context);
        CHECK(withComputer && has(withComputer.value(), "computer") &&
              !has(withComputer.value(), "desktop_computer"));
        CHECK(withComputer &&
              findSpec(withComputer.value(), "computer")->description == "SKY-WINDOW2");
        registry.add(std::make_unique<MaiCatalogTestTool>("apply_patch", "CHANGED"));
        MaiModelToolCatalog restarted(*store, registry);
        const auto pinned = restarted.exposedSpecs(context);
        const auto* patch = pinned ? findSpec(pinned.value(), "apply_patch") : nullptr;
        CHECK(patch && patch->description == "CURRENT");
        CHECK(patch && patch->inputGrammar == "start: \"CURRENT\"\n");
        const auto configuration = store->readLatestSessionRecord("upgrade", "response_item");
        CHECK(configuration && !configuration.value().empty());
        if (configuration && !configuration.value().empty()) {
            const auto record = nlohmann::json::parse(configuration.value());
            CHECK(record.value("type", "") == "configuration_update" &&
                  record.value("configuration", "") == "model_tool_catalog" &&
                  record.value("previous_version", 0) == previousVersion &&
                  record.value("version", 0) == 5 &&
                  record.value("previous_snapshot_key", "") == oldKey &&
                  record.value("snapshot_key", "") == "model-tools-v5-" + suffix);
        }
    }
    std::size_t upgrades = 0;
    MaiFileSystem::walk(
        directory.append(MaiFilePath::fromUtf8("sessions")),
        [&](const MaiFileEntry& entry) {
            if (!entry.isDirectory && entry.nameUtf8.compare(0, 8, "rollout-") == 0) {
                std::string source;
                CHECK(!MaiFileSystem::readFile(entry.path, source));
                for (std::size_t offset = 0; offset < source.size();) {
                    const auto end = source.find('\n', offset);
                    CHECK(end != std::string::npos);
                    if (end == std::string::npos) break;
                    const auto row = nlohmann::json::parse(source.substr(offset, end - offset));
                    if (row.value("type", "") == "response_item" &&
                        row["payload"].value("type", "") == "configuration_update")
                        ++upgrades;
                    offset = end + 1;
                }
            }
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError&) { CHECK(false); });
    CHECK(upgrades == 1);
    MaiFileSystem::removeRecursively(directory);
}

void testNativePatchEstimationUsesVisibleGrammarAndInput() {
    MaiModelRequest request;
    request.model = "gpt-6.1-sol";
    request.tools.push_back({"apply_patch", "Patch files", "{}", "start: \"patch\"\n"});
    const auto first = maiEstimateModelInputTokens(request).tokens;
    request.tools.front().parametersJson = std::string(9000, 'j');
    CHECK(maiEstimateModelInputTokens(request).tokens == first);
    request.tools.front().inputGrammar += std::string(9000, 'g');
    CHECK(maiEstimateModelInputTokens(request).tokens == first + 3000);
    const std::string patch = "*** Begin Patch\n*** Add File: a\n+\"quoted\"\\line\n*** End Patch";
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.invocations.push_back(
        {"call_patch", "apply_patch", nlohmann::json{{"patch", patch}}.dump()});
    request.messages.push_back(assistant);
    const auto compact = maiEstimateModelInputTokens(request).tokens;
    request.messages.front().invocations.front().arguments =
        nlohmann::json{{"patch", patch}}.dump(8);
    CHECK(maiEstimateModelInputTokens(request).tokens == compact);
    const auto unchanged = request.messages.front().invocations.front().arguments;
    request.messages.front().continuation = MaiModelContinuationState{
        "responses.output.v1", "openai", request.model,
        nlohmann::json::array({nlohmann::json{{"type", "custom_tool_call"},
                                              {"call_id", "call_patch"},
                                              {"name", "apply_patch"},
                                              {"input", patch}}})
            .dump()};
    const auto raw = maiEstimateModelInputTokens(request).tokens;
    auto state = nlohmann::json::parse(request.messages.front().continuation->payload);
    state[0]["arguments"] = std::string(12000, 'x');
    request.messages.front().continuation->payload = state.dump(4);
    CHECK(maiEstimateModelInputTokens(request).tokens == raw);
    CHECK(request.messages.front().invocations.front().arguments == unchanged);
}
}  // namespace
int main() {
    run();
    testNewSessionMarkerBeforePendingInputs();
    testPersistentReopenAndToolAdapter();
    testBuiltinDeclarationBudget();
    testMetadataDoesNotReadCredentials();
    {
        auto store = makeMaiMemoryStore();
        testSummarySnapshot(*store);
    }
    testSummaryBoundsFailureAndRace();
    testSummaryUnsupportedPersistenceBoundary();
    testSummaryFirstBudgetFreezesTheWinner();
    testExplicitProtocolUpgradeKeepsOldSnapshot(1);
    testExplicitProtocolUpgradeKeepsOldSnapshot(2);
    testExplicitProtocolUpgradeKeepsOldSnapshot(3);
    testExplicitProtocolUpgradeKeepsOldSnapshot(4);
    testNativePatchEstimationUsesVisibleGrammarAndInput();
    std::printf("MaiModelToolCatalogTests: %d failures\n", failures);
    return failures ? 1 : 0;
}

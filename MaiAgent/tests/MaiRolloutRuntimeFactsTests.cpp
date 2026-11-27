// 文件作用：真实回环 SSE 验证完成原始 item 的边界及失败尾巴隔离。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiResponsesClient.h"
#include "MaiMemoryStore.h"
#include "MaiRolloutRuntimeFacts.h"
#include "MaiContextCompactor.h"
#include "MaiIdGenerator.h"
#include "MaiTurnRunner.h"
#include "MaiThread.h"

using nlohmann::json;

class CaptureStore final : public MaiSessionStore {
public:
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

    std::unique_ptr<MaiSessionStore> inner = makeMaiMemoryStore();
    std::vector<std::pair<std::string, json>> records;
    bool failRead = false;
    bool failWrite = false;
    mutable bool historyReadFailed = false;
    std::optional<MaiContextCheckpoint> checkpoint;
    bool getContextCheckpoint(const std::string&, MaiContextCheckpoint& out) const override {
        if (!checkpoint) return false;
        out = *checkpoint;
        return true;
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
        if (failRead) {
            historyReadFailed = true;
            return {};
        }
        return inner->listMessages(id);
    }
    std::vector<MaiMessage> listMessagesPage(const std::string& id, const std::string& before,
                                             std::size_t limit) const override {
        return inner->listMessagesPage(id, before, limit);
    }
    void recoverInterruptedTools(const std::string& error, std::int64_t completed) override {
        inner->recoverInterruptedTools(error, completed);
    }
    MaiError lastWriteError() const override {
        return inner->lastWriteError();
    }
    MaiError lastReadError() const override {
        return historyReadFailed ? MaiError{MaiErrorCode::Internal, "fixture source read failed"}
                                 : MaiError{};
    }
    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutator) override {
        return inner->mutateSession(id, mutator);
    }
    bool getMessage(const std::string& sid, const std::string& mid,
                    MaiMessage& out) const override {
        return inner->getMessage(sid, mid, out);
    }
    MaiError appendSessionRecord(const std::string&, const std::string& type,
                                 const std::string& payload) override {
        if (failWrite) return {MaiErrorCode::Internal, "fixture source write failed"};
        // 真实联合后端按源身份与载荷去重；捕获夹具保留相同的不可变记录契约。
        const auto value = json::parse(payload);
        if (type == "response_item")
            for (const auto& existing : records)
                if (existing.first == type && existing.second == value) return {};
        records.emplace_back(type, json::parse(payload));
        return {};
    }
    MaiResult<std::string> readLatestSessionRecord(const std::string&,
                                                   const std::string& type) const override {
        for (auto it = records.rbegin(); it != records.rend(); ++it)
            if (it->first == type) return it->second.dump();
        return std::string{};
    }
};

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        if (!passed) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    };
    {
        CaptureStore nativeStore;
        MaiTurnContext nativeTurn{"native-session", "native-turn", "gpt-6.1-sol", {}, {}};
        MaiModelRequest nativeRequest;
        nativeRequest.model = nativeTurn.model;
        const std::string grammar = "start: \"patch\"\n";
        const std::string patch = "*** Begin Patch\n*** End Patch";
        nativeRequest.tools.push_back({"apply_patch", "Patch files", "{}", grammar});
        MaiModelMessage assistant;
        assistant.role = MaiModelRole::Assistant;
        assistant.invocations.push_back(
            {"native-call", "apply_patch", json{{"patch", patch}}.dump()});
        nativeRequest.messages.push_back(assistant);
        MaiModelMessage result;
        result.role = MaiModelRole::ToolResult;
        result.toolCallId = "native-call";
        result.content = "Success";
        result.sourceMessageId = "native-turn";
        result.sourcePartId = "native-result";
        nativeRequest.messages.push_back(result);
        MaiRolloutRuntimeFacts facts(&nativeStore, nativeTurn);
        check(!facts.prepare(nativeRequest, false), "native metadata prepared");
        const auto header = nativeStore.records.front().second["tools"][0];
        const auto format = header.value("format", json::object());
        check(header["type"] == "custom" && format.value("type", "") == "grammar" &&
                  format.value("syntax", "") == "lark" &&
                  format.value("definition", "") == grammar && !header.contains("parameters"),
              "native additional tools retains exact custom grammar");
        check(nativeStore.records[2].second["type"] == "custom_tool_call_output",
              "native incremental result retains custom call output type");
        MaiContextCheckpoint checkpoint;
        checkpoint.revision = 1;
        nativeStore.checkpoint = checkpoint;
        check(!facts.prepare(nativeRequest, true), "native replacement prepared");
        const auto history = nativeStore.records.back().second["replacement_history"];
        bool callFound = false, resultFound = false;
        for (const auto& item : history) {
            if (item.value("type", "") == "custom_tool_call")
                callFound = item.value("input", "") == patch && !item.contains("arguments");
            if (item.value("type", "") == "custom_tool_call_output") resultFound = true;
        }
        check(callFound && resultFound, "native replacement retains canonical custom call pair");
        const auto rawCall = json{{"type", "custom_tool_call"},
                                  {"call_id", "native-call"},
                                  {"name", "apply_patch"},
                                  {"input", patch},
                                  {"provider_field", "preserved"}};
        nativeRequest.messages.front().continuation = MaiModelContinuationState{
            "responses.output.v1", "openai", nativeRequest.model, json::array({rawCall}).dump()};
        nativeRequest.tools.clear();
        nativeStore.checkpoint->revision = 2;
        check(!facts.prepare(nativeRequest, true), "native raw continuation replacement prepared");
        const auto rawHistory = nativeStore.records.back().second["replacement_history"];
        check(rawHistory[2] == rawCall && rawHistory[3]["type"] == "custom_tool_call_output",
              "raw custom continuation and result survive empty tool declarations exactly");
        check(nativeRequest.messages.front().continuation->payload == json::array({rawCall}).dump(),
              "runtime facts preserve original continuation state");
        check(MaiModelInputAnchor{}.estimatorVersion == 3, "new input estimator anchor version");
    }
    httplib::Server server;
    std::string script;
    std::atomic<int> networkRequests{0};
    server.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
        ++networkRequests;
        response.set_content(script, "text/event-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    check(port > 0, "bind");
    if (port <= 0) return 1;
    std::thread worker([&] {
        MaiThread::setCurrentName("mai-rollout-test");
        server.listen_after_bind();
    });
    for (int i = 0; i < 200 && !server.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    MaiModelConfig config;
    config.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    config.maxRetries = 0;
    auto client = makeMaiResponsesClient(config);
    MaiModelRequest request;
    request.model = "gpt-6.1-sol";
    request.callerManagesRetries = true;
    request.messages.push_back({MaiModelRole::User, "test"});
    std::atomic<bool> cancel{false};
    std::vector<MaiModelOutputItem> completed;
    std::string text;
    int calls = 0;
    MaiStreamSink sink;
    sink.onText = [&](std::string_view delta) { text.append(delta); };
    sink.onOutputItemCompleted = [&](const MaiModelOutputItem& item) { completed.push_back(item); };
    sink.onToolCall = [&](const MaiToolInvocation&) { ++calls; };
    const auto event = [](const json& value) { return "data: " + value.dump() + "\n\n"; };
    const json first{{"id", "msg_A"},
                     {"type", "message"},
                     {"role", "assistant"},
                     {"status", "completed"},
                     {"unknown_future", json{{"retain", true}}},
                     {"content", json::array({json{{"type", "output_text"}, {"text", "A"}}})}};
    script = event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}});
    script += event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}});
    script += event(
        {{"type", "response.output_text.delta"}, {"output_index", 1}, {"delta", "unfinished-B"}});
    script += event({{"type", "response.failed"},
                     {"response", json{{"error", json{{"message", "fixture failure"}}}}}});
    auto error = client->stream(request, sink, cancel);
    check(static_cast<bool>(error), "failed tail remains failure");
    check(completed.size() == 1,
          "one actual done retained despite failed tail and repeated snapshot");
    if (completed.size() == 1) {
        check(completed[0].index == 0 && completed[0].id == "msg_A", "native item identity");
        check(json::parse(completed[0].payload) == first, "unknown native fields retained");
    }
    check(calls == 0, "no tools released on failed response");
    completed.clear();
    const json reasoning{{"id", "rs_A"},
                         {"type", "reasoning"},
                         {"encrypted_content", "opaque"},
                         {"future_field", json::array({1, 2})}};
    script = event(
        {{"type", "response.completed"},
         {"response", json{{"status", "completed"}, {"output", json::array({reasoning, first})}}}});
    error = client->stream(request, sink, cancel);
    check(!error, "terminal-only provider success");
    check(completed.size() == 2, "terminal-only native outputs emitted");
    if (completed.size() == 2)
        check(json::parse(completed[0].payload) == reasoning, "opaque reasoning preserved");
    completed.clear();
    script = event(
        {{"type", "response.output_text.done"}, {"output_index", 0}, {"text", "content only"}});
    script += event(
        {{"type", "response.failed"}, {"response", json{{"error", json{{"message", "failed"}}}}}});
    error = client->stream(request, sink, cancel);
    check(completed.empty(), "content done never fabricates native item completion");
    const json unknown{{"id", "future_A"},
                       {"type", "future_native_item"},
                       {"future_payload", json{{"retain", json::array({1, "opaque", nullptr})}}}};
    script = event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", unknown}});
    script += event(
        {{"type", "response.failed"}, {"response", json{{"error", json{{"message", "failed"}}}}}});
    error = client->stream(request, sink, cancel);
    check(completed.size() == 1 && json::parse(completed[0].payload) == unknown,
          "unknown native item preserved without inventing typed projection");

    // 真实 SSE 进入编排层：A 原始 item 完成，B 只有增量，随后失败。
    // 已完成 A 是有效历史，B 保持草稿，不能因失败尾巴回灌给下一次模型。
    {
        CaptureStore streamed;
        MaiSession session;
        session.id = "fixture_streamed";
        session.model = request.model;
        streamed.putSession(session);
        MaiMessage assistant;
        assistant.id = "fixture_streamed_turn";
        assistant.role = MaiRole::Assistant;
        streamed.putMessage(session.id, assistant);
        MaiEventBus bus;
        MaiEventEmitter emitter(bus);
        MaiContextBuilder context;
        MaiToolRegistry tools;
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = &streamed;
        dependencies.model = client.get();
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.tools = &tools;
        script =
            event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}});
        script += event({{"type", "response.output_text.delta"},
                         {"output_index", 1},
                         {"delta", "unfinished-B"}});
        const json call{{"id", "fc_A"},
                        {"type", "function_call"},
                        {"call_id", "call_never_execute"},
                        {"name", "fixture_unsafe"},
                        {"arguments", "{}"},
                        {"future_field", "retained"}};
        script +=
            event({{"type", "response.output_item.done"}, {"output_index", 2}, {"item", call}});
        script += event({{"type", "response.failed"},
                         {"response", json{{"error", json{{"message", "fixture failed tail"}}}}}});
        MaiTurnRunner runner(dependencies, session.id, assistant);
        runner.run(cancel);
        check(!runner.succeeded(), "streamed fixture retains whole response failure");
        const auto history = streamed.listMessages(session.id);
        bool committedA = false;
        bool draftB = false;
        for (const auto& message : history)
            for (const auto& part : message.parts) {
                if (const auto* body = std::get_if<MaiTextPart>(&part.body)) {
                    if (body->text == "A" && !part.modelDraft) committedA = true;
                    if (body->text == "unfinished-B" && part.modelDraft) draftB = true;
                }
                check(!std::holds_alternative<MaiToolPart>(part.body),
                      "raw completed call never enters execution batch on failure");
            }
        check(committedA, "actual native item completion commits A before failed tail");
        check(draftB, "unfinished B remains draft");
        const auto replay = context.build(history);
        std::string replayText;
        for (const auto& message : replay) replayText += message.content;
        check(replayText.find("A") != std::string::npos, "completed A remains model history");
        check(replayText.find("unfinished-B") == std::string::npos, "unfinished B never replayed");
        int nativeItems = 0;
        for (const auto& record : streamed.records)
            if (record.first == "response_item" && (record.second.value("id", "") == "msg_A" ||
                                                    record.second.value("id", "") == "fc_A"))
                ++nativeItems;
        check(nativeItems == 2,
              "native message and function opaque objects recorded before failure");
    }

    CaptureStore store;
    MaiTurnContext turn{"fixture_session", "fixture_turn", request.model, "high", "E:/fixture"};
    MaiRolloutRuntimeFacts facts(&store, turn);
    request.baseInstructions = "fixture developer instructions";
    request.workingDirectory = turn.workingDirectory;
    request.reasoningEffort = turn.reasoningEffort;
    request.messages[0].sourceMessageId = "fixture_user";
    request.messages[0].images.push_back({"E:/fixture/image.png", "image/png"});
    check(!facts.prepare(request, false), "canonical initial request captured");
    const auto initialRecords = store.records.size();
    const auto initialContext = store.records.back().second;
    for (const char* field :
         {"root_turn_id", "disabled_plugin_ids", "cwd", "approval_policy", "approvals_reviewer",
          "sandbox_policy", "permission_profile", "active_permission_profile", "model", "comp_hash",
          "collaboration_mode", "multi_agent_version", "realtime_active", "effort", "summary"})
        check(initialContext.contains(field),
              "required turn_context field is retained even when unknown");
    check(initialContext.value("root_turn_id", "") == turn.turnId &&
              initialContext.value("effort", "") == request.reasoningEffort,
          "turn context reflects actual originating turn and requested effort");
    check(initialContext.value("approval_policy", json()).is_null() &&
              initialContext.value("permission_profile", json()).is_null(),
          "unprovided approval and permission state remains explicitly unknown");
    check(!facts.prepare(request, false), "repeat request captured");
    check(store.records.size() == initialRecords,
          "repeat request never logs full input or headers");
    MaiModelUsage firstUsage;
    firstUsage.inputTokens = 77;
    firstUsage.outputTokens = 19;
    firstUsage.totalTokens = 96;
    firstUsage.totalSource = MaiUsageValueSource::Provider;
    check(!facts.usage(firstUsage), "first measured usage");
    MaiModelMessage tool;
    tool.role = MaiModelRole::ToolResult;
    tool.content = "fixture result";
    tool.toolCallId = "call_fixture";
    tool.sourceMessageId = "fixture_turn";
    tool.sourcePartId = "fixture_tool";
    request.messages.push_back(tool);
    check(!facts.prepare(request, false), "only new tool result captured");
    MaiModelUsage secondUsage;
    secondUsage.inputTokens = 109;
    secondUsage.outputTokens = 5;
    secondUsage.totalTokens = 114;
    secondUsage.totalSource = MaiUsageValueSource::Provider;
    check(!facts.usage(secondUsage), "second measured usage");
    const auto& usageRecord = store.records.back().second;
    check(usageRecord["usage"]["total_tokens"] == 114, "latest request totals");
    check(usageRecord["turn_token_usage"]["total_tokens"] == 210,
          "turn totals separate from latest window");
    check(usageRecord["thread_token_usage"]["total_tokens"] == 210,
          "thread totals separate from latest window");
    check(usageRecord["thread_token_usage"]["history_before_first_measurement_known"] == false,
          "unknown historical measurement retained");
    check(usageRecord["usage"]["reasoning_output_tokens"].is_null(),
          "missing reasoning is not fake zero");
    const auto countType = [&](const char* type) {
        int result = 0;
        for (const auto& record : store.records)
            if (record.first == type) ++result;
        return result;
    };
    check(countType("turn_context") == 1, "turn context recorded once");
    check(countType("response_item") == 4,
          "initial tools and developer plus new user and tool only");
    check(countType("world_state") == 1, "world state only initial full without invented AGENTS");
    MaiContextCheckpoint checkpoint;
    checkpoint.sessionId = turn.sessionId;
    checkpoint.revision = 1;
    checkpoint.sourceMessageCount = 2;
    checkpoint.throughMessageId = "fixture_old_assistant";
    checkpoint.references = "{\"paths\":[\"E:/fixture/image.png\"]}";
    store.checkpoint = checkpoint;
    check(!facts.prepare(request, true), "adopted checkpoint window captured");
    check(store.records.back().first == "compacted", "window adoption records compacted");
    const auto firstWindow = store.records.back().second;
    check(firstWindow["message"] == "", "compaction has empty message");
    check(firstWindow["replacement_history"][0]["type"] == "additional_tools",
          "replacement begins with canonical tools");
    check(firstWindow["replacement_history"][1]["role"] == "developer",
          "replacement second header is developer");
    check(firstWindow["replacement_history"].size() == request.messages.size() + 2,
          "complete effective replacement history");
    check(
        firstWindow["replacement_history"][2]["content"][1]["sourceRef"] == "E:/fixture/image.png",
        "canonical image reference retains path without inline base64");
    check(firstWindow["latest_usage"]["usage"]["total_tokens"] == 114,
          "window retains latest provider request measurement");
    check(firstWindow["retained_context"]["references"]["paths"][0] == "E:/fixture/image.png",
          "retained source references saved");
    const auto afterWindow = store.records.size();
    check(!facts.prepare(request, true) && store.records.size() == afterWindow,
          "same adopted window is not repeated");
    store.checkpoint->revision = 2;
    check(!facts.prepare(request, true), "next adopted checkpoint window captured");
    check(store.records.back().second["window_number"] == 2, "window number advances");
    check(store.records.back().second["previous_window_id"] == firstWindow["window_id"],
          "window chain links exact previous identity");
    MaiTurnContext nextTurn = turn;
    nextTurn.turnId = "fixture_next_turn";
    MaiRolloutRuntimeFacts resumed(&store, nextTurn);
    check(!resumed.prepare(request, false), "resume from latest runtime measurements");
    check(!resumed.usage(firstUsage), "next turn measured usage");
    check(store.records.back().second["turn_token_usage"]["total_tokens"] == 96,
          "new turn resets turn accumulator");
    check(store.records.back().second["thread_token_usage"]["total_tokens"] == 306,
          "thread accumulator survives new runtime instance");
    check(countType("response_item") == 4, "resume never repeats prior input or headers");

    {
        CaptureStore longStore;
        MaiModelRequest longRequest = request;
        longRequest.messages.clear();
        for (int index = 0; index < 1000; ++index) {
            MaiModelMessage oldUser;
            oldUser.role = MaiModelRole::User;
            oldUser.content = "historical input";
            oldUser.sourceMessageId = "fixture_old_user_" + std::to_string(index);
            longRequest.messages.push_back(std::move(oldUser));
        }
        MaiRolloutRuntimeFacts longFacts(&longStore, turn);
        check(!longFacts.prepare(longRequest, false), "large source history prepared");
        const auto contextPayload = longStore.records.back().second.dump();
        check(contextPayload.size() < 1024 &&
                  contextPayload.find("logged_input_sources") == std::string::npos,
              "turn context never repeats lifetime source identities");
        check(!longFacts.usage(firstUsage), "large history actual usage recorded");
        const auto usageSize = longStore.records.back().second.dump().size();
        check(usageSize < 4096 && !longStore.records.back().second.contains("logged_input_sources"),
              "usage record size independent of historical input identities");
        const auto beforeRepeat = longStore.records.size();
        check(!longFacts.prepare(longRequest, false), "large history repeated request");
        check(longStore.records.size() == beforeRepeat,
              "repeated large request adds no old input records");
        check(!longFacts.usage(secondUsage), "large history second usage recorded");
        check(longStore.records.back().second.dump().size() < usageSize + 128,
              "subsequent usage growth stays bounded");
        MaiRolloutRuntimeFacts restart(&longStore, nextTurn);
        const auto beforeRestart = longStore.records.size();
        check(!restart.prepare(longRequest, false), "large history runtime restart");
        check(longStore.records.size() == beforeRestart + 1,
              "source backend identity dedup prevents historical replay after restart");
    }
    {
        CaptureStore overflowStore;
        MaiRolloutRuntimeFacts overflowFacts(&overflowStore, turn);
        check(!overflowFacts.prepare(request, false), "overflow fixture prepared");
        MaiModelUsage maximum = firstUsage;
        maximum.inputTokens = std::numeric_limits<std::size_t>::max();
        maximum.totalTokens = std::numeric_limits<std::size_t>::max();
        check(!overflowFacts.usage(maximum) && !overflowFacts.usage(secondUsage),
              "overflow measurements retained");
        const auto aggregate = overflowStore.records.back().second["thread_token_usage"];
        check(aggregate["total_tokens"].is_null() &&
                  aggregate["total_tokens_source"] == "missing" &&
                  aggregate.value("total_tokens_error", "") == "overflow",
              "overflow never wraps accumulated usage into fake small total");
    }
    {
        // 同一权威事实的实时与重开投影；摘要、换型和检查点重置都不能重贴旧采样标签。
        CaptureStore measuredStore;
        MaiRolloutRuntimeFacts measuredFacts(&measuredStore, turn, "fixture-provider", 20000);
        check(!measuredFacts.prepare(request, false), "usage origin fixture prepared");
        MaiModelUsage measured = firstUsage;
        measured.inputTokens = 15000;
        measured.outputTokens = 1000;
        measured.totalTokens = 16000;
        measured.cachedInputTokens = 14000;
        measured.cachedInputSource = MaiUsageValueSource::Provider;
        measured.responseId = "fixture-measured-response";
        check(!measuredFacts.usage(measured), "actual sampling snapshot committed");
        const auto sampling = measuredFacts.contextUsage();
        check(sampling.samplingValid && maiContextRemainingPercent(sampling) == 50,
              "live snapshot uses last measured total including cache");
        MaiModelUsage summary = measured;
        summary.totalTokens = 300000;
        summary.responseId = "fixture-summary-response";
        check(!measuredFacts.usage(summary, "compaction"), "summary usage committed separately");
        const auto afterSummary = measuredFacts.contextUsage();
        check(afterSummary.latestSampling.total.tokens == 16000 &&
                  afterSummary.responseId == measured.responseId &&
                  afterSummary.turn.total.tokens == 316000 &&
                  afterSummary.thread.total.tokens == 316000,
              "summary only updates cumulative scopes and preserves actual sampling origin");
        const auto cold =
            maiReadModelContextUsage(measuredStore, turn.sessionId, turn.model, "fixture-provider");
        check(cold && cold.value().samplingValid &&
                  cold.value().latestSampling.total.tokens == 16000 &&
                  cold.value().turn.total.tokens == 316000 &&
                  cold.value().responseId == measured.responseId,
              "cold ordinary query restores same source snapshot");
        const auto wrongModel = maiReadModelContextUsage(measuredStore, turn.sessionId,
                                                         "different-model", "fixture-provider");
        check(wrongModel && !wrongModel.value().samplingValid &&
                  wrongModel.value().thread.total.tokens == 316000,
              "model change invalidates occupancy while preserving measured consumption");
        const auto wrongProvider =
            maiReadModelContextUsage(measuredStore, turn.sessionId, turn.model, "other-provider");
        check(wrongProvider && !wrongProvider.value().samplingValid,
              "provider change invalidates old measurement");
        measuredStore.checkpoint = checkpoint;
        check(!measuredFacts.prepare(request, true), "new checkpoint invalidates old sampling");
        check(!measuredFacts.contextUsage().samplingValid, "live old window is explicitly invalid");
        const auto compacted =
            maiReadModelContextUsage(measuredStore, turn.sessionId, turn.model, "fixture-provider");
        check(compacted && !compacted.value().samplingValid,
              "cold old window is explicitly invalid");
        check(!measuredFacts.usage(measured), "new checkpoint receives real measurement");
        check(measuredFacts.contextUsage().samplingValid &&
                  measuredFacts.contextUsage().checkpointRevision == 1,
              "new sample owns current checkpoint identity");
        measuredStore.checkpoint.reset();
        const auto reset =
            maiReadModelContextUsage(measuredStore, turn.sessionId, turn.model, "fixture-provider");
        check(reset && !reset.value().samplingValid,
              "removed checkpoint invalidates cold measurement");
        check(!measuredFacts.prepare(request, false) && !measuredFacts.usage(measured),
              "full context after reset records a new window");
        const auto resetMeasured =
            maiReadModelContextUsage(measuredStore, turn.sessionId, turn.model, "fixture-provider");
        check(resetMeasured && resetMeasured.value().samplingValid &&
                  resetMeasured.value().checkpointRevision == 0,
              "reset requires and accepts a fresh real sampling");
        measuredStore.failWrite = true;
        check(static_cast<bool>(measuredFacts.usage(summary)), "failed source write is explicit");
        check(measuredFacts.contextUsage().responseId == measured.responseId,
              "failed source write never publishes uncommitted measurement");
    }
    {
        CaptureStore lifecycleStore;
        MaiRolloutRuntimeFacts lifecycle(&lifecycleStore, turn);
        check(!lifecycle.started() && !lifecycle.started(), "task start is idempotent");
        MaiTurnOutcome completedTurn;
        completedTurn.turnId = turn.turnId;
        completedTurn.status = MaiTurnStatus::Completed;
        completedTurn.resultMessageId = turn.turnId;
        completedTurn.completed = MaiTime::getCurrentTime();
        check(!lifecycle.finished(completedTurn) && !lifecycle.finished(completedTurn),
              "completed turn event is idempotent");
        check(lifecycleStore.records.size() == 2 &&
                  lifecycleStore.records.back().second["type"] == "task_complete" &&
                  !lifecycleStore.records.back().second.contains("error"),
              "successful completion has standard task_complete without invented error");
        MaiRolloutRuntimeFacts canceled(&lifecycleStore, nextTurn);
        check(!canceled.started(), "canceled task has actual start");
        completedTurn.status = MaiTurnStatus::Interrupted;
        completedTurn.error = {MaiErrorCode::Canceled, "fixture user canceled"};
        check(!canceled.finished(completedTurn), "canceled task has aborted boundary");
        check(lifecycleStore.records.back().second["type"] == "turn_aborted" &&
                  lifecycleStore.records.back().second["reason"] == "interrupted",
              "cancellation uses pinned protocol interrupted abort reason");
    }
    {
        class MeasuredSummary final : public MaiModelClient {
        public:
            int calls = 0;
            MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                            const std::atomic<bool>&) override {
                ++calls;
                MaiModelUsage measured;
                measured.inputTokens = 10;
                measured.outputTokens = 3;
                measured.totalTokens = 13;
                measured.totalSource = MaiUsageValueSource::Provider;
                if (sink.onUsage) sink.onUsage(measured);
                return {MaiErrorCode::Network, "fixture summary failed after measured usage"};
            }
            MaiWireApi wireApi() const override {
                return MaiWireApi::Responses;
            }
            MaiModelContextLimits contextLimits(const std::string&) const override {
                return {8192, 2048};
            }
        } measuredModel;
        CaptureStore summaryStore;
        MaiSession session;
        session.id = "fixture_summary";
        session.model = "fixture";
        summaryStore.putSession(session);
        for (int round = 0; round < 5; ++round) {
            MaiMessage user;
            user.id = MaiIdGenerator::newMessageId();
            user.role = MaiRole::User;
            user.created = 1000 + round * 2;
            user.parts.push_back(
                {MaiIdGenerator::newPartId(), MaiTextPart{std::string(8000, 'x')}, user.created});
            summaryStore.putMessage(session.id, user);
            MaiMessage answer;
            answer.id = MaiIdGenerator::newMessageId();
            answer.role = MaiRole::Assistant;
            answer.created = user.created + 1;
            answer.completed = answer.created + 1;
            answer.parts.push_back({MaiIdGenerator::newPartId(),
                                    MaiTextPart{"Historical completed reply"}, answer.created});
            summaryStore.putMessage(session.id, answer);
        }
        MaiMessage current;
        current.id = MaiIdGenerator::newMessageId();
        current.role = MaiRole::User;
        current.created = 2000;
        current.parts.push_back({MaiIdGenerator::newPartId(), MaiTextPart{"Continue"}, 2000});
        summaryStore.putMessage(session.id, current);
        MaiTurnContext summaryTurn{session.id, "fixture_summary_turn", session.model, "high", ""};
        MaiRolloutRuntimeFacts summaryFacts(&summaryStore, summaryTurn);
        check(!summaryFacts.initialize() && !summaryFacts.usage(firstUsage),
              "summary measurement fixture initialized");
        MaiContextCompactor::Options options;
        options.onUsage = [&](const MaiModelUsage& measured) {
            return summaryFacts.usage(measured, "compaction");
        };
        MaiModelRequest summaryRequest;
        summaryRequest.model = session.model;
        summaryRequest.reasoningEffort = "high";
        summaryRequest.baseInstructions = "Honor current input";
        const auto result = MaiContextCompactor(options).prepare(
            summaryStore, measuredModel, MaiContextBuilder{}, session.id,
            summaryStore.listMessages(session.id), summaryRequest, cancel);
        check(!result && measuredModel.calls == 1,
              "actual internal summary stream measured once even on failure");
        const auto record = summaryStore.records.back().second;
        check(
            record.value("operation", "") == "compaction" && record["usage"]["total_tokens"] == 13,
            "internal summary usage forwarded with explicit operation scope");
        check(record["turn_token_usage"]["total_tokens"] == 109 &&
                  record["thread_token_usage"]["total_tokens"] == 109,
              "actual internal summary counts in turn and thread totals");
        check(record["context_window_usage"]["total_tokens"] == 96,
              "summary request never overwrites actual main window occupancy");
        options.onUsage = [&](const MaiModelUsage&) {
            return MaiError{MaiErrorCode::Protocol, "fixture observation source failure"};
        };
        const auto callsBeforeFailure = measuredModel.calls;
        const auto failedObservation = MaiContextCompactor(options).prepare(
            summaryStore, measuredModel, MaiContextBuilder{}, session.id,
            summaryStore.listMessages(session.id), summaryRequest, cancel);
        check(!failedObservation &&
                  failedObservation.error().message() == "fixture observation source failure" &&
                  measuredModel.calls == callsBeforeFailure + 1,
              "usage source failure cannot be swallowed by compaction recovery or resampled");
    }

    // 编排层必须在历史读取失败或模型事实写入失败时阻止一切网络采样。
    for (const bool readFailure : {true, false}) {
        CaptureStore broken;
        broken.failRead = readFailure;
        broken.failWrite = !readFailure;
        MaiSession session;
        session.id = "fixture_blocked";
        session.model = request.model;
        broken.putSession(session);
        MaiMessage assistant;
        assistant.id = "fixture_blocked_turn";
        assistant.role = MaiRole::Assistant;
        broken.putMessage(session.id, assistant);
        MaiEventBus bus;
        MaiEventEmitter emitter(bus);
        MaiContextBuilder context;
        MaiToolRegistry tools;
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = &broken;
        dependencies.model = client.get();
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.tools = &tools;
        MaiTurnRunner runner(dependencies, session.id, assistant);
        const auto beforeRequests = networkRequests.load();
        runner.run(cancel);
        check(!runner.succeeded(),
              readFailure ? "source read blocks model" : "source write blocks model");
        check(beforeRequests == networkRequests.load(),
              "blocked source causes zero network requests");
    }
    server.stop();
    worker.join();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

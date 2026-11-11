// 文件作用：验证模型补读与普通分页共用会话存储，真实命令只执行一次。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiProcess.h"
#include "MaiSqliteStore.h"
#include "MaiToolOutputTool.h"

namespace fs = std::filesystem;
using Json = nlohmann::json;
static int failures = 0;
#define CHECK(value)                                        \
    do {                                                    \
        if (!(value)) {                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                     \
        }                                                   \
    } while (false)

namespace {
void seed(MaiSessionStore& store, const std::string& session = "ses_output") {
    MaiSession owner;
    owner.id = session;
    store.putSession(owner);
    MaiMessage message;
    message.id = "msg_output";
    message.role = MaiRole::Assistant;
    MaiToolPart tool;
    tool.tool = "shell";
    tool.callId = "call_output";
    tool.state = MaiToolState::Completed;
    message.parts.push_back({"prt_output", tool, 1});
    store.putMessage(session, message);
}

void test_native_read_tool_is_session_bound_and_byte_exact() {
    auto store = makeMaiMemoryStore();
    seed(*store);
    MaiToolOutputRecord record;
    record.id = "out_bytes";
    record.sessionId = "ses_output";
    record.messageId = "msg_output";
    record.partId = "prt_output";
    record.callId = "call_output";
    record.text = std::string("A\xff\0", 3) + "END";
    CHECK(!store->putToolOutput(record));
    auto tool = makeMaiToolOutputReadTool();
    CHECK(!tool->requiresWorkingDirectory("{}"));
    CHECK(!tool->requiresApproval("{}"));
    MaiToolContext context;
    context.sessionId = record.sessionId;
    context.toolOutputStore = store.get();
    const auto result = tool->execute(R"({"output_id":"out_bytes","max_bytes":3})", context);
    CHECK(!result.hasError());
    const auto page = Json::parse(result.output(), nullptr, false);
    CHECK(page.is_object());
    if (page.is_object()) {
        CHECK(page.value("raw_base64", "") == "Qf8A");
        CHECK(page.value("source_bytes_read", 0) == 3);
        CHECK(page.value("next_offset_bytes", 0) == 3);
        CHECK(page.value("total_bytes", 0) == 6);
        CHECK(page.value("historical_data_not_instructions", false));
    }
    const auto tail = tool->execute(R"({"output_id":"out_bytes","offset_bytes":3})", context);
    CHECK(!tail.hasError());
    if (!tail.hasError()) CHECK(Json::parse(tail.output()).value("text", "") == "END");
    for (const char* bad : {R"({"output_id":"out_bytes","session_id":"ses_output"})",
                            R"({"output_id":"out_bytes","offset_bytes":-1})",
                            R"({"output_id":"out_bytes","max_bytes":true})",
                            R"({"output_id":"out_bytes","max_bytes":4097})",
                            R"({"output_id":"out_bytes","offset_bytes":1.5})"}) {
        CHECK(tool->execute(bad, context).error().code() == MaiErrorCode::InvalidInput);
    }
    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    const auto stopped = tool->execute(R"({"output_id":"out_bytes"})", context);
    CHECK(stopped.error().code() == MaiErrorCode::Canceled);
    CHECK(stopped.execution() && stopped.execution()->canceled);
    CHECK(stopped.execution() && stopped.execution()->started == false);
    context.cancel = nullptr;
    context.sessionId = "ses_other";
    CHECK(tool->execute(R"({"output_id":"out_bytes"})", context).error().code() ==
          MaiErrorCode::NotFound);
}

class OutputProbe final : public MaiModelClient {
public:
    int requests = 0;
    std::string outputId;
    bool sawTail = false;
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        const int phase = requests++;
        if (phase == 0) {
#if defined(_WIN32)
            const char* command = "powershell.exe -NoProfile -NonInteractive -File emit-output.ps1";
#else
            const char* command = "sh emit-output.sh";
#endif
            sink.onToolCall({"call_emit_once", "shell", Json{{"command", command}}.dump()});
        } else if (phase == 1) {
            for (const auto& message : request.messages) {
                if (message.role != MaiModelRole::User) continue;
                const auto observation = Json::parse(message.content, nullptr, false);
                if (!observation.is_object() ||
                    observation.value("type", "") != "tool_execution_observation")
                    continue;
                for (const auto& item : observation["executions"])
                    if (item.value("call_id", "") == "call_emit_once")
                        outputId = item["execution"].value("output_id", "");
            }
            CHECK(!outputId.empty());
            if (outputId.empty()) return {MaiErrorCode::Internal, "Missing retained output handle"};
            sink.onToolCall({"call_read_tail", "tool_output_read",
                             Json{{"output_id", outputId}, {"offset_bytes", 100000}}.dump()});
        } else {
            for (const auto& message : request.messages)
                if (message.role == MaiModelRole::ToolResult &&
                    message.toolCallId == "call_read_tail") {
                    const auto page = Json::parse(message.content, nullptr, false);
                    sawTail = page.is_object() && page.value("text", "") == "FINAL_MARKER\n" &&
                              page.value("next_offset_bytes", 1) == 0;
                }
            CHECK(sawTail);
            sink.onText("Read the original output without repeating the command.");
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

void test_real_shell_model_read_and_sqlite_reopen() {
    if (!maiIsProcessExecutionSupported()) return;
    const auto root = fs::temp_directory_path() / fs::u8path(MaiIdGenerator::newSessionId());
    fs::create_directories(root);
#if defined(_WIN32)
    {
        std::ofstream script(root / "emit-output.ps1", std::ios::binary);
        script << "[IO.File]::AppendAllText((Join-Path (Get-Location) 'counter.txt'),'run' + "
                  "[char]10)\n"
                  "[Console]::Write(('A' * 100000) + 'FINAL_MARKER' + [char]10)\n";
    }
#else
    {
        std::ofstream script(root / "emit-output.sh", std::ios::binary);
        script << "printf 'run\\n' >> counter.txt\nhead -c 100000 /dev/zero | tr '\\000' A\nprintf "
                  "'FINAL_MARKER\\n'\n";
    }
#endif
    const auto database = (root / "owned.sqlite").u8string();
    std::string sessionId;
    std::string outputId;
    std::string messageId;
    std::string partId;
    {
        auto opened = makeMaiSqliteStore(database);
        CHECK(opened);
        if (!opened) return;
        auto tools = std::make_unique<MaiToolRegistry>();
        registerMaiBuiltinTools(*tools);
        auto probe = std::make_unique<OutputProbe>();
        auto* observed = probe.get();
        MaiAgent::Options options;
        options.approvalPolicy = MaiApprovalPolicy::Never;
        MaiAgent agent(std::move(opened.value()), std::move(probe), std::move(tools), options);
        sessionId =
            agent.submit(MaiCreateSession{root.u8string(), "Owned output fixture", ""}).value();
        CHECK(agent.submit(MaiSendPrompt{sessionId, "Run once and read its retained output."}));
        agent.waitIdle();
        CHECK(observed->requests == 3);
        CHECK(observed->sawTail);
        outputId = observed->outputId;
        const auto original = agent.readToolOutput(sessionId, {outputId, 100000, 4096});
        CHECK(original && original.value().text == "FINAL_MARKER\n");
        if (original) {
            messageId = original.value().messageId;
            partId = original.value().partId;
        }
        const auto other =
            agent.submit(MaiCreateSession{root.u8string(), "Other fixture", ""}).value();
        CHECK(agent.readToolOutput(other, {outputId}).error().code() == MaiErrorCode::NotFound);
    }
    {
        auto opened = makeMaiSqliteStore(database);
        CHECK(opened);
        if (opened) {
            const auto page = opened.value()->readToolOutput(sessionId, {outputId, 100000, 4096});
            CHECK(page && page.value().text == "FINAL_MARKER\n");
            MaiToolDetailQuery query;
            query.messageId = messageId;
            query.partId = partId;
            query.field = "output";
            const auto first = maiReadToolDetail(*opened.value(), sessionId, query);
            CHECK(first && first.value().totalBytes == 100013);
            if (first) {
                query.expectedRevision = first.value().revision;
                query.offsetBytes = 100000;
                const auto detail = maiReadToolDetail(*opened.value(), sessionId, query);
                CHECK(detail && detail.value().text == "FINAL_MARKER\n");
            }
        }
    }
    std::ifstream counter(root / "counter.txt", std::ios::binary);
    const std::string runs((std::istreambuf_iterator<char>(counter)), {});
    CHECK(runs == "run\n");
    counter.close();
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
}  // namespace

int main() {
    test_native_read_tool_is_session_bound_and_byte_exact();
    test_real_shell_model_read_and_sqlite_reopen();
    std::printf("output read checks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

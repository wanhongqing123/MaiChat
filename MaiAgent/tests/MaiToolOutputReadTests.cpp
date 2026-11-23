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
void checkPreviewTextIsValid(const MaiToolOutputPreview& preview, std::size_t maximumBytes) {
    CHECK(preview.text.size() <= maximumBytes);
    bool serializable = true;
    try {
        (void)Json(preview.text).dump();
    } catch (...) {
        serializable = false;
    }
    CHECK(serializable);
}

void test_preview_preserves_scalars_and_escapes_raw_controls_without_mutation() {
    const std::string utf8 = "A\u4e2d\U0001f642\t\r\nZ";
    const auto entire = maiPreviewToolOutput(utf8, utf8.size());
    CHECK(entire.text == utf8 && !entire.truncated && !entire.escapedBytes);
    CHECK(entire.omittedSourceBytes == 0);
    const std::string raw = std::string("A\0\x01\t\n\r\x7f\xff", 8) + "B";
    const std::string original = raw;
    const std::string expected = "A\\x00\\x01\t\n\r\\x7F\\xFFB";
    const auto escaped = maiPreviewToolOutput(raw, expected.size());
    CHECK(escaped.text == expected && escaped.escapedBytes && !escaped.truncated);
    CHECK(escaped.omittedSourceBytes == 0 && raw == original);
    checkPreviewTextIsValid(escaped, expected.size());
    MaiToolOutputRecord record;
    record.text = raw;
    (void)maiPreviewToolOutput(record.text, 7);
    CHECK(record.text == original);
    const auto gap = maiPreviewToolOutput(std::string("A\0B", 3), 5);
    CHECK(gap.truncated && gap.omittedSourceBytes == 1 && !gap.escapedBytes);
    CHECK(!gap.text.empty() && gap.text.front() == 'A' && gap.text.back() == 'B');
    for (unsigned value = 0; value < 256; ++value) {
        const std::string byte(1, static_cast<char>(value));
        const auto preview = maiPreviewToolOutput(byte, 4);
        const bool mustEscape =
            value >= 127 || (value < 32 && value != '\n' && value != '\r' && value != '\t');
        CHECK(!preview.truncated && preview.omittedSourceBytes == 0);
        CHECK(preview.escapedBytes == mustEscape);
        CHECK(mustEscape ? preview.text.size() == 4 && preview.text.substr(0, 2) == "\\x"
                         : preview.text == byte);
        checkPreviewTextIsValid(preview, 4);
    }
}

void test_preview_head_tail_reports_exact_source_gap() {
    std::string source = "HEAD|";
    for (unsigned index = 0; index < 100; ++index) source += "\u4e2d\U0001f642";
    source += "|TAIL";
    const auto original = source;
    for (const std::size_t maximum : {48u, 64u, 97u}) {
        const auto preview = maiPreviewToolOutput(source, maximum);
        checkPreviewTextIsValid(preview, maximum);
        CHECK(preview.truncated && !preview.escapedBytes);
        CHECK(preview.text.find("HEAD|") != std::string::npos);
        CHECK(preview.text.find("|TAIL") != std::string::npos);
        CHECK(preview.text.find("omitted") != std::string::npos);
        std::size_t prefix = 0, suffix = 0;
        while (prefix < preview.text.size() && prefix < source.size() &&
               preview.text[prefix] == source[prefix])
            ++prefix;
        while (suffix < preview.text.size() - prefix && suffix < source.size() - prefix &&
               preview.text[preview.text.size() - suffix - 1] == source[source.size() - suffix - 1])
            ++suffix;
        CHECK(preview.omittedSourceBytes == source.size() - prefix - suffix);
        CHECK(source == original);
    }
}

void test_preview_tiny_budgets_never_split_escape_or_scalar_units() {
    const std::string raw = std::string("\0\xff\xf0\x9f", 4);
    for (std::size_t maximum = 0; maximum <= 20; ++maximum) {
        const auto preview = maiPreviewToolOutput(raw, maximum);
        checkPreviewTextIsValid(preview, maximum);
        CHECK(preview.omittedSourceBytes <= raw.size());
        CHECK(preview.truncated == (preview.omittedSourceBytes != 0));
        for (std::size_t index = 0; index < preview.text.size(); ++index) {
            if (preview.text[index] != '\\') continue;
            CHECK(index + 3 < preview.text.size());
            if (index + 3 >= preview.text.size()) break;
            CHECK(preview.text[index + 1] == 'x');
            index += 3;
        }
        if (maximum == 0) CHECK(preview.text.empty() && preview.omittedSourceBytes == raw.size());
    }
    for (std::size_t maximum = 0; maximum < 4; ++maximum) {
        const auto scalar = maiPreviewToolOutput("\U0001f642", maximum);
        checkPreviewTextIsValid(scalar, maximum);
        CHECK(scalar.truncated && scalar.omittedSourceBytes == 4 && !scalar.escapedBytes);
    }
    const auto empty = maiPreviewToolOutput({}, 0);
    CHECK(empty.text.empty() && !empty.truncated && !empty.escapedBytes &&
          empty.omittedSourceBytes == 0);
}

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
            sink.onToolCall(
                {"call_emit_once", "shell", Json{{"command", command}, {"login", false}}.dump()});
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

int main(int argc, char* argv[]) {
    test_preview_preserves_scalars_and_escapes_raw_controls_without_mutation();
    test_preview_head_tail_reports_exact_source_gap();
    test_preview_tiny_budgets_never_split_escape_or_scalar_units();
    if (argc == 2 && std::string(argv[1]) == "--preview-only") {
        std::printf("preview checks: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    test_native_read_tool_is_session_bound_and_byte_exact();
    test_real_shell_model_read_and_sqlite_reopen();
    std::printf("output read checks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

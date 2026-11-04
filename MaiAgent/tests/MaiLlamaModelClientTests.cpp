// 文件作用：验证本地主模型边界及真实 GGUF -> 原生工具调用 -> 文件结果回灌 -> 最终答案。
// 不下载权重。MAI_LLAMA_CHAT_TEST_MODEL 指向具原生工具模板的本地指令模型；缺失时明确跳过。
// 文件和标记全为本进程合成数据；观察客户端只记录真实请求，从不提供脚本化模型输出。
#include "MaiLlamaModelClient.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiContextBuilder.h"
#include "MaiFileSystem.h"
#include "MaiFileTools.h"
#include "MaiIdGenerator.h"
#include "MaiLlamaTools.h"
#include "MaiLlamaModelValidation.h"
#include "MaiMemoryStore.h"
#include "MaiThread.h"

namespace {
int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

MaiModelRequest simpleRequest() {
    MaiModelRequest request;
    request.model = "local:qa";
    request.baseInstructions = "You are a helpful assistant. Follow the user's instructions.";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Reply with exactly these Chinese words: \u660e\u4eae\u7684\u65e9\u6668";
    request.messages.push_back(std::move(user));
    return request;
}

bool validUtf8(const std::string& value) {
    try {
        return nlohmann::json::parse(nlohmann::json(value).dump()).get<std::string>() == value;
    } catch (...) {
        return false;
    }
}

void test_preflight_without_model_io() {
    MaiLlamaModelConfig config;
    auto client = makeMaiLlamaModelClient(config);
    CHECK(client->wireApi() == MaiWireApi::Local);
    std::atomic<bool> canceled{true};
    CHECK(client->stream({}, {}, canceled).code() == MaiErrorCode::Canceled);
    canceled = false;
    CHECK(client->stream({}, {}, canceled).code() == MaiErrorCode::InvalidInput);
    auto request = simpleRequest();
    request.messages[0].images.push_back({"not-opened.png", "image/png"});
    CHECK(client->stream(request, {}, canceled).code() == MaiErrorCode::NotSupported);
    request = simpleRequest();
    request.tools.push_back({"file_read", "Read a file", "not JSON"});
    CHECK(client->stream(request, {}, canceled).code() == MaiErrorCode::InvalidInput);
    request = simpleRequest();
    request.messages[0].role = MaiModelRole::ToolResult;
    request.messages[0].toolCallId = "missing-call";
    CHECK(client->stream(request, {}, canceled).code() == MaiErrorCode::InvalidInput);
    request = simpleRequest();
    request.messages[0].content = std::string("\xc0\xaf", 2);
    CHECK(client->stream(request, {}, canceled).code() == MaiErrorCode::InvalidInput);
    config.contextSize = 127;
    CHECK(makeMaiLlamaModelClient(config)->stream(simpleRequest(), {}, canceled).code() ==
          MaiErrorCode::InvalidInput);
    config = {};
    config.projectorPath = MaiFilePath::fromUtf8("not-opened.gguf");
    CHECK(makeMaiLlamaModelClient(config)->stream(simpleRequest(), {}, canceled).code() ==
          MaiErrorCode::NotSupported);
}

void test_system_instruction_utf8_and_separator_budget() {
    auto client = makeMaiLlamaModelClient({});
    std::atomic<bool> cancel{false};
    auto request = simpleRequest();
    MaiModelMessage note;
    note.role = MaiModelRole::System;
    note.content = std::string(1, char(0xff));
    request.messages.insert(request.messages.begin(), note);
    CHECK(client->stream(request, {}, cancel).message() ==
          "Local chat requires bounded UTF-8 messages");

    // 原文恰好 2 MiB 时，合并新增的两个换行也必须计入，不能绕过输入内存边界。
    request.baseInstructions = std::string(2 * 1024 * 1024 - 2, 'x');
    request.messages[0].content = "s";
    request.messages[1].content = "u";
    CHECK(client->stream(request, {}, cancel).message() ==
          "Local chat requires bounded UTF-8 messages");
    request.baseInstructions.resize(2 * 1024 * 1024 - 4);
    CHECK(client->stream(request, {}, cancel).message() ==
          "Local inference requires authorized absolute file paths");
}

void test_real_system_messages_and_old_image_followup(const MaiLlamaModelConfig& config) {
    auto initial = simpleRequest();
    initial.messages[0].content = "\u4f60\u597d";
    MaiModelMessage firstNote;
    firstNote.role = MaiModelRole::System;
    firstNote.content = "This is a trusted runtime note. Follow the latest user request.";
    MaiModelMessage finalNote = firstNote;
    finalNote.content = "Give the user a concise final response.";
    MaiModelMessage emptyNote;
    emptyNote.role = MaiModelRole::System;
    std::vector<MaiModelRequest> requests{initial};
    auto leading = initial;
    leading.messages.insert(leading.messages.begin(), firstNote);
    leading.messages.insert(leading.messages.begin() + 1, emptyNote);
    requests.push_back(leading);
    auto middle = leading;
    MaiModelMessage previous;
    previous.role = MaiModelRole::Assistant;
    previous.content = "Hello.";
    middle.messages.push_back(previous);
    middle.messages.push_back(firstNote);
    middle.messages.push_back(initial.messages[0]);
    middle.messages.push_back(finalNote);
    requests.push_back(middle);
    leading.baseInstructions.clear();
    requests.push_back(leading);

    MaiMessage user;
    user.role = MaiRole::User;
    MaiMessagePart text;
    text.body = MaiTextPart{"\u770b\u4e0b\u62cd\u6444\u7167\u7247"};
    user.parts.push_back(text);
    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    MaiToolPart viewed;
    viewed.tool = "view_image";
    viewed.callId = "call_historical_photo";
    viewed.input = R"({"path":"own-synthetic-photo.jpg"})";
    viewed.output = "Loaded the image from own-synthetic-photo.jpg.";
    viewed.state = MaiToolState::Completed;
    MaiMessagePart receipt;
    receipt.body = viewed;
    assistant.parts.push_back(receipt);
    MaiMessagePart image;
    image.body = MaiImagePart{"own-synthetic-photo.jpg", "image/jpeg"};
    assistant.parts.push_back(image);
    MaiMessage followup = user;
    followup.parts[0].body = MaiTextPart{"\u600e\u4e48\u6ca1\u6709\u4efb\u4f55\u7ed3\u679c"};
    auto historical = initial;
    historical.messages = MaiContextBuilder().build({user, assistant, followup});
    for (const auto& message : historical.messages) CHECK(message.images.empty());
    requests.push_back(historical);
    auto client = makeMaiLlamaModelClient(config);
    std::atomic<bool> cancel{false};
    for (std::size_t index = 0; index < requests.size(); ++index) {
        std::string text;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view delta) { text += delta; };
        const auto error = client->stream(requests[index], sink, cancel);
        if (error) std::printf("System/history case %zu: %s\n", index, error.message().c_str());
        CHECK(!error && !text.empty() && validUtf8(text));
    }
}

void test_bounded_model_validation() {
    const auto directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("local_header_")));
    CHECK(!MaiFileSystem::createDirectories(directory));
    MaiLlamaModelConfig config;
    config.modelPath = directory.append(MaiFilePath::fromUtf8("invalid.gguf"));
    CHECK(!MaiFileSystem::writeFile(config.modelPath, "GGUF" + std::string(24, '\0')));
    CHECK(validateMaiLlamaModelConfig(config).code() == MaiErrorCode::InvalidInput);
    std::string header = "GGUF";
    const auto append = [&](std::uint64_t value, unsigned width) {
        for (unsigned index = 0; index < width; ++index)
            header.push_back(static_cast<char>(value >> (8 * index)));
    };
    append(3, 4);
    append(1, 8);
    append(1, 8);
    append(UINT64_MAX, 8);
    CHECK(!MaiFileSystem::writeFile(config.modelPath, header));
    CHECK(validateMaiLlamaModelConfig(config).code() == MaiErrorCode::InvalidInput);
    const char* templateFile = std::getenv("MAI_LLAMA_CHAT_TEST_TEMPLATE");
    if (templateFile && *templateFile) {
        std::string approved;
        CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(templateFile), approved, 1024 * 1024));
        const auto appendString = [&](const std::string& value) {
            append(value.size(), 8);
            header += value;
        };
        const auto writeTemplates = [&](const std::string& primary, const std::string& toolUse) {
            // 仅构造结构合法的负例元数据，不加载或以此声称真实模型可推断。
            header = "GGUF";
            append(3, 4);
            append(1, 8);
            append(3, 8);
            appendString("general.architecture");
            append(8, 4);
            appendString("qwen2");
            appendString("tokenizer.chat_template");
            append(8, 4);
            appendString(primary);
            appendString("tokenizer.chat_template.tool_use");
            append(8, 4);
            appendString(toolUse);
            appendString("test.weight");
            append(1, 4);
            append(1, 8);
            append(0, 4);
            append(0, 8);
            header.append((32 - header.size() % 32) % 32, '\0');
            header.append(4, '\0');
            CHECK(!MaiFileSystem::writeFile(config.modelPath, header));
        };
        // 加一个无害注释保持模板语义可执行，确保拒绝来自精确摘要，而非语法/工具能力失败。
        writeTemplates(approved, approved);
        CHECK(!validateMaiLlamaModelConfig(config));
        const auto modified = approved + "\n{# unapproved template revision #}";
        writeTemplates(approved, modified);
        CHECK(validateMaiLlamaModelConfig(config).code() == MaiErrorCode::NotSupported);
        writeTemplates(modified, approved);
        CHECK(validateMaiLlamaModelConfig(config).code() == MaiErrorCode::NotSupported);
    } else {
        std::printf(
            "SKIP paired template digest rejection: MAI_LLAMA_CHAT_TEST_TEMPLATE is unset\n");
    }
    const char* tiny = std::getenv("MAI_LLAMA_TEST_MODEL");
    if (tiny && *tiny) {
        config.modelPath = MaiFilePath::fromUtf8(tiny);
        CHECK(validateMaiLlamaModelConfig(config).code() == MaiErrorCode::NotSupported);
    } else {
        std::printf(
            "SKIP TinyStories template rejection: MAI_LLAMA_TEST_MODEL is not configured\n");
    }
    MaiFileSystem::removeRecursively(directory);
}

// 只包裹生产客户端作观测；工具选择和每个输出 token 均来自实际 llama.cpp 推断。
class MaiObservedLocalClient final : public MaiModelClient {
public:
    explicit MaiObservedLocalClient(MaiLlamaModelConfig config)
        : mClient(makeMaiLlamaModelClient(std::move(config))) {}
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        requests.push_back(request);
        MaiStreamSink observed = sink;
        observed.onToolCall = [&](const MaiToolInvocation& call) {
            calls.push_back(call);
            if (sink.onToolCall) sink.onToolCall(call);
        };
        return mClient->stream(request, observed, cancel);
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Local;
    }
    std::vector<MaiModelRequest> requests;
    std::vector<MaiToolInvocation> calls;

private:
    std::unique_ptr<MaiModelClient> mClient;
};

void test_real_stream_limits_and_cancel(const MaiLlamaModelConfig& config) {
    std::atomic<bool> canceled{false};
    std::string text;
    std::size_t chunks = 0;
    MaiStreamSink sink;
    sink.onText = [&](std::string_view delta) {
        CHECK(validUtf8(std::string(delta)));
        text += delta;
        ++chunks;
    };
    const auto result = makeMaiLlamaModelClient(config)->stream(simpleRequest(), sink, canceled);
    if (result) std::printf("Real stream error: %s\n", result.message().c_str());
    CHECK(!result);
    CHECK(!text.empty() && chunks > 1 && validUtf8(text));
    std::printf("Real UTF-8 stream: chunks=%zu text=%s\n", chunks, text.c_str());
    CHECK(std::any_of(text.begin(), text.end(), [](unsigned char value) { return value >= 0x80; }));
    auto automatic = config;
    automatic.maximumTokens = 0;
    std::string automaticText;
    MaiStreamSink automaticSink;
    automaticSink.onText = [&](std::string_view delta) { automaticText += delta; };
    CHECK(!makeMaiLlamaModelClient(automatic)->stream(simpleRequest(), automaticSink, canceled));
    CHECK(!automaticText.empty() && validUtf8(automaticText));
    auto explicitLongBudget = config;
    explicitLongBudget.contextSize = 8192;
    explicitLongBudget.maximumTokens = 5000;
    CHECK(!validateMaiLlamaModelConfig(explicitLongBudget));

    auto toolRequest = simpleRequest();
    auto read = makeMaiReadTool();
    toolRequest.tools.push_back({read->name(), read->description(), read->parametersSchema()});
    toolRequest.messages[0].content = "Use file_read to read validation-marker.txt.";
    std::size_t calls = 0;
    MaiStreamSink limitedSink;
    limitedSink.onToolCall = [&](const MaiToolInvocation&) { ++calls; };
    auto limited = config;
    limited.maximumTokens = 1;
    CHECK(makeMaiLlamaModelClient(limited)->stream(toolRequest, limitedSink, canceled).hasError());
    CHECK(calls == 0);
    limited = config;
    limited.maximumOutputBytes = 1;
    CHECK(makeMaiLlamaModelClient(limited)->stream(toolRequest, limitedSink, canceled).hasError());
    CHECK(calls == 0);

    sink.onText = [&](std::string_view delta) {
        CHECK(validUtf8(std::string(delta)));
        canceled = true;
    };
    CHECK(makeMaiLlamaModelClient(config)->stream(simpleRequest(), sink, canceled).code() ==
          MaiErrorCode::Canceled);
    CHECK(canceled && !getSharedMaiLlamaRuntime()->status().busy);
    canceled = false;
    auto oversized = simpleRequest();
    oversized.messages[0].content = std::string(20000, 'x');
    limited = config;
    limited.contextSize = 256;
    limited.maximumTokens = 32;
    const auto contextError = makeMaiLlamaModelClient(limited)->stream(oversized, {}, canceled);
    CHECK(contextError.code() == MaiErrorCode::InvalidInput);
    CHECK(contextError.message().find("configured context is 256") != std::string::npos);
    CHECK(contextError.message().find("Increase context") != std::string::npos);
}

void test_real_execution_profile_isolation(const MaiLlamaModelConfig& config) {
    auto runtime = getSharedMaiLlamaRuntime();
    auto local = config;
    local.maximumTokens = 0;
    local.preferHardwareAcceleration = true;
    std::atomic<bool> canceled{false};
    CHECK(!makeMaiLlamaModelClient(local)->stream(simpleRequest(), {}, canceled));
    const auto primary = runtime->status();
    CHECK(primary.hasModel);
    CHECK(primary.gpuSelected == (primary.gpuModelBytes != 0));
    CHECK(primary.gpuSelected ? !primary.gpuBackend.empty() : primary.gpuBackend.empty());
    MaiLlamaRequest ordinary;
    ordinary.modelPath = config.modelPath;
    ordinary.contextSize = config.contextSize;
    ordinary.threads = config.threads;
    ordinary.maximumTokens = 8;
    ordinary.prompt = "Once upon a time";
    ordinary.useChatTemplate = false;
    CHECK(runtime->generate(ordinary));
    const auto cpu = runtime->status();
    CHECK(!cpu.gpuSelected && cpu.gpuModelBytes == 0 && cpu.gpuBackend.empty());
    CHECK(cpu.modelGeneration > primary.modelGeneration);
    CHECK(!makeMaiLlamaModelClient(local)->stream(simpleRequest(), {}, canceled));
    CHECK(runtime->status().modelGeneration > cpu.modelGeneration);
}

void test_real_agent_tool_loop(const MaiLlamaModelConfig& config) {
    const auto directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("local_agent_")));
    CHECK(!MaiFileSystem::createDirectories(directory));
    const std::string marker = MaiIdGenerator::generate("LOCAL_AGENT_MARKER_");
    CHECK(!MaiFileSystem::writeFile(
        directory.append(MaiFilePath::fromUtf8("validation-marker.txt")), marker + "\n"));
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(makeMaiReadTool());
    auto primaryConfig = config;
    primaryConfig.maximumTokens = 0;
    auto observed = std::make_unique<MaiObservedLocalClient>(primaryConfig);
    auto* model = observed.get();
    MaiAgent::Options options;
    options.defaultModel = "local:qa";
    options.maxToolIterations = 3;
    options.baseInstructions =
        "You are a file-reading assistant. Use file_read to read requested files before answering. "
        "Never invent file contents. After the tool result, return the complete first line only.";
    {
        std::size_t errors = 0;
        std::size_t deltas = 0;
        MaiAgent agent(makeMaiMemoryStore(), std::move(observed), std::move(tools), options);
        agent.eventBus().subscribe([&](const MaiEvent& event) {
            if (event.type == MaiEventType::SessionError) {
                ++errors;
                std::printf("Real Agent error: %s\n", event.detail.c_str());
            }
            if (event.type == MaiEventType::MessagePartDelta) ++deltas;
        });
        const auto created =
            agent.submit(MaiCreateSession{directory.toUtf8(), "Local QA", "local:qa"});
        CHECK(created);
        if (created) {
            CHECK(agent.submit(MaiSendPrompt{
                created.value(),
                "Read validation-marker.txt with file_read and return its complete first line. "
                "Do not guess or write files."}));
            agent.waitIdle();
            CHECK(errors == 0);
            CHECK(deltas > 1 && model->requests.size() >= 2 && !model->calls.empty());
            if (!model->requests.empty()) {
                CHECK(model->requests[0].baseInstructions.find(marker) == std::string::npos);
                for (const auto& message : model->requests[0].messages)
                    CHECK(message.content.find(marker) == std::string::npos);
            }
            bool matchedResult = false;
            for (std::size_t index = 1; index < model->requests.size(); ++index) {
                for (const auto& message : model->requests[index].messages) {
                    if (message.role != MaiModelRole::ToolResult ||
                        message.content.find(marker) == std::string::npos)
                        continue;
                    for (const auto& call : model->calls)
                        matchedResult |= call.id == message.toolCallId && call.name == "file_read";
                }
            }
            CHECK(matchedResult);
            bool realRead = false;
            std::string answer;
            for (const auto& message : agent.listMessages(created.value())) {
                if (message.role != MaiRole::Assistant) continue;
                answer += message.text();
                for (const auto& part : message.parts) {
                    const auto* tool = std::get_if<MaiToolPart>(&part.body);
                    if (tool && tool->tool == "file_read" &&
                        tool->state == MaiToolState::Completed &&
                        tool->output.find(marker) != std::string::npos)
                        realRead = true;
                }
            }
            CHECK(realRead);
            CHECK(answer.find(marker) != std::string::npos);
            std::printf(
                "Real local Agent: requests=%zu calls=%zu deltas=%zu matched_result=%d "
                "real_read=%d final_marker=%d\n",
                model->requests.size(), model->calls.size(), deltas, matchedResult, realRead,
                answer.find(marker) != std::string::npos);
        }
    }
    MaiFileSystem::removeRecursively(directory);
}
}  // namespace

int main(int argc, char** argv) {
    MaiThread::setCurrentName("mai-local-tests");
    if (argc == 2 && std::string(argv[1]) == "--exit-with-resident") {
        const char* model = std::getenv("MAI_LLAMA_CHAT_TEST_MODEL");
        if (!model || !*model) {
            std::printf("Real resident exit test requires MAI_LLAMA_CHAT_TEST_MODEL\n");
            return 1;
        }
        MaiLlamaModelConfig config;
        config.modelPath = MaiFilePath::fromUtf8(model);
        config.contextSize = 2048;
        config.maximumTokens = 192;
        std::atomic<bool> cancel{false};
        const auto error = makeMaiLlamaModelClient(config)->stream(simpleRequest(), {}, cancel);
        if (error || !getSharedMaiLlamaRuntime()->status().hasModel) {
            std::printf("Real resident exit precondition failed: %s\n", error.message().c_str());
            return 1;
        }
        std::printf("Real inference finished; exiting normally with the model still resident\n");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--system-normalization-only") {
        const char* model = std::getenv("MAI_LLAMA_CHAT_TEST_MODEL");
        if (!model || !*model) return 1;
        MaiLlamaModelConfig config;
        config.modelPath = MaiFilePath::fromUtf8(model);
        config.contextSize = 32768;
        test_system_instruction_utf8_and_separator_budget();
        test_real_system_messages_and_old_image_followup(config);
        CHECK(!getSharedMaiLlamaRuntime()->unload());
        std::printf("System/history normalization: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    test_preflight_without_model_io();
    test_system_instruction_utf8_and_separator_budget();
    test_bounded_model_validation();
    const char* model = std::getenv("MAI_LLAMA_CHAT_TEST_MODEL");
    if (model && *model) {
        MaiLlamaModelConfig config;
        config.modelPath = MaiFilePath::fromUtf8(model);
        config.contextSize = 2048;
        config.maximumTokens = 192;
        const auto before = getSharedMaiLlamaRuntime()->status();
        const auto validated = validateMaiLlamaModelConfig(config);
        if (validated) std::printf("Model validation error: %s\n", validated.message().c_str());
        CHECK(!validated);
        CHECK(before.modelGeneration == getSharedMaiLlamaRuntime()->status().modelGeneration);
        test_real_stream_limits_and_cancel(config);
        test_real_system_messages_and_old_image_followup(config);
        test_real_execution_profile_isolation(config);
        test_real_agent_tool_loop(config);
        CHECK(!getSharedMaiLlamaRuntime()->unload());
    } else {
        std::printf("SKIP real chat/Agent: MAI_LLAMA_CHAT_TEST_MODEL is not configured\n");
    }
    std::printf("MaiLlamaModelClientTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

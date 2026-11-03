// 文件作用：验证本地推理普通 API 与工具共用驻留模型、严格参数边界、取消及清理。
// 默认不下载权重；MAI_LLAMA_TEST_MODEL 可指定本地小型 GGUF，启用真实 CPU 推理回归。
// 单图正向回归须同时设置 MAI_LLAMA_TEST_VISION_MODEL、MAI_LLAMA_TEST_MMPROJ 和
// MAI_LLAMA_TEST_IMAGE；未提供时明确跳过，不以伪模型声称 VLM 已验证。
#include "MaiLlamaTools.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"
#include "MaiTool.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {
using Json = nlohmann::json;

class MaiLlamaWorkspace {
public:
    MaiLlamaWorkspace()
        : mRoot(MaiFileSystem::temporaryDirectory().append(
              MaiFilePath::fromUtf8(MaiIdGenerator::generate("llama_test_")))) {
        CHECK(!MaiFileSystem::createDirectories(mRoot));
    }
    ~MaiLlamaWorkspace() {
        MaiFileSystem::removeRecursively(mRoot);
    }
    MaiFilePath path(const std::string& name) const {
        return mRoot.append(MaiFilePath::fromUtf8(name));
    }
    MaiToolContext context() const {
        MaiToolContext context;
        context.root = mRoot.toUtf8();
        return context;
    }

private:
    MaiFilePath mRoot;
};

MaiLlamaRequest baseRequest(const MaiFilePath& model) {
    MaiLlamaRequest request;
    request.modelPath = model;
    request.prompt = "Once upon a time";
    request.contextSize = 256;
    request.maximumTokens = 32;
    request.threads = 1;
    request.useChatTemplate = false;
    return request;
}

// JSON 编码器严格检查字符串 UTF-8；这里只验证交付契约，不复制生产截断实现。
bool validUtf8(const std::string& value) {
    try {
        const auto encoded = Json(value).dump();
        return Json::parse(encoded).get<std::string>() == value;
    } catch (...) {
        return false;
    }
}

void test_empty_status_and_strict_protocol(const std::shared_ptr<MaiLlamaRuntime>& runtime) {
    CHECK(!runtime->unload());
    CHECK(runtime == getSharedMaiLlamaRuntime());
    const auto before = runtime->status();
    CHECK(!before.busy && !before.hasModel && !before.hasProjector);
    MaiLlamaWorkspace workspace;
    auto tool = makeMaiLlamaTool(runtime);
    CHECK(tool->name() == "llama");
    CHECK(!tool->requiresApproval(R"({"arguments":["--status"]})"));
    CHECK(Json::parse(tool->parametersSchema()).value("additionalProperties", true) == false);
    const auto status = tool->execute(R"({"arguments":["--status"]})", workspace.context());
    CHECK(!status.hasError());
    const auto parsed = Json::parse(status.output(), nullptr, false);
    CHECK(parsed.is_object() && !parsed.value("has_model", true));
    CHECK(!parsed.contains("model_path") && !parsed.contains("prompt"));
    const auto emptyUnload = tool->execute(R"({"arguments":["--unload"]})", workspace.context());
    CHECK(!emptyUnload.hasError());
    CHECK(runtime->status().modelGeneration == before.modelGeneration);
    const std::vector<std::string> rejected = {
        "{}",
        "[]",
        "null",
        "{",
        R"({"arguments":[]})",
        R"({"arguments":[1]})",
        R"({"arguments":["--status"],"unknown":true})",
        R"({"arguments":["--status","--status"]})",
        R"({"arguments":["--unload","--status"]})",
        R"({"arguments":["--unload","--unload"]})",
        R"({"arguments":["--unload","--model","model.gguf"]})",
        R"({"arguments":["--no-conversation","--unload"]})",
        R"({"arguments":["--hf-repo","remote/model"]})",
        R"({"arguments":["--prompt-file","prompt.txt"]})",
        R"({"arguments":["--model","https://example.invalid/model.gguf"]})",
        R"({"arguments":["--model","file:/model.gguf"]})",
        R"({"arguments":["--model","-"]})",
        R"({"arguments":["--model","../outside.gguf"]})",
        R"({"arguments":["-m","a.gguf","--model","b.gguf"]})",
        R"({"arguments":["--threads","-1"]})",
        R"({"arguments":["--threads","4294967296"]})",
        R"({"arguments":["--temp","NaN"]})",
        R"({"arguments":["--temp","{}"]})",
        R"({"arguments":["--prompt"]})"};
    for (const auto& input : rejected) {
        CHECK(tool->execute(input, workspace.context()).hasError());
    }
    std::atomic<bool> canceled{true};
    auto context = workspace.context();
    context.cancel = &canceled;
    CHECK(tool->execute(R"({"arguments":["--status"]})", context).error().code() ==
          MaiErrorCode::Canceled);
    CHECK(runtime->status().modelGeneration == before.modelGeneration);
}

void test_direct_bounds_and_image_preflight(const std::shared_ptr<MaiLlamaRuntime>& runtime) {
    MaiLlamaWorkspace workspace;
    const auto model = workspace.path("invalid.gguf");
    CHECK(!MaiFileSystem::writeFile(model, "GGUF" + std::string(32, '\0')));
    const auto initialGeneration = runtime->status().modelGeneration;
    auto invalid = baseRequest(model);
    invalid.prompt.clear();
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid.prompt = std::string("\xc0\xaf", 2);
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid.prompt = std::string(65537, 'a');
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(model);
    invalid.contextSize = 32769;
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(model);
    invalid.maximumTokens = invalid.contextSize;
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(model);
    invalid.temperature = std::numeric_limits<float>::infinity();
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(model);
    invalid.maximumOutputBytes = 0;
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(model);
    invalid.threads = 65;
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(MaiFilePath::fromUtf8("relative.gguf"));
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(workspace.path("missing.gguf"));
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::NotFound);
    std::atomic<bool> canceled{true};
    CHECK(runtime->generate(invalid, &canceled).error().code() == MaiErrorCode::Canceled);
    invalid = baseRequest(model);
    invalid.projectorPath = model;
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid = baseRequest(model);
    invalid.imagePath = workspace.path("image.ppm");
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    invalid.projectorPath = model;
    CHECK(!MaiFileSystem::writeFile(invalid.imagePath, "unrecognized image"));
    CHECK(runtime->generate(invalid).error().code() == MaiErrorCode::InvalidInput);
    // PNM 头部尺寸已超限，数据体刻意缺失；须在 GGUF 加载及图片完整解码前拒绝。
    CHECK(!MaiFileSystem::writeFile(invalid.imagePath, "P6\n10000 10000\n255\n"));
    const auto oversized = runtime->generate(invalid);
    CHECK(!oversized && oversized.error().code() == MaiErrorCode::InvalidInput);
    CHECK(!runtime->status().hasModel);
    CHECK(runtime->status().modelGeneration == initialGeneration);
    invalid = baseRequest(model);
    const auto malformed = runtime->generate(invalid);
    CHECK(!malformed);
    CHECK(!runtime->status().hasModel && !runtime->status().busy);
    CHECK(runtime->status().modelGeneration == initialGeneration);
}

void test_real_text_and_shared_tool(const std::shared_ptr<MaiLlamaRuntime>& runtime,
                                    const MaiFilePath& fixture) {
    MaiLlamaWorkspace workspace;
    std::string weights;
    CHECK(!MaiFileSystem::readFile(fixture, weights, 64 * 1024 * 1024));
    if (weights.empty()) return;
    const auto localModel = workspace.path("model.gguf");
    CHECK(!MaiFileSystem::writeFile(localModel, weights));
    auto request = baseRequest(localModel);
    const auto first = runtime->generate(request);
    if (!first) {
        std::printf("Real engine failed: %s\n", first.error().message().c_str());
        CHECK(first.isOk());
        return;
    }
    CHECK(!first.value().text.empty() && first.value().generatedTokens > 0);
    CHECK(validUtf8(first.value().text));
    const auto resident = runtime->status();
    CHECK(resident.hasModel && !resident.hasProjector && !resident.busy);
    CHECK(resident.contextSize == request.contextSize);
    const auto repeated = runtime->generate(request);
    CHECK(repeated && repeated.value().text == first.value().text);
    CHECK(runtime->status().modelGeneration == resident.modelGeneration);
    auto tool = makeMaiLlamaTool();
    const auto generated = tool->execute(Json{{"arguments",
                                               {"-m", "model.gguf", "-p", request.prompt, "-n",
                                                "32", "-c", "256", "-t", "1", "--no-conversation"}}}
                                             .dump(),
                                         workspace.context());
    CHECK(!generated.hasError());
    if (!generated.hasError()) {
        const auto value = Json::parse(generated.output());
        CHECK(value.at("text").get<std::string>() == first.value().text);
        CHECK(value.at("truncated").get<bool>() == generated.isTruncated());
    }
    CHECK(runtime->status().modelGeneration == resident.modelGeneration);
    request.maximumOutputBytes = 1;
    const auto bounded = runtime->generate(request);
    CHECK(bounded && bounded.value().text.size() <= 1 && validUtf8(bounded.value().text));
    CHECK(bounded && bounded.value().truncated &&
          bounded.value().stopReason == MaiLlamaStopReason::OutputLimit);
    request.maximumOutputBytes = 65536;
    request.prompt = std::string(2000, 'a');
    const auto tooManyTokens = runtime->generate(request);
    CHECK(!tooManyTokens && tooManyTokens.error().code() == MaiErrorCode::InvalidInput);
    request = baseRequest(localModel);
    request.contextSize = 512;
    CHECK(runtime->generate(request));
    CHECK(runtime->status().modelGeneration == resident.modelGeneration + 1);
    // 不截断仍在 mmap 的 inode：先 unlink，再创建尺寸不同的新 inode，检查路径缓存失效。
    CHECK(!MaiFileSystem::removeFile(localModel));
    CHECK(!MaiFileSystem::writeFile(localModel, weights + '\0'));
    CHECK(runtime->generate(request));
    CHECK(runtime->status().modelGeneration == resident.modelGeneration + 2);
    const auto beforeUnload = runtime->status();
    const auto unloaded = tool->execute(R"({"arguments":["--unload"]})", workspace.context());
    CHECK(!unloaded.hasError());
    if (!unloaded.hasError()) {
        const auto value = Json::parse(unloaded.output());
        CHECK(!value.at("has_model").get<bool>() && !value.at("has_projector").get<bool>());
        CHECK(value.at("context_size").get<std::uint32_t>() == 0);
        CHECK(value.at("model_generation").get<std::uint64_t>() == beforeUnload.modelGeneration);
    }
    CHECK(!runtime->status().hasModel && !runtime->status().hasProjector);
    CHECK(runtime->status().modelGeneration == beforeUnload.modelGeneration);
    std::printf("Real CPU text/tool/reuse/output/file-change tests passed\n");
}

void test_real_cancel_and_waiter(const std::shared_ptr<MaiLlamaRuntime>& runtime,
                                 const MaiFilePath& fixture) {
    auto request = baseRequest(fixture);
    request.contextSize = 32768;
    // 先完成相同配置的模型/context 分配，后续取消目标为真正的推理阶段。
    CHECK(runtime->generate(request));
    request.maximumTokens = 4096;
    request.prompt.clear();
    for (int index = 0; index < 1200; ++index)
        request.prompt += "Once upon a time there was a little girl. ";
    std::atomic<bool> activeCancel{false};
    std::atomic<bool> waiterCancel{false};
    std::atomic<bool> activeDone{false};
    std::atomic<bool> waiterDone{false};
    MaiResult<MaiLlamaResponse> active{MaiErrorCode::Internal, "Not run"};
    MaiResult<MaiLlamaResponse> waiter{MaiErrorCode::Internal, "Not run"};
    std::thread worker([&] {
        MaiThread::setCurrentName("mai-llama-run");
        active = runtime->generate(request, &activeCancel);
        activeDone.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    while (!runtime->status().busy && !activeDone.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(runtime->status().busy && !activeDone.load());
    auto tool = makeMaiLlamaTool(runtime);
    const auto refusedUnload = tool->execute(R"({"arguments":["--unload"]})", {});
    CHECK(refusedUnload.hasError() && refusedUnload.error().code() == MaiErrorCode::Busy);
    CHECK(runtime->status().busy && !activeDone.load());
    std::thread waiting([&] {
        MaiThread::setCurrentName("mai-llama-wait");
        waiter = runtime->generate(baseRequest(fixture), &waiterCancel);
        waiterDone.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    waiterCancel.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!waiterDone.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool canceledWhileActive = waiterDone.load() && !activeDone.load();
    activeCancel.store(true);
    waiting.join();
    worker.join();
    CHECK(canceledWhileActive);
    CHECK(!waiter && waiter.error().code() == MaiErrorCode::Canceled);
    CHECK(!active && active.error().code() == MaiErrorCode::Canceled);
    CHECK(!runtime->status().busy);
    CHECK(runtime->generate(baseRequest(fixture)));
    CHECK(!runtime->unload());
    std::printf("Real CPU active and queued cancellation tests passed\n");
}

void test_optional_real_vision(const std::shared_ptr<MaiLlamaRuntime>& runtime) {
    const char* model = std::getenv("MAI_LLAMA_TEST_VISION_MODEL");
    const char* projector = std::getenv("MAI_LLAMA_TEST_MMPROJ");
    const char* image = std::getenv("MAI_LLAMA_TEST_IMAGE");
    if (!model || !projector || !image) {
        std::printf(
            "SKIP real VLM: set MAI_LLAMA_TEST_VISION_MODEL, "
            "MAI_LLAMA_TEST_MMPROJ and MAI_LLAMA_TEST_IMAGE\n");
        return;
    }
    auto request = baseRequest(MaiFilePath::fromUtf8(model));
    request.contextSize = 4096;
    request.projectorPath = MaiFilePath::fromUtf8(projector);
    request.imagePath = MaiFilePath::fromUtf8(image);
    request.prompt = "Describe the image briefly.";
    request.useChatTemplate = true;
    const auto response = runtime->generate(request);
    if (!response) std::printf("VLM error: %s\n", response.error().message().c_str());
    CHECK(response && response.value().promptTokens > 0 && validUtf8(response.value().text));
    CHECK(runtime->status().hasProjector);
    CHECK(!runtime->unload());
}
}  // namespace

int main() {
    MaiThread::setCurrentName("mai-llama-test");
    const auto runtime = getSharedMaiLlamaRuntime();
    test_empty_status_and_strict_protocol(runtime);
    test_direct_bounds_and_image_preflight(runtime);
    const char* fixture = std::getenv("MAI_LLAMA_TEST_MODEL");
    if (fixture && *fixture) {
        const auto path = MaiFilePath::fromUtf8(fixture);
        test_real_text_and_shared_tool(runtime, path);
        test_real_cancel_and_waiter(runtime, path);
    } else {
        std::printf("SKIP real CPU inference: set MAI_LLAMA_TEST_MODEL to a local GGUF\n");
    }
    test_optional_real_vision(runtime);
    CHECK(!runtime->unload());
    std::printf("MaiLlamaToolsTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

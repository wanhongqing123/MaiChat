// 文件作用：用可控的假 runtime 验证普通 C ABI 的 job 所有权、重复 run、取消与结果释放。
// 这里只替换推理器，不声称通过真实权重/解码测试；真核心无模型测试另在 MaiLocalInferenceTests。
#include "MaiLocalInference.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "MaiLlamaTools.h"

namespace {

std::mutex gateMutex;
std::condition_variable gateWake;
std::atomic<int> entered{0};
std::atomic<int> waitingCount{0};
std::atomic<int> unloadEntries{0};
std::atomic<bool> resident{true};
std::mutex operationMutex;

void check(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

MaiLocalInferenceRequest request(const char* prompt = "fixture") {
    auto result = mai_local_inference_request_defaults();
    static constexpr const char model[] = "/synthetic/model.gguf";
    result.model_path = {model, sizeof(model) - 1};
    result.prompt = {prompt, std::strlen(prompt)};
    return result;
}

void expectStatus(MaiLocalInferenceResult result, MaiLocalInferenceStatus status) {
    check(result.status == status, "unexpected boundary status");
    if (status != MAI_LOCAL_INFERENCE_OK) {
        check(result.text_utf8 == nullptr && result.text_size == 0, "failure returned partial text");
        check(result.error_utf8 != nullptr, "failure has no safe explanation");
        check(std::strstr(result.error_utf8, "private-marker") == nullptr, "boundary exposed raw engine exception");
    }
    mai_local_inference_result_free(&result);
    mai_local_inference_result_free(&result);
    check(result.text_utf8 == nullptr && result.error_utf8 == nullptr, "free did not reset result ownership");
}

void waitsForEntry(int previous) {
    std::unique_lock<std::mutex> lock(gateMutex);
    check(gateWake.wait_for(lock, std::chrono::seconds(3), [&] { return entered.load() > previous; }),
          "fake runtime did not enter");
}

void runChecks() {
    const auto initial = mai_local_inference_status();
    check(initial.status == MAI_LOCAL_INFERENCE_OK && !initial.busy && initial.has_model && initial.has_projector &&
              initial.context_size == 2048 && initial.model_generation == 17,
          "status did not come from the shared runtime");
    expectStatus(mai_local_inference_generate(0, nullptr), MAI_LOCAL_INFERENCE_NOT_FOUND);
    check(mai_local_inference_job_cancel(0) == 0 && mai_local_inference_job_destroy(0) == 0, "zero ID was accepted");
    for (const auto* text : {"fixture", "fail", "throw", "fixture"}) {
        const auto id = mai_local_inference_job_create();
        auto input = request(text);
        auto output = mai_local_inference_generate(id, &input);
        if (std::strcmp(text, "fixture") == 0) {
            check(output.status == MAI_LOCAL_INFERENCE_OK && output.text_size == 12 &&
                      std::string(output.text_utf8, output.text_size) == "fixture text" && output.prompt_tokens == 7 &&
                      output.generated_tokens == 3 && output.stop_reason == MAI_LOCAL_INFERENCE_OUTPUT_LIMIT &&
                      output.truncated == 1,
                  "successful result metadata was changed");
            mai_local_inference_result_free(&output);
        } else {
            expectStatus(output, MAI_LOCAL_INFERENCE_INTERNAL);
        }
        expectStatus(mai_local_inference_generate(id, &input), MAI_LOCAL_INFERENCE_BUSY);
        check(mai_local_inference_job_destroy(id) == 1, "finished job was not released");
        check(mai_local_inference_job_destroy(id) == 0, "ID was destroyed twice");
    }

    const int callsBeforeInvalid = entered.load();
    for (int kind = 0; kind < 7; ++kind) {
        auto input = request();
        constexpr const char malformed[] = {'\xED', '\xA0', '\x80'};
        constexpr const char nul[] = {'a', '\0', 'b'};
        if (kind == 0) input.prompt = {nul, sizeof(nul)};
        if (kind == 1) input.prompt = {malformed, sizeof(malformed)};
        if (kind == 2) input.prompt = {nullptr, 4};
        if (kind == 3) input.prompt.size = 64 * 1024 + 1;
        if (kind == 4) input.model_path = {"relative.gguf", 13};
        if (kind == 5) input.image_path = {"/image.png", 10};
        if (kind == 6) input.use_chat_template = 2;
        const auto id = mai_local_inference_job_create();
        expectStatus(mai_local_inference_generate(id, &input), MAI_LOCAL_INFERENCE_INVALID_INPUT);
        mai_local_inference_job_destroy(id);
    }
    check(entered.load() == callsBeforeInvalid, "malformed request reached runtime");

    const auto canceled = mai_local_inference_job_create();
    check(mai_local_inference_job_cancel(canceled) == 1, "pre-run cancellation was lost");
    auto input = request();
    expectStatus(mai_local_inference_generate(canceled, &input), MAI_LOCAL_INFERENCE_CANCELED);
    mai_local_inference_job_destroy(canceled);

    // destroy 必须能与已经进入同步生成的 cancel 指针读取重叠；不能只测返回后的释放。
    for (int iteration = 0; iteration < 40; ++iteration) {
        const auto id = mai_local_inference_job_create();
        const int previous = entered.load();
        auto running = std::async(std::launch::async, [id] {
            auto waiting = request("wait");
            return mai_local_inference_generate(id, &waiting);
        });
        waitsForEntry(previous);
        const auto busyState = mai_local_inference_status();
        check(busyState.status == MAI_LOCAL_INFERENCE_OK && busyState.busy,
              "status did not remain available during generation");
        expectStatus(mai_local_inference_generate(id, &input), MAI_LOCAL_INFERENCE_BUSY);
        std::thread canceler([id] { mai_local_inference_job_cancel(id); });
        check(mai_local_inference_job_destroy(id) == 1, "running job was not detached");
        canceler.join();
        check(running.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
              "destroy did not cooperatively cancel running job");
        expectStatus(running.get(), MAI_LOCAL_INFERENCE_CANCELED);
        const auto next = mai_local_inference_job_create();
        check(next != 0 && next != id, "destroyed ID was reused");
        check(mai_local_inference_job_cancel(id) == 0, "stale ID canceled another job");
        mai_local_inference_job_destroy(next);
    }

    std::vector<MaiLocalInferenceJob> held;
    for (int index = 0; index < 64; ++index) {
        const auto id = mai_local_inference_job_create();
        check(id != 0, "job capacity leaked after cleanup");
        held.push_back(id);
    }
    check(mai_local_inference_job_create() == 0, "job registry is unbounded");
    for (const auto id : held) mai_local_inference_job_destroy(id);
    const auto recovered = mai_local_inference_job_create();
    check(recovered != 0, "job capacity did not recover");
    mai_local_inference_job_destroy(recovered);
}

void checkUnload() {
    const auto job = mai_local_inference_job_create();
    const int previous = entered.load();
    auto generating = std::async(std::launch::async, [job] {
        auto input = request("wait");
        return mai_local_inference_generate(job, &input);
    });
    waitsForEntry(previous);
    const int previousUnload = unloadEntries.load();
    auto unloading = std::async(std::launch::async, [] { return mai_local_inference_unload(); });
    bool unloadEntered = false;
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        unloadEntered =
            gateWake.wait_for(lock, std::chrono::seconds(3), [&] { return unloadEntries.load() > previousUnload; });
    }
    const bool waited = unloading.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
    const bool stillGenerating = waitingCount.load() == 1;
    // 只取消本测试持有的 job；unload 不能为了释放模型自行中止另一个宿主。
    mai_local_inference_job_cancel(job);
    expectStatus(generating.get(), MAI_LOCAL_INFERENCE_CANCELED);
    mai_local_inference_job_destroy(job);
    const auto unloaded = unloading.get();
    check(unloadEntered && waited && stillGenerating, "unload canceled or skipped another host's generation");
    check(unloaded == MAI_LOCAL_INFERENCE_OK && mai_local_inference_unload() == MAI_LOCAL_INFERENCE_OK,
          "unload was not successful/idempotent");
    const auto state = mai_local_inference_status();
    check(state.status == MAI_LOCAL_INFERENCE_OK && !state.has_model && !state.has_projector && !state.busy &&
              state.context_size == 0 && state.model_generation == 17,
          "unload did not release resident state or changed generation");
}

}  // namespace

// 与正式 ABI 同名的假推理实现只链接进本测试，不链接 maiagent；因此可确定性阻塞并制造异常。
// 文件路径转换/错误类仍使用正式实现，测试没有任何下载或模型文件。
class MaiLlamaRuntime::Implementation {};
MaiLlamaRuntime::MaiLlamaRuntime() : mImplementation(std::make_unique<Implementation>()) {}
MaiLlamaRuntime::~MaiLlamaRuntime() = default;
std::shared_ptr<MaiLlamaRuntime> getSharedMaiLlamaRuntime() {
    static auto runtime = std::shared_ptr<MaiLlamaRuntime>(new MaiLlamaRuntime);
    return runtime;
}
MaiResult<MaiLlamaResponse> MaiLlamaRuntime::generate(const MaiLlamaRequest& input, const std::atomic<bool>* cancel) {
    std::lock_guard<std::mutex> operation(operationMutex);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        if (input.prompt == "wait") ++waitingCount;
        ++entered;
    }
    gateWake.notify_all();
    if (input.prompt == "wait") {
        std::unique_lock<std::mutex> lock(gateMutex);
        while (!cancel->load()) gateWake.wait_for(lock, std::chrono::milliseconds(2));
        --waitingCount;
        return {MaiErrorCode::Canceled, "private-marker canceled"};
    }
    if (input.prompt == "throw") throw std::runtime_error("private-marker exception");
    if (input.prompt == "fail") return {MaiErrorCode::Internal, "private-marker error"};
    resident.store(true);
    return MaiLlamaResponse{"fixture text", 7, 3, MaiLlamaStopReason::OutputLimit, true};
}
MaiLlamaStatus MaiLlamaRuntime::status() const {
    return {waitingCount.load() != 0, resident.load(), resident.load(), resident.load() ? 2048U : 0U, 17};
}
MaiError MaiLlamaRuntime::unload() {
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        ++unloadEntries;
    }
    gateWake.notify_all();
    std::lock_guard<std::mutex> operation(operationMutex);
    resident.store(false);
    return {};
}

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::strcmp(argv[1], "--unload-only") != 0) runChecks();
        checkUnload();
        std::puts("PASS local inference C ABI lifecycle (synthetic runtime only)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}

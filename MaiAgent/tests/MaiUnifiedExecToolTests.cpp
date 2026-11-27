// 文件作用：按 Codex 0.172 的公开参数和输出契约验证命令工具，不调用真实模型或用户数据。
#include <cstdio>
#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiExecCommandTool.h"
#include "MaiIdGenerator.h"
#include "MaiProcess.h"
#include "MaiShellSessionManager.h"
#include "MaiTool.h"

namespace {
using json = nlohmann::json;
int failures = 0;
std::string executable;
#define CHECK(value)                                        \
    do {                                                    \
        if (!(value)) {                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                     \
        }                                                   \
    } while (0)

struct Workspace {
    MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-unified-exec-" + MaiIdGenerator::generate("test")));
    Workspace() {
        CHECK(!MaiFileSystem::createDirectories(path));
    }
    ~Workspace() {
        MaiFileSystem::removeRecursively(path);
    }
    MaiToolContext context() const {
        MaiToolContext value;
        value.root = path.toUtf8();
        value.sessionId = "owned-exec-session";
        return value;
    }
};

json object(const MaiToolResult& result) {
    CHECK(!result.hasError());
    auto value = json::parse(result.output(), nullptr, false);
    CHECK(value.is_object());
    return value.is_object() ? value : json::object();
}

void test_catalog_and_schema() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK(!registry.find("shell"));
    CHECK(!registry.find("shell_input"));
    auto* exec = registry.find("exec_command");
    auto* input = registry.find("write_stdin");
    CHECK((exec != nullptr) == maiIsProcessExecutionSupported());
    CHECK((input != nullptr) == maiIsProcessExecutionSupported());
    if (!exec || !input) return;
    const auto spec = json::parse(exec->parametersSchema());
    CHECK(spec["required"] == json::array({"cmd"}));
    CHECK(!spec["properties"].contains("command"));
    CHECK(!spec["properties"].contains("timeout_ms"));
    CHECK(spec["properties"]["tty"]["type"] == "boolean");
    const auto stdinSpec = json::parse(input->parametersSchema());
    CHECK(stdinSpec["required"] == json::array({"session_id"}));
    CHECK(stdinSpec["properties"]["session_id"]["type"] == "integer");
    CHECK(!stdinSpec["properties"].contains("terminate"));
}

void test_completed_shape_and_no_footer() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* exec = registry.find("exec_command");
    auto* input = registry.find("write_stdin");
    if (!exec || !input) return;
    const json args{{"cmd", "echo owned-unified-exec"}, {"login", false}};
    const auto result = exec->execute(args.dump(), workspace.context());
    const auto value = object(result);
    CHECK(value.contains("chunk_id") && value["chunk_id"].is_string());
    CHECK(value.contains("wall_time_seconds") && value["wall_time_seconds"].is_number());
    CHECK(value.value("exit_code", -999) == 0);
    CHECK(!value.contains("session_id"));
    CHECK(value.value("output", "").find("owned-unified-exec") != std::string::npos);
    CHECK(value.size() == 4);
    CHECK(result.output().find("shell_result") == std::string::npos);
    CHECK(result.execution() && result.execution()->exitCode == 0);
    CHECK(exec->execute(R"({"command":"echo legacy"})", workspace.context()).hasError());
    CHECK(exec->execute(R"({"cmd":"echo legacy","timeout_ms":120000})", workspace.context())
              .hasError());
    for (const auto& id : {json("1"), json(0), json(-1), json(1.5), json(2147483648ULL)}) {
        CHECK(input->execute(json{{"session_id", id}}.dump(), workspace.context()).hasError());
    }
    registry.closeSession(workspace.context().sessionId);
}

void test_preview_budget_and_typed_validation() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* exec = registry.find("exec_command");
    if (!exec) return;
    const auto command = "echo HEAD-" + std::string(4096, 'x') + "-TAIL";
    for (const int budget : {0, 1, 4, 128}) {
        const auto result = exec->execute(
            json{{"cmd", command}, {"login", false}, {"max_output_tokens", budget}}.dump(),
            workspace.context());
        const auto value = object(result);
        CHECK(value.value("output", "").size() <= static_cast<std::size_t>(budget) * 4);
        CHECK(value.value("original_token_count", 0) >= 1024);
        CHECK(value.value("output_unavailable", false));
        CHECK(!value.contains("output_id"));
        CHECK(result.execution() && result.execution()->outputId.empty());
        CHECK(result.output().find("pipeline_status_note") == std::string::npos);
    }
    for (const auto& value : {json(-1), json(true), json(2.5), json("100")}) {
        CHECK(exec->execute(json{{"cmd", "echo invalid"}, {"yield_time_ms", value}}.dump(),
                            workspace.context())
                  .hasError());
    }
    const auto oversized =
        exec->execute(json{{"cmd", "echo bounded"},
                           {"login", false},
                           {"yield_time_ms", std::numeric_limits<std::uint64_t>::max()},
                           {"max_output_tokens", std::numeric_limits<std::uint64_t>::max()}}
                          .dump(),
                      workspace.context());
    if (oversized.hasError())
        std::printf("oversized owned request: %s\n", oversized.error().message().c_str());
    CHECK(object(oversized).value("exit_code", -1) == 0);
    registry.closeSession(workspace.context().sessionId);
}

void test_default_empty_poll_waits_for_background_bound(bool talkative) {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto manager = std::make_shared<MaiShellSessionManager>();
    MaiShellSessionOptions options;
    options.workdir = workspace.path.toUtf8();
    options.login = false;
#if defined(_WIN32)
    options.shell = "powershell";
    options.command = "& '" + executable + "' " +
                      (talkative ? "--owned-talkative-child" : "--owned-silent-child");
#else
    options.command =
        "'" + executable + "' " + (talkative ? "--owned-talkative-child" : "--owned-silent-child");
#endif
    const auto id = manager->start(workspace.context().sessionId, options);
    CHECK(id);
    if (!id) return;
    auto input = makeMaiWriteStdinTool(manager);
    const auto began = std::chrono::steady_clock::now();
    const auto result =
        input->execute(json{{"session_id", std::stoi(id.value())}}.dump(), workspace.context());
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    CHECK(seconds >= 4.8 && seconds < 8.0);
    CHECK(object(result).value("session_id", 0) == std::stoi(id.value()));
    manager->stopOwner(workspace.context().sessionId);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--owned-silent-child" ||
                      std::string(argv[1]) == "--owned-talkative-child")) {
        if (std::string(argv[1]) == "--owned-talkative-child") {
            std::cout << "owned-first-chunk\n" << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::seconds(12));
        return 0;
    }
    executable = MaiFileSystem::resolve(MaiFilePath::fromUtf8(argv[0])).toUtf8();
    test_catalog_and_schema();
    test_completed_shape_and_no_footer();
    test_preview_budget_and_typed_validation();
    test_default_empty_poll_waits_for_background_bound(false);
    test_default_empty_poll_waits_for_background_bound(true);
    std::printf("Unified exec protocol failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}

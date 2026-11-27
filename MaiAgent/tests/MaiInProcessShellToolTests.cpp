// 文件作用：验证移动模型命令入口与普通进程内核心使用相同文件边界和真实状态。
#include <atomic>
#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiInProcessShellTool.h"

namespace {
int failures = 0;
#define CHECK(value)                                        \
    do {                                                    \
        if (!(value)) {                                     \
            ++failures;                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #value); \
        }                                                   \
    } while (false)

void testNativeMobileScript() {
    const auto app = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-mobile-command-")));
    const auto workspace = app.append(MaiFilePath::fromUtf8("workspace"));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    MaiToolContext context;
    context.root = workspace.toUtf8();
    context.fileAccessRoot = app.toUtf8();
    context.sessionId = "owned-mobile-a";
    auto tool = makeMaiInProcessShellTool();
    CHECK(tool->name() == "exec_command");
    CHECK(tool->requiresApproval("{}"));
    const auto execute = [&](const std::string& command) {
        return tool->execute(nlohmann::json{{"cmd", command}}.dump(), context);
    };
    // 中文空格路径、变量、重定向、短路与管道都通过真实普通文件系统。
    const auto run = execute(
        "NAME='\u6587\u4ef6 a.txt'; echo hello > \"$NAME\"; "
        "cat \"$NAME\" | grep -F hello && cp \"$NAME\" copied.txt");
    CHECK(!run.hasError());
    if (run.hasError()) std::printf("SCRIPT_DIAG %s\n", run.error().message().c_str());
    if (!run.hasError()) {
        const auto receipt = nlohmann::json::parse(run.output());
        CHECK(receipt.value("exit_code", -1) == 0);
        CHECK(receipt.value("execution_mode", "") == "in_process");
        CHECK(receipt.value("output", "").find("hello") != std::string::npos);
        CHECK(!receipt.contains("session_id"));
    }
    std::string content;
    CHECK(!MaiFileSystem::readFile(workspace.append(MaiFilePath::fromUtf8("copied.txt")), content));
    CHECK(content == "hello\n");
    const auto read = execute("read-text copied.txt");
    CHECK(!read.hasError() && read.output().find("hello") != std::string::npos);
    // 目录不能逃出 App 容器；预解析拒绝后半段未实现语法时不能先创建前面的文件。
    CHECK(execute("echo secret > ../../outside.txt").hasError());
    CHECK(execute("echo bad > partial.txt; echo $(pwd)").hasError());
    CHECK(!MaiFileSystem::exists(workspace.append(MaiFilePath::fromUtf8("partial.txt"))));
    CHECK(execute("curl --version").hasError());
    CHECK(tool->execute(R"({"cmd":"pwd","tty":true})", context).hasError());
    CHECK(tool->execute(R"({"cmd":"pwd","max_output_tokens":false})", context).hasError());
    CHECK(tool->execute(R"({"cmd":"pwd","workdir":42})", context).hasError());
    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    CHECK(execute("echo no > canceled.txt").error().code() == MaiErrorCode::Canceled);
    CHECK(!MaiFileSystem::exists(workspace.append(MaiFilePath::fromUtf8("canceled.txt"))));
    context.cancel = nullptr;
    const auto large = tool->execute(
        nlohmann::json{{"cmd", "echo '" + std::string(4096, 'a') + "'"}, {"max_output_tokens", 256}}
            .dump(),
        context);
    CHECK(!large.hasError());
    if (!large.hasError()) {
        const auto receipt = nlohmann::json::parse(large.output());
        CHECK(receipt.value("output_truncated", false));
        CHECK(receipt.value("output", "").size() <= 1024);
    }
    CHECK(execute("find . -name '*.txt' --limit 1").output().find("next_cursor") !=
          std::string::npos);
    CHECK(!execute("stat copied.txt").hasError());
    CHECK(!execute("mkdir empty; rmdir empty").hasError());
    CHECK(execute("rmdir .").hasError());
    CHECK(tool->description().find("Use native file_glob") == std::string::npos);
    CHECK(tool->description().find("apply_patch") != std::string::npos);
    // 通过同一薄工具的两个独立调用续页，不能因每次创建 host 而丢失可信游标服务。
    const auto pagePath = workspace.append(MaiFilePath::fromUtf8("pages.txt"));
    CHECK(!MaiFileSystem::writeFile(pagePath, "one\ntwo\nthree\n"));
    const auto first = execute("read-text --limit 1 pages.txt");
    CHECK(!first.hasError());
    if (!first.hasError()) {
        const auto outer = nlohmann::json::parse(first.output());
        const auto page = nlohmann::json::parse(outer.at("output").get<std::string>());
        const auto token = page.value("next_cursor", std::string());
        CHECK(!token.empty());
        const auto next = execute("read-text --limit 1 --cursor '" + token + "' pages.txt");
        CHECK(!next.hasError());
        if (!next.hasError()) {
            const auto receipt = nlohmann::json::parse(next.output());
            const auto value = nlohmann::json::parse(receipt.at("output").get<std::string>());
            CHECK(value.at("lines").at(0).at("number") == 2);
        }
        context.sessionId = "owned-mobile-b";
        CHECK(execute("read-text --cursor '" + token + "' pages.txt").hasError());
        context.sessionId = "owned-mobile-a";
        CHECK(!MaiFileSystem::writeFile(pagePath, "changed and longer\n"));
        CHECK(execute("read-text --cursor '" + token + "' pages.txt").hasError());
    }
    MaiFileSystem::removeRecursively(app);
    CHECK(!MaiFileSystem::exists(app));
}
}  // namespace

int main() {
    testNativeMobileScript();
    std::printf("in-process shell tool failures=%d\n", failures);
    return failures ? 1 : 0;
}

// 文件作用：通过真实宿主回调验证受限脚本解析、执行次序、变量、重定向和失败边界。
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "MaiCommandScript.h"

static int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {
struct HostFixture {
    MaiCommandScriptOptions options;
    MaiCommandScriptHost host;
    std::vector<MaiCommandInvocation> calls;
    std::vector<std::string> effects;
    std::map<std::string, std::string> files;
    bool cancel = false;

    HostFixture() {
        options.workingDirectory = MaiFilePath::fromUtf8("/workspace");
        options.isCanceled = [this] { return cancel; };
        host.execute =
            [this](const MaiCommandInvocation& command) -> MaiResult<MaiCommandExecution> {
            calls.push_back(command);
            effects.push_back(command.arguments.front());
            MaiCommandExecution result;
            if (command.arguments.front() == "emit") {
                for (std::size_t index = 1; index < command.arguments.size(); ++index) {
                    if (index > 1) result.standardOutput += " ";
                    result.standardOutput += command.arguments[index];
                }
            } else if (command.arguments.front() == "copy") {
                result.standardOutput = command.standardInput;
            } else if (command.arguments.front() == "fail") {
                result.exitCode = 7;
                result.standardError = "failed\n";
                if (command.arguments.size() > 1) result.standardOutput = command.arguments[1];
            } else if (command.arguments.front() == "cancel") {
                cancel = true;
            } else if (command.arguments.front() == "clip") {
                result.standardOutput = "partial";
                result.outputTruncated = true;
            } else {
                return {MaiErrorCode::NotSupported, "Unknown fixture command."};
            }
            return result;
        };
        host.read = [this](const MaiFilePath& path) -> MaiResult<std::string> {
            const auto key = path.toGenericUtf8();
            effects.push_back("read:" + key);
            const auto found = files.find(key);
            if (found == files.end()) return {MaiErrorCode::NotFound, "Missing fixture input."};
            return found->second;
        };
        host.write = [this](const MaiFilePath& path, const std::string& content, bool append) {
            const auto key = path.toGenericUtf8();
            effects.push_back(std::string(append ? "append:" : "write:") + key);
            if (append)
                files[key] += content;
            else
                files[key] = content;
            return MaiError{};
        };
        host.changeDirectory = [this](const MaiFilePath& path) -> MaiResult<MaiFilePath> {
            effects.push_back("cd:" + path.toGenericUtf8());
            if (path.toGenericUtf8().find("forbidden") != std::string::npos)
                return {MaiErrorCode::InvalidInput, "Directory denied by fixture host."};
            return path;
        };
    }

    MaiResult<MaiCommandExecution> run(const std::string& script) {
        return maiRunCommandScript(script, options, host);
    }
};

void testQuotingVariablesAndStatus() {
    HostFixture fixture;
    const auto result = fixture.run(
        "A='two words'; EMPTY=; emit \"$A\" $A '$A' \"\" \"${EMPTY}\" escaped\\ space "
        "foo#bar # ignored\n"
        "fail || emit $?; LOCAL=x emit \"$A\"; emit \"$LOCAL\" \"$HOME\"; "
        "emit \"\u4e2d\u6587\" \\\njoined");
    CHECK(result.isOk());
    CHECK(fixture.calls.size() == 6);
    if (fixture.calls.size() != 6) return;
    CHECK(fixture.calls[0].arguments ==
          (std::vector<std::string>{"emit", "two words", "two", "words", "$A", "", "",
                                    "escaped space", "foo#bar"}));
    CHECK(fixture.calls[2].arguments == (std::vector<std::string>{"emit", "7"}));
    CHECK(fixture.calls[3].variables.at("LOCAL") == "x");
    CHECK(fixture.calls[4].arguments == (std::vector<std::string>{"emit", "", ""}));
    CHECK(fixture.calls[5].arguments ==
          (std::vector<std::string>{"emit", "\u4e2d\u6587", "joined"}));
}

void testPipelineShortCircuitAndRedirects() {
    HostFixture fixture;
    fixture.files["/workspace/source"] = "from-file";
    const auto result = fixture.run(
        "fail && emit skipped || emit recovered; emit input | copy > result; "
        "copy < source >> result; cd child; pwd; emit done");
    CHECK(result.isOk());
    if (result) {
        CHECK(result.value().exitCode == 0);
        CHECK(result.value().standardOutput == "recovered/workspace/child\ndone");
        CHECK(result.value().standardError == "failed\n");
    }
    CHECK(fixture.files["/workspace/result"] == "inputfrom-file");
    CHECK(fixture.effects ==
          (std::vector<std::string>{"fail", "emit", "emit", "copy", "write:/workspace/result",
                                    "read:/workspace/source", "copy", "append:/workspace/result",
                                    "cd:/workspace/child", "emit"}));
    CHECK(fixture.calls.back().workingDirectory.toGenericUtf8() == "/workspace/child");
    HostFixture pipeline;
    const auto finalStatus = pipeline.run("fail | emit last");
    CHECK(finalStatus.isOk());
    if (finalStatus) CHECK(finalStatus.value().exitCode == 0);
    HostFixture failedOutput;
    const auto recoveredPipeline = failedOutput.run("fail upstream | copy");
    CHECK(recoveredPipeline.isOk());
    if (recoveredPipeline) {
        CHECK(recoveredPipeline.value().exitCode == 0);
        CHECK(recoveredPipeline.value().standardOutput == "upstream");
    }
}

void testQuotedDollarAndUtf8OutputBoundary() {
    HostFixture quoted;
    CHECK(quoted.run("emit \"$\" '$('").isOk());
    CHECK(quoted.calls.size() == 1);
    if (quoted.calls.size() == 1)
        CHECK(quoted.calls.front().arguments == (std::vector<std::string>{"emit", "$", "$("}));
    HostFixture utf8;
    utf8.options.maxOutputBytes = 4;
    const auto result = utf8.run("emit '\u4e2d\u6587'");
    CHECK(result.isOk());
    if (result) {
        CHECK(result.value().standardOutput == "\u4e2d");
        CHECK(result.value().outputTruncated);
    }
}

void testUnsupportedSyntaxHasNoEffects() {
    const std::vector<std::string> invalid{"emit first; emit $(pwd)",
                                           "emit first; emit `pwd`",
                                           "emit first & emit last",
                                           "emit first; (emit second)",
                                           "emit first; if true; then emit x; fi",
                                           "emit first; for x in a; do emit x; done",
                                           "emit first; while true; do emit x; done",
                                           "emit first; copy << END",
                                           "emit first; emit x 2> error",
                                           "emit first; emit x >&1",
                                           "emit first; emit x |",
                                           "emit first; emit x &&",
                                           "emit first; emit x >",
                                           "emit first; emit 'unterminated",
                                           "emit first; emit ${A:-fallback}",
                                           "emit first; emit *.txt",
                                           "emit first; emit ~",
                                           "emit first; emit {a,b}",
                                           "emit first; emit x || ; emit y",
                                           "emit first; emit x >a >b",
                                           "emit first; A=(one two)",
                                           "emit first; function f() { emit x; }"};
    for (const auto& script : invalid) {
        HostFixture fixture;
        const auto result = fixture.run(script);
        CHECK(!result);
        if (!result) CHECK(result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(fixture.effects.empty());
    }
}

void testBudgetsCancellationAndHostErrors() {
    HostFixture fixture;
    fixture.options.maxCommands = 1;
    CHECK(!fixture.run("emit first; emit second"));
    CHECK(fixture.effects.empty());
    fixture.options.maxCommands = 128;
    fixture.options.maxArguments = 2;
    CHECK(!fixture.run("emit first second"));
    CHECK(fixture.effects.empty());
    fixture.options.maxArguments = 256;
    fixture.options.maxPipelineCommands = 1;
    CHECK(!fixture.run("emit first | copy"));
    CHECK(fixture.effects.empty());
    fixture.options.maxPipelineCommands = 16;
    fixture.options.maxOutputBytes = 3;
    const auto clipped = fixture.run("emit abcdef");
    CHECK(clipped.isOk());
    if (clipped) {
        CHECK(clipped.value().standardOutput == "abc");
        CHECK(clipped.value().outputTruncated);
    }
    HostFixture oversized;
    oversized.options.maxOutputBytes = 3;
    CHECK(!oversized.run("emit abcdef | copy"));
    CHECK(oversized.calls.size() == 1);
    CHECK(!oversized.run("emit abcdef > target"));
    CHECK(oversized.files.empty());
    HostFixture partial;
    CHECK(!partial.run("clip | copy"));
    CHECK(partial.calls.size() == 1);
    HostFixture canceled;
    canceled.cancel = true;
    const auto canceledResult = canceled.run("emit never");
    CHECK(!canceledResult);
    if (!canceledResult) CHECK(canceledResult.error().code() == MaiErrorCode::Canceled);
    CHECK(canceled.effects.empty());
    canceled.cancel = false;
    CHECK(!canceled.run("cancel; emit never"));
    CHECK(canceled.calls.size() == 1);
    HostFixture denied;
    CHECK(!denied.run("cd forbidden; emit never"));
    CHECK(denied.calls.empty());
    CHECK(!denied.run("unknown"));
    CHECK(denied.calls.size() == 1);
    HostFixture malformed;
    CHECK(!malformed.run(std::string("emit ") + static_cast<char>(0xff)));
    CHECK(malformed.effects.empty());
}
}  // namespace

int main() {
    testQuotingVariablesAndStatus();
    testPipelineShortCircuitAndRedirects();
    testQuotedDollarAndUtf8OutputBoundary();
    testUnsupportedSyntaxHasNoEffects();
    testBudgetsCancellationAndHostErrors();
    std::printf("MaiCommandScriptTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

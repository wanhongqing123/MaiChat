// 文件作用：验证普通进程内文件命令宿主的字节语义、参数、预算、取消和容器路径边界。
// 所有文件均在本测试创建的独立临时目录内；没有 shell、子进程或真实用户数据。
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "MaiFileCommands.h"
#include "MaiFileSystem.h"
#include "MaiFileSearch.h"
#include "MaiTextFileCursorCache.h"

static int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static MaiFilePath child(const MaiFilePath& root, const std::string& relative) {
    return root.append(MaiFilePath::fromUtf8(relative));
}

static MaiResult<MaiCommandExecution> run(const MaiCommandScriptHost& host,
                                          std::vector<std::string> arguments,
                                          const MaiFilePath& cwd = {},
                                          const std::string& input = {}) {
    MaiCommandInvocation invocation;
    invocation.arguments = std::move(arguments);
    invocation.workingDirectory = cwd;
    invocation.standardInput = input;
    auto result = host.execute(invocation);
    std::string argumentsPreview = nlohmann::json(invocation.arguments).dump(-1, ' ', true);
    if (argumentsPreview.size() > 512)
        argumentsPreview = argumentsPreview.substr(0, 512) + "... [" +
                           std::to_string(argumentsPreview.size()) + " argument bytes]";
    // 只打印本测试自有夹具的命令和失败事实，便于保留真机原始失败而不放宽断言。
    if (!result) {
        std::printf("COMMAND_DIAG args=%s cwd=%s stdin_bytes=%zu host_error=%s message=%s\n",
                    argumentsPreview.c_str(), cwd.toGenericUtf8().c_str(), input.size(),
                    maiErrorCodeToString(result.error().code()), result.error().message().c_str());
    } else if (result.value().exitCode != 0) {
        std::printf("COMMAND_DIAG args=%s cwd=%s stdin_bytes=%zu exit=%d stderr=%s\n",
                    argumentsPreview.c_str(), cwd.toGenericUtf8().c_str(), input.size(),
                    result.value().exitCode, result.value().standardError.c_str());
    }
    return result;
}

static void checkOutput(const MaiResult<MaiCommandExecution>& result, const std::string& expected,
                        int status = 0) {
    CHECK(result.isOk());
    if (!result) return;
    CHECK(result.value().exitCode == status);
    CHECK(result.value().standardOutput == expected);
    CHECK(status == 0 || !result.value().standardError.empty());
}

static void testOrdinaryCommands(const MaiFilePath& root) {
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    const auto host = makeMaiFileCommandHost(options);
    const std::string folder = u8"\u6587\u4ef6 \u76ee\u5f55";
    checkOutput(run(host, {"echo", "a", "b"}), "a b\n");
    checkOutput(run(host, {"echo", "-n", "a"}), "a");
    checkOutput(run(host, {"pwd"}), MaiFileSystem::resolve(root).toGenericUtf8() + "\n");
    checkOutput(run(host, {"mkdir", folder}), "");
    CHECK(host.changeDirectory(child(root, folder)).isOk());
    const auto cwd = child(root, folder);
    checkOutput(run(host, {"mkdir", "-p", "nested/deep"}, cwd), "");
    CHECK(MaiFileSystem::isDirectory(child(cwd, "nested/deep")));
    const std::string bytes("first\r\n\0last\n", 13);
    CHECK(!host.write(child(cwd, "a b.bin"), bytes, false));
    const auto raw = host.read(child(cwd, "a b.bin"));
    CHECK(raw && raw.value() == bytes);
    checkOutput(run(host, {"cat", "a b.bin"}, cwd), bytes);
    checkOutput(run(host, {"cat"}, cwd, bytes), bytes);
    checkOutput(run(host, {"cp", "a b.bin", "copy.bin"}, cwd), "");
    checkOutput(run(host, {"mv", "copy.bin", "moved.bin"}, cwd), "");
    CHECK(!MaiFileSystem::exists(child(cwd, "copy.bin")));
    CHECK(host.read(child(cwd, "moved.bin")).value() == bytes);
    checkOutput(run(host, {"touch", "moved.bin"}, cwd), "");
    CHECK(host.read(child(cwd, "moved.bin")).value() == bytes);
    checkOutput(run(host, {"touch", "new.txt"}, cwd), "");
    CHECK(host.read(child(cwd, "new.txt")).value().empty());
    checkOutput(run(host, {"ls"}, cwd), "a b.bin\nmoved.bin\nnested/\nnew.txt\n");
    CHECK(!host.write(child(cwd, ".hidden"), "x", false));
    checkOutput(run(host, {"ls", "-a"}, cwd), ".hidden\na b.bin\nmoved.bin\nnested/\nnew.txt\n");
    CHECK(!host.write(child(cwd, "new.txt"), "one\r\n", false));
    CHECK(!host.write(child(cwd, "new.txt"), "two\n", true));
    CHECK(host.read(child(cwd, "new.txt")).value() == "one\r\ntwo\n");
    checkOutput(run(host, {"rm", "moved.bin"}, cwd), "");
    CHECK(!MaiFileSystem::exists(child(cwd, "moved.bin")));
}

static void testFilters(const MaiFilePath& root) {
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    const auto host = makeMaiFileCommandHost(options);
    const std::string lines = "alpha\r\nbeta\ngamma";
    checkOutput(run(host, {"head", "-n", "2"}, root, lines), "alpha\r\nbeta\n");
    checkOutput(run(host, {"tail", "-n", "2"}, root, lines), "beta\ngamma");
    checkOutput(run(host, {"head", "-n", "0"}, root, lines), "");
    checkOutput(run(host, {"tail", "-n", "0"}, root, lines), "");
    checkOutput(run(host, {"grep", "-F", "-n", "a"}, root, lines), "1:alpha\r\n2:beta\n3:gamma");
    checkOutput(run(host, {"grep", "-F", "."}, root, lines), "", 1);
    checkOutput(run(host, {"head", "-n", "-1"}, root, lines), "", 2);
    checkOutput(run(host, {"tail", "-n", "999999999999999999999"}, root, lines), "", 2);
    checkOutput(run(host, {"grep", "-R", "alpha"}, root, lines), "", 2);
    CHECK(!host.write(child(root, "filter.txt"), lines, false));
    checkOutput(run(host, {"head", "-n", "1", "filter.txt"}), "alpha\r\n");
    checkOutput(run(host, {"tail", "-n", "1", "filter.txt"}), "gamma");
    checkOutput(run(host, {"grep", "-F", "beta", "filter.txt"}), "beta\n");
}

static void testErrorsAndBudget(const MaiFilePath& root) {
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    const auto host = makeMaiFileCommandHost(options);
    CHECK(!host.write(child(root, "keep"), "original", false));
    checkOutput(run(host, {"cp", "filter.txt", "keep"}), "", 1);
    checkOutput(run(host, {"mv", "filter.txt", "keep"}), "", 1);
    CHECK(host.read(child(root, "keep")).value() == "original");
    CHECK(MaiFileSystem::exists(child(root, "filter.txt")));
    checkOutput(run(host, {"rm", "keep", "extra"}), "", 2);
    CHECK(MaiFileSystem::exists(child(root, "keep")));
    checkOutput(run(host, {"mkdir", "-r", "not-created"}), "", 2);
    CHECK(!MaiFileSystem::exists(child(root, "not-created")));
    checkOutput(run(host, {"rm", "-r", "keep"}), "", 2);
    checkOutput(run(host, {"cat", "missing"}), "", 1);
    checkOutput(run(host, {"unknown-external-program"}), "", 127);
    options.maxOutputBytes = 4;
    const auto bounded = makeMaiFileCommandHost(options);
    CHECK(!run(bounded, {"abcd"}));
    CHECK(!bounded.read(child(root, "keep")));
    CHECK(!run(bounded, {"echo", "12345"}));
    CHECK(bounded.write(child(root, "keep"), "12345", false));
    CHECK(host.read(child(root, "keep")).value() == "original");
    CHECK(!run(bounded, {"cat"}, root, "12345"));
    options.maxOutputBytes = 1024;
    options.isCanceled = [] { return true; };
    const auto canceled = makeMaiFileCommandHost(options);
    CHECK(run(canceled, {"echo"}).error().code() == MaiErrorCode::Canceled);
    CHECK(canceled.read(child(root, "keep")).error().code() == MaiErrorCode::Canceled);
    CHECK(canceled.write(child(root, "keep"), "x", false).code() == MaiErrorCode::Canceled);
    CHECK(canceled.changeDirectory(root).error().code() == MaiErrorCode::Canceled);
    options.isCanceled = {};
    options.accessRoot = {};
    CHECK(!run(makeMaiFileCommandHost(options), {"echo", "x"}));
}

static void testContainerBoundary(const MaiFilePath& root, const MaiFilePath& outside) {
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    const auto host = makeMaiFileCommandHost(options);
    CHECK(!MaiFileSystem::writeFile(child(outside, "secret"), "outside"));
    CHECK(!host.read(child(outside, "secret")));
    CHECK(host.write(child(outside, "new"), "bad", false));
    CHECK(!host.changeDirectory(outside));
    CHECK(!run(host, {"cat", "secret"}, outside));
    CHECK(!run(host, {"cp", "keep", child(outside, "copy").toUtf8()}));
    CHECK(!run(host, {"touch", "../escaped"}));
    CHECK(!MaiFileSystem::exists(child(root.dirName(), "escaped")));
    const auto link = child(root, "outside-link");
    bool linked = false;
#if defined(_WIN32)
    linked = ::CreateSymbolicLinkW(link.value().c_str(), outside.value().c_str(),
                                   SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != 0;
    if (!linked)
        linked = ::CreateSymbolicLinkW(link.value().c_str(), outside.value().c_str(),
                                       SYMBOLIC_LINK_FLAG_DIRECTORY) != 0;
#else
    linked = ::symlink(outside.value().c_str(), link.value().c_str()) == 0;
#endif
    if (linked) {
        CHECK(!host.read(child(link, "secret")));
        CHECK(host.write(child(link, "new"), "bad", false));
        CHECK(!host.changeDirectory(link));
        CHECK(!run(host, {"rm", "outside-link/secret"}));
        CHECK(MaiFileSystem::exists(child(outside, "secret")));
#if defined(_WIN32)
        CHECK(::RemoveDirectoryW(link.value().c_str()) != 0);
#else
        CHECK(::unlink(link.value().c_str()) == 0);
#endif
    } else {
        std::printf("SKIP symlink boundary: platform refused owned symlink creation\n");
    }
}

static void testScriptHostPipeline(const MaiFilePath& root) {
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    const auto host = makeMaiFileCommandHost(options);
    MaiCommandScriptOptions script;
    script.workingDirectory = root;
    const auto processDirectory = MaiFileSystem::resolve(MaiFilePath::fromUtf8("."));
    const auto result = maiRunCommandScript(
        "mkdir -p 'pipeline dir'; cd 'pipeline dir'; "
        "echo alpha > 'data x.txt'; echo beta >> 'data x.txt'; "
        "cat 'data x.txt' | grep -F beta | head -n 1; cat missing || echo fallback",
        script, host);
    checkOutput(result, "beta\nfallback\n");
    CHECK(MaiFileSystem::resolve(MaiFilePath::fromUtf8(".")) == processDirectory);
    const auto written = host.read(child(root, "pipeline dir/data x.txt"));
    CHECK(written && written.value() == "alpha\nbeta\n");
}

static void testSearchAndMetadataCommands(const MaiFilePath& root) {
    const auto directory = child(root, "search fixtures");
    CHECK(!MaiFileSystem::createDirectories(child(directory, "nested")));
    CHECK(!MaiFileSystem::writeFile(child(directory, "a.txt"), "Alpha\nbeta\n"));
    CHECK(!MaiFileSystem::writeFile(child(directory, "nested/b.txt"), "ALPHA\nthird\n"));
    CHECK(!MaiFileSystem::writeFile(child(directory, "nested/c.bin"), "other\n"));
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    options.search = std::make_shared<MaiFileSearch>();
    options.scopeKey = "owned-session-a";
    const auto host = makeMaiFileCommandHost(options);
    const auto first = run(host, {"find", "search fixtures", "-name", "*.txt", "--limit", "1"});
    CHECK(first && first.value().exitCode == 0);
    if (first && first.value().exitCode == 0) {
        const auto status = nlohmann::json::parse(first.value().standardError, nullptr, false);
        CHECK(status.is_object());
        if (status.is_object()) {
            CHECK(status.value("has_more", false));
            const auto cursor = status.value("next_cursor", "");
            CHECK(!cursor.empty());
            const auto second = run(
                makeMaiFileCommandHost(options),
                {"find", "search fixtures", "-name", "*.txt", "--limit", "1", "--cursor", cursor});
            CHECK(second && second.value().exitCode == 0);
            if (second) CHECK(second.value().standardOutput != first.value().standardOutput);
            auto other = options;
            other.scopeKey = "owned-session-b";
            CHECK(!run(makeMaiFileCommandHost(other), {"find", "search fixtures", "-name", "*.txt",
                                                       "--limit", "1", "--cursor", cursor}));
        }
    }
    const auto matches = run(host, {"grep", "-r", "-i", "-n", "^alpha$", "search fixtures"});
    CHECK(matches && matches.value().exitCode == 0);
    if (matches) {
        CHECK(matches.value().standardOutput.find("a.txt:1:Alpha") != std::string::npos);
        CHECK(matches.value().standardOutput.find("b.txt:1:ALPHA") != std::string::npos);
    }
    const auto names = run(host, {"grep", "-r", "-l", "-i", "alpha", "search fixtures"});
    CHECK(names && names.value().exitCode == 0);
    if (names) CHECK(names.value().standardOutput.find("nested/b.txt") != std::string::npos);
    checkOutput(run(host, {"grep", "-c", "a", "search fixtures/a.txt"}), "2\n");
    const auto invalidRegex = run(host, {"grep", "[", "search fixtures/a.txt"});
    CHECK(!invalidRegex || invalidRegex.value().exitCode != 0);
    const auto badGlob = run(host, {"find", "search fixtures", "-name", "{a,b"});
    CHECK(!badGlob || badGlob.value().exitCode != 0);
    const auto badArgs = run(host, {"find", "search fixtures", "-name", "*.txt", "--limit", "-1"});
    CHECK(!badArgs || badArgs.value().exitCode == 2);
    const auto metadata = run(host, {"stat", "search fixtures/a.txt"});
    CHECK(metadata && metadata.value().exitCode == 0);
    if (metadata && metadata.value().exitCode == 0) {
        const auto object = nlohmann::json::parse(metadata.value().standardOutput, nullptr, false);
        CHECK(object.is_object());
        if (object.is_object()) CHECK(object.value("bytes", 0) == 11);
    }
    checkOutput(run(host, {"rmdir", "search fixtures/nested"}), "", 1);
    CHECK(MaiFileSystem::exists(child(directory, "nested/b.txt")));
    CHECK(!MaiFileSystem::createDirectories(child(directory, "empty")));
    checkOutput(run(host, {"rmdir", "search fixtures/empty"}), "");
    CHECK(!MaiFileSystem::exists(child(directory, "empty")));
    const auto current = child(directory, "empty-current");
    CHECK(!MaiFileSystem::createDirectories(current));
    CHECK(!run(host, {"rmdir", "."}, current));
    CHECK(MaiFileSystem::isDirectory(current));
    CHECK(!run(host, {"rmdir", directory.toUtf8()}, current));
    const auto excessiveRegex =
        run(host, {"grep", std::string(65537, 'a'), "search fixtures/a.txt"});
    CHECK(!excessiveRegex || excessiveRegex.value().exitCode == 2);
    CHECK(!MaiFileSystem::writeFile(child(directory, "long.txt"),
                                    std::string(20000, 'x') + "needle\n"));
    auto compact = options;
    compact.maxResultBytes = 1024;
    const auto partial =
        run(makeMaiFileCommandHost(compact), {"grep", "needle", "search fixtures/long.txt"});
    CHECK(partial && partial.value().exitCode == 0);
    if (partial && partial.value().exitCode == 0) {
        CHECK(partial.value().standardOutput.size() + partial.value().standardError.size() <= 1024);
        const auto status = nlohmann::json::parse(partial.value().standardError, nullptr, false);
        CHECK(status.is_object() && status.value("partial", false) &&
              status.value("truncated", false));
    }
    const auto invalidStdin = run(host, {"grep", "["}, root, "owned stdin");
    CHECK(!invalidStdin || invalidStdin.value().exitCode != 0);
    bool temporaryFound = false;
    MaiFileSystem::walk(root, [&](const MaiFileEntry& entry) {
        if (entry.path.dirName() == root && entry.nameUtf8.find(".mai-command-stdin-") == 0)
            temporaryFound = true;
        return entry.isDirectory ? MaiWalkAction::SkipDirectory : MaiWalkAction::Continue;
    });
    CHECK(!temporaryFound);
}

static void testTextPagesAndCursorBinding(const MaiFilePath& root) {
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    options.scopeKey = "text-session-a";
    options.textCursors = std::make_shared<MaiTextFileCursorCache>();
    const auto path = child(root, u8"\u65e7\u7f16\u7801 text.txt");
    CHECK(!MaiFileSystem::writeFile(path, std::string("caf\xe9\nsecond\n", 12)));
#if defined(__ANDROID__)
    // 独立 ELF 没有 App 的 JNI 宿主，缺少旧编码回调必须明确报未配置。
    const auto unconfigured =
        run(makeMaiFileCommandHost(options),
            {"read-text", "--encoding", "latin1", "--limit", "1", path.toUtf8()});
    CHECK(!unconfigured && unconfigured.error().code() == MaiErrorCode::NotConfigured);
#endif
    int decodeCalls = 0;
    options.decodeText = [&decodeCalls](const std::string& bytes,
                                        const std::string& encoding) -> MaiResult<std::string> {
        ++decodeCalls;
        if (encoding != "latin1")
            return {MaiErrorCode::NotSupported, "Owned test decoder only accepts latin1"};
        // ISO-8859-1 的每个字节都是一个确定标量；严格逐字节编码为 UTF-8，
        // 不替换无效字节、不调用平台默认编码，也不冒充 Android App 的完整解码后端。
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const unsigned char byte : bytes) {
            if (byte < 0x80)
                result += static_cast<char>(byte);
            else {
                result += static_cast<char>(0xc0 | (byte >> 6));
                result += static_cast<char>(0x80 | (byte & 0x3f));
            }
        }
        return result;
    };
    auto host = makeMaiFileCommandHost(options);
    const auto legacy = run(host, {"read-text", "--encoding", "latin1", "--offset", "1", "--limit",
                                   "1", path.toUtf8()});
    CHECK(legacy && legacy.value().exitCode == 0);
    CHECK(decodeCalls > 0);
    if (legacy && legacy.value().exitCode == 0) {
        const auto page = nlohmann::json::parse(legacy.value().standardOutput, nullptr, false);
        CHECK(page.is_object());
        if (page.is_object()) {
            CHECK(page["lines"][0].value("text", "") == u8"caf\u00e9");
            CHECK(page.value("encoding", "") == "latin1");
            CHECK(!page.value("eof", true));
            CHECK(!page.value("next_cursor", "").empty());
        }
    }
    const auto longPath = child(root, "multi-megabyte.txt");
    const std::string scalar = u8"\u4e2d";
    std::string expected;
    while (expected.size() < 2 * 1024 * 1024) expected += scalar;
    CHECK(!MaiFileSystem::writeFile(longPath, expected));
    auto compact = options;
    compact.maxResultBytes = 1024;
    const auto compactPage =
        run(makeMaiFileCommandHost(compact), {"read-text", "--limit", "1", longPath.toUtf8()});
    CHECK(compactPage && compactPage.value().exitCode == 0);
    if (compactPage && compactPage.value().exitCode == 0) {
        CHECK(compactPage.value().standardOutput.size() <= 1024);
        const auto page = nlohmann::json::parse(compactPage.value().standardOutput, nullptr, false);
        CHECK(page.is_object() && !page.value("eof", true) &&
              !page.value("next_cursor", "").empty());
    }
    const auto scan = run(host, {"read-text", "--offset", "2", longPath.toUtf8()});
    CHECK(scan && scan.value().exitCode == 0);
    if (scan && scan.value().exitCode == 0) {
        const auto page = nlohmann::json::parse(scan.value().standardOutput, nullptr, false);
        CHECK(page.is_object() && !page.value("eof", true) &&
              page.value("stop_reason", "") == "scan_limit");
        CHECK(!page.value("next_cursor", "").empty());
    }
    std::string cursor, restored;
    bool eof = false;
    int pages = 0;
    while (!eof && ++pages < 200) {
        std::vector<std::string> arguments{"read-text", "--limit", "1"};
        if (!cursor.empty()) arguments.insert(arguments.end(), {"--cursor", cursor});
        arguments.push_back(longPath.toUtf8());
        const auto result = run(makeMaiFileCommandHost(options), arguments);
        CHECK(result && result.value().exitCode == 0);
        if (!result || result.value().exitCode != 0) break;
        const auto page = nlohmann::json::parse(result.value().standardOutput, nullptr, false);
        CHECK(page.is_object());
        if (!page.is_object()) break;
        for (const auto& line : page["lines"]) {
            CHECK(line.value("number", 0) == 1);
            CHECK(line.value("byte_offset", std::size_t(0)) == restored.size());
            restored += line.value("text", "");
        }
        eof = page.value("eof", false);
        cursor = page.value("next_cursor", "");
        if (!eof) CHECK(!cursor.empty() && page.value("truncated", false));
        if (pages == 1 && !cursor.empty()) {
            auto other = options;
            other.scopeKey = "text-session-b";
            CHECK(!run(makeMaiFileCommandHost(other),
                       {"read-text", "--limit", "1", "--cursor", cursor, longPath.toUtf8()}));
        }
    }
    CHECK(eof && pages > 1 && restored == expected);
    CHECK(!run(host, {"read-text", "--offset", "2", "--cursor", cursor, longPath.toUtf8()}));
    const auto first = run(host, {"read-text", "--limit", "1", longPath.toUtf8()});
    if (first && first.value().exitCode == 0) {
        const auto page = nlohmann::json::parse(first.value().standardOutput, nullptr, false);
        const auto oldCursor = page.value("next_cursor", "");
        CHECK(!oldCursor.empty());
        CHECK(!MaiFileSystem::writeFile(longPath, "changed"));
        CHECK(!run(host, {"read-text", "--cursor", oldCursor, longPath.toUtf8()}));
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto sequence = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto parent =
        child(MaiFileSystem::temporaryDirectory(), "mai-file-commands-" + std::to_string(sequence));
    CHECK(!MaiFileSystem::exists(parent));
    if (MaiFileSystem::exists(parent)) return 1;
    const auto root = child(parent, "container");
    const auto outside = child(parent, "container-outside");
    CHECK(!MaiFileSystem::createDirectories(root));
    CHECK(!MaiFileSystem::createDirectories(outside));
    MaiFileCommandOptions options;
    options.accessRoot = root;
    options.workingDirectory = root;
    const auto host = makeMaiFileCommandHost(options);
    CHECK(host.execute && host.read && host.write && host.changeDirectory);
    if (host.execute && host.read && host.write && host.changeDirectory) {
        testOrdinaryCommands(root);
        testFilters(root);
        testErrorsAndBudget(root);
        testContainerBoundary(root, outside);
        testScriptHostPipeline(root);
        testSearchAndMetadataCommands(root);
        testTextPagesAndCursorBinding(root);
    }
    MaiFileSystem::removeRecursively(parent);
    std::printf("MaiFileCommandTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// 文件作用：验证“定义 Agent 工具接口、参数和执行结果”的正确行为和边界条件。
// 验证入口：test_path_escape_is_blocked、test_sibling_with_shared_prefix_is_outside、test_write_cannot_escape；检查可观察结果而非实现细节。
// 工具层测试。
//
// 顺序是有意的：先测路径边界，再测功能。
// 边界错了后面全都不重要——模型会主动试着越界（有时是它自己想看 ../.env，有时是被提示词注入诱导），
// 这不是假想威胁。
//
// 写这些用例时踩到的坑，记在这里免得重犯：**在 Windows 上不要写 `some_path / "中文目录"`。
// ** 源码里的字面量是 UTF-8，
// 而 MSVC 的 fs::path 把 narrow 字符串按当前 ANSI 代码页（中文机器是 GBK）
// 解释，拼出来是乱码路径——实测直接让进程挂掉，不是返回错误。
// 测试里要用 std::filesystem::u8path("中文")，产品代码里用
// MaiPathUtf8::fromUtf8()。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiEditTool.h"
#include "MaiApplyPatchTool.h"
#include "MaiCvVideoTools.h"
#include "MaiTool.h"
#include "MaiFileTools.h"
#include "MaiPathGuard.h"
#include "MaiMemoryStore.h"
#include "MaiToolDetail.h"

namespace fs = std::filesystem;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 造一个临时工作区，里面放好几种文件。
// ── 这一组常量是非 ASCII 用例的素材，不是普通测试数据 ──────────
//
// 写成 \u 转义而不是直接的汉字：规范要求代码里除注释外不出现中文。
// 但这几条用例测的**就是**非 ASCII 路径和内容——MSVC 的 fs::path 会把
// narrow 字符串按当前 ANSI 代码页（中文机器上是 GBK）解释，而我们的路径全来自 JSON、是 UTF-8。
// 当初这个 bug 不是返回错误，是进程直接挂掉（STATUS_STACK_BUFFER_OVERRUN）。
// 所以字节本身一个都不能改。
//
//   kCjkDir      \u65b0\u76ee\u5f55                新目录
//   kCjkFile     \u65b0\u6587\u4ef6.txt            新文件.txt
//   kCjkContent  \u5199\u5165\u7684\u5185\u5bb9 🙂  写入的内容 🙂
//   kCjkLine2    \u7b2c\u4e8c\u884c                第二行
const char* kCjkDir = "\u65b0\u76ee\u5f55";
const char* kCjkFile = "\u65b0\u6587\u4ef6.txt";
const char* kCjkContent = "\u5199\u5165\u7684\u5185\u5bb9 \U0001F642";
const char* kCjkOverwrite = "\u8986\u76d6\u4e86";
const char* kCjkLine1 = "\u7b2c\u4e00\u884c";
const char* kCjkLine2 = "\u7b2c\u4e8c\u884c";
const char* kCjkLine3 = "\u7b2c\u4e09\u884c";
const char* kCjkMissingFile = "\u4e0d\u5b58\u5728.txt";         // 不存在.txt
const char* kCjkNotYetExists = "\u8fd8\u4e0d\u5b58\u5728.txt";  // 还不存在的文件.txt
const char* kSecretMarker = "TOP-SECRET";

struct Workspace {
    fs::path root;
    fs::path outside;  // root 之外的一个目录，用来验证越界被挡住

    Workspace() {
        const auto base =
            fs::temp_directory_path() / ("maiagent-tools-" + std::to_string(std::rand()));
        root = base / "workspace";
        outside = base / "outside";
        fs::create_directories(root / "src");
        fs::create_directories(root / "node_modules" / "junk");
        fs::create_directories(outside);

        write(root / "README.md",
              std::string(kCjkLine1) + "\n" + kCjkLine2 + " hello\n" + kCjkLine3 + "\n");
        write(root / "src" / "main.cpp", "#include <cstdio>\nint main() { return 0; }\n");
        write(root / "src" / "util.cpp", "void helper() {}\n// TODO: fill this in\n");
        write(root / "src" / "util.h", "void helper();\n");
        write(root / "src" / "mockup.png", "image");
        write(root / "src" / "mockup.jpg", "image");
        // node_modules 里也放一个能匹配的，验证它被跳过
        write(root / "node_modules" / "junk" / "main.cpp", "should not be found\n");
        write(outside / "secret.txt", std::string(kSecretMarker) + " outside the workspace\n");
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    static void write(const fs::path& p, const std::string& body) {
        std::ofstream out(p, std::ios::binary);
        out << body;
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_test";
        c.root = root.string();
        return c;
    }
};

std::string args(const json& j) {
    return j.dump();
}

// 新检索合同的回归使用系统文件 API，且只操作本用例拥有的临时目录。
struct SearchWorkspace {
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
        "maiagent-search-regression-" + std::to_string(std::rand())));

    SearchWorkspace() { CHECK(!MaiFileSystem::createDirectories(root).hasError()); }
    ~SearchWorkspace() { MaiFileSystem::removeRecursively(root); }
    void write(const std::string& name, const std::string& text) {
        CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8(name)), text).hasError());
    }
    MaiToolContext context() const {
        MaiToolContext result;
        result.sessionId = "ses_search_regression";
        result.root = root.toUtf8();
        return result;
    }
};

json fileResultMetadata(const MaiToolResult& result) {
    const std::string marker = "\n\n[file_result]\n";
    const auto position = result.output().rfind(marker);
    if (position == std::string::npos) return json::object();
    const auto parsed = json::parse(result.output().substr(position + marker.size()), nullptr,
                                    /*allow_exceptions=*/false);
    return parsed.is_object() ? parsed : json::object();
}

void test_search_long_line_is_matched_before_display_clipping() {
    SearchWorkspace workspace;
    workspace.write("long.txt", std::string(480, 'x') + "OWNED_TAIL_NEEDLE\n");
    auto grep = makeMaiGrepTool();
    const auto found = grep->execute(args({{"path", "long.txt"},
                                          {"pattern", "OWNED_TAIL_NEEDLE"},
                                          {"mode", "literal"}}), workspace.context());
    CHECK(!found.hasError());
    CHECK(found.output().find("long.txt:1:") != std::string::npos);
    CHECK(found.output().find("No content matches") == std::string::npos);
    CHECK(fileResultMetadata(found).value("complete", false));
}

void test_read_offset_reaches_beyond_the_old_eight_mebibyte_prefix() {
    SearchWorkspace workspace;
    workspace.write("large.txt", std::string(8u * 1024 * 1024 + 17, 'x') +
                                     "\nOWNED_AFTER_PREFIX\n");
    auto read = makeMaiReadTool();
    const auto found = read->execute(args({{"path", "large.txt"}, {"offset", 2}, {"limit", 1}}),
                                     workspace.context());
    CHECK(!found.hasError());
    CHECK(found.output().find("2\tOWNED_AFTER_PREFIX") != std::string::npos);
    CHECK(found.output().find("no content at or after") == std::string::npos);
}

void test_search_continuation_and_unknown_arguments_are_not_silent_replays() {
    SearchWorkspace workspace;
    std::string contents;
    for (int line = 1; line <= 260; ++line)
        contents += "OWNED_HIT_" + std::to_string(line) + "\n";
    workspace.write("hits.txt", contents);
    auto grep = makeMaiGrepTool();
    json query = {{"path", "hits.txt"}, {"pattern", "OWNED_HIT_"},
                  {"mode", "literal"}, {"limit", 100}};
    const auto first = grep->execute(args(query), workspace.context());
    const auto metadata = fileResultMetadata(first);
    CHECK(!first.hasError());
    const std::string cursor = metadata.value("next_cursor", std::string());
    CHECK(!cursor.empty());
    if (!cursor.empty()) {
        query["cursor"] = cursor;
        const auto second = grep->execute(args(query), workspace.context());
        CHECK(!second.hasError());
        CHECK(second.output().find("hits.txt:101:") != std::string::npos);
        CHECK(second.output().find("hits.txt:1:") == std::string::npos);
        CHECK(second.execution().has_value());
        if (second.execution())
            CHECK(second.execution()->effectStatus == MaiToolEffectStatus::None);
    }
    const auto unsupported = grep->execute(
        args({{"path", "hits.txt"}, {"pattern", "OWNED_HIT_"}, {"offset", 200}}),
        workspace.context());
    CHECK(unsupported.error().code() == MaiErrorCode::InvalidInput);
}

void test_filename_query_finds_ranked_names_without_guessing_a_glob() {
    SearchWorkspace workspace;
    workspace.write("MaiCloudDriveDialog.java", "class Owned {}\n");
    auto glob = makeMaiGlobTool();
    const auto found = glob->execute(args({{"query", "CloudDriveDialog"}}), workspace.context());
    CHECK(!found.hasError());
    CHECK(found.output().find("MaiCloudDriveDialog.java") != std::string::npos);
}

void test_read_cursor_reassembles_utf8_and_is_bound_to_its_session_and_source() {
    SearchWorkspace workspace;
    std::string original;
    for (int index = 0; index < 4000; ++index) original += "\u4f60\U0001f642";
    workspace.write("long-utf8.txt", original);
    auto reader = makeMaiReadTool();
    auto context = workspace.context();
    json request = {{"path", "long-utf8.txt"}, {"limit", 1}, {"max_bytes", 4096}};
    std::string assembled;
    std::string firstCursor;
    for (int page = 0; page < 100; ++page) {
        const auto result = reader->execute(args(request), context);
        CHECK(!result.hasError());
        if (result.hasError()) break;
        const auto metadata = fileResultMetadata(result);
        CHECK(metadata.value("first_line_utf8_offset", std::size_t{}) == assembled.size());
        const auto end = result.output().find('\n');
        CHECK(result.output().compare(0, 2, "1\t") == 0 && end != std::string::npos);
        if (end == std::string::npos) break;
        assembled += result.output().substr(2, end - 2);
        const auto cursor = metadata.value("next_cursor", std::string());
        if (cursor.empty()) {
            CHECK(metadata.value("eof", false));
            break;
        }
        if (firstCursor.empty()) firstCursor = cursor;
        request["cursor"] = cursor;
    }
    CHECK(assembled == original);
    CHECK(!firstCursor.empty());
    request["cursor"] = firstCursor;
    auto other = context;
    other.sessionId = "ses_other_search_reader";
    CHECK(reader->execute(args(request), other).error().code() == MaiErrorCode::InvalidInput);
    workspace.write("long-utf8.txt", "owned changed source");
    CHECK(reader->execute(args(request), context).error().code() == MaiErrorCode::InvalidInput);
}

void test_file_queries_reject_wrong_types_and_unsupported_arguments() {
    SearchWorkspace workspace;
    workspace.write("one.txt", "owned\n");
    const auto context = workspace.context();
    for (const auto& request : {json{{"path", "one.txt"}, {"max_bytes", true}},
                                json{{"path", "one.txt"}, {"encoding", json::array()}},
                                json{{"path", "one.txt"}, {"after", 3}}})
        CHECK(makeMaiReadTool()->execute(args(request), context).error().code() == MaiErrorCode::InvalidInput);
    CHECK(makeMaiGlobTool()->execute(args({{"query", "one"}, {"pattern", "*.txt"}}), context)
              .error().code() == MaiErrorCode::InvalidInput);
    CHECK(makeMaiGrepTool()->execute(args({{"pattern", "owned"}, {"limit", 1001}}), context)
              .error().code() == MaiErrorCode::InvalidInput);
}

void test_search_context_does_not_repeat_long_paths_for_each_context_line() {
    SearchWorkspace workspace;
    std::string text;
    for (int line = 1; line <= 25; ++line)
        text += (line == 12 ? "OWNED_CONTEXT_NEEDLE " : "owned context ") +
                std::string(300, 'x') + "\n";
    workspace.write("context.txt", text);
    const auto result = makeMaiGrepTool()->execute(args({{"path", "context.txt"},
        {"pattern", "OWNED_CONTEXT_NEEDLE"}, {"mode", "literal"},
        {"context_before", 10}, {"context_after", 10}}), workspace.context());
    CHECK(!result.hasError());
    const auto first = result.output().find("context.txt");
    CHECK(first != std::string::npos);
    CHECK(first == std::string::npos || result.output().find("context.txt", first + 1) == std::string::npos);
    CHECK(result.output().find("  2- owned context") != std::string::npos);
    CHECK(result.output().find("context.txt:12: OWNED_CONTEXT_NEEDLE") != std::string::npos);
    CHECK(result.output().find("  22- owned context") != std::string::npos);
    CHECK(fileResultMetadata(result).value("complete", false));
}

// ── 1. 路径边界 ─────────────────────────────────────────────────

void test_path_escape_is_blocked() {
    Workspace workspace;
    auto read = makeMaiReadTool();
    const auto context = workspace.context();

    // 各种越界写法都要被挡住
    const char* escapes[] = {
        "../outside/secret.txt",
        "../../etc/passwd",
        "src/../../outside/secret.txt",
        "./src/./../../outside/secret.txt",
    };
    for (const char* p : escapes) {
        const auto r = read->execute(args({{"path", p}}), context);
        CHECK(r.error().code() == MaiErrorCode::InvalidInput);
        if (r.error().code() != MaiErrorCode::InvalidInput)
            std::printf("  escape was not blocked: %s -> %s\n", p, r.output().c_str());
        // 而且不能把机密内容漏出去
        CHECK(r.output().find(kSecretMarker) == std::string::npos);
    }

    // 绝对路径指向外面，同样挡住
    const auto abs =
        read->execute(args({{"path", (workspace.outside / "secret.txt").string()}}), context);
    CHECK(abs.error().code() == MaiErrorCode::InvalidInput);
    CHECK(abs.output().find(kSecretMarker) == std::string::npos);
}

// 同前缀的兄弟目录不能算"在里面"。
//
// 这条是冲着一类经典实现错误来的：用字符串前缀判断包含关系。
// "/server/app-secrets" 确实以 "/server/app" 开头，但它是**另一个目录**。真按前缀判，
// root 旁边随便放一个同前缀的目录就全漏了。
//
// MaiFilePath::isParentOf 是逐段比的，所以这里必须被挡住。
void test_sibling_with_shared_prefix_is_outside() {
    Workspace workspace;

    // workspace.root 叫 "workspace"，在它旁边造一个 "workspace-secrets"
    const MaiFilePath rootPath = MaiFilePath::fromUtf8(workspace.root.u8string());
    const MaiFilePath sibling =
        rootPath.dirName().append(MaiFilePath::fromUtf8("workspace-secrets"));
    MaiFileSystem::createDirectories(sibling);
    const MaiFilePath leaked = sibling.append(MaiFilePath::fromUtf8("secret.txt"));
    MaiFileSystem::writeFile(leaked, std::string(kSecretMarker) + " sibling directory\n");

    // 用绝对路径直接指过去
    CHECK(maiResolvePathWithinRoot(workspace.root.u8string(), leaked.toUtf8()).empty());

    // 用相对路径绕过去（..\workspace-secrets\secret.txt）
    const std::string relative = "../workspace-secrets/secret.txt";
    CHECK(maiResolvePathWithinRoot(workspace.root.u8string(), relative).empty());

    // 走真正的工具，确认内容没漏出去
    auto read = makeMaiReadTool();
    const auto viaAbsolute = read->execute(args({{"path", leaked.toUtf8()}}), workspace.context());
    CHECK(viaAbsolute.error().code() == MaiErrorCode::InvalidInput);
    CHECK(viaAbsolute.output().find(kSecretMarker) == std::string::npos);

    const auto viaRelative = read->execute(args({{"path", relative}}), workspace.context());
    CHECK(viaRelative.error().code() == MaiErrorCode::InvalidInput);
    CHECK(viaRelative.output().find(kSecretMarker) == std::string::npos);

    MaiFileSystem::removeRecursively(sibling);
}

void test_write_cannot_escape() {
    Workspace workspace;
    auto write = makeMaiWriteTool();
    const auto r = write->execute(args({{"path", "../outside/pwned.txt"}, {"content", "x"}}),
                                  workspace.context());
    CHECK(r.error().code() == MaiErrorCode::InvalidInput);
    CHECK(!fs::exists(workspace.outside / "pwned.txt"));
}

void test_unrestricted_host_can_read_and_write_outside_working_directory() {
    Workspace workspace;
    MaiToolContext context = workspace.context();
    context.allowOutsideWorkingDirectory = true;
    const std::string source = (workspace.outside / "secret.txt").string();
    const std::string output = (workspace.outside / "created.txt").string();

    CHECK(context.resolvePath(source) ==
          MaiFileSystem::resolve(MaiFilePath::fromUtf8(source)).toUtf8());
    const auto read = makeMaiReadTool()->execute(args({{"path", source}}), context);
    CHECK(!read.hasError());
    CHECK(read.output().find(kSecretMarker) != std::string::npos);
    const auto write = makeMaiWriteTool()->execute(
        args({{"path", output}, {"content", "outside content"}}), context);
    CHECK(!write.hasError());
    CHECK(fs::exists(workspace.outside / "created.txt"));
    CHECK(write.output().find(MaiFilePath::fromUtf8(output).toGenericUtf8()) != std::string::npos);
}

void test_app_container_allows_sibling_of_workspace_but_not_other_container() {
    Workspace workspace;
    Workspace other;
    MaiToolContext context = workspace.context();
    context.fileAccessRoot = workspace.root.parent_path().string();

    const auto insideApp =
        makeMaiReadTool()->execute(args({{"path", "../outside/secret.txt"}}), context);
    CHECK(!insideApp.hasError());
    CHECK(insideApp.output().find(kSecretMarker) != std::string::npos);
    const auto outsideApp = makeMaiReadTool()->execute(
        args({{"path", (other.outside / "secret.txt").string()}}), context);
    CHECK(outsideApp.error().code() == MaiErrorCode::InvalidInput);
}

void test_write_and_edit_share_a_file_specific_approval_key() {
    Workspace workspace;
    const MaiToolContext context = workspace.context();
    const std::string path = (workspace.root / "README.md").string();
    const auto write = makeMaiWriteTool()->approvalKeys(
        args({{"path", "README.md"}, {"content", "new"}}), context);
    const auto edit = makeMaiEditTool()->approvalKeys(
        args({{"path", path}, {"old_string", "old"}, {"new_string", "new"}}), context);
    CHECK(write.size() == 1);
    CHECK(write == edit);
    CHECK(write.front().find("file:") == 0);
}

void test_invalid_path_types_fail_without_throwing() {
    Workspace workspace;
    const MaiToolContext context = workspace.context();
    bool safe = true;
    try {
        safe = makeMaiReadTool()->execute(R"({"path":42})", context).hasError() &&
               makeMaiWriteTool()->execute(R"({"path":42,"content":"x"})", context).hasError() &&
               makeMaiEditTool()
                   ->execute(R"({"path":42,"old_string":"a","new_string":"b"})", context)
                   .hasError() &&
               makeMaiApplyPatchTool()->execute(R"({"patch":42})", context).hasError();
    } catch (...) {
        safe = false;
    }
    CHECK(safe);
}

void test_maiResolvePathWithinRoot_directly() {
    Workspace workspace;
    const std::string root = workspace.root.string();

    // 合法的
    CHECK(!maiResolvePathWithinRoot(root, "README.md").empty());
    CHECK(!maiResolvePathWithinRoot(root, "src/main.cpp").empty());
    CHECK(!maiResolvePathWithinRoot(root, kCjkNotYetExists).empty());  // write 要能创建

    // 非法的
    CHECK(maiResolvePathWithinRoot(root, "../outside/secret.txt").empty());
    CHECK(maiResolvePathWithinRoot(root, "").empty());
    CHECK(maiResolvePathWithinRoot("", "README.md").empty());

    // 前缀不等于包含：同级的 workspace-evil 不能通过 workspace 的检查。
    // 用字符串前缀判断的实现会在这里漏。
    const fs::path sibling = workspace.root.parent_path() / "workspace-evil";
    fs::create_directories(sibling);
    Workspace::write(sibling / "x.txt", "evil");
    CHECK(maiResolvePathWithinRoot(root, (sibling / "x.txt").string()).empty());
}

// ── 2. read ─────────────────────────────────────────────────────

void test_read() {
    Workspace workspace;
    auto read = makeMaiReadTool();
    const auto context = workspace.context();

    const auto r = read->execute(args({{"path", "README.md"}}), context);
    CHECK(!r.hasError());
    CHECK(r.output().find(std::string(kCjkLine2) + " hello") != std::string::npos);
    CHECK(r.output().find("1\t") != std::string::npos);  // 带行号

    const auto off =
        read->execute(args({{"path", "README.md"}, {"offset", 2}, {"limit", 1}}), context);
    CHECK(!off.hasError());
    CHECK(off.output().find(kCjkLine2) != std::string::npos);
    CHECK(off.output().find(kCjkLine1) == std::string::npos);
    CHECK(off.output().find(kCjkLine3) == std::string::npos);

    const auto missing = read->execute(args({{"path", kCjkMissingFile}}), context);
    CHECK(missing.error().code() == MaiErrorCode::NotFound);

    // 目录要给一句有用的话，而不是一个看不懂的失败
    const auto dir = read->execute(args({{"path", "src"}}), context);
    CHECK(dir.error().code() == MaiErrorCode::InvalidInput);
    CHECK(dir.error().message().find("glob") != std::string::npos);

    const auto no_path = read->execute("{}", context);
    CHECK(no_path.error().code() == MaiErrorCode::InvalidInput);

    // 畸形 JSON 不能让工具崩
    const auto junk = read->execute("this is not json", context);
    CHECK(junk.error().code() == MaiErrorCode::InvalidInput);
}

// ── 3. write ────────────────────────────────────────────────────

void test_write() {
    Workspace workspace;
    auto write = makeMaiWriteTool();
    const auto context = workspace.context();

    const std::string cjkPath = std::string(kCjkDir) + "/" + kCjkFile;
    const auto r = write->execute(args({{"path", cjkPath}, {"content", kCjkContent}}), context);
    CHECK(!r.hasError());
    // 测试里自己拼路径时也要走 UTF-8：workspace.root 是 fs::path，但字面量
    // "新目录" 是 UTF-8 的 char*，`path / "中文"` 同样会按 ANSI 代码页解释。
    const fs::path written =
        workspace.root / std::filesystem::u8path(kCjkDir) / std::filesystem::u8path(kCjkFile);
    CHECK(fs::exists(written));

    std::ifstream in(written, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body == kCjkContent);

    // 覆盖
    write->execute(args({{"path", cjkPath}, {"content", kCjkOverwrite}}), context);
    std::ifstream in2(written, std::ios::binary);
    std::string body2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    CHECK(body2 == kCjkOverwrite);

    CHECK(write->execute(args({{"path", "x.txt"}}), context).error().code() ==
          MaiErrorCode::InvalidInput);
    // write 会改东西，必须标记为需要审批（权限闸门靠这个标志）。
    // 现在审批是按这一次调用的参数判的，所以要把参数传进去——
    // write 的危险程度不随参数变，给什么都该是 true。
    CHECK(write->requiresApproval(args({{"path", "x.txt"}, {"content", "y"}})));
    CHECK(write->requiresApproval("not json at all"));
    CHECK(!makeMaiReadTool()->requiresApproval(args({{"path", "x.txt"}})));
}

// 分别验证旧编码、BOM 和非法二进制输入；模型看到的必须是解码后的文本，错误编码不能静默变乱码。
void test_read_decodes_non_utf8_files_for_the_model() {
    Workspace workspace;
    const MaiToolContext context = workspace.context();
    auto read = makeMaiReadTool();
    Workspace::write(workspace.root / "utf16.txt",
                     std::string("\xFF\xFE\x60\x4F\x7D\x59\x0A\x00", 8));
    Workspace::write(workspace.root / "gb18030.txt", std::string("\xC4\xE3\xBA\xC3\x0A", 5));
    Workspace::write(workspace.root / "utf16be.txt",
                     std::string("\xFE\xFF\x4F\x60\x59\x7D\x00\x0A", 8));
    Workspace::write(workspace.root / "utf16emoji.txt",
                     std::string("\xFF\xFE\x3D\xD8\x42\xDE\x0A\x00", 8));
    Workspace::write(workspace.root / "utf8bom.txt",
                     std::string("\xEF\xBB\xBF", 3) + "\u4f60\u597d\n");
    Workspace::write(workspace.root / "binary.dat", std::string("\x00\x01\x02", 3));
    const std::string greeting = "\u4f60\u597d";
    const auto utf16 = read->execute(args({{"path", "utf16.txt"}}), context);
    CHECK(!utf16.hasError());
    CHECK(utf16.output().find(greeting) != std::string::npos);
    const auto gb18030 =
        read->execute(args({{"path", "gb18030.txt"}, {"encoding", "gb18030"}}), context);
    CHECK(!gb18030.hasError());
    CHECK(gb18030.output().find(greeting) != std::string::npos);
    const auto bigEndian = read->execute(args({{"path", "utf16be.txt"}}), context);
    CHECK(!bigEndian.hasError());
    CHECK(bigEndian.output().find(greeting) != std::string::npos);
    const auto surrogate = read->execute(args({{"path", "utf16emoji.txt"}}), context);
    CHECK(!surrogate.hasError());
    CHECK(surrogate.output().find("\U0001f642") != std::string::npos);
    const auto withBom = read->execute(args({{"path", "utf8bom.txt"}}), context);
    CHECK(!withBom.hasError());
    CHECK(withBom.output().find(greeting) != std::string::npos);
    CHECK(withBom.output().find("\xEF\xBB\xBF") == std::string::npos);
    CHECK(read->execute(args({{"path", "binary.dat"}}), context).hasError());
    CHECK(
        read->execute(args({{"path", "gb18030.txt"}, {"encoding", "utf-8"}}), context).hasError());
    CHECK(read->execute(args({{"path", "utf16.txt"}, {"encoding", 42}}), context).hasError());

    MaiToolContext mobile = context;
    int decoderCalls = 0;
    mobile.decodeText = [&](const std::string& bytes,
                            const std::string& encoding) -> MaiResult<std::string> {
        ++decoderCalls;
        CHECK(bytes == std::string("\xC4\xE3\xBA\xC3\x0A", 5));
        CHECK(encoding == "gb18030");
        return greeting + "\n";
    };
    const auto viaHost =
        read->execute(args({{"path", "gb18030.txt"}, {"encoding", "gb18030"}}), mobile);
    CHECK(!viaHost.hasError());
    CHECK(viaHost.output().find(greeting) != std::string::npos);
    CHECK(decoderCalls == 1);
}

void test_grep_returns_utf8_for_legacy_files() {
    Workspace workspace;
    Workspace::write(workspace.root / "legacy-search.txt", std::string("\xC4\xE3\xBA\xC3\x0A", 5));
    auto grep = makeMaiGrepTool();
    const auto result = grep->execute(
        args({{"pattern", "\u4f60\u597d"}, {"glob", "legacy-search.txt"}, {"encoding", "gb18030"}}),
        workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("legacy-search.txt:1:") != std::string::npos);
    CHECK(result.output().find("\u4f60\u597d") != std::string::npos);
}

void test_explicit_create_and_delete_tools_use_utf8_paths() {
    Workspace workspace;
    const MaiToolContext context = workspace.context();
    auto createDirectory = makeMaiCreateDirectoryTool();
    auto createFile = makeMaiCreateFileTool();
    auto deleteFile = makeMaiDeleteFileTool();
    const std::string directory = std::string(kCjkDir) + "/\u5b50\u76ee\u5f55";
    const std::string file = directory + "/" + kCjkFile;
    const fs::path native = workspace.root / fs::u8path(directory) / fs::u8path(kCjkFile);

    CHECK(createDirectory->requiresApproval("{}"));
    CHECK(createFile->requiresApproval("{}"));
    CHECK(deleteFile->requiresApproval("{}"));
    CHECK(!createDirectory->execute(args({{"path", directory}}), context).hasError());
    CHECK(fs::is_directory(native.parent_path()));
    CHECK(!createDirectory->execute(args({{"path", directory}}), context).hasError());
    CHECK(!createFile->execute(args({{"path", file}}), context).hasError());
    CHECK(fs::is_regular_file(native));
    CHECK(fs::file_size(native) == 0);
    Workspace::write(native, "must stay intact");
    CHECK(createFile->execute(args({{"path", file}}), context).hasError());
    CHECK(fs::file_size(native) == std::string("must stay intact").size());
    CHECK(createDirectory->execute(args({{"path", file}}), context).hasError());
    CHECK(deleteFile->execute(args({{"path", directory}}), context).hasError());
    const fs::path shortcut = workspace.root / "shortcut.txt";
    std::error_code symlinkError;
    fs::create_symlink(native, shortcut, symlinkError);
    if (!symlinkError) {
        CHECK(deleteFile->execute(args({{"path", "shortcut.txt"}}), context).hasError());
        CHECK(fs::exists(native));
        fs::remove(shortcut);
    }

    const auto createKey = createFile->approvalKeys(args({{"path", file}}), context);
    const auto deleteKey = deleteFile->approvalKeys(args({{"path", file}}), context);
    CHECK(createKey == deleteKey);
    CHECK(createDirectory->approvalKeys(args({{"path", directory}}), context) != createKey);
    CHECK(!deleteFile->execute(args({{"path", file}}), context).hasError());
    CHECK(!fs::exists(native));
    CHECK(deleteFile->execute(args({{"path", file}}), context).error().code() ==
          MaiErrorCode::NotFound);
    CHECK(deleteFile->execute(args({{"path", "../outside/secret.txt"}}), context).hasError());
    CHECK(fs::exists(workspace.outside / "secret.txt"));
    CHECK(createFile->execute(R"({"path":42})", context).hasError());
    CHECK(createDirectory->execute(R"({"path":42})", context).hasError());
    CHECK(deleteFile->execute(R"({"path":42})", context).hasError());
}

// ── 4. glob ─────────────────────────────────────────────────────

void test_glob() {
    Workspace workspace;
    auto glob = makeMaiGlobTool();
    const auto context = workspace.context();

    const auto cpp = glob->execute(args({{"pattern", "**/*.cpp"}}), context);
    CHECK(!cpp.hasError());
    CHECK(cpp.output().find("src/main.cpp") != std::string::npos);
    CHECK(cpp.output().find("src/util.cpp") != std::string::npos);
    // node_modules 必须被跳过，否则在真实仓库里会走几十万个文件
    CHECK(cpp.output().find("node_modules") == std::string::npos);

    const auto h = glob->execute(args({{"pattern", "*.h"}}), context);
    CHECK(!h.hasError());
    CHECK(h.output().find("util.h") != std::string::npos);
    CHECK(h.output().find(".cpp") == std::string::npos);

    const auto none = glob->execute(args({{"pattern", "**/*.rs"}}), context);
    CHECK(!none.hasError());
    CHECK(none.output().find("No files match") != std::string::npos);

    const auto images = glob->execute(args({{"pattern", "**/*.{png,jpg,jpeg,webp}"}}), context);
    CHECK(!images.hasError());
    CHECK(images.output().find("src/mockup.png") != std::string::npos);
    CHECK(images.output().find("src/mockup.jpg") != std::string::npos);
    CHECK(images.output().find("src/main.cpp") == std::string::npos);
    CHECK(glob->execute(args({{"pattern", "**/*.{png,}"}}), context).error().code() ==
          MaiErrorCode::InvalidInput);

#if !defined(_WIN32)
    auto rootContext = context;
    rootContext.root = "/";
    CHECK(glob->execute(args({{"pattern", "**/*.png"}}), rootContext).error().code() ==
          MaiErrorCode::InvalidInput);
#endif

    CHECK(glob->execute("{}", context).error().code() == MaiErrorCode::InvalidInput);
}

// ── 5. grep ─────────────────────────────────────────────────────

void test_grep() {
    Workspace workspace;
    auto grep = makeMaiGrepTool();
    const auto context = workspace.context();

    const auto todo = grep->execute(args({{"pattern", "TODO"}}), context);
    CHECK(!todo.hasError());
    CHECK(todo.output().find("src/util.cpp") != std::string::npos);
    CHECK(todo.output().find(":2:") != std::string::npos);  // 带行号

    const auto filtered = grep->execute(args({{"pattern", "helper"}, {"glob", "*.h"}}), context);
    CHECK(!filtered.hasError());
    CHECK(filtered.output().find("util.h") != std::string::npos);
    CHECK(filtered.output().find("util.cpp") == std::string::npos);

    const auto none = grep->execute(args({{"pattern", "definitely-not-there-xyzzy"}}), context);
    CHECK(!none.hasError());
    // grep 说的是 "No content matches"，glob 说的是 "No files match"——两边措辞不同是有意的：
    // 以前都叫"没有匹配"，断言根本分不出是谁产出的。
    CHECK(none.output().find("No content matches") != std::string::npos);

    // 坏正则要给一句能改的话，而不是崩掉
    const auto bad = grep->execute(args({{"pattern", "([unclosed"}}), context);
    CHECK(bad.error().code() == MaiErrorCode::InvalidInput);

    // 中文能搜到
    const auto cn = grep->execute(args({{"pattern", kCjkLine2}}), context);
    CHECK(!cn.hasError());
    CHECK(cn.output().find("README.md") != std::string::npos);
}

void test_grep_specific_file_preserves_scope() {
    Workspace workspace;
    const auto file = workspace.root / fs::u8path(kCjkFile);
    Workspace::write(file, "IP-CIDR,11.164.94.213/32,DIRECT\n");
    Workspace::write(workspace.root / "unrelated.svg", "<path d=\"M164.94 10\"/>\n");
    auto grep = makeMaiGrepTool();
    const auto result = grep->execute(
        args({{"pattern", R"(11\.164\.94\.213|11\.164\.94\.|164\.94)"}, {"path", file.u8string()}}),
        workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find(std::string(kCjkFile) + ":1:") != std::string::npos);
    CHECK(result.output().find("11.164.94.213/32,DIRECT") != std::string::npos);
    CHECK(result.output().find("unrelated.svg") == std::string::npos);
}

void test_grep_specific_oversized_file_reports_limit() {
    Workspace workspace;
    const auto file = workspace.root / "large.conf";
    std::string bytes(2u * 1024 * 1024 + 1, 'x');
    bytes.replace(0, 14, "11.164.94.213\n");
    // 保持精确的 2 MiB + 1，不让首行替换改变越界条件。
    bytes.resize(2u * 1024 * 1024 + 1, 'x');
    Workspace::write(file, bytes);
    const auto result = makeMaiGrepTool()->execute(
        args({{"pattern", R"(11\.164\.94\.213)"}, {"path", file.u8string()}}), workspace.context());
    CHECK(result.hasError());
    CHECK(result.error().message().find("2 MiB") != std::string::npos);
    CHECK(result.output().find("No content matches") == std::string::npos);
}

#if !defined(_WIN32)
void test_grep_specific_unreadable_file_preserves_error() {
    Workspace workspace;
    const auto file = workspace.root / "unreadable.conf";
    Workspace::write(file, "11.164.94.213\n");
    CHECK(::chmod(file.c_str(), 0000) == 0);
    std::string content;
    const auto originalReadError = MaiFileSystem::readFile(MaiFilePath(file.string()), content);
    if (!originalReadError.hasError()) {
        // 提权的测试用户可能仍可读取；不把这种运行环境算作拒读路径已验。
        std::printf("SKIP unreadable-file fixture: process can still read mode-000 file\n");
        ::chmod(file.c_str(), 0600);
        return;
    }
    const auto result = makeMaiGrepTool()->execute(
        args({{"pattern", R"(11\.164\.94\.213)"}, {"path", file.u8string()}}), workspace.context());
    CHECK(::chmod(file.c_str(), 0600) == 0);
    CHECK(result.hasError());
    CHECK(result.error().code() == originalReadError.code());
    CHECK(result.output().find("No content matches") == std::string::npos);
}
#endif

void test_grep_pre_canceled_is_not_completed_no_match() {
    Workspace workspace;
    auto context = workspace.context();
    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    const auto result = makeMaiGrepTool()->execute(args({{"pattern", "never-present"}}), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::Canceled);
    CHECK(result.output().find("No content matches") == std::string::npos);
}

void test_grep_cancel_before_scanning_decoded_lines() {
    Workspace workspace;
    Workspace::write(workspace.root / "cancel.legacy", "\xff");
    auto context = workspace.context();
    std::atomic<bool> canceled{false};
    context.cancel = &canceled;
    context.decodeText = [&](const std::string&, const std::string&) -> MaiResult<std::string> {
        // 解码器返回时取消已经发生；不能把后面的行当作一次已完成的成功搜索。
        canceled.store(true);
        return std::string("11.164.94.213\n");
    };
    const auto result = makeMaiGrepTool()->execute(
        args({{"pattern", R"(11\.164\.94\.213)"}, {"glob", "*.legacy"}}), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::Canceled);
}

void test_grep_read_budget_does_not_claim_complete_no_match() {
    Workspace workspace;
    // 总原始字节超过 32 MiB，单个文件仍满足原先 2 MiB 限制。
    // 平台解码器用于固定内容与工作量，不访问用户文件或依赖主机字符集。
    std::string bytes(2u * 1024 * 1024, 'x');
    bytes[0] = '\xff';
    for (int index = 0; index < 18; ++index)
        Workspace::write(workspace.root / ("budget-" + std::to_string(index) + ".legacy"), bytes);
    auto context = workspace.context();
    int decodedFiles = 0;
    context.decodeText = [&](const std::string&, const std::string&) -> MaiResult<std::string> {
        ++decodedFiles;
        return std::string("no requested address here\n");
    };
    const auto result = makeMaiGrepTool()->execute(
        args({{"pattern", R"(11\.164\.94\.213|11\.164\.94\.|164\.94)"}, {"glob", "*.legacy"}}),
        context);
    CHECK(!result.hasError());
    CHECK(decodedFiles > 0 && decodedFiles < 18);
    CHECK(result.isTruncated());
    CHECK(result.output().find("scan limit") != std::string::npos);
    CHECK(result.output().find("No content matches") == std::string::npos);
}

void test_grep_time_budget_checks_after_decoder_returns() {
    Workspace workspace;
    Workspace::write(workspace.root / "slow.legacy", "\xff");
    auto context = workspace.context();
    context.decodeText = [](const std::string&, const std::string&) -> MaiResult<std::string> {
        // 预算是协作式检查，不声称能抢占宿主解码器、单次 I/O 或 regex_search。
        std::this_thread::sleep_for(std::chrono::milliseconds(5100));
        return std::string("11.164.94.213\n");
    };
    const auto result = makeMaiGrepTool()->execute(
        args({{"pattern", R"(11\.164\.94\.213)"}, {"glob", "*.legacy"}}), context);
    CHECK(!result.hasError());
    CHECK(result.isTruncated());
    CHECK(result.output().find("scan limit") != std::string::npos);
    CHECK(result.output().find("slow.legacy:1:") == std::string::npos);
}

#if !defined(_WIN32)
void test_grep_does_not_open_named_pipes() {
    Workspace workspace;
    const auto fifo = workspace.root / "search.pipe";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    std::atomic<bool> finished{false};
    std::atomic<bool> opened{false};
    // 原实现若尝试打开 FIFO，由自有写端解阻塞并记录事实，避免 RED 用例永久挂住。
    std::thread writer([&] {
        while (!finished.load()) {
            const int handle = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
            if (handle >= 0) {
                opened.store(true);
                ::close(handle);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto result = makeMaiGrepTool()->execute(
        args({{"pattern", "never-present"}, {"glob", "*.pipe"}}), workspace.context());
    finished.store(true);
    writer.join();
    CHECK(!result.hasError());
    CHECK(!opened.load());
}
#endif

// ── 6. 注册表 ───────────────────────────────────────────────────

void test_registry() {
    MaiToolRegistry reg;
    CHECK(reg.isEmpty());
    registerMaiBuiltinTools(reg);
    CHECK(!reg.isEmpty());

    // **按名字查，不比个数。** 比个数的话每加一个工具都要来改一次这里，
    // 而这条用例想守的是「这些工具都在」，不是「一共有几个」。
    for (const char* n :
         {"file_read", "file_create", "file_create_directory", "file_delete", "file_write",
          "file_edit", "file_patch", "file_glob", "file_grep", "current_time"}) {
        CHECK(reg.find(n) != nullptr);
    }
    CHECK(reg.find("no-such-tool") == nullptr);

    const auto schemas = reg.specs();
    CHECK(schemas.size() == static_cast<std::size_t>(reg.specs().size()));
    CHECK(!schemas.empty());
    for (const auto& s : schemas) {
        CHECK(!s.name.empty());
        CHECK(!s.description.empty());
        // schema 必须是合法 JSON，否则请求体构造时会被悄悄换成空对象，模型就不知道参数怎么填了
        const auto parsed = json::parse(s.parametersJson, nullptr, false);
        CHECK(!parsed.is_discarded());
        if (!parsed.is_discarded()) {
            CHECK(parsed.contains("properties"));
            // 有参数的工具要声明必填条件；文件名定位可在 pattern/query 中选择一种。
            if (!parsed["properties"].empty())
                CHECK(parsed.contains("required") || parsed.contains("oneOf") || parsed.contains("anyOf"));
        }
    }

    // 同名覆盖而不是并存
    const std::size_t before = reg.specs().size();
    reg.add(makeMaiReadTool());
    CHECK(reg.specs().size() == before);

    MaiToolRegistry internal;
    internal.addInternal(makeMaiReadTool());
    CHECK(internal.isEmpty());
    CHECK(internal.find("file_read") != nullptr);
    CHECK(internal.findForModel("file_read") == nullptr);
    CHECK(internal.specs().empty());
    CHECK(internal.specialists().empty());
    internal.add(makeMaiReadTool());
    CHECK(!internal.isEmpty());
    CHECK(internal.findForModel("file_read") != nullptr);
    CHECK(internal.specs().size() == 1);
}

MaiCvVideoAnalysisResult sampleVideoAnalysis(const std::string&,
                                             const MaiCvVideoAnalysisOptions& options,
                                             const MaiToolContext&) {
    MaiCvVideoAnalysisResult result;
    result.durationSeconds = 4;
    result.decodedFrames = 120;
    result.sampledFrames = 8;
    result.segments = options.kind == MaiCvVideoAnalysisKind::Scene
                          ? std::vector<MaiCvTimeSegment>{{0, 2}, {2, 4}}
                          : std::vector<MaiCvTimeSegment>{{1, 3}};
    return result;
}

void test_cv_video_tool_registration_and_results() {
    Workspace workspace;
    MaiToolRegistry registry;
    registry.add(makeMaiCvSceneDetectTool(sampleVideoAnalysis));
    registry.add(makeMaiCvMotionDetectTool(sampleVideoAnalysis));
    CHECK(registry.find("cv_scene_detect") != nullptr);
    CHECK(registry.find("cv_motion_detect") != nullptr);
    const MaiToolContext context = workspace.context();
    const MaiToolResult scene =
        registry.find("cv_scene_detect")
            ->execute(args({{"path", "src/mockup.png"}, {"sample_interval_s", 0.5}}), context);
    CHECK(!scene.hasError());
    if (!scene.hasError()) {
        const json output = json::parse(scene.output());
        CHECK(output["segments"].size() == 2);
        CHECK(output["segments"][1]["start_s"] == 2);
        CHECK(output["duration_s"] == 4);
    }
    const MaiToolResult motion =
        registry.find("cv_motion_detect")
            ->execute(args({{"path", "src/mockup.png"}, {"motion_ratio_threshold", 0.02}}),
                      context);
    CHECK(!motion.hasError());
    if (!motion.hasError()) {
        const json output = json::parse(motion.output());
        CHECK(output["segments"].size() == 1);
        CHECK(output["segments"][0]["start_s"] == 1);
    }
    CHECK(registry.find("cv_scene_detect")
              ->execute(args({{"path", "../outside/secret.txt"}}), context)
              .hasError());
    CHECK(registry.find("cv_motion_detect")
              ->execute(args({{"path", "src/mockup.png"}, {"pixel_threshold", 0}}), context)
              .hasError());
}

void test_cancel_stops_traversal() {
    Workspace workspace;
    auto glob = makeMaiGlobTool();
    auto context = workspace.context();
    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    // 已取消时不该继续遍历。这里只验证它不崩、能立刻返回。
    const auto r = glob->execute(args({{"pattern", "**/*"}}), context);
    CHECK(!r.hasError() || r.error().code() != MaiErrorCode::Internal);
}

// 详情只能回查已落库的原调用；分页不能执行命令、解析路径或把下一版输出混入当前页。
void test_tool_detail_pages_and_revision_binding() {
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "ses_detail";
    store->putSession(session);
    MaiSession other;
    other.id = "ses_other";
    store->putSession(other);
    MaiToolPart tool;
    tool.tool = "command";
    tool.callId = "call_original";
    tool.input = R"({"command":"fixture only; never executed","cwd":"/not-read"})";
    tool.output = std::string(4095, 'x') + "\u4f60\U0001f642" + std::string(4099, 'y');
    tool.error = "stored fixture error";
    tool.state = MaiToolState::Running;
    MaiMessage message;
    message.id = "msg_detail";
    message.role = MaiRole::Assistant;
    message.parts.push_back({"prt_tool", tool, 1});
    message.parts.push_back({"prt_text", MaiTextPart{"not a tool"}, 1});
    store->putMessage(session.id, message);
    const std::string revision = maiToolDetailRevision(tool);
    CHECK(revision.size() == 64);
    CHECK(revision.find_first_not_of("0123456789abcdef") == std::string::npos);

    for (const char* field : {"input", "output", "error"}) {
        MaiToolDetailQuery query;
        query.messageId = message.id;
        query.partId = "prt_tool";
        query.field = field;
        std::string assembled;
        do {
            const auto result = maiReadToolDetail(*store, session.id, query);
            CHECK(result.isOk());
            if (!result) break;
            const auto& page = result.value();
            CHECK(page.messageId == message.id && page.partId == "prt_tool");
            CHECK(page.callId == tool.callId && page.tool == tool.tool);
            CHECK(page.state == MaiToolState::Running && page.revision == revision);
            CHECK(page.field == field && page.offsetBytes == assembled.size());
            CHECK(page.sourceBytesRead == page.text.size() && page.text.size() <= 4096);
            CHECK(page.textIsValidUtf8);
            assembled += page.text;
            query.offsetBytes = page.nextOffsetBytes;
            query.expectedRevision = page.revision;
            CHECK(query.offsetBytes == 0 || query.offsetBytes == assembled.size());
        } while (query.offsetBytes != 0);
        CHECK(assembled == (query.field == "input"    ? tool.input
                            : query.field == "output" ? tool.output
                                                      : tool.error));
    }

    MaiToolDetailQuery query;
    query.messageId = message.id;
    query.partId = "prt_tool";
    query.field = "output";
    const auto first = maiReadToolDetail(*store, session.id, query);
    CHECK(first && first.value().sourceBytesRead == 4095 && first.value().nextOffsetBytes == 4095);
    query.offsetBytes = 4095;
    CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
          MaiErrorCode::InvalidInput);
    query.expectedRevision = revision;
    auto changed = tool;
    changed.output += "next generation";
    message.parts[0].body = changed;
    store->putMessage(session.id, message);
    const auto stale = maiReadToolDetail(*store, session.id, query);
    CHECK(stale.error().code() == MaiErrorCode::Busy);
    CHECK(stale.error().message().find("detail_changed") != std::string::npos);
    query.offsetBytes = 0;
    CHECK(maiReadToolDetail(*store, session.id, query).error().code() == MaiErrorCode::Busy);
    query.expectedRevision.clear();
    CHECK(maiReadToolDetail(*store, session.id, query).value().revision != revision);

    for (int field = 0; field < 6; ++field) {
        auto different = tool;
        if (field == 0) different.tool += "x";
        if (field == 1) different.callId += "x";
        if (field == 2) different.input += "x";
        if (field == 3) different.output += "x";
        if (field == 4) different.error += "x";
        if (field == 5) different.state = MaiToolState::Completed;
        CHECK(maiToolDetailRevision(different) != revision);
    }
    MaiToolPart left, right;
    left.tool = "a";
    left.callId = "bc";
    right.tool = "ab";
    right.callId = "c";
    CHECK(maiToolDetailRevision(left) != maiToolDetailRevision(right));
    CHECK(maiReadToolDetail(*store, other.id, query).error().code() == MaiErrorCode::NotFound);
    query.messageId = "msg_missing";
    CHECK(maiReadToolDetail(*store, session.id, query).error().code() == MaiErrorCode::NotFound);
    query.messageId = message.id;
    for (const char* part : {"prt_missing", "prt_text"}) {
        query.partId = part;
        CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
              MaiErrorCode::NotFound);
    }
    MaiMessage saved;
    CHECK(store->getMessage(session.id, message.id, saved));
    CHECK(std::get<MaiToolPart>(saved.parts[0].body).output == changed.output);
    CHECK(saved.completed == 0 && store->listMessages(session.id).size() == 1);
}

void test_tool_detail_utf8_and_invalid_queries() {
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "ses_utf8_detail";
    store->putSession(session);
    MaiToolPart tool;
    tool.output = "A\u4f60\U0001f642Z";
    // 非法旧字节含孤立续字节、过长编码、代理项、越界标量、截断序列和NUL。
    tool.error = std::string("\x80\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82\0", 13);
    MaiMessage message;
    message.id = "msg_utf8_detail";
    message.parts.push_back({"prt_utf8_detail", tool, 0});
    store->putMessage(session.id, message);
    MaiToolDetailQuery query;
    query.messageId = message.id;
    query.partId = "prt_utf8_detail";
    query.field = "output";
    query.maxBytes = 4;
    const auto first = maiReadToolDetail(*store, session.id, query);
    CHECK(first && first.value().text == "A\u4f60" && first.value().nextOffsetBytes == 4);
    query.expectedRevision = maiToolDetailRevision(tool);
    query.offsetBytes = 4;
    const auto second = maiReadToolDetail(*store, session.id, query);
    CHECK(second && second.value().text == "\U0001f642" && second.value().nextOffsetBytes == 8);
    query.offsetBytes = 8;
    CHECK(maiReadToolDetail(*store, session.id, query).value().text == "Z");
    query.offsetBytes = 9;
    const auto end = maiReadToolDetail(*store, session.id, query);
    CHECK(end && end.value().text.empty() && end.value().nextOffsetBytes == 0);
    for (const auto offset : {std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{7},
                              std::size_t{10}, std::size_t(-1)}) {
        query.offsetBytes = offset;
        CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
              MaiErrorCode::InvalidInput);
    }
    query.offsetBytes = 1;
    query.maxBytes = 1;
    CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
          MaiErrorCode::InvalidInput);
    query.offsetBytes = 0;
    for (const auto limit : {std::size_t{0}, std::size_t{4097}}) {
        query.maxBytes = limit;
        CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
              MaiErrorCode::InvalidInput);
    }
    query.maxBytes = 1;
    query.field = "error";
    std::string recovered;
    for (std::size_t index = 0; index < tool.error.size(); ++index) {
        query.offsetBytes = index;
        const auto page = maiReadToolDetail(*store, session.id, query);
        CHECK(page && page.value().text.size() == 1 && page.value().sourceBytesRead == 1);
        if (page) {
            recovered += page.value().text;
            CHECK(page.value().textIsValidUtf8 == (index == tool.error.size() - 1));
        }
    }
    CHECK(recovered == tool.error);
    query.offsetBytes = 0;
    query.field = "media_path";
    CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
          MaiErrorCode::InvalidInput);
    query.field = "input";
    query.expectedRevision.clear();
    const auto empty = maiReadToolDetail(*store, session.id, query);
    CHECK(empty && empty.value().totalBytes == 0 && empty.value().textIsValidUtf8);
    query.messageId.clear();
    CHECK(maiReadToolDetail(*store, session.id, query).error().code() ==
          MaiErrorCode::InvalidInput);
}

}  // namespace

// 每个用例跑之前先打一行，崩了的话能立刻看出停在哪一个。这次就是靠它定位的：
// 上一版整个进程直接挂掉，一条输出都没有。
#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_search_long_line_is_matched_before_display_clipping);
    RUN(test_read_offset_reaches_beyond_the_old_eight_mebibyte_prefix);
    RUN(test_search_continuation_and_unknown_arguments_are_not_silent_replays);
    RUN(test_filename_query_finds_ranked_names_without_guessing_a_glob);
    RUN(test_read_cursor_reassembles_utf8_and_is_bound_to_its_session_and_source);
    RUN(test_file_queries_reject_wrong_types_and_unsupported_arguments);
    RUN(test_search_context_does_not_repeat_long_paths_for_each_context_line);
    RUN(test_tool_detail_pages_and_revision_binding);
    RUN(test_tool_detail_utf8_and_invalid_queries);
    RUN(test_path_escape_is_blocked);
    RUN(test_sibling_with_shared_prefix_is_outside);
    RUN(test_write_cannot_escape);
    RUN(test_unrestricted_host_can_read_and_write_outside_working_directory);
    RUN(test_app_container_allows_sibling_of_workspace_but_not_other_container);
    RUN(test_write_and_edit_share_a_file_specific_approval_key);
    RUN(test_invalid_path_types_fail_without_throwing);
    RUN(test_maiResolvePathWithinRoot_directly);
    RUN(test_read);
    RUN(test_read_decodes_non_utf8_files_for_the_model);
    RUN(test_grep_returns_utf8_for_legacy_files);
    RUN(test_write);
    RUN(test_explicit_create_and_delete_tools_use_utf8_paths);
    RUN(test_glob);
    RUN(test_grep);
    RUN(test_grep_specific_file_preserves_scope);
    RUN(test_grep_specific_oversized_file_reports_limit);
#if !defined(_WIN32)
    RUN(test_grep_specific_unreadable_file_preserves_error);
#endif
    RUN(test_grep_pre_canceled_is_not_completed_no_match);
    RUN(test_grep_cancel_before_scanning_decoded_lines);
    RUN(test_grep_read_budget_does_not_claim_complete_no_match);
    RUN(test_grep_time_budget_checks_after_decoder_returns);
#if !defined(_WIN32)
    RUN(test_grep_does_not_open_named_pipes);
#endif
    RUN(test_registry);
    RUN(test_cv_video_tool_registration_and_results);
    RUN(test_cancel_stops_traversal);
    if (failures == 0) std::printf("tool tests passed\n");
    return failures == 0 ? 0 : 1;
}

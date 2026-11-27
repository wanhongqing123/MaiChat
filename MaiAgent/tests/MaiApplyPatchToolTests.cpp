// 文件作用：用真实文件验证 Codex 补丁语法、逐文件执行、字节结果和失败边界。
// 普通 maiApplyPatch 与模型工具共用核心；预期来自固定上游实现，不声称跨文件事务。
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <json.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "MaiTool.h"
#include "MaiApplyPatchTool.h"

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

struct Workspace {
    fs::path root;

    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-patch-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& name, const std::string& body) const {
        const fs::path target = root / fs::u8path(name);
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << body;
    }

    std::string read(const std::string& name) const {
        std::ifstream in(root / fs::u8path(name), std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    bool has(const std::string& name) const {
        return fs::exists(root / fs::u8path(name));
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_patch";
        c.root = root.u8string();
        return c;
    }
};

std::string patchArgs(const std::string& patch) {
    return json({{"patch", patch}}).dump();
}

void test_updates_several_places_in_one_file() {
    Workspace workspace;
    workspace.write("a.txt", "a\nb\nc\nd\ne\nf\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: a.txt\n"
                                                         "@@\n"
                                                         " a\n"
                                                         "-b\n"
                                                         "+B\n"
                                                         "@@\n"
                                                         " d\n"
                                                         "-e\n"
                                                         "+E\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("a.txt") == "a\nB\nc\nd\nE\nf\n");
}

void test_one_patch_can_touch_several_files() {
    // 这是它相对 edit 的全部意义：改接口同时改所有调用方，一次提交。
    Workspace workspace;
    workspace.write("one.txt", "old\n");
    workspace.write("two.txt", "old\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: one.txt\n"
                                                         "@@\n"
                                                         "-old\n"
                                                         "+new\n"
                                                         "*** Update File: two.txt\n"
                                                         "@@\n"
                                                         "-old\n"
                                                         "+new\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("one.txt") == "new\n");
    CHECK(workspace.read("two.txt") == "new\n");
}

void test_earlier_hunks_remain_applied_when_a_later_hunk_fails() {
    // 和 Codex 一样按块顺序提交：第二个上下文失败时，先前成功操作保持落盘。
    Workspace workspace;
    workspace.write("good.txt", "old\n");
    workspace.write("bad.txt", "something else\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: good.txt\n"
                                                         "@@\n"
                                                         "-old\n"
                                                         "+new\n"
                                                         "*** Update File: bad.txt\n"
                                                         "@@\n"
                                                         "-this line is not in the file\n"
                                                         "+whatever\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(result.hasError());
    CHECK(workspace.read("good.txt") == "new\n");
    CHECK(workspace.read("bad.txt") == "something else\n");
    // 报错列出已经落盘的操作，不能诱导模型盲目重放整份补丁。
    CHECK(result.error().message().find("M good.txt") != std::string::npos);
}

void test_add_and_delete_files() {
    Workspace workspace;
    workspace.write("gone.txt", "bye\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Add File: made/up.txt\n"
                                                         "+first\n"
                                                         "+second\n"
                                                         "*** Delete File: gone.txt\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("made/up.txt") == "first\nsecond\n");
    CHECK(!workspace.has("gone.txt"));
    // 报给模型的状态要分得清增删改。
    CHECK(result.output().find("A made/up.txt") != std::string::npos);
    CHECK(result.output().find("D gone.txt") != std::string::npos);
}

void test_add_overwrites_an_existing_file_like_codex() {
    // Codex Add 的行为是创建或覆盖；工具说明和授权必须如实描述。
    Workspace workspace;
    workspace.write("there.txt", "precious\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Add File: there.txt\n"
                                                         "+overwritten\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("there.txt") == "overwritten\n");
}

void test_move_writes_the_new_path_and_removes_the_old() {
    Workspace workspace;
    workspace.write("from.txt", "keep\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: from.txt\n"
                                                         "*** Move to: to.txt\n"
                                                         "@@\n"
                                                         "-keep\n"
                                                         "+moved\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("to.txt") == "moved\n");
    CHECK(!workspace.has("from.txt"));
}

void test_crlf_files_keep_their_line_endings() {
    // **Windows 上最容易踩的一脚。** 把 CRLF 的文件写成 LF，git 会把整个文件
    // 报成改动过，真正的那几行淹没在里面。
    Workspace workspace;
    workspace.write("crlf.txt", "a\r\nb\r\nc\r\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: crlf.txt\n"
                                                         "@@\n"
                                                         " a\n"
                                                         "-b\n"
                                                         "+B\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("crlf.txt") == "a\r\nB\r\nc\r\n");
}

void test_context_is_matched_leniently_on_whitespace() {
    // 模型抄上下文时最常见的偏差就是行尾空格。一次都不让步的话大部分补丁都会失败，
    // 而失败的理由（少了一个空格）它自己看不出来。空白匹配和 Codex seek_sequence 一致，Unicode 标点放松另有用例。
    Workspace workspace;
    workspace.write("ws.txt", "keep   \n  target\nafter\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: ws.txt\n"
                                                         "@@\n"
                                                         " keep\n"
                                                         "-  target\n"
                                                         "+  replaced\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("ws.txt").find("replaced") != std::string::npos);
}

void test_malformed_patches_are_rejected_clearly() {
    Workspace workspace;
    workspace.write("x.txt", "a\n");
    auto tool = makeMaiApplyPatchTool();

    // 没有信封
    CHECK(tool->execute(patchArgs("*** Update File: x.txt\n@@\n-a\n+b\n"), workspace.context())
              .hasError());
    // 没有收尾
    CHECK(tool->execute(patchArgs("*** Begin Patch\n*** Update File: x.txt\n@@\n-a\n+b\n"),
                        workspace.context())
              .hasError());
    // 空补丁
    CHECK(tool->execute(patchArgs("*** Begin Patch\n*** End Patch\n"), workspace.context())
              .hasError());
    // 改动行前面没有标记字符，分不清是上下文还是别的什么
    CHECK(tool->execute(patchArgs("*** Begin Patch\n*** Update File: x.txt\n@@\nbare line\n"
                                  "*** End Patch\n"),
                        workspace.context())
              .hasError());
    // 一个都不许落盘
    CHECK(workspace.read("x.txt") == "a\n");
}

void test_cannot_escape_the_working_directory() {
    // 路径边界是安全边界。每个碰文件的工具都要自己被验一遍，
    // 不能假设别人验过。
    Workspace workspace;
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult update = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: ../outside.txt\n"
                                                         "@@\n"
                                                         "-a\n"
                                                         "+b\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(update.hasError());
    CHECK(update.error().message().find("outside the area accessible") != std::string::npos);

    // 新建和移动的目标路径同样要挡。
    const MaiToolResult add = tool->execute(patchArgs("*** Begin Patch\n"
                                                      "*** Add File: ../escaped.txt\n"
                                                      "+x\n"
                                                      "*** End Patch\n"),
                                            workspace.context());
    CHECK(add.hasError());
    CHECK(!fs::exists(workspace.root.parent_path() / "escaped.txt"));
}

void test_missing_file_is_reported_as_not_found() {
    Workspace workspace;
    auto tool = makeMaiApplyPatchTool();
    const MaiToolResult result = tool->execute(patchArgs("*** Begin Patch\n"
                                                         "*** Update File: nope.txt\n"
                                                         "@@\n"
                                                         "-a\n"
                                                         "+b\n"
                                                         "*** End Patch\n"),
                                               workspace.context());
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotFound);
}

void test_apply_patch_always_needs_approval() {
    auto tool = makeMaiApplyPatchTool();
    CHECK(tool->requiresApproval(patchArgs("*** Begin Patch\n*** End Patch\n")));
    CHECK(tool->requiresApproval("not json at all"));
    CHECK(tool->approvalKey(patchArgs("anything")) == "apply_patch");
}

void test_delete_and_move_require_approval_for_each_affected_file() {
    Workspace workspace;
    auto tool = makeMaiApplyPatchTool();
    const auto context = workspace.context();
    const auto keys = tool->approvalKeys(patchArgs("*** Begin Patch\n"
                                                   "*** Delete File: gone.txt\n"
                                                   "*** Update File: from.txt\n"
                                                   "*** Move to: to.txt\n"
                                                   "@@\n"
                                                   "-old\n"
                                                   "+new\n"
                                                   "*** End Patch\n"),
                                         context);
    CHECK(keys.size() == 3);
    CHECK(std::find(keys.begin(), keys.end(), "file:" + context.resolvePath("gone.txt")) !=
          keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "file:" + context.resolvePath("from.txt")) !=
          keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "file:" + context.resolvePath("to.txt")) !=
          keys.end());
}

// 以固定 Codex 源码的 parser/file_update/text_file/seek_sequence 为预期，
// 使用真实文件核对落盘字节，而不是复述本工具的返回文本。
void test_codex_name_is_available_to_the_bootstrap_catalog() {
    auto tool = makeMaiApplyPatchTool();
    CHECK(tool->name() == "apply_patch");
}

void test_change_context_selects_the_named_function() {
    Workspace workspace;
    workspace.write("anchor.txt", "first\nold\nsecond\nold\n");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: anchor.txt\n"
                                                "@@ second\n-old\n+new\n*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("anchor.txt") == "first\nold\nsecond\nnew\n");
}

void test_add_only_chunk_appends_and_terminates_the_last_line() {
    Workspace workspace;
    workspace.write("append.txt", "before");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: append.txt\n"
                                                "@@\n+after\n*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("append.txt") == "before\nafter\n");
}

void test_end_of_file_does_not_match_a_middle_occurrence() {
    Workspace workspace;
    workspace.write("eof.txt", "match\nlast\n");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: eof.txt\n"
                                                "@@\n-match\n+bad\n*** End of File\n"
                                                "*** End Patch\n"),
                                      workspace.context());
    CHECK(result.hasError());
    CHECK(workspace.read("eof.txt") == "match\nlast\n");
}

void test_context_bytes_and_mixed_line_endings_are_retained() {
    Workspace workspace;
    workspace.write("mixed.txt", "keep   \r\nold\nlast\r");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: mixed.txt\n"
                                                "@@\n keep\n-old\n+new\n last\n"
                                                "*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("mixed.txt") == "keep   \r\nnew\r\nlast\r");
}

void test_unicode_context_normalization_preserves_original_context() {
    Workspace workspace;
    const std::string original = u8"title \u2014 \u201cquote\u201d\nold\n";
    workspace.write("unicode.txt", original);
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: unicode.txt\n"
                                                "@@\n title - \"quote\"\n-old\n+new\n"
                                                "*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("unicode.txt") == u8"title \u2014 \u201cquote\u201d\nnew\n");
}

void test_update_header_looking_context_remains_context() {
    Workspace workspace;
    workspace.write("markers.txt", "*** Update File: untouched.txt\nold\n");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: markers.txt\n"
                                                "@@\n *** Update File: untouched.txt\n-old\n+new\n"
                                                "*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("markers.txt") == "*** Update File: untouched.txt\nnew\n");
    CHECK(!workspace.has("untouched.txt"));
}

void test_codex_rejects_malformed_hunks_before_writing() {
    const std::string patches[] = {
        "*** Begin Patch\n*** Add File: new.txt\n+x\n*** End Patch\ngarbage\n",
        "*** Begin Patch\n*** Update File: original.txt\n*** End Patch\n",
        "*** Begin Patch\n*** Update File: original.txt\n@@\n*** End Patch\n",
        "*** Begin Patch\n*** Update File: original.txt\n@@\n@@\n-old\n+new\n*** End Patch\n",
        "*** Begin Patch\n*** Update File: original.txt\n@@@\n-old\n+new\n*** End Patch\n",
        "*** Begin Patch\n*** Add File: new.txt\n+x\n\n*** End Patch\n",
        "*** Begin Patch\n*** Update File: original.txt\n@@\n-old\n+new\n*** Move to: "
        "moved.txt\n*** End Patch\n",
    };
    for (const auto& patch : patches) {
        Workspace workspace;
        workspace.write("original.txt", "old\n");
        auto tool = makeMaiApplyPatchTool();
        const auto result = tool->execute(patchArgs(patch), workspace.context());
        CHECK(result.hasError());
        CHECK(workspace.read("original.txt") == "old\n");
        CHECK(!workspace.has("new.txt"));
        CHECK(!workspace.has("moved.txt"));
    }
}

void test_sequential_add_then_update_uses_the_created_file() {
    Workspace workspace;
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Add File: new.txt\n+old\n"
                                                "*** Update File: new.txt\n@@\n-old\n+new\n"
                                                "*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("new.txt") == "new\n");
}

void test_missing_final_newline_is_added_on_update() {
    Workspace workspace;
    workspace.write("last.txt", "old");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: last.txt\n"
                                                "@@\n-old\n+new\n*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("last.txt") == "new\n");
}

void test_cr_only_files_are_logical_lines() {
    Workspace workspace;
    workspace.write("cr.txt", "keep\rold\rlast\r");
    auto tool = makeMaiApplyPatchTool();
    const auto result = tool->execute(patchArgs("*** Begin Patch\n*** Update File: cr.txt\n"
                                                "@@\n-old\n+new\n*** End Patch\n"),
                                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("cr.txt") == "keep\rnew\rlast\r");
}

MaiPatchOptions ordinaryOptions(const Workspace& workspace) {
    MaiPatchOptions options;
    options.root = workspace.root.u8string();
    return options;
}

void test_ordinary_result_records_only_successful_changes_before_failure() {
    Workspace workspace;
    workspace.write("good.txt", "old\n");
    workspace.write("bad.txt", "different\n");
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Update File: good.txt\n@@\n-old\n+new\n"
        "*** Update File: bad.txt\n@@\n-old\n+new\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(result.error.hasError());
    CHECK(result.failurePath == "bad.txt");
    CHECK(!result.failedWriteMayHaveChangedFile);
    CHECK(result.appliedChanges.size() == 1);
    if (!result.appliedChanges.empty()) {
        CHECK(result.appliedChanges[0].path == "good.txt");
        CHECK(result.appliedChanges[0].status == 'M');
        CHECK(result.appliedChanges[0].bytesWritten == 4);
        CHECK(fs::u8path(result.appliedChanges[0].resolvedPath) == workspace.root / "good.txt");
    }
    CHECK(workspace.read("good.txt") == "new\n");
    CHECK(workspace.read("bad.txt") == "different\n");
}

void test_move_overwrites_destination_then_later_hunk_uses_it() {
    Workspace workspace;
    workspace.write("from.txt", "old\n");
    workspace.write("to.txt", "overwritten\n");
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Update File: from.txt\n"
        "*** Move to: to.txt\n@@\n-old\n+middle\n"
        "*** Update File: to.txt\n@@\n-middle\n+final\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(!result.error.hasError());
    CHECK(result.failurePath.empty());
    CHECK(result.appliedChanges.size() == 3);
    CHECK(workspace.read("to.txt") == "final\n");
    CHECK(!workspace.has("from.txt"));
}

void test_add_only_and_later_chunks_match_the_original_source() {
    Workspace workspace;
    workspace.write("source.txt", "a\nb\n");
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Update File: source.txt\n@@\n+tail\n"
        "@@\n-b\n+B\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(!result.error.hasError());
    CHECK(workspace.read("source.txt") == "a\nB\ntail\n");
}

void test_trailing_empty_context_is_an_optional_newline_sentinel() {
    Workspace workspace;
    workspace.write("sentinel.txt", "old\n");
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Update File: sentinel.txt\n"
        "@@\n-old\n+new\n \n*** End of File\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(!result.error.hasError());
    CHECK(workspace.read("sentinel.txt") == "new\n");
}

void test_utf8_bom_is_literal_source_text_like_codex() {
    Workspace workspace;
    const std::string bom = "\xef\xbb\xbf";
    workspace.write("bom.txt", bom + "header\nold\n");
    const auto result = maiApplyPatch("*** Begin Patch\n*** Update File: bom.txt\n@@\n " + bom +
                                          "header\n-old\n+new\n*** End Patch\n",
                                      ordinaryOptions(workspace));
    CHECK(!result.error.hasError());
    CHECK(workspace.read("bom.txt") == bom + "header\nnew\n");
    const auto missingBom = maiApplyPatch(
        "*** Begin Patch\n*** Update File: bom.txt\n"
        "@@\n-header\n+changed\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(missingBom.error.hasError());
    CHECK(workspace.read("bom.txt") == bom + "header\nnew\n");
}

void test_non_utf8_update_is_rejected_without_writing() {
    Workspace workspace;
    const std::string invalid = std::string("old\n") + char(0xff);
    workspace.write("invalid.txt", invalid);
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Update File: invalid.txt\n"
        "@@\n-old\n+new\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(result.error.hasError());
    CHECK(result.appliedChanges.empty());
    CHECK(workspace.read("invalid.txt") == invalid);
}

void test_unicode_paths_work_for_add_move_and_delete() {
    Workspace workspace;
    const std::string first = u8"\u8865\u4e01/\u539f\u6587.txt";
    const std::string second = u8"\u8865\u4e01/\u79fb\u52a8.txt";
    const auto result =
        maiApplyPatch("*** Begin Patch\n*** Add File: " + first +
                          "\n+old\n"
                          "*** Update File: " +
                          first + "\n*** Move to: " + second + "\n@@\n-old\n+new\n*** End Patch\n",
                      ordinaryOptions(workspace));
    CHECK(!result.error.hasError());
    CHECK(!workspace.has(first));
    CHECK(workspace.read(second) == "new\n");
    const auto removed =
        maiApplyPatch("*** Begin Patch\n*** Delete File: " + second + "\n*** End Patch\n",
                      ordinaryOptions(workspace));
    CHECK(!removed.error.hasError());
    CHECK(!workspace.has(second));
}

void test_write_failure_reports_known_changes_and_unknown_failed_write() {
    Workspace workspace;
    fs::create_directories(workspace.root / "occupied");
    workspace.write("occupied/keep.txt", "retained\n");
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Add File: good.txt\n+done\n"
        "*** Add File: occupied\n+cannot-write-directory\n*** End Patch\n",
        ordinaryOptions(workspace));
    CHECK(result.error.hasError());
    CHECK(result.appliedChanges.size() == 1);
    CHECK(result.failurePath == "occupied");
    CHECK(result.failedWriteMayHaveChangedFile);
    CHECK(workspace.read("good.txt") == "done\n");
    CHECK(workspace.read("occupied/keep.txt") == "retained\n");
}

void test_ordinary_and_model_entries_use_the_same_patch_behavior() {
    Workspace ordinary;
    Workspace model;
    ordinary.write("source.txt", "old\r\n");
    model.write("source.txt", "old\r\n");
    const std::string patch =
        "*** Begin Patch\n*** Update File: source.txt\n@@\n-old\n+new\n"
        "*** Add File: added.txt\n+added\n*** End Patch\n";
    const auto result = maiApplyPatch(patch, ordinaryOptions(ordinary));
    auto tool = makeMaiApplyPatchTool();
    const auto modelResult = tool->execute(patchArgs(patch), model.context());
    CHECK(!result.error.hasError());
    CHECK(!modelResult.hasError());
    CHECK(ordinary.read("source.txt") == model.read("source.txt"));
    CHECK(ordinary.read("added.txt") == model.read("added.txt"));
}

void test_heredoc_wrapper_is_accepted_like_codex_lenient_parser() {
    const std::string wrappers[] = {"<<EOF", "<<'EOF'", "<<\"EOF\""};
    for (const auto& wrapper : wrappers) {
        Workspace workspace;
        const auto result = maiApplyPatch(wrapper +
                                              "\n*** Begin Patch\n*** Add File: new.txt\n"
                                              "+new\n*** End Patch\nEOF\n",
                                          ordinaryOptions(workspace));
        CHECK(!result.error.hasError());
        CHECK(workspace.read("new.txt") == "new\n");
    }
}

#if defined(_WIN32)
void test_failed_source_delete_retains_successful_move_destination() {
    Workspace workspace;
    workspace.write("from.txt", "old\n");
    const auto path = workspace.root / "from.txt";
    // 原生共享模式阻止删除而允许读写，真实覆盖“目标写成功，源删除失败”的阶段。
    const HANDLE held =
        ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    if (held == INVALID_HANDLE_VALUE) return;
    const auto result = maiApplyPatch(
        "*** Begin Patch\n*** Update File: from.txt\n"
        "*** Move to: to.txt\n@@\n-old\n+new\n*** End Patch\n",
        ordinaryOptions(workspace));
    ::CloseHandle(held);
    CHECK(result.error.hasError());
    CHECK(result.failurePath == "from.txt");
    CHECK(!result.failedWriteMayHaveChangedFile);
    CHECK(result.appliedChanges.size() == 1);
    if (!result.appliedChanges.empty()) {
        CHECK(result.appliedChanges[0].path == "to.txt");
        CHECK(result.appliedChanges[0].bytesWritten == 4);
    }
    CHECK(workspace.read("to.txt") == "new\n");
    CHECK(workspace.read("from.txt") == "old\n");
}
#endif

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_ordinary_result_records_only_successful_changes_before_failure);
    RUN(test_move_overwrites_destination_then_later_hunk_uses_it);
    RUN(test_add_only_and_later_chunks_match_the_original_source);
    RUN(test_trailing_empty_context_is_an_optional_newline_sentinel);
    RUN(test_utf8_bom_is_literal_source_text_like_codex);
    RUN(test_non_utf8_update_is_rejected_without_writing);
    RUN(test_unicode_paths_work_for_add_move_and_delete);
    RUN(test_write_failure_reports_known_changes_and_unknown_failed_write);
    RUN(test_ordinary_and_model_entries_use_the_same_patch_behavior);
    RUN(test_heredoc_wrapper_is_accepted_like_codex_lenient_parser);
#if defined(_WIN32)
    RUN(test_failed_source_delete_retains_successful_move_destination);
#endif

    RUN(test_codex_name_is_available_to_the_bootstrap_catalog);
    RUN(test_change_context_selects_the_named_function);
    RUN(test_add_only_chunk_appends_and_terminates_the_last_line);
    RUN(test_end_of_file_does_not_match_a_middle_occurrence);
    RUN(test_context_bytes_and_mixed_line_endings_are_retained);
    RUN(test_unicode_context_normalization_preserves_original_context);
    RUN(test_update_header_looking_context_remains_context);
    RUN(test_codex_rejects_malformed_hunks_before_writing);
    RUN(test_sequential_add_then_update_uses_the_created_file);
    RUN(test_missing_final_newline_is_added_on_update);
    RUN(test_cr_only_files_are_logical_lines);

    RUN(test_updates_several_places_in_one_file);
    RUN(test_one_patch_can_touch_several_files);
    RUN(test_earlier_hunks_remain_applied_when_a_later_hunk_fails);
    RUN(test_add_and_delete_files);
    RUN(test_add_overwrites_an_existing_file_like_codex);
    RUN(test_move_writes_the_new_path_and_removes_the_old);
    RUN(test_crlf_files_keep_their_line_endings);
    RUN(test_context_is_matched_leniently_on_whitespace);
    RUN(test_malformed_patches_are_rejected_clearly);
    RUN(test_cannot_escape_the_working_directory);
    RUN(test_missing_file_is_reported_as_not_found);
    RUN(test_apply_patch_always_needs_approval);
    RUN(test_delete_and_move_require_approval_for_each_affected_file);
    if (failures == 0) std::printf("apply_patch tool tests passed\n");
    return failures == 0 ? 0 : 1;
}

// 文件作用：验证普通搜索入口的完整行匹配、可重放分页、过滤与安全边界。
// 测试只在独立临时目录创建数据，不读取仓库或用户文件。
#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "MaiFileSearch.h"
#include "MaiFileSystem.h"
#include "MaiThread.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

class Fixture {
public:
    Fixture();
    ~Fixture();
    void write(const std::string& relative, const std::string& text);
    MaiFilePath path() const;
    fs::path native() const;

private:
    fs::path mPath;
};

Fixture::Fixture() {
    static unsigned sequence = 0;
    mPath = fs::temp_directory_path() / ("mai-file-search-" + std::to_string(++sequence));
    // 测试目录只使用本进程创建且不存在的路径，不删除可能属于其他运行的旧数据。
    while (fs::exists(mPath)) mPath += "-next";
    fs::create_directories(mPath);
}

Fixture::~Fixture() {
    std::error_code ignored;
    fs::remove_all(mPath, ignored);
}
void Fixture::write(const std::string& relative, const std::string& text) {
    const auto destination = mPath / fs::u8path(relative);
    fs::create_directories(destination.parent_path());
    std::ofstream stream(destination, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}
MaiFilePath Fixture::path() const {
    return MaiFilePath::fromUtf8(mPath.u8string());
}
fs::path Fixture::native() const {
    return mPath;
}

void test_full_line_literal_regex_and_context() {
    Fixture fixture;
    fixture.write("long.txt", "before\n" + std::string(480, 'x') + "NEEDLE" +
                                  std::string(600, 'z') + "\nafter\n");
    MaiFileSearch search;
    MaiFileContentSearchOptions options;
    options.root = fixture.path();
    options.pattern = "NEEDLE";
    options.contextBefore = 1;
    options.contextAfter = 1;
    auto page = search.searchText(options);
    CHECK(!page.error);
    CHECK(page.matches.size() == 1);
    CHECK(!page.hasMore);
    if (!page.matches.empty()) {
        CHECK(page.matches[0].line == 2);
        CHECK(page.matches[0].text.find("NEEDLE") != std::string::npos);
        CHECK(page.matches[0].displayOffsetBytes > 0);
        CHECK(page.matches[0].truncated);
        CHECK(page.matches[0].before.size() == 1);
        CHECK(page.matches[0].after.size() == 1);
    }
    options.isRegex = true;
    options.pattern = "x+NEEDLEz+";
    page = search.searchText(options);
    CHECK(!page.error);
    CHECK(page.matches.size() == 1);
    options.pattern = "needle";
    options.isRegex = false;
    options.caseSensitive = false;
    CHECK(search.searchText(options).matches.size() == 1);
    options.caseSensitive = true;
    CHECK(search.searchText(options).matches.empty());
}

void test_name_paging_ranking_filters_and_freshness() {
    Fixture fixture;
    for (int index = 0; index < 365; ++index)
        fixture.write("src/file" + std::to_string(index) + ".cpp", "sample\n");
    fixture.write("README.md", "read me\n");
    fixture.write("deep/readme/other.md", "other\n");
    fixture.write("node_modules/hidden.cpp", "dependency\n");
    fixture.write(".hidden.cpp", "hidden\n");
    fixture.write(".gitignore", "ignored/\n*.tmp\n!keep.tmp\n");
    fixture.write("ignored/found.cpp", "ignored\n");
    fixture.write("drop.tmp", "drop\n");
    fixture.write("keep.tmp", "keep\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = fixture.path();
    options.query = "**/*.cpp";
    auto first = search.findFiles(options);
    CHECK(!first.error);
    CHECK(first.files.size() == 300);
    CHECK(first.hasMore);
    CHECK(!first.nextCursor.empty());
    std::set<std::string> names;
    for (const auto& match : first.files) names.insert(match.path);
    options.cursor = first.nextCursor;
    auto second = search.findFiles(options);
    CHECK(!second.error);
    CHECK(second.files.size() == 65);
    CHECK(!second.hasMore);
    for (const auto& match : second.files) names.insert(match.path);
    CHECK(names.size() == 365);
    const auto replay = search.findFiles(options);
    CHECK(replay.files.size() == second.files.size());
    options.query = "different";
    CHECK(search.findFiles(options).error.code() == MaiErrorCode::InvalidInput);
    options.cursor.clear();
    options.query = "readme";
    options.mode = MaiFileNameSearchMode::Fuzzy;
    auto fuzzy = search.findFiles(options);
    CHECK(!fuzzy.files.empty());
    if (!fuzzy.files.empty()) CHECK(fuzzy.files.front().path == "README.md");
    options.mode = MaiFileNameSearchMode::Glob;
    options.query = "*.tmp";
    auto ignored = search.findFiles(options);
    CHECK(ignored.files.size() == 1);
    if (!ignored.files.empty()) CHECK(ignored.files[0].path == "keep.tmp");
    options.query = "new.txt";
    CHECK(search.findFiles(options).files.empty());
    fixture.write("new.txt", "new\n");
    CHECK(search.findFiles(options).files.empty());
    options.refresh = true;
    CHECK(search.findFiles(options).files.size() == 1);
    options.refresh = false;
    if (!second.files.empty()) fixture.write(second.files.front().path, "changed source\n");
    options.query = "**/*.cpp";
    options.cursor = first.nextCursor;
    CHECK(search.findFiles(options).error.code() == MaiErrorCode::InvalidInput);
}

void test_unicode_fuzzy_brace_globs_and_scope() {
    Fixture fixture;
    fixture.write(u8"\u4e00\u6f3d\u5020.txt", "false UTF-8 byte subsequence\n");
    fixture.write(u8"\u4f60.txt", "actual scalar\n");
    fixture.write("src/first.cpp", "source\n");
    fixture.write("src/second.h", "header\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = fixture.path();
    options.scopeKey = "policy-a";
    options.mode = MaiFileNameSearchMode::Fuzzy;
    options.query = u8"\u4f60";
    auto page = search.findFiles(options);
    CHECK(page.files.size() == 1);
    if (!page.files.empty()) CHECK(page.files.front().path == u8"\u4f60.txt");
    options.mode = MaiFileNameSearchMode::Glob;
    options.query = "**/*.{cpp,h}";
    options.pageSize = 1;
    page = search.findFiles(options);
    CHECK(page.files.size() == 1);
    CHECK(page.hasMore);
    CHECK(page.visitedEntries == 0);
    options.cursor = page.nextCursor;
    options.scopeKey = "policy-b";
    CHECK(search.findFiles(options).error.code() == MaiErrorCode::InvalidInput);
    options.cursor.clear();
    page = search.findFiles(options);
    CHECK(page.visitedEntries > 0);
    CHECK(!page.cacheReused);
    options.cursor = page.nextCursor;
    page = search.findFiles(options);
    CHECK(!page.error);
    CHECK(page.files.size() == 1);
    CHECK(page.visitedEntries == 0);
    options.cursor.clear();
    options.refresh = true;
    page = search.findFiles(options);
    CHECK(page.visitedEntries > 0);
    options.refresh = false;
    search.invalidate(options.root);
    page = search.findFiles(options);
    CHECK(page.visitedEntries > 0);
}

void test_empty_and_malformed_glob_braces_are_rejected() {
    Fixture fixture;
    fixture.write("image.png", "image\n");
    fixture.write("source.cpp", "code\n");
    fixture.write("{.txt", "literal brace\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = fixture.path();
    MaiFileContentSearchOptions content;
    content.root = fixture.path();
    content.pattern = "code";
    const std::vector<std::string> invalid = {"**/*.{png,}", "{,png}", "{png,,h}",  "{}",
                                              "{png,h",      "png,h}", "{png,{h,}}"};
    for (const auto& pattern : invalid) {
        options.query = pattern;
        CHECK(search.findFiles(options).error.code() == MaiErrorCode::InvalidInput);
        options.query = "**/*";
        options.filter.includeGlobs = {pattern};
        CHECK(search.findFiles(options).error.code() == MaiErrorCode::InvalidInput);
        content.filter.includeGlobs = {pattern};
        CHECK(search.searchText(content).error.code() == MaiErrorCode::InvalidInput);
        options.filter.includeGlobs.clear();
        options.filter.excludeGlobs = {pattern};
        CHECK(search.findFiles(options).error.code() == MaiErrorCode::InvalidInput);
        content.filter.includeGlobs.clear();
        content.filter.excludeGlobs = {pattern};
        CHECK(search.searchText(content).error.code() == MaiErrorCode::InvalidInput);
        options.filter.excludeGlobs.clear();
        content.filter.excludeGlobs.clear();
    }
    options.query = "**/*.{png,{h,cpp}}";
    CHECK(search.findFiles(options).files.size() == 2);
    options.query = "[{}].txt";
    CHECK(search.findFiles(options).files.size() == 1);
    // 模糊查询不是 glob 语法；字面花括号仍是普通 Unicode 标量。
    options.mode = MaiFileNameSearchMode::Fuzzy;
    options.query = "{";
    CHECK(search.findFiles(options).files.size() == 1);
}

void test_cancel_and_deadline_while_waiting_for_busy_search() {
    Fixture fixture;
    fixture.write("source.txt", "match\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions slow;
    slow.root = fixture.path();
    std::atomic<bool> entered{false};
    MaiFileSearchCallbacks delayed;
    delayed.pathAllowed = [&](const MaiFilePath&) {
        entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    };
    std::thread worker([&] {
        MaiThread::setCurrentName("mai-search-test");
        search.findFiles(slow, delayed);
    });
    while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    MaiFileSearchCallbacks canceled;
    canceled.isCanceled = [] { return true; };
    const auto begin = std::chrono::steady_clock::now();
    CHECK(search.findFiles(slow, canceled).canceled);
    const auto canceledMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
    CHECK(canceledMs < 50);
    auto impatient = slow;
    impatient.budget.maxMilliseconds = 10;
    const auto deadlineBegin = std::chrono::steady_clock::now();
    const auto page = search.findFiles(impatient);
    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - deadlineBegin)
                              .count();
    CHECK(page.partial);
    CHECK(!page.issues.empty());
    CHECK(waitedMs < 75);
    worker.join();
    std::printf("Cancel admission: %lld ms; deadline admission: %lld ms\n", canceledMs, waitedMs);
}

void test_enumeration_failure_and_stale_content_replay() {
    Fixture fixture;
    fixture.write("gone/child.txt", "needle\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = fixture.path();
    options.query = "not-present";
    MaiFileSearchCallbacks callbacks;
    callbacks.pathAllowed = [&](const MaiFilePath& path) {
        if (path.baseName().toUtf8() == "gone") {
            std::error_code ignored;
            fs::remove_all(fixture.native() / "gone", ignored);
        }
        return true;
    };
    const auto incomplete = search.findFiles(options, callbacks);
    CHECK(!incomplete.error);
    CHECK(incomplete.partial);
    CHECK(incomplete.files.empty());
    bool enumeration = false;
    for (const auto& issue : incomplete.issues)
        enumeration = enumeration || issue.reason == "enumeration_failed";
    CHECK(enumeration);
    fixture.write("source.txt", "needle\nneedle\n");
    MaiFileContentSearchOptions content;
    content.root = fixture.path();
    content.pattern = "needle";
    content.refresh = true;
    content.pageSize = 1;
    auto page = search.searchText(content);
    CHECK(page.hasMore);
    content.refresh = false;
    content.cursor = page.nextCursor;
    page = search.searchText(content);
    CHECK(!page.error);
    CHECK(page.matches.size() == 1);
    fixture.write("source.txt", "changed\n");
    page = search.searchText(content);
    CHECK(page.error.code() == MaiErrorCode::InvalidInput);
    CHECK(page.matches.empty());
    CHECK(page.nextCursor.empty());
}

void test_exact_file_root_and_targeted_errors() {
    Fixture fixture;
    fixture.write("source.txt", std::string(480, 'x') + "needle\nneedle\nneedle\n");
    fixture.write("sibling.txt", "needle in an unrelated file\n");
    MaiFileSearch search;
    MaiFileContentSearchOptions options;
    options.root = fixture.path().append(MaiFilePath::fromUtf8("source.txt"));
    options.pattern = "needle";
    options.pageSize = 2;
    auto page = search.searchText(options);
    CHECK(!page.error);
    CHECK(page.visitedEntries == 0);
    CHECK(page.matches.size() == 2);
    CHECK(page.hasMore);
    if (!page.matches.empty()) {
        CHECK(page.matches.front().text.find("needle") != std::string::npos);
        CHECK(page.matches.front().path == "source.txt");
    }
    options.cursor = page.nextCursor;
    page = search.searchText(options);
    CHECK(!page.error);
    CHECK(page.matches.size() == 1);
    CHECK(!page.hasMore);
    options.cursor.clear();
    MaiFileSearchCallbacks decoder;
    decoder.decodeText = [](const MaiFilePath&, const std::string&) -> MaiResult<std::string> {
        return {MaiErrorCode::NotSupported, "test unsupported encoding"};
    };
    page = search.searchText(options, decoder);
    CHECK(page.error.code() == MaiErrorCode::NotSupported);
    CHECK(page.error.message() == "test unsupported encoding");
    fixture.write("big.txt", std::string(2u * 1024 * 1024 + 1, 'x'));
    options.root = fixture.path().append(MaiFilePath::fromUtf8("big.txt"));
    page = search.searchText(options);
    CHECK(page.error.code() == MaiErrorCode::InvalidInput);
    CHECK(page.error.message().find("2 MiB") != std::string::npos);
    options.root = fixture.path().append(MaiFilePath::fromUtf8("missing.txt"));
    CHECK(search.searchText(options).error.code() == MaiErrorCode::NotFound);
#if defined(_WIN32)
    options.root = fixture.path().append(MaiFilePath::fromUtf8("source.txt"));
    const auto handle = ::CreateFileW(options.root.value().c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle != INVALID_HANDLE_VALUE) {
        std::string ignored;
        const auto expected = MaiFileSystem::readFile(options.root, ignored);
        CHECK(expected.hasError());
        page = search.searchText(options);
        CHECK(page.error.code() == expected.code());
        CHECK(page.error.message() == expected.message());
        ::CloseHandle(handle);
    }
#endif
}

void test_oversized_cache_never_returns_dead_cursor() {
    Fixture fixture;
    fixture.write("source.txt", "match\n");
    MaiFileSearch search;
    MaiFileContentSearchOptions options;
    options.root = fixture.path();
    options.pattern = "match";
    options.pageSize = 1;
    MaiFileSearchCallbacks callbacks;
    callbacks.decodeText = [](const MaiFilePath&, const std::string&) -> MaiResult<std::string> {
        std::string text(64u * 1024 * 1024, 'x');
        text.replace(0, 6, "match\n");
        return text;
    };
    const auto page = search.searchText(options, callbacks);
    CHECK(!page.error);
    CHECK(page.partial);
    CHECK(page.matches.size() == 1);
    CHECK(!page.hasMore);
    CHECK(page.nextCursor.empty());
    bool capacity = false;
    for (const auto& issue : page.issues) capacity = capacity || issue.reason == "cache_capacity";
    CHECK(capacity);
}

void test_warm_queries_and_content_cursors_do_not_walk_again() {
    Fixture fixture;
    const std::string prefix(140, 'p');
    for (int index = 0; index < 10002; ++index)
        fixture.write(prefix + "file" + std::to_string(index) + ".txt", "match\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = fixture.path();
    options.pageSize = 1;
    std::size_t authorizations = 0;
    MaiFileSearchCallbacks callbacks;
    callbacks.pathAllowed = [&](const MaiFilePath&) {
        ++authorizations;
        return true;
    };
    auto timedName = [&](const char* label) {
        authorizations = 0;
        const auto begin = std::chrono::steady_clock::now();
        auto result = search.findFiles(options, callbacks);
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        std::printf("%s: %.3f ms visited=%zu authorizations=%zu\n", label, ms,
                    result.visitedEntries, authorizations);
        return result;
    };
    auto cold = timedName("cold-name");
    CHECK(cold.visitedEntries == 10002);
    CHECK(authorizations >= 10002);
    options.mode = MaiFileNameSearchMode::Fuzzy;
    options.query = "file1";
    auto warm = timedName("changed-query");
    CHECK(warm.visitedEntries == 0);
    CHECK(authorizations <= 3);
    CHECK(warm.cacheReused);
    options.cursor = warm.nextCursor;
    auto next = timedName("next-name");
    CHECK(next.visitedEntries == 0);
    CHECK(authorizations <= 3);
    auto replay = timedName("replay-name");
    CHECK(replay.visitedEntries == 0);
    CHECK(authorizations <= 3);
    MaiFileContentSearchOptions content;
    content.root = fixture.path();
    content.pattern = "match";
    content.pageSize = 1;
    authorizations = 0;
    auto page = search.searchText(content, callbacks);
    CHECK(page.visitedEntries == 0);
    CHECK(authorizations <= 5);
    CHECK(page.hasMore);
    content.cursor = page.nextCursor;
    authorizations = 0;
    page = search.searchText(content, callbacks);
    CHECK(page.visitedEntries == 0);
    CHECK(authorizations <= 5);
    authorizations = 0;
    page = search.searchText(content, callbacks);
    CHECK(page.visitedEntries == 0);
    CHECK(page.readBytes == 0);
    CHECK(page.searchedFiles == 0);
    CHECK(authorizations <= 3);
    options.cursor.clear();
    for (int index = 0; index < 5; ++index) {
        options.query = "missing-query-" + std::to_string(index);
        CHECK(search.findFiles(options, callbacks).visitedEntries == 0);
    }
    // 八个会话共享同一较大目录索引；共享内存不能按会话重复计费并提前淘汰首个游标。
    options.mode = MaiFileNameSearchMode::Glob;
    options.query = "**/*";
    options.cursor = cold.nextCursor;
    const auto retained = search.findFiles(options, callbacks);
    CHECK(!retained.error);
    CHECK(retained.files.size() == 1);
}

void test_content_continuation_modes_and_budget() {
    Fixture fixture;
    std::string text;
    for (int index = 0; index < 470; ++index) text += "match " + std::to_string(index) + "\n";
    fixture.write("many.txt", text);
    fixture.write("other.txt", "match\nno\nmatch\n");
    MaiFileSearch search;
    MaiFileContentSearchOptions options;
    options.root = fixture.path();
    options.pattern = "match";
    std::set<std::string> identities;
    auto page = search.searchText(options);
    CHECK(page.matches.size() == 200);
    CHECK(page.hasMore);
    int calls = 0;
    while (true) {
        CHECK(!page.error);
        for (const auto& match : page.matches)
            identities.insert(match.path + ":" + std::to_string(match.line));
        if (!page.hasMore || ++calls > 10) break;
        options.cursor = page.nextCursor;
        page = search.searchText(options);
    }
    CHECK(identities.size() == 472);
    CHECK(!page.hasMore);
    options.cursor.clear();
    options.filter.includeGlobs = {"other.txt"};
    options.pageSize = 2;
    page = search.searchText(options);
    CHECK(page.matches.size() == 2);
    CHECK(!page.hasMore);
    options.pageSize = 200;
    options.filter.includeGlobs.clear();
    options.cursor.clear();
    options.mode = MaiFileContentSearchMode::Count;
    page = search.searchText(options);
    CHECK(page.matches.size() == 2);
    if (page.matches.size() == 2) {
        CHECK(page.matches[0].count == 470);
        CHECK(page.matches[1].count == 2);
    }
    options.mode = MaiFileContentSearchMode::Files;
    page = search.searchText(options);
    CHECK(page.matches.size() == 2);
    options.mode = MaiFileContentSearchMode::Lines;
    options.budget.maxOutputBytes = 1024;
    page = search.searchText(options);
    CHECK(page.hasMore);
    CHECK(!page.matches.empty());
    const auto nextLine = page.matches.back().line + 1;
    options.cursor = page.nextCursor;
    page = search.searchText(options);
    CHECK(!page.matches.empty());
    if (!page.matches.empty()) CHECK(page.matches.front().line == nextLine);
}

void test_regex_safety_partial_cancel_and_permissions() {
    Fixture fixture;
    fixture.write("hostile.txt", std::string(20000, 'a') + "!\n");
    fixture.write("words.txt", "alpha123 beta\nplain\n");
    MaiFileSearch search;
    MaiFileContentSearchOptions options;
    options.root = fixture.path();
    options.pattern = "(a+)+$";
    options.isRegex = true;
    options.budget.maxRegexSteps = 1000;
    auto page = search.searchText(options);
    CHECK(!page.error);
    CHECK(page.partial);
    CHECK(!page.issues.empty());
    options.pattern = "(a)\\1";
    CHECK(search.searchText(options).error.code() == MaiErrorCode::InvalidInput);
    options.pattern = "(?=alpha)";
    CHECK(search.searchText(options).error.code() == MaiErrorCode::InvalidInput);
    options.pattern = "^\\w+\\s\\w+$";
    options.budget.maxRegexSteps = 16000000;
    CHECK(search.searchText(options).matches.size() == 1);
    options.pattern = "\\balpha\\d{2,4}\\b";
    CHECK(search.searchText(options).matches.size() == 1);
    options.pattern = "^alpha\\d{3} beta$";
    CHECK(search.searchText(options).matches.size() == 1);
    options.pattern = "^alpha\\d{4} beta$";
    CHECK(search.searchText(options).matches.empty());
    options.pattern = "^alpha\\d{2,} beta$";
    CHECK(search.searchText(options).matches.size() == 1);
    options.pattern = "^alpha\\d{129} beta$";
    CHECK(search.searchText(options).error.code() == MaiErrorCode::InvalidInput);
    options.pattern = "alpha";
    MaiFileSearchCallbacks callbacks;
    callbacks.isCanceled = [] { return true; };
    CHECK(search.searchText(options, callbacks).canceled);
    callbacks.isCanceled = {};
    callbacks.pathAllowed = [](const MaiFilePath&) { return false; };
    CHECK(search.searchText(options, callbacks).error.code() == MaiErrorCode::InvalidInput);
    options.isRegex = false;
    options.pattern = "absent";
    options.budget.maxEntries = 1;
    page = search.searchText(options);
    CHECK(page.partial);
    // 编码和二进制跳过必须可见，不能作为已完整检查的无命中文件。
    fixture.write("binary.dat", std::string("x\0needle", 8));
    options.budget.maxEntries = 50000;
    options.pattern = "needle";
    options.refresh = true;
    page = search.searchText(options);
    CHECK(page.partial);
}

void test_nested_ignore_include_unicode_and_symlink() {
    Fixture fixture;
    fixture.write(".gitignore", "*.tmp\n");
    fixture.write("src/.gitignore", "!keep.tmp\n/local.txt\n");
    fixture.write("src/keep.tmp", "keep\n");
    fixture.write("src/drop.tmp", "drop\n");
    fixture.write("src/local.txt", "local\n");
    fixture.write("other/local.txt", "visible\n");
    fixture.write(u8"src/\u6587\u4ef6.txt", u8"\u641c\u7d22 needle\n");
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = fixture.path();
    options.query = "**/*";
    options.filter.includeGlobs = {"*.tmp"};
    auto page = search.findFiles(options);
    CHECK(page.files.size() == 1);
    if (!page.files.empty()) CHECK(page.files[0].path == "src/keep.tmp");
    MaiFileContentSearchOptions content;
    content.root = fixture.path();
    content.pattern = u8"\u641c\u7d22";
    auto matches = search.searchText(content);
    CHECK(matches.matches.size() == 1);
    Fixture outside;
    outside.write("secret.txt", "needle\n");
    std::error_code error;
    fs::create_directory_symlink(outside.native(), fixture.native() / "escape", error);
    if (!error) {
        content.pattern = "needle";
        matches = search.searchText(content);
        CHECK(matches.matches.size() == 1);
        for (const auto& match : matches.matches)
            CHECK(match.path.find("escape") == std::string::npos);
    } else
        std::printf("SKIP symlink creation is unavailable: %d\n", error.value());
}

void test_regex_subset_agrees_with_ecmascript_for_short_text() {
    Fixture fixture;
    const std::vector<std::string> lines = {
        "", "a", "ab", "aaa", "aab", "bab", "abba", "alpha123 beta", "123", "x-y", "a b", "ABBA"};
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    fixture.write("source.txt", text);
    const std::vector<std::string> patterns = {
        "a",     "a*",          "a+",     "a?",     "^a$",    "^(ab|a)+$",      "a.*b",
        "[ab]+", "[^a]+",       "[a-z]+", "a{0}",   "a{0,2}", "a{2,3}",         "(ab){1,3}",
        "a{2,}", "(a?){2,3}",   "^$",     "a|b",    "(a|)",   "\\b\\w{2,5}\\b", "\\d+",
        "\\D+",  "\\w+\\s\\w+", "a\\Bb",  "[\\d]+", "[\\w-]+"};
    MaiFileSearch search;
    MaiFileContentSearchOptions options;
    options.root = fixture.path();
    options.isRegex = true;
    for (bool sensitive : {true, false}) {
        options.caseSensitive = sensitive;
        for (const auto& pattern : patterns) {
            options.pattern = pattern;
            const auto page = search.searchText(options);
            CHECK(!page.error);
            CHECK(!page.partial);
            std::set<std::size_t> actual;
            for (const auto& match : page.matches) actual.insert(match.line);
            std::set<std::size_t> expected;
            const auto flags =
                std::regex::ECMAScript | (sensitive ? std::regex::flag_type{} : std::regex::icase);
            const std::regex reference(pattern, flags);
            for (std::size_t index = 0; index < lines.size(); ++index)
                if (std::regex_search(lines[index], reference)) expected.insert(index + 1);
            if (actual != expected)
                std::printf("Regex difference: %s sensitive=%d\n", pattern.c_str(), sensitive);
            CHECK(actual == expected);
        }
    }
}

}  // namespace

int main() {
    test_full_line_literal_regex_and_context();
    test_name_paging_ranking_filters_and_freshness();
    test_content_continuation_modes_and_budget();
    test_regex_safety_partial_cancel_and_permissions();
    test_nested_ignore_include_unicode_and_symlink();
    test_regex_subset_agrees_with_ecmascript_for_short_text();
    test_unicode_fuzzy_brace_globs_and_scope();
    test_empty_and_malformed_glob_braces_are_rejected();
    test_cancel_and_deadline_while_waiting_for_busy_search();
    test_enumeration_failure_and_stale_content_replay();
    test_exact_file_root_and_targeted_errors();
    test_oversized_cache_never_returns_dead_cursor();
    test_warm_queries_and_content_cursors_do_not_walk_again();
    std::printf("MaiFileSearchTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

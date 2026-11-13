// 文件作用：验证原生文本分页的真实文件进度、字符边界、快照及预算契约。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "MaiFileSystem.h"
#include "MaiTextFileReader.h"

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
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        mRoot = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-text-reader-" + std::to_string(stamp)));
        CHECK(!MaiFileSystem::createDirectories(mRoot));
    }
    ~Fixture() {
        MaiFileSystem::removeRecursively(mRoot);
    }
    MaiFilePath write(const std::string& name, const std::string& contents) {
        const auto path = mRoot.append(MaiFilePath::fromUtf8(name));
        CHECK(!MaiFileSystem::writeFile(path, contents));
        return path;
    }

private:
    MaiFilePath mRoot;
};

void test_reads_beyond_old_eight_megabyte_prefix(Fixture& fixture) {
    std::string bytes(8 * 1024 * 1024 + 19, 'a');
    bytes += "\nexpected tail\nlast";
    const auto path = fixture.write("large.txt", bytes);
    MaiTextFileReadOptions options;
    options.offset = 2;
    options.limit = 1;
    const auto result = MaiTextFileReader::read(path, options);
    CHECK(result);
    if (!result) return;
    CHECK(result.value().lines.size() == 1);
    if (result.value().lines.empty()) return;
    CHECK(result.value().lines.front().number == 2);
    CHECK(result.value().lines.front().text == "expected tail");
    CHECK(!result.value().eof);
    CHECK(result.value().nextCursor);
}

void test_single_line_continues_beyond_eight_megabytes(Fixture& fixture) {
    constexpr std::size_t expectedBytes = 9 * 1024 * 1024 + 17;
    const auto path = fixture.write("huge-line.txt", std::string(expectedBytes, 'q'));
    MaiTextFileReadOptions options;
    options.maxOutputBytes = 1024 * 1024;
    std::size_t collected = 0;
    bool sawPositionBeyondOldPrefix = false;
    bool reachedEof = false;
    for (int page = 0; page < 12; ++page) {
        const auto result = MaiTextFileReader::read(path, options);
        CHECK(result);
        if (!result) return;
        CHECK(result.value().lines.size() == 1);
        if (result.value().lines.size() != 1) return;
        const auto& line = result.value().lines.front();
        CHECK(line.number == 1);
        CHECK(line.byteOffset == collected);
        CHECK(std::all_of(line.text.begin(), line.text.end(),
                          [](char value) { return value == 'q'; }));
        collected += line.text.size();
        if (result.value().eof) {
            CHECK(line.complete);
            reachedEof = true;
            break;
        }
        CHECK(!line.complete);
        CHECK(result.value().stopReason == MaiTextFileStopReason::OutputLimit);
        CHECK(result.value().nextCursor);
        if (!result.value().nextCursor) return;
        sawPositionBeyondOldPrefix |= result.value().nextCursor->byteOffset >= 8 * 1024 * 1024;
        options.cursor = result.value().nextCursor;
    }
    CHECK(reachedEof);
    CHECK(sawPositionBeyondOldPrefix);
    CHECK(collected == expectedBytes);
}

void test_unicode_long_line_continues_without_loss(Fixture& fixture) {
    const std::string unit = "x\u4e2d\U0001f642";
    std::string original;
    for (int index = 0; index < 25000; ++index) original += unit;
    const auto path = fixture.write("unicode-\u6587.txt", original + "\r\nnext");
    MaiTextFileReadOptions options;
    options.maxOutputBytes = 127;
    std::string collected;
    bool completed = false;
    for (int page = 0; page < 3000; ++page) {
        const auto result = MaiTextFileReader::read(path, options);
        CHECK(result);
        if (!result) return;
        std::size_t outputBytes = 0;
        for (const auto& line : result.value().lines) {
            outputBytes += line.text.size();
            if (line.number == 1) {
                CHECK(line.byteOffset == collected.size());
                collected += line.text;
                if (line.complete) completed = true;
            }
        }
        CHECK(outputBytes <= options.maxOutputBytes);
        if (result.value().eof) break;
        CHECK(result.value().nextCursor);
        if (!result.value().nextCursor) return;
        options.cursor = result.value().nextCursor;
    }
    CHECK(completed);
    CHECK(collected == original);
}

void test_scan_budget_resumes_seek(Fixture& fixture) {
    const auto path = fixture.write("scan.txt", std::string(10000, 'x') + "\ntarget\n");
    MaiTextFileReadOptions options;
    options.offset = 2;
    options.maxScanBytes = 17;
    std::uint64_t previous = 0;
    bool found = false;
    for (int page = 0; page < 1000; ++page) {
        const auto result = MaiTextFileReader::read(path, options);
        CHECK(result);
        if (!result) return;
        for (const auto& line : result.value().lines) {
            CHECK(line.number == 2);
            CHECK(line.text == "target");
            found = true;
        }
        if (result.value().eof) break;
        CHECK(result.value().stopReason == MaiTextFileStopReason::ScanLimit);
        CHECK(result.value().nextCursor);
        if (!result.value().nextCursor) return;
        CHECK(result.value().nextCursor->byteOffset > previous);
        previous = result.value().nextCursor->byteOffset;
        options.offset = 1;
        options.cursor = result.value().nextCursor;
    }
    CHECK(found);
}

void test_crlf_and_empty_lines(Fixture& fixture) {
    const auto path = fixture.write("lines.txt", "a\r\n\r\nb\rc\n");
    const auto result = MaiTextFileReader::read(path);
    CHECK(result);
    if (!result) return;
    CHECK(result.value().eof);
    CHECK(result.value().lines.size() == 3);
    if (result.value().lines.size() != 3) return;
    CHECK(result.value().lines[0].text == "a");
    CHECK(result.value().lines[1].text.empty());
    CHECK(result.value().lines[2].text == "b\rc");
}

void test_boms_and_utf16_surrogate_boundaries(Fixture& fixture) {
    const std::string little =
        std::string("\xff\xfe", 2) + std::string("A\0\x2d\x4e\x3d\xd8\x42\xde\r\0\n\0Z\0", 14);
    const auto littlePath = fixture.write("little.txt", little);
    MaiTextFileReadOptions options;
    options.maxOutputBytes = 4;
    auto result = MaiTextFileReader::read(littlePath, options);
    CHECK(result);
    if (!result) return;
    CHECK(result.value().encoding == "utf-16le");
    CHECK(result.value().lines.size() == 1);
    if (result.value().lines.empty()) return;
    CHECK(result.value().lines[0].text == "A\u4e2d");
    options.cursor = result.value().nextCursor;
    result = MaiTextFileReader::read(littlePath, options);
    CHECK(result);
    if (!result || result.value().lines.empty()) return;
    CHECK(result.value().lines[0].text == "\U0001f642");
    CHECK(result.value().lines[0].complete);

    const auto bigPath = fixture.write("big.txt", std::string("\xfe\xff\0A\0\n\0B", 8));
    const auto big = MaiTextFileReader::read(bigPath);
    CHECK(big);
    if (big) {
        CHECK(big.value().lines.size() == 2);
        CHECK(big.value().encoding == "utf-16be");
    }
    const auto utf8Path = fixture.write("bom.txt", std::string("\xef\xbb\xbf") + "ok\n");
    const auto utf8 = MaiTextFileReader::read(utf8Path);
    CHECK(utf8);
    if (utf8 && !utf8.value().lines.empty()) CHECK(utf8.value().lines[0].text == "ok");
}

void test_legacy_encoded_character_crosses_buffer_boundary(Fixture& fixture) {
    const auto path = fixture.write("gbk.txt", std::string(65535, 'a') + "\xd6\xd0\n");
    MaiTextFileReadOptions options;
    options.encoding = "gbk";
    options.maxOutputBytes = 65536;
    const auto first = MaiTextFileReader::read(path, options);
    CHECK(first);
    if (!first) return;
    CHECK(first.value().nextCursor);
    CHECK(first.value().lines.size() == 1);
    if (!first.value().nextCursor) return;
    options.cursor = first.value().nextCursor;
    const auto second = MaiTextFileReader::read(path, options);
    CHECK(second);
    if (second && !second.value().lines.empty()) {
        CHECK(second.value().lines[0].text == "\u4e2d");
        CHECK(second.value().lines[0].complete);
        CHECK(second.value().eof);
    }
}

void test_host_decoder_receives_bounded_complete_character_batches(Fixture& fixture) {
    const auto path = fixture.write("host.txt", "\xd6\xd0\xd6\xd0");
    MaiTextFileReadOptions options;
    options.encoding = "gbk";
    options.maxOutputBytes = 4;
    int calls = 0;
    options.decodeText = [&](const std::string& bytes,
                             const std::string& encoding) -> MaiResult<std::string> {
        ++calls;
        CHECK(bytes == "\xd6\xd0\xd6\xd0");
        CHECK(encoding == "gbk");
        return std::string("\u4e2d\u4e2d");
    };
    const auto result = MaiTextFileReader::read(path, options);
    CHECK(result);
    CHECK(calls == 1);
    if (result && !result.value().lines.empty()) CHECK(result.value().lines[0].text == "\u4e2d");
}

void test_large_legacy_decode_uses_chunk_count_callbacks(Fixture& fixture) {
    constexpr std::size_t characters = 200000;
    std::string source;
    source.reserve(characters * 2);
    for (std::size_t index = 0; index < characters; ++index) source += "\xd6\xd0";
    const auto path = fixture.write("batch-legacy.txt", source);
    MaiTextFileReadOptions options;
    options.encoding = "gbk";
    options.maxOutputBytes = 1024 * 1024;
    std::size_t calls = 0;
    std::size_t decodedSourceBytes = 0;
    options.decodeText = [&](const std::string& bytes,
                             const std::string&) -> MaiResult<std::string> {
        ++calls;
        decodedSourceBytes += bytes.size();
        CHECK(!bytes.empty() && bytes.size() <= 64 * 1024);
        CHECK(bytes.size() % 2 == 0);
        std::string text;
        text.reserve(bytes.size() / 2 * 3);
        for (std::size_t index = 0; index < bytes.size(); index += 2) {
            CHECK(bytes.compare(index, 2, "\xd6\xd0") == 0);
            text += "\u4e2d";
        }
        return text;
    };
    const auto result = MaiTextFileReader::read(path, options);
    CHECK(result);
    CHECK(calls > 1 && calls <= 8);
    CHECK(decodedSourceBytes == source.size());
    if (result && !result.value().lines.empty()) {
        CHECK(result.value().eof);
        CHECK(result.value().lines.front().text.size() == characters * 3);
    }
    calls = 0;
    const auto ascii =
        MaiTextFileReader::read(fixture.write("legacy-ascii.txt", "plain ASCII"), options);
    CHECK(ascii);
    CHECK(calls == 0);

    options.decodeText = [](const std::string&, const std::string&) -> MaiResult<std::string> {
        return std::string("?");
    };
    const auto badMapping = MaiTextFileReader::read(path, options);
    CHECK(!badMapping);
    if (!badMapping) CHECK(badMapping.error().code() == MaiErrorCode::InvalidInput);
}

void test_snapshot_change_and_cancellation_during_scan(Fixture& fixture) {
    const auto path = fixture.write("changing.txt", "\xd6\xd0\n");
    MaiTextFileReadOptions options;
    options.encoding = "gbk";
    options.decodeText = [&](const std::string&, const std::string&) -> MaiResult<std::string> {
        CHECK(!MaiFileSystem::writeFile(path, "modified while decoding\n"));
        return std::string("\u4e2d\n");
    };
    const auto changed = MaiTextFileReader::read(path, options);
    CHECK(!changed);
    if (!changed) CHECK(changed.error().code() == MaiErrorCode::InvalidInput);

    const auto scanPath =
        fixture.write("cancel-scan.txt", std::string(8 * 1024 * 1024, 'x') + "\ntail");
    options = {};
    options.offset = 2;
    int checks = 0;
    options.isCanceled = [&] { return ++checks > 100; };
    const auto canceled = MaiTextFileReader::read(scanPath, options);
    CHECK(!canceled);
    if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
}

void test_exact_output_boundary_and_legacy_four_byte_character(Fixture& fixture) {
    const auto path = fixture.write("exact.txt", "abcd\r\n");
    MaiTextFileReadOptions options;
    options.maxOutputBytes = 4;
    const auto exact = MaiTextFileReader::read(path, options);
    CHECK(exact);
    if (exact) {
        CHECK(exact.value().eof);
        CHECK(exact.value().lines.size() == 1);
        if (!exact.value().lines.empty()) {
            CHECK(exact.value().lines.front().text == "abcd");
            CHECK(exact.value().lines.front().complete);
        }
    }
    // GB18030 的四字节序列跨缓冲区边界，转换后仍须作为一个 UTF-8 字符交付。
    const auto legacyPath =
        fixture.write("four-byte.txt", std::string(65535, 'a') + "\x90\x30\x81\x30\n");
    options.encoding = "gb18030";
    options.maxOutputBytes = 65536;
    const auto first = MaiTextFileReader::read(legacyPath, options);
    CHECK(first);
    if (!first || !first.value().nextCursor) return;
    options.cursor = first.value().nextCursor;
    const auto second = MaiTextFileReader::read(legacyPath, options);
    CHECK(second);
    if (second && !second.value().lines.empty()) {
        CHECK(second.value().lines[0].text == "\U00010000");
        CHECK(second.value().eof);
    }
}

void test_gb18030_native_backend_preserves_non_bmp_and_rejects_invalid_bytes(Fixture& fixture) {
    MaiTextFileReadOptions options;
    options.encoding = "gb18030";
    struct DecodeCase {
        const char* name;
        const char* bytes;
        const char* expected;
    };
    const DecodeCase valid[] = {
        {"gb18030-first-non-bmp.txt", "\x90\x30\x81\x30", "\U00010000"},
        {"gb18030-first-bmp.txt", "\x81\x30\x81\x30", "\u0080"},
        {"gb18030-last-scalar.txt", "\xe3\x32\x9a\x35", "\U0010ffff"},
        {"gb18030-mixed.txt", "\xd6\xd0\x90\x30\x81\x30\n", "\u4e2d\U00010000"},
    };
    for (const auto& item : valid) {
        const auto result = MaiTextFileReader::read(fixture.write(item.name, item.bytes), options);
        CHECK(result);
        if (!result) {
            std::printf("GB18030 %s: %s\n", item.name, result.error().message().c_str());
            continue;
        }
        CHECK(result.value().eof);
        CHECK(result.value().lines.size() == 1);
        if (!result.value().lines.empty())
            CHECK(result.value().lines.front().text == item.expected);
    }
    const char* invalid[] = {"\x90\x30\x81", "\x90\x30\x81\x2f", "\xff", "\xe3\x32\x9a\x36",
                             "\x84\x31\xa5\x30"};
    int index = 0;
    for (const auto* bytes : invalid) {
        const auto result = MaiTextFileReader::read(
            fixture.write("gb18030-invalid-" + std::to_string(index++) + ".txt", bytes), options);
        CHECK(!result);
        if (result)
            std::printf(
                "accepted invalid GB18030 file ending with %02x\n",
                static_cast<unsigned char>(bytes[std::char_traits<char>::length(bytes) - 1]));
        if (!result) CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    }
}

void test_public_legacy_decoder_converts_complete_text_and_preserves_host_contract() {
    const auto whole =
        MaiTextFileReader::decodeLegacyText("\xd6\xd0\r\n\x90\x30\x81\x30", "GB18030");
    CHECK(whole);
    if (whole) CHECK(whole.value() == "\u4e2d\r\n\U00010000");
    const auto automatic = MaiTextFileReader::decodeLegacyText("plain\n", "auto");
    CHECK(automatic);
    if (automatic) CHECK(automatic.value() == "plain\n");
    const auto cp936 = MaiTextFileReader::decodeLegacyText("\xd6\xd0", "CP936");
    CHECK(cp936);
    if (cp936) CHECK(cp936.value() == "\u4e2d");
    const auto latin = MaiTextFileReader::decodeLegacyText("\xe9", "ISO_8859_1");
    CHECK(latin);
    if (latin) CHECK(latin.value() == "\u00e9");
    const auto windows1252 = MaiTextFileReader::decodeLegacyText("\x80", "windows-1252");
    CHECK(windows1252);
    if (windows1252) CHECK(windows1252.value() == "\u20ac");
    const auto shiftJis = MaiTextFileReader::decodeLegacyText("\x82\xa0", "shift_jis");
    CHECK(shiftJis);
    if (shiftJis) CHECK(shiftJis.value() == "\u3042");
    const char* invalid[] = {"\x90\x30\x81", "\x90\x30\x81\x2f", "\xff", "\xe3\x32\x9a\x36",
                             "\x84\x31\xa5\x30"};
    for (const auto* bytes : invalid) {
        const auto rejected = MaiTextFileReader::decodeLegacyText(bytes, "gb18030");
        CHECK(!rejected);
        if (rejected)
            std::printf(
                "accepted invalid GB18030 text ending with %02x\n",
                static_cast<unsigned char>(bytes[std::char_traits<char>::length(bytes) - 1]));
        if (!rejected) CHECK(rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    const auto host = MaiTextFileReader::decodeLegacyText(
        "\xff", "CP936",
        [](const std::string& bytes, const std::string& encoding) -> MaiResult<std::string> {
            CHECK(bytes == "\xff");
            CHECK(encoding == "cp936");
            return std::string("host result");
        });
    CHECK(host);
    if (host) CHECK(host.value() == "host result");
    const auto custom = MaiTextFileReader::decodeLegacyText(
        "custom", "HOST_PAGE",
        [](const std::string&, const std::string& encoding) -> MaiResult<std::string> {
            CHECK(encoding == "host-page");
            return std::string("custom result");
        });
    CHECK(custom);
    if (custom) CHECK(custom.value() == "custom result");
    const auto damaged = MaiTextFileReader::decodeLegacyText(
        "input", "auto", [](const std::string&, const std::string&) -> MaiResult<std::string> {
            return std::string("\xf0\x90");
        });
    CHECK(!damaged);
    if (!damaged) CHECK(damaged.error().code() == MaiErrorCode::InvalidInput);
}

void test_stale_cursor_rejects_mutation_and_other_path(Fixture& fixture) {
    const auto path = fixture.write("stale.txt", "abcdef\n");
    MaiTextFileReadOptions options;
    options.maxOutputBytes = 4;
    const auto first = MaiTextFileReader::read(path, options);
    CHECK(first);
    if (!first) return;
    options.cursor = first.value().nextCursor;
    CHECK(options.cursor);
    const auto other = fixture.write("other.txt", "abcdef\n");
    const auto wrongPath = MaiTextFileReader::read(other, options);
    CHECK(!wrongPath);
    CHECK(!MaiFileSystem::writeFile(path, "changed content\n"));
    const auto stale = MaiTextFileReader::read(path, options);
    CHECK(!stale);
    if (!stale) CHECK(stale.error().code() == MaiErrorCode::InvalidInput);
}

void test_walk_reports_failures_and_accepts_normal_empty_directory(Fixture& fixture) {
    const auto seed = fixture.write("walk-seed.txt", "seed");
    const auto empty = seed.dirName().append(MaiFilePath::fromUtf8("walk-empty"));
    CHECK(!MaiFileSystem::createDirectories(empty));
    int errors = 0;
    int entries = 0;
    const auto visit = [&](const MaiFileEntry&) {
        ++entries;
        return MaiWalkAction::Continue;
    };
    const auto error = [&](const MaiFilePath&, const MaiError& failure) {
        CHECK(failure.hasError());
        ++errors;
    };
    MaiFileSystem::walk(empty, visit, error);
    CHECK(entries == 0 && errors == 0);
    MaiFileSystem::walk(empty.append(MaiFilePath::fromUtf8("missing")), visit, error);
    CHECK(errors == 1);
    MaiFileSystem::walk(seed, visit, error);
    CHECK(errors == 2);

    const auto root = seed.dirName().append(MaiFilePath::fromUtf8("walk-removed"));
    CHECK(!MaiFileSystem::createDirectories(root.append(MaiFilePath::fromUtf8("first"))));
    CHECK(!MaiFileSystem::createDirectories(root.append(MaiFilePath::fromUtf8("second"))));
    MaiFilePath queued;
    int visited = 0;
    int descendantErrors = 0;
    MaiFileSystem::walk(
        root,
        [&](const MaiFileEntry& entry) {
            ++visited;
            if (queued.isEmpty())
                queued = entry.path;
            else
                MaiFileSystem::removeRecursively(queued);
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath& failedPath, const MaiError& failure) {
            CHECK(failedPath == queued);
            CHECK(failure.hasError());
            ++descendantErrors;
        });
    CHECK(visited == 2);
    CHECK(descendantErrors == 1);
}

void test_validation_cancellation_invalid_encoding_and_eof(Fixture& fixture) {
    const auto path = fixture.write("empty.txt", "");
    const auto empty = MaiTextFileReader::read(path);
    CHECK(empty);
    if (empty) {
        CHECK(empty.value().eof);
        CHECK(empty.value().lines.empty());
        CHECK(!empty.value().nextCursor);
    }
    MaiTextFileReadOptions options;
    options.offset = 0;
    CHECK(!MaiTextFileReader::read(path, options));
    options.offset = 1;
    options.isCanceled = [] { return true; };
    const auto canceled = MaiTextFileReader::read(path, options);
    CHECK(!canceled);
    if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
    options.isCanceled = {};
    options.encoding = "utf-8";
    const auto invalid = fixture.write("invalid.txt", "\xf0\x9f");
    CHECK(!MaiTextFileReader::read(invalid, options));
    options.encoding = "utf-16le";
    CHECK(!MaiTextFileReader::read(fixture.write("odd.txt", "A"), options));
    options.encoding = "utf-8";
    CHECK(!MaiTextFileReader::read(fixture.write("binary.txt", std::string("a\0b", 3)), options));
    options.encoding = "iso-2022-jp";
    const auto stateful = MaiTextFileReader::read(path, options);
    CHECK(!stateful);
    if (!stateful) CHECK(stateful.error().code() == MaiErrorCode::NotSupported);
    options.encoding = "auto";
    options.offset = 100;
    const auto beyond = MaiTextFileReader::read(fixture.write("short.txt", "a\n"), options);
    CHECK(beyond);
    if (beyond) CHECK(beyond.value().eof && beyond.value().lines.empty());
}

void test_one_page_reuses_one_handle_and_reads_real_eof(Fixture& fixture) {
    const auto path = fixture.write("page-io.txt", "first\n" + std::string(16 * 1024 * 1024, 'x'));
    MaiTextFileReadOptions options;
    options.limit = 1;
    const auto first = MaiTextFileReader::read(path, options);
    CHECK(first);
    if (first) {
        CHECK(first.value().io.openOperations == 1);
        CHECK(first.value().io.readOperations == 1);
        CHECK(first.value().io.bytesRead == 64 * 1024);
        CHECK(!first.value().eof);
    }
    options = {};
    options.offset = 2;
    const auto largePrefix = fixture.write(
        "large-prefix-io.txt", std::string(16 * 1024 * 1024, 'x') + "\nexpected tail\n");
    const auto started = std::chrono::steady_clock::now();
    const auto tail = MaiTextFileReader::read(largePrefix, options);
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    CHECK(tail);
    if (tail) {
        CHECK(tail.value().io.openOperations == 1);
        CHECK(tail.value().lines.size() == 1);
        CHECK(tail.value().lines.front().number == 2);
        CHECK(tail.value().lines.front().text == "expected tail");
        std::printf(
            "reader-perf initial-16MiB-line-prefix ms=%.3f opens=%llu reads=%llu bytes=%llu\n",
            elapsed, static_cast<unsigned long long>(tail.value().io.openOperations),
            static_cast<unsigned long long>(tail.value().io.readOperations),
            static_cast<unsigned long long>(tail.value().io.bytesRead));
    }
    const auto exact =
        MaiTextFileReader::read(fixture.write("exact-block.txt", std::string(65536, 'a')));
    CHECK(exact);
    if (exact) {
        CHECK(exact.value().eof);
        CHECK(exact.value().io.openOperations == 1);
        CHECK(exact.value().io.readOperations == 2);
        CHECK(exact.value().io.bytesRead == 65536);
    }
#if defined(__linux__) && !defined(__ANDROID__)
    // procfs 的普通文件 st_size=0；真实读取必须返回内容，不能由元数据误判 EOF。
    MaiRegularFileInfo virtualInfo;
    const auto virtualPath = MaiFilePath::fromUtf8("/proc/version");
    if (MaiFileSystem::regularFileInfo(virtualPath, virtualInfo)) {
        CHECK(virtualInfo.bytes == 0);
        const auto virtualPage = MaiTextFileReader::read(virtualPath);
        CHECK(virtualPage);
        if (virtualPage) {
            CHECK(!virtualPage.value().lines.empty());
            CHECK(!virtualPage.value().lines.front().text.empty());
            CHECK(virtualPage.value().eof);
            CHECK(virtualPage.value().io.bytesRead > 0);
        }
        MaiTextFileReadOptions smallVirtualPage;
        smallVirtualPage.maxOutputBytes = 4;
        const auto virtualContinuation = MaiTextFileReader::read(virtualPath, smallVirtualPage);
        CHECK(!virtualContinuation);
        if (!virtualContinuation)
            CHECK(virtualContinuation.error().code() == MaiErrorCode::NotSupported);
    }
#endif
}

void test_read_deadline_returns_cursor_after_block_decoder(Fixture& fixture) {
    const auto path = fixture.write("deadline.txt", "\xd6\xd0\xd6\xd0");
    MaiTextFileReadOptions options;
    options.encoding = "gbk";
    options.maxDuration = std::chrono::milliseconds(2);
    options.decodeText = [](const std::string&, const std::string&) -> MaiResult<std::string> {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return std::string("\u4e2d\u4e2d");
    };
    const auto result = MaiTextFileReader::read(path, options);
    CHECK(result);
    if (result) {
        CHECK(!result.value().eof);
        CHECK(result.value().stopReason == MaiTextFileStopReason::TimeLimit);
        CHECK(result.value().nextCursor);
        CHECK(result.value().io.openOperations == 1);
    }
}

}  // namespace

int main() {
    Fixture fixture;
    test_reads_beyond_old_eight_megabyte_prefix(fixture);
    test_single_line_continues_beyond_eight_megabytes(fixture);
    test_unicode_long_line_continues_without_loss(fixture);
    test_scan_budget_resumes_seek(fixture);
    test_crlf_and_empty_lines(fixture);
    test_boms_and_utf16_surrogate_boundaries(fixture);
    test_legacy_encoded_character_crosses_buffer_boundary(fixture);
    test_host_decoder_receives_bounded_complete_character_batches(fixture);
    test_large_legacy_decode_uses_chunk_count_callbacks(fixture);
    test_snapshot_change_and_cancellation_during_scan(fixture);
    test_exact_output_boundary_and_legacy_four_byte_character(fixture);
    test_gb18030_native_backend_preserves_non_bmp_and_rejects_invalid_bytes(fixture);
    test_public_legacy_decoder_converts_complete_text_and_preserves_host_contract();
    test_stale_cursor_rejects_mutation_and_other_path(fixture);
    test_walk_reports_failures_and_accepts_normal_empty_directory(fixture);
    test_validation_cancellation_invalid_encoding_and_eof(fixture);
    test_one_page_reuses_one_handle_and_reads_real_eof(fixture);
    test_read_deadline_returns_cursor_after_block_decoder(fixture);
    std::printf("MaiTextFileReaderTests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

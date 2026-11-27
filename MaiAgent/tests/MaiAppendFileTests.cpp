// 文件作用：以实际文件验证追加、不变前缀、写锁和跨进程串行化。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "MaiAppendFile.h"
#include "MaiFileSystem.h"
#include "MaiThread.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

bool appendRecords(const MaiFilePath& path, char marker, int count) {
    auto opened = MaiAppendFile::open(path);
    if (!opened) return false;
    for (int index = 0; index < count; ++index) {
        if (opened.value()->lockWriter()) return false;
        auto before = opened.value()->size();
        auto added = opened.value()->append(std::string(128, marker) + "\n");
        bool valid = before && added && before.value() == added.value();
        opened.value()->unlockWriter();
        if (!valid) return false;
    }
    return true;
}

MaiFilePath childReadyPath(const MaiFilePath& path) {
    return path.dirName().append(MaiFilePath::fromUtf8("child-ready"));
}

bool runChild(const MaiFilePath& path) {
    if (MaiFileSystem::writeFile(childReadyPath(path), "ready")) return false;
    return appendRecords(path, 'c', 60);
}

void checkChildBlocked(const MaiFilePath& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!MaiFileSystem::exists(childReadyPath(path)) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(MaiFileSystem::exists(childReadyPath(path)));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::uint64_t length = 1;
    CHECK(MaiFileSystem::fileSize(path, length));
    CHECK(length == 0);
}

void testAppendAndRead(const MaiFilePath& root) {
    std::printf("-> append, offsets, immutable prefix, reopen, flush, lock contract\n");
    const auto path = root.append(MaiFilePath::fromUtf8("prefix-\u65e5\u5fd7.jsonl"));
    const std::string prefix = "{\"first\":1}\n";
    CHECK(!MaiFileSystem::writeFile(path, prefix));
    auto opened = MaiAppendFile::open(path);
    CHECK(opened.isOk());
    if (!opened) return;
    auto& file = *opened.value();
    std::string contents = "stale";
    CHECK(!file.size());
    CHECK(!file.append("unlocked"));
    CHECK(file.flush().code() == MaiErrorCode::InvalidInput);
    CHECK(file.readAt(0, 1, contents).code() == MaiErrorCode::InvalidInput);
    CHECK(contents.empty());
    CHECK(!file.lockWriter());
    CHECK(file.lockWriter().code() == MaiErrorCode::Busy);
    auto appended = file.append("{\"next\":2}\n");
    CHECK(appended && appended.value() == prefix.size());
    auto empty = file.append("");
    CHECK(empty && empty.value() == prefix.size() + 11);
    CHECK(!file.flush());
    CHECK(!file.readAt(2, 5, contents));
    CHECK(contents == prefix.substr(2, 5));
    CHECK(!file.readAt(999, 1, contents));
    CHECK(contents.empty());
    CHECK(file.readAt(0, 0, contents).code() == MaiErrorCode::InvalidInput);
    CHECK(file.readAt(0, 1024 * 1024 + 1, contents).code() == MaiErrorCode::InvalidInput);
    CHECK(file.readAt(std::numeric_limits<std::uint64_t>::max(), 1, contents).code() ==
          MaiErrorCode::InvalidInput);

    // 独立读句柄无需取得写锁；保留锁字节不能阻止读取旧前缀。
    auto reader = MaiFileReadHandle::open(path);
    CHECK(reader.isOk());
    if (reader) {
        CHECK(!reader.value()->readAt(0, prefix.size(), contents));
        CHECK(contents == prefix);
    }
    file.unlockWriter();
    file.unlockWriter();
    opened.value().reset();
    auto reopened = MaiAppendFile::open(path);
    CHECK(reopened.isOk());
    if (reopened) {
        CHECK(!reopened.value()->lockWriter());
        CHECK(!reopened.value()->readAt(0, 100, contents));
        CHECK(contents == prefix + "{\"next\":2}\n");
        // 释放时仍持锁，析构必须解锁。
    }
}

void testBoundedChunks(const MaiFilePath& root) {
    std::printf("-> bounded chunks over one MiB\n");
    auto file = MaiAppendFile::open(root.append(MaiFilePath::fromUtf8("chunks.jsonl")));
    CHECK(file.isOk());
    if (!file) return;
    CHECK(!file.value()->lockWriter());
    const std::string bytes(1024 * 1024 + 19, 'x');
    auto added = file.value()->append(bytes);
    CHECK(added && added.value() == 0);
    std::string contents;
    CHECK(!file.value()->readAt(0, 1024 * 1024, contents));
    CHECK(contents == bytes.substr(0, 1024 * 1024));
    CHECK(!file.value()->readAt(1024 * 1024, 100, contents));
    CHECK(contents == std::string(19, 'x'));
    CHECK(!file.value()->flush());
}

void testOpenBoundaries(const MaiFilePath& root) {
    std::printf("-> missing parent, directory, final symlink, native permissions\n");
    CHECK(!MaiAppendFile::open(MaiFilePath()));
#if defined(_WIN32)
    CHECK(!MaiAppendFile::open(MaiFilePath(std::wstring(L"bad\0suffix", 10))));
#else
    CHECK(!MaiAppendFile::open(MaiFilePath(std::string("bad\0suffix", 10))));
#endif
    CHECK(!MaiAppendFile::open(root));
    CHECK(!MaiAppendFile::open(root.append(MaiFilePath::fromUtf8("missing/file.jsonl"))));
    const auto target = root.append(MaiFilePath::fromUtf8("target.jsonl"));
    const auto link = root.append(MaiFilePath::fromUtf8("link.jsonl"));
    CHECK(!MaiFileSystem::writeFile(target, "original"));
#if defined(_WIN32)
    if (::CreateSymbolicLinkW(link.value().c_str(), target.value().c_str(), 0x2)) {
        CHECK(!MaiAppendFile::open(link));
    } else {
        std::printf("SKIP final symlink creation: Win32 %lu\n", ::GetLastError());
    }
#else
    CHECK(::symlink(target.value().c_str(), link.value().c_str()) == 0);
    CHECK(!MaiAppendFile::open(link));
    const auto privatePath = root.append(MaiFilePath::fromUtf8("private.jsonl"));
    auto privateFile = MaiAppendFile::open(privatePath);
    CHECK(privateFile.isOk());
    struct stat information{};
    CHECK(::stat(privatePath.value().c_str(), &information) == 0);
    CHECK((information.st_mode & 0777) == 0600);
#endif
    std::string contents;
    CHECK(!MaiFileSystem::readFile(target, contents));
    CHECK(contents == "original");
}

void testHandlesAndThreads(const MaiFilePath& root) {
    std::printf("-> separate handles serialize and unrelated read-close preserves lock\n");
    const auto path = root.append(MaiFilePath::fromUtf8("threads.jsonl"));
    auto first = MaiAppendFile::open(path);
    auto second = MaiAppendFile::open(path);
    CHECK(first && second);
    if (!first || !second) return;
    CHECK(!first.value()->lockWriter());
    std::atomic<bool> started{false};
    std::atomic<bool> acquired{false};
    std::atomic<bool> succeeded{false};
    std::thread contender([&] {
        MaiThread::setCurrentName("append-test");
        started = true;
        if (second.value()->lockWriter()) return;
        acquired = true;
        succeeded = static_cast<bool>(second.value()->append("second\n"));
        second.value()->unlockWriter();
    });
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(!acquired);
    {
        auto reader = MaiFileReadHandle::open(path);
        CHECK(reader.isOk());
        auto unrelatedWriter = MaiAppendFile::open(path);
        CHECK(unrelatedWriter.isOk());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(!acquired);
    CHECK(first.value()->append("first\n").isOk());
    first.value()->unlockWriter();
    contender.join();
    CHECK(acquired && succeeded);
    first.value().reset();
    second.value().reset();

    std::atomic<int> failedThreads{0};
    std::vector<std::thread> writers;
    for (int index = 0; index < 4; ++index) {
        writers.emplace_back([&, index] {
            MaiThread::setCurrentName("append-test");
            if (!appendRecords(path, static_cast<char>('a' + index), 40)) ++failedThreads;
        });
    }
    for (auto& writer : writers) writer.join();
    CHECK(failedThreads == 0);
    std::string bytes;
    CHECK(!MaiFileSystem::readFile(path, bytes));
    CHECK(bytes.substr(0, 13) == "first\nsecond\n");
    CHECK(bytes.size() == 13 + 4 * 40 * 129);
    for (std::size_t offset = 13; offset + 129 <= bytes.size(); offset += 129) {
        CHECK(bytes[offset + 128] == '\n');
        CHECK(bytes.substr(offset, 128) == std::string(128, bytes[offset]));
    }
}

void testProcesses(const MaiFilePath& root) {
    std::printf("-> real processes serialize records\n");
    const auto path = root.append(MaiFilePath::fromUtf8("process.jsonl"));
    const auto start = MaiAppendFile::open(path);
    CHECK(start.isOk());
    if (!start) return;
    // 父进程先持锁，让子进程必须穿过实际进程锁竞争，再与父进程同时追加。
    CHECK(!start.value()->lockWriter());
#if defined(_WIN32)
    wchar_t executable[32768]{};
    CHECK(::GetModuleFileNameW(nullptr, executable, 32768) != 0);
    std::wstring command =
        L"\"" + std::wstring(executable) + L"\" --child \"" + path.value() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool launched = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CHECK(launched);
    if (launched) checkChildBlocked(path);
    start.value()->unlockWriter();
    CHECK(appendRecords(path, 'p', 60));
    if (launched) {
        CHECK(::WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
        DWORD code = 1;
        CHECK(::GetExitCodeProcess(process.hProcess, &code));
        CHECK(code == 0);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    }
#else
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        // fork 后不复用父句柄，子进程只新开独立的文件描述。
        _exit(runChild(path) ? 0 : 1);
    }
    if (child > 0) checkChildBlocked(path);
    start.value()->unlockWriter();
    CHECK(appendRecords(path, 'p', 60));
    int status = 1;
    if (child > 0) CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    std::string bytes;
    CHECK(!MaiFileSystem::readFile(path, bytes));
    CHECK(bytes.size() == 120 * 129);
    std::size_t parentCount = 0;
    std::size_t childCount = 0;
    for (std::size_t offset = 0; offset + 129 <= bytes.size(); offset += 129) {
        CHECK(bytes[offset + 128] == '\n');
        CHECK(bytes.substr(offset, 128) == std::string(128, bytes[offset]));
        if (bytes[offset] == 'p') ++parentCount;
        if (bytes[offset] == 'c') ++childCount;
    }
    CHECK(parentCount == 60 && childCount == 60);
}

void testLongSourcePaths(const MaiFilePath& root) {
#if defined(_WIN32)
    const auto unc =
        MaiFilePath::fromUtf8("\\\\owned-server\\owned-share\\" + std::string(260, 'a'));
    CHECK(MaiFileSystem::windowsApiPath(unc).compare(0, 8, L"\\\\?\\UNC\\") == 0);
    auto directory = root;
    while (directory.toUtf8().size() < 290)
        directory = directory.append(MaiFilePath::fromUtf8("owned-long-rollout-component"));
    const auto created = MaiFileSystem::createDirectories(directory);
    CHECK(!created);
    if (created) return;
    const auto path = directory.append(MaiFilePath::fromUtf8("owned-source.jsonl"));
    {
        auto opened = MaiAppendFile::open(path);
        CHECK(opened);
        if (!opened) return;
        CHECK(!opened.value()->lockWriter());
        CHECK(opened.value()->append("owned-long-source\n"));
        CHECK(!opened.value()->flush());
    }
    std::string bytes;
    CHECK(!MaiFileSystem::readFile(path, bytes));
    CHECK(bytes == "owned-long-source\n");
    std::uint64_t size = 0;
    CHECK(MaiFileSystem::fileSize(path, size));
    CHECK(size == bytes.size());
#else
    (void)root;
#endif
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--child")
        return runChild(MaiFilePath(argv[2])) ? 0 : 1;
    const auto processId = ::GetCurrentProcessId();
#else
int main() {
    const auto processId = ::getpid();
#endif
    const auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-append-" + std::to_string(processId)));
    CHECK(!MaiFileSystem::createDirectories(root));
    testAppendAndRead(root);
    testBoundedChunks(root);
    testOpenBoundaries(root);
    testHandlesAndThreads(root);
    testProcesses(root);
    testLongSourcePaths(root);
    MaiFileSystem::removeRecursively(root);
    std::printf("MaiAppendFileTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

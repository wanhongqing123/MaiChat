// 文件作用：验证技能暂存目录的无覆盖原子发布与普通文件执行权限边界。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "MaiFileSystem.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {
MaiFilePath child(const MaiFilePath& root, const std::string& name) {
    return root.append(MaiFilePath::fromUtf8(name));
}

void testPublish(const MaiFilePath& root) {
    const auto source = child(root, "staging");
    const auto destination = child(root, "installed");
    CHECK(!MaiFileSystem::createDirectories(child(source, "scripts")));
    CHECK(!MaiFileSystem::writeFile(child(source, "scripts/run.sh"), "echo installed\n"));
    CHECK(!MaiFileSystem::publishNewDirectory(source, destination));
    CHECK(!MaiFileSystem::exists(source));
    std::string content;
    CHECK(!MaiFileSystem::readFile(child(destination, "scripts/run.sh"), content));
    CHECK(content == "echo installed\n");

    CHECK(!MaiFileSystem::createDirectories(source));
    CHECK(MaiFileSystem::publishNewDirectory(source, destination));
    CHECK(MaiFileSystem::isDirectory(source));
    CHECK(!MaiFileSystem::readFile(child(destination, "scripts/run.sh"), content));
    CHECK(content == "echo installed\n");

    const auto empty = child(root, "existing-empty");
    CHECK(!MaiFileSystem::createDirectories(empty));
    const auto emptyBefore = MaiFileSystem::stat(empty);
    CHECK(emptyBefore);
    CHECK(MaiFileSystem::publishNewDirectory(source, empty));
    CHECK(MaiFileSystem::isDirectory(source));
    CHECK(MaiFileSystem::isDirectory(empty));
    const auto emptyAfter = MaiFileSystem::stat(empty);
    CHECK(emptyAfter);
    CHECK(emptyBefore && emptyAfter && emptyBefore.value().identity == emptyAfter.value().identity);
    CHECK(MaiFileSystem::publishNewDirectory(source, child(root, "missing-parent/installed")));
    CHECK(MaiFileSystem::isDirectory(source));
    const auto ordinary = child(root, "ordinary");
    CHECK(!MaiFileSystem::writeFile(ordinary, "keep"));
    CHECK(MaiFileSystem::publishNewDirectory(source, ordinary));
    CHECK(MaiFileSystem::publishNewDirectory(ordinary, child(root, "invalid-file-source")));
    CHECK(!MaiFileSystem::readFile(ordinary, content));
    CHECK(content == "keep");
    CHECK(MaiFileSystem::publishNewDirectory({}, child(root, "invalid-empty-source")));
    CHECK(MaiFileSystem::publishNewDirectory(source, {}));
    CHECK(MaiFileSystem::publishNewDirectory(source, source));
    const auto emptySource = child(root, "empty-source");
    const auto emptyDestination = child(root, "empty-published");
    CHECK(!MaiFileSystem::createDirectories(emptySource));
    CHECK(!MaiFileSystem::publishNewDirectory(emptySource, emptyDestination));
    CHECK(!MaiFileSystem::exists(emptySource));
    CHECK(MaiFileSystem::isDirectory(emptyDestination));
}

void testExclusiveCreate(const MaiFilePath& root) {
    const auto target = child(root, "exclusive-created");
    std::atomic<bool> ready{false};
    MaiError firstError;
    std::thread creator([&] {
        ready.store(true, std::memory_order_release);
        firstError = MaiFileSystem::createNewDirectory(target);
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto secondError = MaiFileSystem::createNewDirectory(target);
    creator.join();
    CHECK(firstError.hasError() != secondError.hasError());
    CHECK(MaiFileSystem::isDirectory(target));
    const auto before = MaiFileSystem::stat(target);
    CHECK(MaiFileSystem::createNewDirectory(target));
    const auto after = MaiFileSystem::stat(target);
    CHECK(before && after && before.value().identity == after.value().identity);
    CHECK(MaiFileSystem::createNewDirectory({}));
    CHECK(MaiFileSystem::createNewDirectory(child(root, "missing-parent/exclusive")));
    CHECK(!MaiFileSystem::exists(child(root, "missing-parent")));
    const auto file = child(root, "exclusive-file");
    CHECK(!MaiFileSystem::writeFile(file, "keep"));
    CHECK(MaiFileSystem::createNewDirectory(file));
    std::string contents;
    CHECK(!MaiFileSystem::readFile(file, contents));
    CHECK(contents == "keep");
}

void testRace(const MaiFilePath& root) {
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto first = child(root, "race-a-" + std::to_string(iteration));
        const auto second = child(root, "race-b-" + std::to_string(iteration));
        const auto target = child(root, "race-target-" + std::to_string(iteration));
        CHECK(!MaiFileSystem::createDirectories(first));
        CHECK(!MaiFileSystem::createDirectories(second));
        // 空目录竞争同样不能先检查再普通 rename，否则两个发布者可能都报告成功。
        const bool withContents = iteration % 2 == 0;
        if (withContents) {
            CHECK(!MaiFileSystem::writeFile(child(first, "winner"), "first"));
            CHECK(!MaiFileSystem::writeFile(child(second, "winner"), "second"));
        }
        std::atomic<bool> ready{false};
        MaiError firstError;
        std::thread publisher([&] {
            ready.store(true, std::memory_order_release);
            firstError = MaiFileSystem::publishNewDirectory(first, target);
        });
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto secondError = MaiFileSystem::publishNewDirectory(second, target);
        publisher.join();
        CHECK(firstError.hasError() != secondError.hasError());
        CHECK(MaiFileSystem::exists(first) == firstError.hasError());
        CHECK(MaiFileSystem::exists(second) == secondError.hasError());
        if (withContents) {
            std::string content;
            CHECK(!MaiFileSystem::readFile(child(target, "winner"), content));
            CHECK(content == (firstError ? "second" : "first"));
        }
    }
}

void testLinksAndPermissions(const MaiFilePath& root) {
    const auto file = child(root, "executable.sh");
    CHECK(!MaiFileSystem::writeFile(file, "echo test\n"));
#if !defined(_WIN32)
    CHECK(::chmod(file.value().c_str(), 0640) == 0);
#endif
    CHECK(!MaiFileSystem::setUserExecutable(file));
    CHECK(MaiFileSystem::setUserExecutable(root));
    CHECK(MaiFileSystem::setUserExecutable({}));
    CHECK(MaiFileSystem::setUserExecutable(child(root, "missing")));
#if !defined(_WIN32)
    struct stat permissions{};
    CHECK(::lstat(file.value().c_str(), &permissions) == 0);
    CHECK((permissions.st_mode & 0777) == 0740);
#endif
    const auto alias = child(root, "directory-link");
    const auto fileAlias = child(root, "file-link");
#if defined(_WIN32)
    const bool directoryLinked = ::CreateSymbolicLinkW(MaiFileSystem::windowsApiPath(alias).c_str(),
                                                       MaiFileSystem::windowsApiPath(root).c_str(),
                                                       SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != FALSE;
    const bool fileLinked =
        ::CreateSymbolicLinkW(MaiFileSystem::windowsApiPath(fileAlias).c_str(),
                              MaiFileSystem::windowsApiPath(file).c_str(), 0x2) != FALSE;
#else
    const bool directoryLinked = ::symlink(root.value().c_str(), alias.value().c_str()) == 0;
    const bool fileLinked = ::symlink(file.value().c_str(), fileAlias.value().c_str()) == 0;
#endif
    if (directoryLinked) {
        CHECK(MaiFileSystem::createNewDirectory(alias));
        CHECK(MaiFileSystem::publishNewDirectory(alias, child(root, "link-published")));
        CHECK(MaiFileSystem::publishNewDirectory(MaiFilePath::fromUtf8(alias.toUtf8() + "/"),
                                                 child(root, "trailing-link-published")));
        CHECK(MaiFileSystem::publishNewDirectory(child(root, "staging"), alias));
        CHECK(MaiFileSystem::isSymbolicLink(alias));
    } else {
        std::printf("SKIP directory symlink: host does not permit creation\n");
    }
    if (fileLinked) {
        CHECK(MaiFileSystem::setUserExecutable(fileAlias));
        CHECK(MaiFileSystem::isSymbolicLink(fileAlias));
    } else {
        std::printf("SKIP file symlink: host does not permit creation\n");
    }
}
}  // namespace

int main() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = child(MaiFileSystem::temporaryDirectory(), "MaiDirectoryPublish-" + unique);
    CHECK(!MaiFileSystem::createDirectories(root));
    testPublish(root);
    testExclusiveCreate(root);
    testRace(root);
    testLinksAndPermissions(root);
    auto unicode = child(root, "\u6d4b\u8bd5-\u6280\u80fd");
    for (int index = 0; index < 6; ++index) unicode = child(unicode, std::string(45, 'x'));
    CHECK(!MaiFileSystem::createDirectories(unicode));
    const auto longTarget = child(unicode.dirName(), "published-\u5b8c\u6574");
    CHECK(!MaiFileSystem::publishNewDirectory(unicode, longTarget));
    CHECK(MaiFileSystem::isDirectory(longTarget));
    const auto longExecutable = child(longTarget, "\u6267\u884c.sh");
    CHECK(!MaiFileSystem::writeFile(longExecutable, "echo unicode\n"));
    CHECK(!MaiFileSystem::setUserExecutable(longExecutable));
    auto filesystemRoot = MaiFileSystem::resolve(root);
    while (filesystemRoot != filesystemRoot.dirName()) filesystemRoot = filesystemRoot.dirName();
    CHECK(MaiFileSystem::publishNewDirectory(filesystemRoot, child(root, "forbidden-root")));
    CHECK(MaiFileSystem::createNewDirectory(filesystemRoot));
    CHECK(MaiFileSystem::publishNewDirectory(child(root, "staging"), filesystemRoot));
    MaiFileSystem::removeRecursively(root);
    std::printf("MaiSkillDirectoryPublishTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

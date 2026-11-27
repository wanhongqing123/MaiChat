// 文件作用：模拟旧版 Android 缺少新版 libc 文件接口，验证不会试探原始 syscall 或误覆盖。
// 仅在 Android 以 --wrap=dlsym 构建；所有文件都属于本用例的临时目录。
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <unistd.h>

#include "MaiAndroidFileOperations.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {
int failures = 0;
int lookups = 0;
int fallbackCalls = 0;
#define CHECK(value)                                        \
    do {                                                    \
        if (!(value)) {                                     \
            ++failures;                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #value); \
        }                                                   \
    } while (false)

int denyPublication(const char*, const char*) {
    ++fallbackCalls;
    return EACCES;
}
}  // namespace

extern "C" void* __real_dlsym(void*, const char*);
extern "C" void* __wrap_dlsym(void* handle, const char* name) {
    if (name != nullptr && std::strcmp(name, "renameat2") == 0) {
        ++lookups;
        return nullptr;
    }
    return __real_dlsym(handle, name);
}

int main() {
    const auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("android-file-api-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    maiSetAndroidPublishNewFileCallback(denyPublication);
    const auto source = root.append(MaiFilePath::fromUtf8("source"));
    const auto target = root.append(MaiFilePath::fromUtf8("target"));
    CHECK(!MaiFileSystem::writeFile(source, "owned original"));
    CHECK(MaiFileSystem::publishNewFile(source, target));
    CHECK(fallbackCalls == 1);
    CHECK(MaiFileSystem::exists(source));
    CHECK(!MaiFileSystem::exists(target));
    // 不依赖上一个错误案例的文件状态，旧实现在此反例中已错误消耗源也能继续诊断。
    const auto moveSource = root.append(MaiFilePath::fromUtf8("move-source"));
    const auto moveTarget = root.append(MaiFilePath::fromUtf8("move-target"));
    CHECK(!MaiFileSystem::writeFile(moveSource, "owned move"));
    CHECK(MaiFileSystem::moveFile(moveSource, moveTarget));
    CHECK(fallbackCalls == 2);
    CHECK(MaiFileSystem::exists(moveSource));
    CHECK(!MaiFileSystem::exists(moveTarget));
    const auto relative = root.append(MaiFilePath::fromUtf8("relative-source"));
    CHECK(!MaiFileSystem::writeFile(relative, "relative original"));
    char originalDirectory[PATH_MAX]{};
    const bool changedDirectory =
        ::getcwd(originalDirectory, sizeof(originalDirectory)) != nullptr &&
        ::chdir(root.value().c_str()) == 0;
    CHECK(changedDirectory);
    if (changedDirectory) {
        const auto rejected = MaiFileSystem::moveFile(MaiFilePath::fromUtf8("relative-source"),
                                                      MaiFilePath::fromUtf8("relative-target"));
        CHECK(rejected.code() == MaiErrorCode::InvalidInput);
        CHECK(fallbackCalls == 3);
        CHECK(::chdir(originalDirectory) == 0);
    }
    CHECK(MaiFileSystem::exists(relative));
    CHECK(!MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("relative-target"))));
    maiSetAndroidPublishNewFileCallback(nullptr);
    const auto stage = root.append(MaiFilePath::fromUtf8("stage"));
    const auto installed = root.append(MaiFilePath::fromUtf8("installed"));
    CHECK(!MaiFileSystem::createDirectories(stage));
    const auto directoryResult = MaiFileSystem::publishNewDirectory(stage, installed);
    CHECK(directoryResult.code() == MaiErrorCode::NotSupported);
    CHECK(MaiFileSystem::isDirectory(stage));
    CHECK(!MaiFileSystem::exists(installed));
    const auto noHost = root.append(MaiFilePath::fromUtf8("no-host"));
    const auto noHostTarget = root.append(MaiFilePath::fromUtf8("no-host-target"));
    CHECK(!MaiFileSystem::writeFile(noHost, "keep"));
    CHECK(MaiFileSystem::moveFile(noHost, noHostTarget).code() == MaiErrorCode::NotSupported);
    CHECK(MaiFileSystem::exists(noHost));
    CHECK(!MaiFileSystem::exists(noHostTarget));
    CHECK(lookups > 0);
    MaiFileSystem::removeRecursively(root);
    std::printf("Android API availability: failures=%d lookups=%d fallback_calls=%d\n", failures,
                lookups, fallbackCalls);
    return failures ? 1 : 0;
}

#include "agent/DesktopSessionDatabaseUseCheck.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <libproc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <limits>
#include <initializer_list>
#include <vector>
#endif
#endif

MaiError checkDesktopSessionDatabaseUse(const MaiFilePath& databasePath) {
#if defined(__APPLE__) && TARGET_OS_OSX
    if (databasePath.isEmpty()) return {MaiErrorCode::InvalidInput, "empty session database path"};
    // 恢复中原名可能已换成索引；保留旧库仍可能被旧App通过原FD持有。
    for (const auto& path :
         {databasePath, MaiFilePath(databasePath.value() + ".legacy-session-backup")}) {
        struct stat info{};
        if (::lstat(path.value().c_str(), &info) != 0) {
            if (errno == ENOENT) continue;
            return {MaiErrorCode::Internal,
                    "cannot inspect session database use; close the other app and retry"};
        }
        if (!S_ISREG(info.st_mode))
            return {MaiErrorCode::InvalidInput,
                    "session database use check requires an ordinary non-link file"};
        // 内核按文件引用匹配路径，包含硬链接/已打开inode，不枚举或输出其它用户文件名。
        const int requested = ::proc_listpidspath(PROC_UID_ONLY, ::geteuid(), path.value().c_str(),
                                                  PROC_LISTPIDSPATH_EXCLUDE_EVTONLY, nullptr, 0);
        if (requested < 0)
            return {MaiErrorCode::Internal,
                    "cannot check session database use; close the other app and retry"};
        std::size_t capacity =
            std::max<std::size_t>(64, std::size_t(requested) / sizeof(pid_t) + 64);
        bool complete = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (capacity > std::size_t(std::numeric_limits<int>::max()) / sizeof(pid_t)) break;
            std::vector<pid_t> pids(capacity);
            const int bytes = int(pids.size() * sizeof(pid_t));
            const int returned =
                ::proc_listpidspath(PROC_UID_ONLY, ::geteuid(), path.value().c_str(),
                                    PROC_LISTPIDSPATH_EXCLUDE_EVTONLY, pids.data(), bytes);
            if (returned < 0 || returned > bytes || returned % int(sizeof(pid_t)) != 0) break;
            for (int index = 0; index < returned / int(sizeof(pid_t)); ++index)
                if (pids[std::size_t(index)] > 0 && pids[std::size_t(index)] != ::getpid())
                    return {MaiErrorCode::Busy,
                            "session database is used by another app; close the other MaiChat app "
                            "and retry"};
            if (returned < bytes) {
                complete = true;
                break;
            }
            capacity *= 2;
        }
        if (!complete)
            return {
                MaiErrorCode::Internal,
                "session database use changed during inspection; close the other app and retry"};
    }
#else
    (void)databasePath;
#endif
    return {};
}

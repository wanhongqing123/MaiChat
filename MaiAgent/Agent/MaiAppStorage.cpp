// 文件作用：共享应用存储枚举、不可扩大的清理预览与执行边界。
#include "MaiAppStorage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <mutex>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {
constexpr std::size_t kMaximumFiles = 20000;
constexpr std::size_t kMaximumCandidates = 1000;

bool canceled(const MaiAppStorageContext& context) {
    return context.cancel && context.cancel->load();
}

std::vector<std::string> components(const std::string& path) {
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        result.push_back(path.substr(offset, end - offset));
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return result;
}

bool validRelative(const std::string& path, bool pattern) {
    if (path.empty() || path.size() > 1024 || path.back() == '/' || path.front() == '/' ||
        path.find_first_of("\\:") != std::string::npos || path.find('\0') != std::string::npos ||
        path.find_first_of("?[]") != std::string::npos || path.find("**") != std::string::npos)
        return false;
    for (const auto& item : components(path))
        if (item.empty() || item == "." || item == ".." ||
            (!pattern && item.find('*') != std::string::npos))
            return false;
    return true;
}

bool workspaceFamily(const std::string& path) {
    const auto parts = components(path);
    if (parts.empty()) return false;
    if (parts[0].compare(0, 7, "frames_") == 0 && parts[0].size() > 7) return true;
    if (parts[0] == "wmframes" || parts[0] == "screenshots" || parts[0] == "search" ||
        parts[0] == "browser")
        return true;
    return parts.size() >= 2 && parts[0] == ".maiagent" && parts[1] == "model-previews";
}

bool workspaceIntermediateFile(const std::string& path) {
    const auto parts = components(path);
    return workspaceFamily(path) && parts.size() >= (parts[0] == ".maiagent" ? 3u : 2u);
}

// * 不跨目录，避免一个看似单文件模式扩张到未知子目录。
bool matches(const std::string& pattern, const std::string& path) {
    std::size_t p = 0, value = 0, star = std::string::npos, restart = 0;
    while (value < path.size()) {
        if (p < pattern.size() && pattern[p] == path[value]) {
            ++p;
            ++value;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            restart = value;
        } else if (star != std::string::npos && restart < path.size() && path[restart] != '/') {
            p = star + 1;
            value = ++restart;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool selected(const MaiAppStorageSelection& selection, const std::string& relative) {
    if (!selection.scope.empty()) {
        const auto parts = components(selection.scope);
        const auto fileParts = components(relative);
        if (parts.size() > fileParts.size()) return false;
        for (std::size_t i = 0; i < parts.size(); ++i)
            if (!matches(parts[i], fileParts[i])) return false;
    }
    return selection.patterns.empty() ||
           std::any_of(selection.patterns.begin(), selection.patterns.end(),
                       [&](const std::string& pattern) { return matches(pattern, relative); });
}

bool mayContainSelection(const MaiAppStorageSelection& selection, const std::string& relative) {
    const auto directory = components(relative);
    const auto compatible = [&](const std::string& pattern, bool scope) {
        const auto parts = components(pattern);
        if (!scope && directory.size() >= parts.size()) return false;
        for (std::size_t i = 0; i < std::min(parts.size(), directory.size()); ++i)
            if (!matches(parts[i], directory[i])) return false;
        return true;
    };
    if (!selection.scope.empty() && !compatible(selection.scope, true)) return false;
    return selection.patterns.empty() ||
           std::any_of(selection.patterns.begin(), selection.patterns.end(),
                       [&](const std::string& pattern) { return compatible(pattern, false); });
}

bool boundedSelection(const MaiAppStorageSelection& selection) {
    if (!selection.scope.empty()) return true;
    return !selection.patterns.empty() &&
           std::all_of(selection.patterns.begin(), selection.patterns.end(),
                       [](const std::string& pattern) {
                           const auto slash = pattern.find('/');
                           return slash != std::string::npos && slash > 0 &&
                                  pattern.substr(0, slash).find('*') == std::string::npos;
                       });
}

MaiError validate(const MaiAppStorageSelection& selection) {
    if (selection.minimumAgeHours < 0 || selection.minimumAgeHours > 24 * 365 ||
        selection.patterns.size() > 20)
        return {MaiErrorCode::InvalidInput, "min_age_hours must be 0..8760; at most 20 patterns"};
    if (selection.target == MaiAppStorageTarget::App &&
        (!selection.scope.empty() || !selection.patterns.empty() ||
         selection.minimumAgeHours != 24))
        return {MaiErrorCode::InvalidInput, "select one target before specifying cleanup scope"};
    if (!selection.scope.empty()) {
        const bool frameScope =
            selection.target == MaiAppStorageTarget::Workspace && selection.scope == "frames_*";
        if (!validRelative(selection.scope, frameScope))
            return {MaiErrorCode::InvalidInput, "scope must be a bounded relative path"};
    }
    for (const auto& pattern : selection.patterns)
        if (!validRelative(pattern, true) || pattern == "*" || pattern == "*.*")
            return {MaiErrorCode::InvalidInput, "patterns must name bounded relative files"};
    if (selection.target == MaiAppStorageTarget::Workspace) {
        if (selection.scope.empty() && selection.patterns.empty())
            return {MaiErrorCode::InvalidInput,
                    "workspace cleanup requires a regenerable intermediate scope or patterns"};
        if ((!selection.scope.empty() && !workspaceFamily(selection.scope)) ||
            (selection.scope.empty() &&
             std::any_of(selection.patterns.begin(), selection.patterns.end(),
                         [](const std::string& pattern) { return !workspaceFamily(pattern); })))
            return {
                MaiErrorCode::InvalidInput,
                "workspace scope must be frames_<id>, wmframes, screenshots, search, "
                "browser, or .maiagent/model-previews; originals and final media are protected"};
    }
    if (selection.minimumAgeHours == 0 && !boundedSelection(selection) &&
        selection.target != MaiAppStorageTarget::Workspace)
        return {MaiErrorCode::InvalidInput, "zero age requires an explicit bounded scope"};
    return {};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char item) { return static_cast<char>(std::tolower(item)); });
    return value;
}

bool protectedFile(const MaiFilePath& path, const std::string& relative,
                   const MaiAppStorageContext& context, bool bounded) {
    for (const auto& item : components(relative))
        if (lower(item).compare(0, 8, "gallery-") == 0) return true;
    for (const auto& item : context.protectedPaths) {
        const auto protectedPath = MaiFileSystem::resolve(MaiFilePath::fromUtf8(item));
        if (!protectedPath.isEmpty() && (protectedPath == path || protectedPath.isParentOf(path)))
            return true;
    }
    const auto name = lower(path.baseName().toUtf8());
    const auto dot = name.find_last_of('.');
    const auto extension = dot == std::string::npos ? std::string{} : name.substr(dot);
    // 临时目录里的权重可能仍在使用，不能因为其年龄就认定是可再生垃圾。
    return !bounded && (extension == ".gguf" || extension == ".onnx");
}

bool withinWithoutLinks(const MaiFilePath& root, const MaiFilePath& path) {
    if (!root.isParentOf(path)) return false;
    auto current = path;
    while (current != root) {
        if (MaiFileSystem::isSymbolicLink(current)) return false;
        const auto parent = current.dirName();
        if (parent == current || parent.isEmpty()) return false;
        current = parent;
    }
    const auto resolved = MaiFileSystem::resolve(path);
    return resolved == path && root.isParentOf(resolved);
}

std::string relativeTo(const MaiFilePath& root, const MaiFilePath& path) {
    const auto rootParts = root.components();
    const auto parts = path.components();
    std::string result;
    for (std::size_t index = rootParts.size(); index < parts.size(); ++index) {
        if (!result.empty()) result += '/';
        result += MaiFilePath(parts[index]).toUtf8();
    }
    return result;
}
}  // namespace

class MaiAppStorage::Implementation {
public:
    explicit Implementation(MaiAppStoragePaths paths) : paths(std::move(paths)) {}
    struct Candidate {
        MaiAppStorageCandidate value;
        MaiRegularFileInfo info;
        MaiFilePath root;
    };
    MaiAppStoragePaths paths;
    std::mutex mutex;
    std::string previewId, sessionId;
    bool needsProtection = false;
    std::int64_t createdAt = 0;
    std::array<MaiFilePath, 3> previewRoots;
    std::vector<Candidate> candidates;

    MaiResult<std::array<MaiFilePath, 3>> roots(const MaiAppStorageContext& context) const {
        const auto work = context.workspace.empty() ? paths.workspace : context.workspace;
        std::array<MaiFilePath, 3> result{
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(paths.temporary)),
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(paths.cache)),
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(work))};
        if (!paths.workspace.empty() &&
            result[2] != MaiFileSystem::resolve(MaiFilePath::fromUtf8(paths.workspace)))
            return {MaiErrorCode::InvalidInput,
                    "workspace changed; create a matching storage service"};
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (result[i].isEmpty() || result[i].dirName() == result[i] ||
                !MaiFileSystem::isDirectory(result[i]))
                return {MaiErrorCode::InvalidInput, "App storage roots are missing or unsafe"};
            for (std::size_t j = 0; j < i; ++j)
                if (result[i] == result[j] || result[i].isParentOf(result[j]) ||
                    result[j].isParentOf(result[i]))
                    return {MaiErrorCode::InvalidInput, "App storage roots overlap"};
        }
        return result;
    }
};

MaiAppStorage::MaiAppStorage(MaiAppStoragePaths paths)
    : mImplementation(std::make_unique<Implementation>(std::move(paths))) {}
MaiAppStorage::~MaiAppStorage() = default;

MaiResult<MaiAppStoragePreview> MaiAppStorage::preview(const MaiAppStorageSelection& selection,
                                                       const MaiAppStorageContext& context) {
    const auto invalid = validate(selection);
    if (invalid) return invalid;
    if (context.sessionId.empty())
        return {MaiErrorCode::InvalidInput, "a session identity is required for storage previews"};
    if (selection.target == MaiAppStorageTarget::Workspace && !context.protectionComplete)
        return {MaiErrorCode::InvalidInput, "workspace protection records are unavailable"};
    const auto rootsResult = mImplementation->roots(context);
    if (!rootsResult) return rootsResult.error();
    const auto roots = rootsResult.value();
    const auto cutoff = static_cast<std::int64_t>(std::time(nullptr)) -
                        static_cast<std::int64_t>(selection.minimumAgeHours) * 3600;
    MaiAppStoragePreview result;
    result.minimumAgeHours = selection.minimumAgeHours;
    result.selection = selection;
    std::vector<Implementation::Candidate> candidates;
    static const std::array<const char*, 3> names{"temporary", "cache", "workspace"};
    for (std::size_t index = 0; index < roots.size(); ++index) {
        if (selection.target != MaiAppStorageTarget::App &&
            static_cast<int>(selection.target) != static_cast<int>(index) + 1)
            continue;
        const auto& root = roots[index];
        // 显式 scope 不允许借中间目录链接跳出根目录；枚举也不跟随任何链接。
        if (!selection.scope.empty() && selection.scope.find('*') == std::string::npos) {
            const auto scoped = root.append(MaiFilePath::fromUtf8(selection.scope));
            if (!withinWithoutLinks(root, scoped))
                return {MaiErrorCode::InvalidInput,
                        "scope contains a symbolic link or leaves its root"};
        }
        MaiAppStorageSummary summary;
        summary.target = names[index];
        std::size_t visited = 0;
        MaiFileSystem::walk(root, [&](const MaiFileEntry& entry) {
            if (canceled(context) || ++visited > kMaximumFiles) {
                summary.truncated = true;
                return MaiWalkAction::Stop;
            }
            const auto relative = relativeTo(root, entry.path);
            if (MaiFileSystem::isSymbolicLink(entry.path)) return MaiWalkAction::SkipDirectory;
            if (entry.isDirectory) {
                if (!mayContainSelection(selection, relative)) return MaiWalkAction::SkipDirectory;
                if (selection.target == MaiAppStorageTarget::Workspace &&
                    !workspaceFamily(relative) && relative != ".maiagent")
                    return MaiWalkAction::SkipDirectory;
                return MaiWalkAction::Continue;
            }
            if (!selected(selection, relative)) return MaiWalkAction::Continue;
            MaiRegularFileInfo info;
            if (!withinWithoutLinks(root, entry.path) ||
                !MaiFileSystem::regularFileInfo(entry.path, info))
                return MaiWalkAction::Continue;
            ++summary.files;
            summary.bytes += info.bytes;
            const bool allowed =
                index != 2 || (selection.target == MaiAppStorageTarget::Workspace &&
                               workspaceIntermediateFile(relative));
            if (!allowed) {
                ++summary.excludedPolicyFiles;
            } else if (info.modified <= 0 || info.modified > cutoff) {
                ++summary.excludedRecentFiles;
                summary.excludedRecentBytes += info.bytes;
            } else if (protectedFile(entry.path, relative, context, boundedSelection(selection))) {
                ++summary.excludedProtectedFiles;
            } else {
                ++summary.eligibleFiles;
                summary.eligibleBytes += info.bytes;
                if (candidates.size() < kMaximumCandidates)
                    candidates.push_back(
                        {{entry.path.toUtf8(), relative, names[index], info.bytes, info.modified},
                         info,
                         root});
                else
                    result.limited = true;
            }
            return MaiWalkAction::Continue;
        });
        result.summaries.push_back(summary);
    }
    if (canceled(context)) return {MaiErrorCode::Canceled, "storage preview was canceled"};
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.value.path < b.value.path; });
    for (const auto& candidate : candidates) {
        result.candidates.push_back(candidate.value);
        result.candidateBytes += candidate.value.bytes;
    }
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    result.id = MaiIdGenerator::generate("storage_");
    mImplementation->previewId = result.id;
    mImplementation->sessionId = context.sessionId;
    mImplementation->needsProtection = selection.target == MaiAppStorageTarget::Workspace;
    mImplementation->createdAt = static_cast<std::int64_t>(std::time(nullptr));
    mImplementation->previewRoots = roots;
    mImplementation->candidates = std::move(candidates);
    return result;
}

MaiResult<MaiAppStorageCleanup> MaiAppStorage::cleanup(const std::string& previewId,
                                                       const MaiAppStorageContext& context) {
    const auto rootsResult = mImplementation->roots(context);
    if (!rootsResult) return rootsResult.error();
    std::vector<Implementation::Candidate> candidates;
    {
        std::lock_guard<std::mutex> lock(mImplementation->mutex);
        const auto age = static_cast<std::int64_t>(std::time(nullptr)) - mImplementation->createdAt;
        if (previewId.empty() || previewId != mImplementation->previewId ||
            context.sessionId != mImplementation->sessionId || age < 0 || age > 600 ||
            rootsResult.value() != mImplementation->previewRoots)
            return {MaiErrorCode::InvalidInput,
                    "scan storage again for a fresh preview in this session"};
        if (mImplementation->needsProtection && !context.protectionComplete)
            return {MaiErrorCode::InvalidInput,
                    "workspace cleanup requires current delivery protection records"};
        candidates = std::move(mImplementation->candidates);
        mImplementation->previewId.clear();
    }
    MaiAppStorageCleanup result;
    for (const auto& candidate : candidates) {
        if (canceled(context)) return {MaiErrorCode::Canceled, "storage cleanup was canceled"};
        const auto path = MaiFilePath::fromUtf8(candidate.value.path);
        MaiRegularFileInfo info;
        if (!withinWithoutLinks(candidate.root, path) ||
            protectedFile(path, candidate.value.relativePath, context, true) ||
            !MaiFileSystem::regularFileInfo(path, info) || info.bytes != candidate.info.bytes ||
            info.modified != candidate.info.modified || info.identity != candidate.info.identity ||
            MaiFileSystem::removeFile(path).hasError()) {
            ++result.skippedFiles;
            continue;
        }
        ++result.removedFiles;
        result.reclaimedBytes += info.bytes;
    }
    return result;
}

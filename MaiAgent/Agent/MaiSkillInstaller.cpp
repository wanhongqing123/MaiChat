// 文件作用：从固定提交安装经过校验的 GitHub 技能文件。
#include "MaiSkillInstaller.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <json.hpp>
#include <openssl/evp.h>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {
using Json = nlohmann::json;
using Cancel = std::function<bool()>;
constexpr std::size_t kTreeResponseBytes = 8 * 1024 * 1024;
constexpr const char* kManifest = ".mai-skill-source.json";

MaiFilePath child(const MaiFilePath& path, const std::string& name) {
    return path.append(MaiFilePath::fromUtf8(name));
}
bool canceled(const Cancel& check) {
    return check && check();
}
std::string lower(std::string text) {
    for (auto& character : text)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return text;
}
bool safeComponent(const std::string& component) {
    if (component.empty() || component == "." || component == ".." || component.size() > 255 ||
        component.back() == '.' || component.back() == ' ')
        return false;
    for (const unsigned char character : component)
        if (character < 32 || character == 127 ||
            std::string("\\/:*?\"<>|").find(character) != std::string::npos)
            return false;
    const auto base = lower(component.substr(0, component.find('.')));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
        base == "conout$" ||
        (base.size() == 4 && (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") &&
         base[3] >= '0' && base[3] <= '9'))
        return false;
    return true;
}
bool safePath(const std::string& path, bool allowEmpty = false) {
    if (path.empty()) return allowEmpty;
    if (path.size() > 4096) return false;
    std::istringstream stream(path);
    std::size_t components = 0;
    for (std::string component; std::getline(stream, component, '/');)
        if (!safeComponent(component) || ++components > 32) return false;
    return path.back() != '/';
}
std::string encode(const std::string& text, bool preserveSlashes = false) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : text) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.' || character == '~' || (preserveSlashes && character == '/')) {
            result += static_cast<char>(character);
        } else {
            result += '%';
            result += digits[character >> 4];
            result += digits[character & 15];
        }
    }
    return result;
}
bool isSha(const std::string& sha) {
    return sha.size() == 40 && sha.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}
struct Address {
    std::string sourceUrl;
    std::string owner;
    std::string repository;
    std::string ref;
    std::string path;
    bool selected = false;
};
MaiResult<Address> addressFor(const MaiSkillInstallRequest& request) {
    constexpr const char* prefix = "https://github.com/";
    if (request.sourceUrl.compare(0, 19, prefix) != 0 || request.sourceUrl.size() > 4096 ||
        request.sourceUrl.find_first_of("?#%\\\r\n\t ") != std::string::npos)
        return {
            MaiErrorCode::InvalidInput,
            "source must be a public HTTPS github.com URL without credentials, query or fragment"};
    std::string tail = request.sourceUrl.substr(19);
    if (!tail.empty() && tail.back() == '/') tail.pop_back();
    std::vector<std::string> pieces;
    std::istringstream stream(tail);
    for (std::string part; std::getline(stream, part, '/');) pieces.push_back(part);
    if (pieces.size() < 2)
        return {MaiErrorCode::InvalidInput, "GitHub source must identify an owner and repository"};
    for (const auto& piece : pieces)
        if (!safeComponent(piece)) return {MaiErrorCode::InvalidInput, "unsafe GitHub URL path"};
    for (std::size_t index = 0; index < 2; ++index)
        if (pieces[index].find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
            std::string::npos)
            return {MaiErrorCode::InvalidInput, "unsupported GitHub owner or repository"};
    Address result;
    result.sourceUrl = request.sourceUrl;
    result.owner = pieces[0];
    result.repository = pieces[1];
    if (result.repository.size() > 4 &&
        result.repository.compare(result.repository.size() - 4, 4, ".git") == 0)
        result.repository.resize(result.repository.size() - 4);
    if (pieces.size() > 2) {
        if (pieces.size() < 4 || (pieces[2] != "tree" && pieces[2] != "blob"))
            return {MaiErrorCode::InvalidInput,
                    "GitHub URL must identify a repository, tree or SKILL.md blob"};
        result.ref = pieces[3];
        std::size_t pathStart = 4;
        // 含斜杠分支由显式 ref 消歧；固定提交覆盖 URL 的移动 ref 时仍保留原目录。
        if (!request.ref.empty() && !isSha(request.ref)) {
            std::string prefixRef;
            for (std::size_t index = 3; index < pieces.size(); ++index) {
                if (!prefixRef.empty()) prefixRef += '/';
                prefixRef += pieces[index];
                if (prefixRef == request.ref) {
                    pathStart = index + 1;
                    break;
                }
            }
        }
        for (std::size_t index = pathStart; index < pieces.size(); ++index) {
            if (!result.path.empty()) result.path += '/';
            result.path += pieces[index];
        }
        if (pieces[2] == "blob") {
            if (result.path != "SKILL.md" &&
                (result.path.size() < 9 ||
                 result.path.compare(result.path.size() - 9, 9, "/SKILL.md") != 0))
                return {MaiErrorCode::InvalidInput, "GitHub blob source must be SKILL.md"};
            const auto slash = result.path.rfind('/');
            result.path = slash == std::string::npos ? std::string{} : result.path.substr(0, slash);
            result.selected = true;
        } else {
            result.selected = !result.path.empty();
        }
    }
    if (!request.ref.empty()) result.ref = request.ref;
    if (result.ref.empty()) result.ref = "HEAD";
    if (result.ref.size() > 256 || result.ref.find("..") != std::string::npos ||
        result.ref.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-") !=
            std::string::npos)
        return {MaiErrorCode::InvalidInput, "unsupported GitHub ref; use a branch, tag or commit"};
    if (!request.path.empty()) {
        result.path = request.path == "." ? std::string{} : request.path;
        result.selected = true;
    }
    if (!safePath(result.path, true))
        return {MaiErrorCode::InvalidInput, "unsafe skill directory path"};
    return result;
}

struct CurlResponse {
    std::string body;
    std::size_t maxBytes = 0;
    const Cancel* isCanceled = nullptr;
    bool exceeded = false;
};
std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* opaque) {
    auto& response = *static_cast<CurlResponse*>(opaque);
    if (size != 0 && count > response.maxBytes / size) {
        response.exceeded = true;
        return 0;
    }
    const auto bytes = size * count;
    if (bytes > response.maxBytes - response.body.size()) {
        response.exceeded = true;
        return 0;
    }
    if (canceled(*response.isCanceled)) return 0;
    response.body.append(data, bytes);
    return bytes;
}
int transferProgress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return canceled(*static_cast<const Cancel*>(opaque)) ? 1 : 0;
}
MaiResult<std::string> defaultFetch(const std::string& url, std::size_t maxBytes,
                                    const Cancel& check) {
    static std::once_flag initialized;
    static CURLcode initialization = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { initialization = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialization != CURLE_OK)
        return {MaiErrorCode::Internal, "cannot initialize skill source transfer"};
    if (canceled(check)) return {MaiErrorCode::Canceled, "skill source transfer canceled"};
    auto* handle = curl_easy_init();
    if (!handle) return {MaiErrorCode::Internal, "cannot create skill source transfer"};
    CurlResponse response;
    response.maxBytes = maxBytes;
    response.isCanceled = &check;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "MaiAgent-SkillInstaller/1.0");
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
#if defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 128L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maxBytes));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, transferProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &check);
    const auto transfer = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(handle);
    if (canceled(check)) return {MaiErrorCode::Canceled, "skill source transfer canceled"};
    if (response.exceeded || transfer == CURLE_FILESIZE_EXCEEDED)
        return {MaiErrorCode::InvalidInput, "skill source response exceeds byte budget"};
    if (transfer != CURLE_OK)
        return {MaiErrorCode::Network,
                std::string("skill source transfer failed: ") + curl_easy_strerror(transfer)};
    if (status != 200)
        return {status == 404                    ? MaiErrorCode::NotFound
                : status == 429 || status == 403 ? MaiErrorCode::RateLimited
                                                 : MaiErrorCode::Network,
                "GitHub source returned status " + std::to_string(status) +
                    "; redirects and authenticated sources are unsupported"};
    return response.body;
}
MaiResult<std::string> fetchBounded(const MaiSkillInstaller::Fetch& fetch, const std::string& url,
                                    std::size_t maxBytes, const Cancel& check) {
    if (canceled(check)) return {MaiErrorCode::Canceled, "skill source operation canceled"};
    auto result = fetch(url, maxBytes, check);
    if (canceled(check)) return {MaiErrorCode::Canceled, "skill source operation canceled"};
    if (!result) return result.error();
    if (result.value().size() > maxBytes)
        return {MaiErrorCode::InvalidInput, "skill source response exceeds byte budget"};
    return result.value();
}
MaiError validOptions(const MaiSkillInstallerOptions& options) {
    if (options.maxFrontmatterBytes < 8 || options.maxFrontmatterBytes > 1024 * 1024 ||
        options.maxBodyBytes == 0 || options.maxBodyBytes > 4 * 1024 * 1024 ||
        options.maxFileBytes == 0 || options.maxFileBytes > 16 * 1024 * 1024 ||
        options.maxTotalBytes == 0 || options.maxTotalBytes > 64 * 1024 * 1024 ||
        options.maxFiles == 0 || options.maxFiles > 4096 || options.maxTreeEntries == 0 ||
        options.maxTreeEntries > 50000 || options.maxReadmeBytes == 0 ||
        options.maxReadmeBytes > 256 * 1024)
        return {MaiErrorCode::InvalidInput, "invalid skill installer byte or entry budget"};
    return {};
}
struct Entry {
    std::string path;
    std::string mode;
    std::string type;
    std::uint64_t size = 0;
};
struct Repository {
    Address address;
    std::string commit;
    std::vector<Entry> entries;
};
MaiResult<Repository> loadRepository(const MaiSkillInstallRequest& request,
                                     const MaiSkillInstallerOptions& options,
                                     const MaiSkillInstaller::Fetch& fetch, const Cancel& check) {
    const auto optionError = validOptions(options);
    if (optionError) return optionError;
    auto address = addressFor(request);
    if (!address) return address.error();
    const auto base =
        "https://api.github.com/repos/" + address.value().owner + "/" + address.value().repository;
    auto response =
        fetchBounded(fetch, base + "/commits/" + encode(address.value().ref), 128 * 1024, check);
    if (!response) return response.error();
    const auto metadata = Json::parse(response.value(), nullptr, false);
    if (metadata.is_discarded() || !metadata.is_object() || !metadata.contains("sha") ||
        !metadata["sha"].is_string() || !isSha(metadata["sha"].get<std::string>()) ||
        !metadata.contains("commit") || !metadata["commit"].is_object() ||
        !metadata["commit"].contains("tree") || !metadata["commit"]["tree"].is_object() ||
        !metadata["commit"]["tree"].contains("sha") ||
        !metadata["commit"]["tree"]["sha"].is_string() ||
        !isSha(metadata["commit"]["tree"]["sha"].get<std::string>()))
        return {MaiErrorCode::Protocol,
                "GitHub commit response is missing immutable commit/tree identity"};
    Repository repository;
    repository.address = address.value();
    repository.commit = lower(metadata["sha"].get<std::string>());
    if (isSha(address.value().ref) && lower(address.value().ref) != repository.commit)
        return {MaiErrorCode::Protocol,
                "GitHub commit response differs from requested pinned commit"};
    auto treeResponse =
        fetchBounded(fetch,
                     base + "/git/trees/" + metadata["commit"]["tree"]["sha"].get<std::string>() +
                         "?recursive=1",
                     kTreeResponseBytes, check);
    if (!treeResponse) return treeResponse.error();
    const auto tree = Json::parse(treeResponse.value(), nullptr, false);
    if (tree.is_discarded() || !tree.is_object() || !tree.contains("tree") ||
        !tree["tree"].is_array() || !tree.contains("truncated") ||
        !tree["truncated"].is_boolean() || tree["truncated"].get<bool>() ||
        tree["tree"].size() > options.maxTreeEntries)
        return {
            MaiErrorCode::InvalidInput,
            "GitHub tree is incomplete or exceeds entry budget; select a smaller skill repository"};
    std::set<std::string> paths;
    for (const auto& item : tree["tree"]) {
        if (canceled(check)) return {MaiErrorCode::Canceled, "skill discovery canceled"};
        if (!item.is_object() || !item.contains("path") || !item["path"].is_string() ||
            !item.contains("mode") || !item["mode"].is_string() || !item.contains("type") ||
            !item["type"].is_string())
            return {MaiErrorCode::Protocol, "invalid GitHub tree entry"};
        Entry entry;
        entry.path = item["path"].get<std::string>();
        entry.mode = item["mode"].get<std::string>();
        entry.type = item["type"].get<std::string>();
        if (!safePath(entry.path) || !paths.insert(entry.path).second)
            return {MaiErrorCode::InvalidInput, "unsafe or duplicate repository path"};
        if (item.contains("size")) {
            if (!item["size"].is_number_unsigned())
                return {MaiErrorCode::Protocol, "invalid repository file size"};
            entry.size = item["size"].get<std::uint64_t>();
        }
        repository.entries.push_back(std::move(entry));
    }
    return repository;
}
std::vector<MaiSkillRepositoryCandidate> candidatesFor(const Repository& repository) {
    std::vector<MaiSkillRepositoryCandidate> result;
    for (const auto& entry : repository.entries) {
        if (entry.type != "blob" || (entry.mode != "100644" && entry.mode != "100755")) continue;
        if (entry.path == "SKILL.md")
            result.push_back({{}, {}, {}});
        else if (entry.path.size() > 9 &&
                 entry.path.compare(entry.path.size() - 9, 9, "/SKILL.md") == 0)
            result.push_back({entry.path.substr(0, entry.path.size() - 9), {}, {}});
    }
    return result;
}
std::string rawUrl(const Repository& repository, const std::string& path) {
    return "https://raw.githubusercontent.com/" + repository.address.owner + "/" +
           repository.address.repository + "/" + repository.commit + "/" + encode(path, true);
}
bool selectedEntry(const std::string& path, const std::string& directory) {
    return directory.empty() ||
           (path.size() > directory.size() && path.compare(0, directory.size(), directory) == 0 &&
            path[directory.size()] == '/');
}
std::string sha256(const std::string& contents) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(contents.data(), contents.size(), digest.data(), &length, EVP_sha256(),
                   nullptr) != 1)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int index = 0; index < length; ++index) {
        result += hex[digest[index] >> 4];
        result += hex[digest[index] & 15];
    }
    return result;
}
// 清理只接受本次随机暂存目录和未变化的来源清单，绝不递归清理活动技能根。
struct Stage {
    MaiFilePath directory;
    MaiFilePath parent;
    std::string emptyDirectoryIdentity;
    MaiRegularFileInfo manifestIdentity;
    bool owned = false;
    ~Stage() {
        if (directory.isEmpty() || MaiFileSystem::resolve(directory) != directory ||
            directory.dirName() != parent)
            return;
        if (!owned) {
            // 清单创建失败时只尝试删除身份未变的空目录，绝不递归处理未知内容。
            if (!emptyDirectoryIdentity.empty()) {
                MaiDirectoryDeleteOptions emptyOptions;
                emptyOptions.expectedIdentity = emptyDirectoryIdentity;
                (void)MaiFileSystem::deleteDirectory(directory, emptyOptions);
            }
            return;
        }
        MaiRegularFileInfo current;
        if (!MaiFileSystem::regularFileInfo(child(directory, kManifest), current) ||
            current.identity != manifestIdentity.identity ||
            current.bytes != manifestIdentity.bytes)
            return;
        const auto info = MaiFileSystem::stat(directory);
        if (!info || !info.value().isDirectory) return;
        MaiDirectoryDeleteOptions options;
        options.recursive = true;
        options.expectedIdentity = info.value().identity;
        options.pathAllowed = [this](const MaiFilePath& path) {
            const auto resolved = MaiFileSystem::resolve(path);
            return !resolved.isEmpty() && (resolved == directory || directory.isParentOf(resolved));
        };
        (void)MaiFileSystem::deleteDirectory(directory, options);
    }
};
}  // namespace

MaiSkillInstaller::MaiSkillInstaller(MaiSkillInstallerOptions options, Fetch fetch)
    : mOptions(std::move(options)), mFetch(fetch ? std::move(fetch) : Fetch(defaultFetch)) {}

MaiResult<MaiSkillRepositoryDiscovery> MaiSkillInstaller::discover(
    const MaiSkillInstallRequest& request, const std::function<bool()>& isCanceled) const {
    auto repository = loadRepository(request, mOptions, mFetch, isCanceled);
    if (!repository) return repository.error();
    MaiSkillRepositoryDiscovery result;
    result.sourceUrl = request.sourceUrl;
    result.commit = repository.value().commit;
    result.candidates = candidatesFor(repository.value());
    if (result.candidates.size() > mOptions.maxFiles)
        return {MaiErrorCode::InvalidInput, "skill candidate count exceeds discovery budget"};
    // 只取根 README 和显式目录的 README；最多两份，链接/上下文预算独立有界。
    std::size_t readmeBytes = 0;
    std::size_t readmeFiles = 0;
    for (const auto& entry : repository.value().entries) {
        const auto slash = entry.path.rfind('/');
        const auto parent =
            slash == std::string::npos ? std::string{} : entry.path.substr(0, slash);
        const auto name =
            lower(slash == std::string::npos ? entry.path : entry.path.substr(slash + 1));
        if (name != "readme.md" || entry.type != "blob" ||
            (entry.mode != "100644" && entry.mode != "100755") ||
            (!parent.empty() &&
             (!repository.value().address.selected || parent != repository.value().address.path)))
            continue;
        if (readmeBytes >= mOptions.maxReadmeBytes || readmeFiles >= 2) break;
        const auto body = fetchBounded(mFetch, rawUrl(repository.value(), entry.path),
                                       mOptions.maxReadmeBytes - readmeBytes, isCanceled);
        if (!body) return body.error();
        ++readmeFiles;
        readmeBytes += body.value().size();
        std::size_t offset = 0;
        while ((offset = body.value().find("https://github.com/", offset)) != std::string::npos) {
            const auto end = body.value().find_first_of(" \t\r\n)\"<>]", offset);
            auto url = body.value().substr(
                offset, end == std::string::npos ? std::string::npos : end - offset);
            while (!url.empty() && (url.back() == '.' || url.back() == ',' || url.back() == ';'))
                url.pop_back();
            MaiSkillInstallRequest linked;
            linked.sourceUrl = url;
            if (addressFor(linked)) {
                const auto lineStart = body.value().rfind('\n', offset);
                const auto begin = lineStart == std::string::npos ? 0 : lineStart + 1;
                const auto lineEnd = body.value().find('\n', offset);
                auto context = body.value().substr(
                    begin, std::min<std::size_t>(
                               512, (lineEnd == std::string::npos ? body.value().size() : lineEnd) -
                                        begin));
                if (result.links.size() >= 64)
                    return {MaiErrorCode::InvalidInput,
                            "README link count exceeds discovery budget"};
                result.links.push_back({std::move(url), std::move(context)});
            }
            offset += 19;
        }
    }
    return result;
}

MaiResult<MaiSkillInstallResult> MaiSkillInstaller::install(
    const MaiSkillInstallRequest& request, const std::function<bool()>& isCanceled) const {
    if (request.scope != MaiSkillScope::User && request.scope != MaiSkillScope::Workspace)
        return {MaiErrorCode::InvalidInput, "installation scope must be user or workspace"};
    const auto configuredRoot =
        request.scope == MaiSkillScope::User ? mOptions.homeDirectory : mOptions.workspaceDirectory;
    if (configuredRoot.isEmpty() || !configuredRoot.isAbsolute())
        return {MaiErrorCode::NotConfigured,
                "host has not configured an absolute skill installation root"};
    auto repository = loadRepository(request, mOptions, mFetch, isCanceled);
    if (!repository) return repository.error();
    const auto candidates = candidatesFor(repository.value());
    if (candidates.size() > mOptions.maxFiles)
        return {MaiErrorCode::InvalidInput, "skill candidate count exceeds budget"};
    auto selectedPath = repository.value().address.path;
    if (!repository.value().address.selected) {
        if (candidates.size() != 1)
            return {MaiErrorCode::InvalidInput,
                    "repository does not identify one skill; discover candidates and select a path "
                    "first"};
        selectedPath = candidates.front().path;
    }
    if (std::none_of(candidates.begin(), candidates.end(),
                     [&](const MaiSkillRepositoryCandidate& candidate) {
                         return candidate.path == selectedPath;
                     }))
        return {MaiErrorCode::NotFound, "selected repository directory has no regular SKILL.md"};
    const auto slash = selectedPath.rfind('/');
    const auto destinationName =
        selectedPath.empty() ? repository.value().address.repository
                             : selectedPath.substr(slash == std::string::npos ? 0 : slash + 1);
    if (!safeComponent(destinationName))
        return {MaiErrorCode::InvalidInput, "unsafe destination skill directory name"};
    struct Download {
        Entry entry;
        std::string relativePath;
        std::string contents;
        std::string hash;
    };
    std::vector<Download> downloads;
    std::map<std::string, std::string> casePaths;
    std::size_t totalBytes = 0;
    for (const auto& entry : repository.value().entries) {
        if (!selectedEntry(entry.path, selectedPath)) continue;
        if (entry.type == "tree" && entry.mode == "040000") continue;
        if (entry.type != "blob" || (entry.mode != "100644" && entry.mode != "100755"))
            return {MaiErrorCode::NotSupported,
                    "selected skill contains a link, submodule or unsupported file mode"};
        const auto relative =
            selectedPath.empty() ? entry.path : entry.path.substr(selectedPath.size() + 1);
        if (lower(relative) == kManifest)
            return {MaiErrorCode::InvalidInput,
                    "skill conflicts with reserved installer provenance file"};
        std::size_t end = 0;
        do {
            end = relative.find('/', end);
            const auto prefix = relative.substr(0, end);
            const auto inserted = casePaths.emplace(lower(prefix), prefix);
            if (!inserted.second && inserted.first->second != prefix)
                return {MaiErrorCode::InvalidInput,
                        "skill contains case-colliding repository paths"};
            if (end != std::string::npos) ++end;
        } while (end != std::string::npos);
        if (downloads.size() >= mOptions.maxFiles || entry.size > mOptions.maxFileBytes ||
            entry.size > mOptions.maxTotalBytes - totalBytes)
            return {MaiErrorCode::InvalidInput, "selected skill exceeds file count or byte budget"};
        const auto budget = std::min(mOptions.maxFileBytes, mOptions.maxTotalBytes - totalBytes);
        if (budget == 0)
            return {MaiErrorCode::InvalidInput, "selected skill exceeds total byte budget"};
        auto contents =
            fetchBounded(mFetch, rawUrl(repository.value(), entry.path), budget, isCanceled);
        if (!contents) return contents.error();
        totalBytes += contents.value().size();
        const auto hash = sha256(contents.value());
        if (hash.empty()) return {MaiErrorCode::Internal, "cannot hash downloaded skill file"};
        downloads.push_back({entry, relative, std::move(contents.value()), hash});
    }
    const auto skill = std::find_if(downloads.begin(), downloads.end(), [](const Download& file) {
        return file.relativePath == "SKILL.md";
    });
    if (skill == downloads.end())
        return {MaiErrorCode::NotFound, "selected skill is missing SKILL.md"};
    const auto hostRoot = MaiFileSystem::resolve(configuredRoot);
    if (hostRoot.isEmpty()) return {MaiErrorCode::InvalidInput, "cannot resolve host skill root"};
    const auto agentsRoot = child(hostRoot, ".agents");
    const auto skillsRoot = child(agentsRoot, "skills");
    if (MaiFileSystem::isSymbolicLink(agentsRoot) || MaiFileSystem::isSymbolicLink(skillsRoot) ||
        MaiFileSystem::resolve(agentsRoot) != agentsRoot ||
        MaiFileSystem::resolve(skillsRoot) != skillsRoot)
        return {MaiErrorCode::InvalidInput,
                "skill installation roots must not redirect through links"};
    const auto destination = child(skillsRoot, destinationName);
    if (MaiFileSystem::exists(destination) || MaiFileSystem::isSymbolicLink(destination))
        return {MaiErrorCode::InvalidInput,
                "skill destination already exists; existing skills are never overwritten"};
    auto metadata =
        MaiSkillManager::validateMetadata(skill->contents, child(destination, "SKILL.md"),
                                          mOptions.maxFrontmatterBytes, mOptions.maxBodyBytes);
    if (!metadata) return metadata.error();
    if (canceled(isCanceled))
        return {MaiErrorCode::Canceled, "skill installation canceled before staging"};
    auto error = MaiFileSystem::createDirectories(skillsRoot);
    if (error) return error;
    if (MaiFileSystem::resolve(configuredRoot) != hostRoot ||
        MaiFileSystem::resolve(skillsRoot) != skillsRoot)
        return {MaiErrorCode::InvalidInput, "host skill root changed during installation"};
    Stage stage;
    stage.parent = agentsRoot;
    stage.directory = child(agentsRoot, ".skill-stage-" + MaiIdGenerator::generate(""));
    if (MaiFileSystem::exists(stage.directory) || MaiFileSystem::isSymbolicLink(stage.directory))
        return {MaiErrorCode::Busy, "skill staging directory already exists"};
    error = MaiFileSystem::createNewDirectory(stage.directory);
    if (error) return error;
    const auto emptyStage = MaiFileSystem::stat(stage.directory);
    if (!emptyStage || !emptyStage.value().isDirectory ||
        MaiFileSystem::resolve(stage.directory) != stage.directory)
        return {MaiErrorCode::InvalidInput, "cannot identify newly created staging directory"};
    stage.emptyDirectoryIdentity = emptyStage.value().identity;
    const auto manifestPath = child(stage.directory, kManifest);
    error = MaiFileSystem::createEmptyFile(manifestPath);
    if (error) return error;
    Json manifest = {{"source_url", request.sourceUrl},
                     {"commit", repository.value().commit},
                     {"path", selectedPath},
                     {"file_count", downloads.size()},
                     {"total_bytes", totalBytes},
                     {"dependencies_installed", false},
                     {"files", Json::array()}};
    for (const auto& file : downloads)
        manifest["files"].push_back({{"path", file.relativePath},
                                     {"sha256", file.hash},
                                     {"bytes", file.contents.size()},
                                     {"mode", file.entry.mode}});
    error = MaiFileSystem::writeFile(manifestPath, manifest.dump(2) + "\n");
    if (error) {
        // 即便写入失败，仍只认本次排他创建的普通清单文件。
        stage.owned = MaiFileSystem::regularFileInfo(manifestPath, stage.manifestIdentity);
        return error;
    }
    stage.owned = MaiFileSystem::regularFileInfo(manifestPath, stage.manifestIdentity);
    if (!stage.owned) return {MaiErrorCode::InvalidInput, "cannot identify owned staging manifest"};
    for (const auto& file : downloads) {
        if (canceled(isCanceled))
            return {MaiErrorCode::Canceled, "skill installation canceled during staging"};
        const auto target = MaiFileSystem::resolve(child(stage.directory, file.relativePath));
        if (target.isEmpty() || !stage.directory.isParentOf(target))
            return {MaiErrorCode::InvalidInput, "invalid path in owned staging directory"};
        error = MaiFileSystem::createDirectories(target.dirName());
        if (error) return error;
        if (MaiFileSystem::resolve(target) != target || !stage.directory.isParentOf(target))
            return {MaiErrorCode::InvalidInput, "staging path redirected outside owned directory"};
        error = MaiFileSystem::createEmptyFile(target);
        if (error) return error;
        error = MaiFileSystem::writeFile(target, file.contents);
        if (error) return error;
        if (file.entry.mode == "100755") {
            error = MaiFileSystem::setUserExecutable(target);
            if (error) return error;
        }
    }
    // 发布前从磁盘重读正文，保证实际写入版本符合相同技能解析和大小契约。
    std::string stagedSkill;
    bool truncated = false;
    error = MaiFileSystem::readFile(child(stage.directory, "SKILL.md"), stagedSkill,
                                    mOptions.maxBodyBytes, &truncated);
    if (error) return error;
    if (truncated || stagedSkill != skill->contents)
        return {MaiErrorCode::InvalidInput, "staged skill differs from validated download"};
    metadata =
        MaiSkillManager::validateMetadata(stagedSkill, child(destination, "SKILL.md"),
                                          mOptions.maxFrontmatterBytes, mOptions.maxBodyBytes);
    if (!metadata) return metadata.error();
    if (canceled(isCanceled))
        return {MaiErrorCode::Canceled, "skill installation canceled before publication"};
    if (MaiFileSystem::resolve(configuredRoot) != hostRoot ||
        MaiFileSystem::resolve(skillsRoot) != skillsRoot ||
        MaiFileSystem::resolve(stage.directory) != stage.directory)
        return {MaiErrorCode::InvalidInput, "installation path changed before publication"};
    error = MaiFileSystem::publishNewDirectory(stage.directory, destination);
    if (error) return error;
    stage.owned = false;
    MaiSkillInstallResult result;
    result.metadata = metadata.value();
    result.metadata.scope = request.scope;
    result.directory = destination;
    result.sourceUrl = request.sourceUrl;
    result.commit = repository.value().commit;
    result.sourcePath = selectedPath;
    result.fileCount = downloads.size();
    result.totalBytes = totalBytes;
    return result;
}

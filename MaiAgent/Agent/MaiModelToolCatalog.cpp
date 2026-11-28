// 文件作用：按会话和型号缓存追加式工具声明，保持已发送前缀。
#include "MaiModelToolCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <json.hpp>
#include <openssl/evp.h>

#include "MaiSessionStore.h"
#include "MaiTool.h"

namespace {
using Json = nlohmann::json;
constexpr const char* kModeKey = "model-tool-catalog-mode-v1";
constexpr std::size_t kMaxAppendRecords = 1024;
constexpr std::size_t kSummaryBytes = 8192;
constexpr std::array<const char*, 8> kBootstrapTools = {
    "exec_command", "write_stdin", "skill_read",      "tool_search",
    "apply_patch",  "view_image",  "session_history", "tool_output_read"};

bool missing(const MaiError& error) {
    return error.code() == MaiErrorCode::NotFound || error.code() == MaiErrorCode::NotSupported;
}

MaiResult<std::string> scopeKey(const std::string& model, const char* prefix = "model-tools-v5-") {
    if (model.empty())
        return {MaiErrorCode::InvalidInput, "A model is required for tool discovery."};
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(model.data(), model.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
        return {MaiErrorCode::Internal, "Cannot identify the model tool catalog."};
    constexpr char hex[] = "0123456789abcdef";
    // v5 对齐桌面文件注册范围；旧快照不能继续声明宿主已经移除的 file_*。
    std::string key = prefix;
    for (unsigned int index = 0; index < length; ++index) {
        key += hex[digest[index] >> 4];
        key += hex[digest[index] & 15];
    }
    return key;
}

std::string encode(const std::string& model, const std::vector<MaiToolSpec>& specs) {
    Json list = Json::array();
    for (const auto& spec : specs)
        list.push_back({{"name", spec.name},
                        {"description", spec.description},
                        {"parameters", spec.parametersJson},
                        {"input_grammar", spec.inputGrammar}});
    return Json{{"model", model}, {"specs", list}}.dump();
}

MaiResult<std::vector<MaiToolSpec>> decode(const std::string& model, const std::string& bytes) {
    const auto value = Json::parse(bytes, nullptr, false);
    const auto invalid = [] {
        return MaiResult<std::vector<MaiToolSpec>>{MaiErrorCode::Internal,
                                                   "Invalid persisted model tool catalog."};
    };
    if (!value.is_object() || !value.contains("model") || value["model"] != model ||
        !value.contains("specs") || !value["specs"].is_array())
        return invalid();
    std::vector<MaiToolSpec> specs;
    std::set<std::string> names;
    for (const auto& item : value["specs"]) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
            !item.contains("description") || !item["description"].is_string() ||
            !item.contains("parameters") || !item["parameters"].is_string() ||
            !item.contains("input_grammar") || !item["input_grammar"].is_string())
            return invalid();
        MaiToolSpec spec{item["name"].get<std::string>(), item["description"].get<std::string>(),
                         item["parameters"].get<std::string>(),
                         item["input_grammar"].get<std::string>()};
        if (spec.name.empty() || !names.insert(spec.name).second) return invalid();
        specs.push_back(std::move(spec));
    }
    return specs;
}

bool contains(const std::vector<MaiToolSpec>& specs, const std::string& name) {
    return std::any_of(specs.begin(), specs.end(),
                       [&](const auto& spec) { return spec.name == name; });
}

std::string lower(std::string text) {
    for (auto& byte : text)
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    return text;
}

// 描述只提供发现线索；按 UTF-8 边界截短，Schema 与正文始终留在目录核心中。
std::string shortDescription(const std::string& description) {
    std::size_t end = std::min<std::size_t>(description.size(), 512);
    while (end < description.size() && end &&
           (static_cast<unsigned char>(description[end]) & 0xc0) == 0x80)
        --end;
    return description.substr(0, end);
}

// 这里只裁剪发现线索，不修改工具本身的使用合同；先折叠换行，避免一项占用多行。
// 与 Chromium TruncateUTF8ToByteSize 一样退回完整码点边界，不截断多字节字符。
std::string summaryPurpose(const std::string& description, std::size_t budget) {
    std::string text;
    for (const unsigned char byte : description) {
        if (byte <= 0x20 || byte == 0x7f) {
            if (!text.empty() && text.back() != ' ') text += ' ';
        } else {
            text += static_cast<char>(byte);
        }
        if (text.size() > budget + 4) break;
    }
    if (text.size() > budget) {
        std::size_t end = budget - 3;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
        text += "...";
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

MaiResult<std::string> renderSummary(std::vector<MaiToolMetadata> metadata,
                                     std::size_t byteBudget) {
    // 只宣传当前模型真实可达的帮助/技能入口；旧持久化摘要仍由原 v1 读取路径保留。
    const auto hasTool = [&](const char* name) {
        return std::any_of(metadata.begin(), metadata.end(),
                           [&](const auto& item) { return item.name == name; });
    };
    const bool hasHelp = hasTool("tool_help");
    const bool hasSkills = hasTool("skill_read");
    metadata.erase(std::remove_if(metadata.begin(), metadata.end(),
                                  [](const auto& item) {
                                      return std::find(kBootstrapTools.begin(),
                                                       kBootstrapTools.end(),
                                                       item.name) != kBootstrapTools.end();
                                  }),
                   metadata.end());
    if (metadata.empty()) return std::string{};
    std::sort(metadata.begin(), metadata.end(),
              [](const auto& left, const auto& right) { return left.name < right.name; });
    for (const auto& item : metadata) {
        if (item.name.empty() || item.name.size() > 256)
            return {MaiErrorCode::Internal, "Invalid public capability name."};
        if (const auto error = maiValidateSessionContextContent(item.name)) return error;
    }
    std::string header =
        "Builtin capability index (names and short purposes only). Use tool_search query or "
        "select (up to 5 exact names); schemas appear in the next request. Call tools only after "
        "schema declaration. No permissions, credentials or backend readiness are implied.\n";
    if (hasHelp)
        header +=
            "For grouped help select tool_help, then call arguments:[\"-h\"] or "
            "[group,\"-h\"].\n";
    if (hasSkills) header += "For complex guidance use skill_read mode=discover.\n";
    // 先压缩用途以保留全部真实名称；极大目录才省略整项，并始终保留可达的续查入口。
    for (const std::size_t purposeBytes : {128u, 64u, 0u}) {
        std::string result = header;
        std::size_t included = 0;
        for (const auto& item : metadata) {
            // 非普通名称使用可逆 JSON 引号，不能让名称换行伪装成另一项，也不改写真实名称。
            const bool quoteName =
                std::any_of(item.name.begin(), item.name.end(), [](unsigned char byte) {
                    return byte <= 0x20 || byte == 0x7f || byte == '"' || byte == '\\';
                });
            std::string line = "- " + (quoteName ? Json(item.name).dump() : item.name);
            if (purposeBytes) line += ": " + summaryPurpose(item.description, purposeBytes);
            line += '\n';
            const std::string continuation =
                "Additional capabilities omitted; browse tool_search "
                "with an empty query and offset/limit.\n";
            if (result.size() + line.size() + continuation.size() > byteBudget) {
                if (purposeBytes) break;
                result += continuation;
                return result;
            }
            result += line;
            ++included;
        }
        if (included == metadata.size()) return result;
    }
    return {MaiErrorCode::Internal, "Cannot fit the capability discovery instructions."};
}
}  // namespace

struct MaiModelToolCatalog::Impl {
    struct Snapshot {
        std::vector<MaiToolSpec> specs;
        std::size_t nextOrdinal = 0;
    };
    MaiSessionStore& store;
    const MaiToolRegistry& registry;
    std::mutex mutex;
    std::map<std::string, bool> legacySessions;
    std::map<std::pair<std::string, std::string>, Snapshot> snapshots;
    std::map<std::pair<std::string, std::string>, std::string> summaries;
    Impl(MaiSessionStore& value, const MaiToolRegistry& tools) : store(value), registry(tools) {}

    MaiResult<std::string> saved(const std::string& session, const std::string& key,
                                 const std::string& content) {
        if (const auto error = maiValidateSessionContextContent(content)) return error;
        const auto result = store.saveSessionContext(session, key, content);
        if (!result && result.error().code() == MaiErrorCode::NotSupported) return content;
        return result;
    }

    MaiResult<bool> isLegacy(const MaiTurnContext& context) {
        const auto cached = legacySessions.find(context.sessionId);
        if (cached != legacySessions.end()) return cached->second;
        const auto stored = store.getSessionContext(context.sessionId, kModeKey);
        if (const auto error = store.lastReadError()) return error;
        std::string mode;
        if (stored) {
            mode = stored.value();
        } else if (missing(stored.error())) {
            // 首次采样前 user 和 Running assistant 已落库；它们不是迁移前历史。
            const auto messages = store.listMessagesPage(context.sessionId, {}, 3);
            if (const auto error = store.lastReadError()) return error;
            bool legacy = messages.size() > 2;
            std::size_t users = 0;
            for (const auto& message : messages) {
                if (message.role == MaiRole::User)
                    ++users;
                else if (message.role != MaiRole::Assistant || message.id != context.turnId)
                    legacy = true;
            }
            if (users > 1) legacy = true;
            const auto winner = saved(context.sessionId, kModeKey, legacy ? "legacy" : "bounded");
            if (!winner) return winner.error();
            mode = winner.value();
        } else {
            return stored.error();
        }
        if (mode != "legacy" && mode != "bounded")
            return {MaiErrorCode::Internal, "Invalid persisted tool discovery mode."};
        const bool legacy = mode == "legacy";
        legacySessions.emplace(context.sessionId, legacy);
        return legacy;
    }

    MaiResult<Snapshot> prepare(const MaiTurnContext& context, const std::string& key) {
        const auto identity = std::make_pair(context.sessionId, context.model);
        const auto cached = snapshots.find(identity);
        if (cached != snapshots.end()) return cached->second;
        MaiSession session;
        if (!store.getSession(context.sessionId, session)) {
            if (const auto error = store.lastReadError()) return error;
            return {MaiErrorCode::NotFound, "Tool discovery session does not exist."};
        }
        const auto legacy = isLegacy(context);
        if (!legacy) return legacy.error();
        const auto stored = store.getSessionContext(context.sessionId, key);
        if (const auto error = store.lastReadError()) return error;
        std::string base;
        if (stored) {
            base = stored.value();
        } else if (missing(stored.error())) {
            std::vector<MaiToolSpec> initial;
            if (legacy.value()) {
                initial = registry.specs(context.model);
            } else {
                // 来源补读工具必须始终自举，否则压缩器不能安全地只发送来源索引。
                for (const auto* name : kBootstrapTools)
                    if (const auto* tool = registry.findForModel(name, context.model))
                        initial.push_back(tool->specForModel(context.model));
            }
            const auto winner = saved(context.sessionId, key, encode(context.model, initial));
            if (!winner) return winner.error();
            base = winner.value();
        } else {
            return stored.error();
        }
        const auto decoded = decode(context.model, base);
        if (!decoded) return decoded.error();
        // 配置升级可能使供应商提示缓存失效一次；不可变旧目录和原始对话/续接完全不改写。
        // 稳定原生 ID 由联合存储去重；先写事实再发布快照，写失败时下次重试同一记录。
        std::string previousKey;
        int previousVersion = 0;
        for (const int version : {4, 3, 2, 1}) {
            const auto candidate = "model-tools-v" + std::to_string(version) + "-" +
                                   key.substr(std::string("model-tools-v5-").size());
            const auto previous = store.getSessionContext(context.sessionId, candidate);
            if (const auto error = store.lastReadError()) return error;
            if (previous) {
                previousKey = candidate;
                previousVersion = version;
                break;
            }
            if (!missing(previous.error())) return previous.error();
        }
        if (previousVersion) {
            const auto error =
                store.appendSessionRecord(context.sessionId, "response_item",
                                          Json{{"type", "configuration_update"},
                                               {"id", "tool-catalog-upgrade-" + key},
                                               {"configuration", "model_tool_catalog"},
                                               {"model", context.model},
                                               {"previous_version", previousVersion},
                                               {"version", 5},
                                               {"previous_snapshot_key", previousKey},
                                               {"snapshot_key", key},
                                               {"reason", "desktop_native_file_tool_scope"}}
                                              .dump());
            if (error && error.code() != MaiErrorCode::NotSupported) return error;
        }
        Snapshot snapshot;
        snapshot.specs = decoded.value();
        // 只在冷加载时顺序查询小型不可变追加记录；后续请求只读缓存，不扫描 JSONL。
        for (; snapshot.nextOrdinal < kMaxAppendRecords; ++snapshot.nextOrdinal) {
            const auto append = store.getSessionContext(
                context.sessionId, key + "-" + std::to_string(snapshot.nextOrdinal));
            if (const auto error = store.lastReadError()) return error;
            if (!append) {
                if (missing(append.error())) break;
                return append.error();
            }
            const auto additions = decode(context.model, append.value());
            if (!additions) return additions.error();
            for (const auto& spec : additions.value()) {
                if (contains(snapshot.specs, spec.name))
                    return {MaiErrorCode::Internal, "Duplicate persisted model tool declaration."};
                snapshot.specs.push_back(spec);
            }
        }
        snapshots.emplace(identity, snapshot);
        return snapshot;
    }
};
MaiModelToolCatalog::MaiModelToolCatalog(MaiSessionStore& store, const MaiToolRegistry& registry)
    : mImplementation(std::make_unique<Impl>(store, registry)) {}
MaiModelToolCatalog::~MaiModelToolCatalog() = default;
MaiResult<std::string> MaiModelToolCatalog::capabilitySummary(const MaiTurnContext& context,
                                                              std::size_t firstRenderByteBudget) {
    const auto key = scopeKey(context.model, "model-tool-summary-v1-");
    if (!key) return key.error();
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    const auto identity = std::make_pair(context.sessionId, context.model);
    const auto cached = mImplementation->summaries.find(identity);
    if (cached != mImplementation->summaries.end()) return cached->second;
    MaiSession session;
    if (!mImplementation->store.getSession(context.sessionId, session)) {
        if (const auto error = mImplementation->store.lastReadError()) return error;
        return {MaiErrorCode::NotFound, "Capability summary session does not exist."};
    }
    const auto stored = mImplementation->store.getSessionContext(context.sessionId, key.value());
    if (const auto error = mImplementation->store.lastReadError()) return error;
    std::string summary;
    if (stored) {
        summary = stored.value();
    } else if (missing(stored.error())) {
        MaiResult<std::string> candidate = std::string{};
        if (mImplementation->registry.findForModel("tool_search", context.model))
            candidate =
                renderSummary(mImplementation->registry.metadata(context.model),
                              std::clamp(firstRenderByteBudget, std::size_t{512}, kSummaryBytes));
        if (!candidate) return candidate.error();
        const auto winner =
            mImplementation->saved(context.sessionId, key.value(), candidate.value());
        if (!winner) return winner.error();
        summary = winner.value();
    } else {
        return stored.error();
    }
    if (summary.size() > kSummaryBytes)
        return {MaiErrorCode::Internal, "Persisted capability summary exceeds its byte budget."};
    if (const auto error = maiValidateSessionContextContent(summary)) return error;
    mImplementation->summaries.emplace(identity, summary);
    return summary;
}
MaiError MaiModelToolCatalog::initializeNewSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    MaiSession session;
    if (!mImplementation->store.getSession(sessionId, session)) {
        if (const auto error = mImplementation->store.lastReadError()) return error;
        return {MaiErrorCode::NotFound, "Cannot initialize tools before the session exists."};
    }
    // 创建路径拥有新会话身份，必须在发布 ID 前固定模式；后续 pending 输入数量不能改判。
    // insert-if-absent 同时保证误传已有会话时保留历史模式，不覆盖已发送的完整声明。
    const auto winner = mImplementation->saved(sessionId, kModeKey, "bounded");
    if (!winner) return winner.error();
    if (winner.value() != "bounded" && winner.value() != "legacy")
        return {MaiErrorCode::Internal, "Invalid persisted tool discovery mode."};
    mImplementation->legacySessions.emplace(sessionId, winner.value() == "legacy");
    return {};
}
MaiResult<std::vector<MaiToolSpec>> MaiModelToolCatalog::exposedSpecs(
    const MaiTurnContext& context) {
    const auto key = scopeKey(context.model);
    if (!key) return key.error();
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    const auto snapshot = mImplementation->prepare(context, key.value());
    if (!snapshot) return snapshot.error();
    return snapshot.value().specs;
}
MaiResult<MaiToolCatalogPage> MaiModelToolCatalog::search(const MaiTurnContext& context,
                                                          const MaiToolCatalogQuery& query) {
    if (query.query.size() > 4096 || query.limit == 0 || query.limit > 10 ||
        query.select.size() > 5)
        return {MaiErrorCode::InvalidInput,
                "Expected query <=4096 bytes, limit 1..10 and <=5 selections."};
    const auto key = scopeKey(context.model);
    if (!key) return key.error();
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    const auto prepared = mImplementation->prepare(context, key.value());
    if (!prepared) return prepared.error();
    auto& snapshot = mImplementation->snapshots.at({context.sessionId, context.model});
    std::set<std::string> selected;
    std::vector<MaiToolSpec> additions;
    // 先校验整个选择，未知/隐藏/不支持项不能让同批次的合法项被提前激活。
    for (const auto& name : query.select) {
        if (name.empty() || name.size() > 256)
            return {MaiErrorCode::InvalidInput, "Selected tool names must contain 1..256 bytes."};
        const auto* tool = mImplementation->registry.findForModel(name, context.model);
        if (!tool || !selected.insert(name).second)
            return {MaiErrorCode::InvalidInput,
                    "Selected tool is unknown, unavailable or duplicated: " + name};
        if (!contains(snapshot.specs, name)) additions.push_back(tool->specForModel(context.model));
    }
    if (!additions.empty()) {
        if (snapshot.nextOrdinal >= kMaxAppendRecords)
            return {MaiErrorCode::InvalidInput,
                    "The session tool discovery append limit was reached."};
        const auto winner = mImplementation->saved(
            context.sessionId, key.value() + "-" + std::to_string(snapshot.nextOrdinal),
            encode(context.model, additions));
        if (!winner) return winner.error();
        const auto actual = decode(context.model, winner.value());
        if (!actual) return actual.error();
        for (const auto& spec : actual.value()) {
            if (contains(snapshot.specs, spec.name))
                return {MaiErrorCode::Internal, "Duplicate persisted tool discovery append."};
        }
        snapshot.specs.insert(snapshot.specs.end(), actual.value().begin(), actual.value().end());
        ++snapshot.nextOrdinal;
        // 其他实例占用了该序号时只采用真实赢家，不能声称请求的工具已经加载。
        for (const auto& name : query.select)
            if (!contains(snapshot.specs, name))
                return {MaiErrorCode::Busy,
                        "The catalog changed concurrently; retry tool selection."};
    }
    MaiToolCatalogPage page;
    page.selected = query.select;
    auto candidates = mImplementation->registry.metadata(context.model);
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) { return left.name < right.name; });
    const auto needle = lower(query.query);
    for (const auto& spec : candidates) {
        if (lower(spec.name + " " + spec.description).find(needle) == std::string::npos) continue;
        const auto position = page.total++;
        if (position < query.offset || page.tools.size() >= query.limit) continue;
        if (spec.name.size() > 256)
            return {MaiErrorCode::Internal,
                    "A public tool name exceeds the discovery metadata limit."};
        page.tools.push_back(
            {spec.name, shortDescription(spec.description), contains(snapshot.specs, spec.name)});
    }
    page.nextOffset = query.offset >= page.total ? page.total : query.offset + page.tools.size();
    return page;
}
void MaiModelToolCatalog::forgetSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    mImplementation->legacySessions.erase(sessionId);
    for (auto item = mImplementation->summaries.begin(); item != mImplementation->summaries.end();)
        if (item->first.first == sessionId)
            item = mImplementation->summaries.erase(item);
        else
            ++item;
    for (auto item = mImplementation->snapshots.begin(); item != mImplementation->snapshots.end();)
        if (item->first.first == sessionId)
            item = mImplementation->snapshots.erase(item);
        else
            ++item;
}

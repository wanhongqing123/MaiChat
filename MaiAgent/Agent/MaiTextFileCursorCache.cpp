// 文件作用：保存按宿主绑定的文本游标，不信任外部构造的文件偏移或快照。
#include "MaiTextFileCursorCache.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>

#include "MaiIdGenerator.h"

struct MaiTextFileCursorCache::Impl {
    using Clock = std::chrono::steady_clock;
    struct Record {
        MaiTextFileCursor cursor;
        std::string binding;
        Clock::time_point touched;
    };
    std::mutex mutex;
    std::map<std::string, Record> records;

    void prune(Clock::time_point now) {
        for (auto item = records.begin(); item != records.end();) {
            if (now - item->second.touched >= std::chrono::seconds(120))
                item = records.erase(item);
            else
                ++item;
        }
    }
};

MaiTextFileCursorCache::MaiTextFileCursorCache() : mImpl(std::make_unique<Impl>()) {}
MaiTextFileCursorCache::~MaiTextFileCursorCache() = default;

MaiResult<std::string> MaiTextFileCursorCache::remember(const MaiTextFileCursor& cursor,
                                                        const std::string& binding) {
    if (binding.empty() || binding.size() > 64 * 1024 || cursor.sourcePath.isEmpty())
        return {MaiErrorCode::InvalidInput, "Invalid trusted text cursor binding"};
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    const auto now = Impl::Clock::now();
    mImpl->prune(now);
    while (mImpl->records.size() >= 128) {
        const auto oldest = std::min_element(mImpl->records.begin(), mImpl->records.end(),
                                             [](const auto& left, const auto& right) {
                                                 return left.second.touched < right.second.touched;
                                             });
        mImpl->records.erase(oldest);
    }
    const auto token = MaiIdGenerator::generate("text_cursor_");
    if (!mImpl->records.emplace(token, Impl::Record{cursor, binding, now}).second)
        return {MaiErrorCode::Internal, "Cannot allocate a unique text cursor"};
    return token;
}

MaiResult<MaiTextFileCursor> MaiTextFileCursorCache::recall(const std::string& token,
                                                            const std::string& binding) {
    if (token.empty() || token.size() > 256 || binding.empty() || binding.size() > 64 * 1024)
        return {MaiErrorCode::InvalidInput, "Invalid text cursor token or binding"};
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    const auto now = Impl::Clock::now();
    mImpl->prune(now);
    const auto record = mImpl->records.find(token);
    if (record == mImpl->records.end() || record->second.binding != binding)
        return {MaiErrorCode::InvalidInput,
                "Text cursor expired or belongs to a different Session, path or encoding"};
    record->second.touched = now;
    return record->second.cursor;
}

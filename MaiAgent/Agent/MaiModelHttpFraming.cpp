// 文件作用：只将未压缩、有精确长度的完整实体视作可安全结束的 HTTP 响应。
#include "MaiModelHttpFraming.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
void MaiModelHttpFraming::onHeader(std::string_view header) {
    if (header.substr(0, 5) == "HTTP/") {
        mLength = -1;
        mReceived = 0;
        mEncoded = false;
        return;
    }
    const auto colon = header.find(':');
    if (colon == std::string_view::npos) return;
    auto name = header.substr(0, colon);
    auto value = header.substr(colon + 1);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    const auto equals = [&](std::string_view expected) {
        return name.size() == expected.size() &&
               std::equal(name.begin(), name.end(), expected.begin(),
                          [](unsigned char a, unsigned char b) { return std::tolower(a) == b; });
    };
    if (equals("content-length")) {
        long long length = -1;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
        // 重复长度、畸形/负数均关闭复用优化，不改变模型终止帧的成功语义。
        if (mLength >= 0 || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            length < 0)
            mEncoded = true;
        else
            mLength = length;
    } else if (equals("content-encoding") || equals("transfer-encoding")) {
        // curl 写回调看到的是解压后字节，不能与压缩 Content-Length 比较；chunked 也不猜边界。
        if (value != "identity") mEncoded = true;
    }
}
void MaiModelHttpFraming::received(std::size_t bytes) {
    if (bytes > std::numeric_limits<std::size_t>::max() - mReceived)
        mEncoded = true;
    else
        mReceived += bytes;
}
bool MaiModelHttpFraming::isComplete() const {
    return !mEncoded && mLength >= 0 && static_cast<unsigned long long>(mLength) == mReceived;
}

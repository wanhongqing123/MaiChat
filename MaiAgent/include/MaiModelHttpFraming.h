// 文件作用：内部记录 HTTP 实体边界，只有确认读完响应时才允许保留活连接。
#pragma once
#include <cstddef>
#include <string_view>
class MaiModelHttpFraming {
public:
    void onHeader(std::string_view header);
    void received(std::size_t bytes);
    bool isComplete() const;

private:
    long long mLength = -1;
    std::size_t mReceived = 0;
    bool mEncoded = false;
};

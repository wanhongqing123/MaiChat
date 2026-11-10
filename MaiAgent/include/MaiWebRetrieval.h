// 文件作用：结构化检索和网页提取的中立值类型；供应商缺失的字段保持未知。
#pragma once

#include <optional>
#include <string>
#include <vector>

// code 为空表示无错误；httpStatus=0 表示尚未收到 HTTP 响应。
// HTTP 状态属于供应商 API，不能据此虚构目标网站的实际状态。404 的多种来源无法进一步判定。
// attempts 是实际已发请求数；最多两次。retryable 表示错误类别可重试，不代表客户端会无限重试。
struct MaiWebRetrievalError {
    std::string code;
    std::string reason;
    long httpStatus = 0;
    unsigned attempts = 0;
    bool retryable = false;
};

// query/oriQuestion 最多128个Unicode字符；query原样发送，保留site/lang/日期等操作符。
// localization 只接受用户已提供的 IPv4/IPv6，不是语言或地区代码；未知时留空，不能猜造或查 IP。
// 不自动读取或上送设备 IP；needContent 默认关闭以控制正文大小。
struct MaiWebSearchRequest {
    std::string query;
    unsigned count = 10;
    std::string oriQuestion;
    std::string localization;
    bool needContent = false;
};

// 元数据仅取供应商原值，缺失日期/网站为null，authority=-1代表未知，不推测发布时间或权威度。
struct MaiWebSearchItem {
    std::string title;
    std::string url;
    std::string snippet;
    std::string summary;
    std::string content;
    std::optional<std::string> site;
    std::optional<std::string> date;
    int authority = -1;
    double rerankScore = 0;
};

struct MaiWebSearchResponse {
    std::vector<MaiWebSearchItem> results;
    MaiWebRetrievalError error;
    unsigned attempts = 0;
};

struct MaiWebFetchResponse {
    std::string title;
    std::string content;
    bool truncated = false;
    MaiWebRetrievalError error;
    unsigned attempts = 0;
};

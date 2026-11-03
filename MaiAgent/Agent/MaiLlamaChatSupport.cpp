// 文件作用：固定上游 chat 的纯字符串/token 辅助闭包，不链接 CLI、文件工具或日志线程。
// 字符串/tokenizer/sampling helper 摘自 llama.cpp ee17f293 的 common.cpp/sampling.cpp，
// 遵循 third_party/llama.cpp/LICENSE（MIT）；上游源码不修改，外部同名符号仅供该闭包使用。
#include "MaiLlamaChatSupport.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <common.h>
#include <log.h>
#include <sampling.h>

// 上游解析错误日志含完整提示词/工具参数。固定禁用接收器，既不格式化敏感数据，
// 也不创建上游进程级日志线程；宿主只接收 MaiError 的稳定文案。
int common_log_get_verbosity_thold() {
    return -1;
}
common_log* common_log_main() {
    return nullptr;
}
void common_log_add(common_log*, ggml_log_level, const char*, ...) {}

std::string string_format(const char* fmt, ...) {
    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int size = vsnprintf(NULL, 0, fmt, ap);
    GGML_ASSERT(size >= 0 && size < INT_MAX);  // NOLINT
    std::vector<char> buf(size + 1);
    int size2 = vsnprintf(buf.data(), size + 1, fmt, ap2);
    GGML_ASSERT(size2 == size);
    va_end(ap2);
    va_end(ap);
    return std::string(buf.data(), size);
}

std::string string_strip(const std::string& str) {
    size_t start = 0;
    size_t end = str.size();
    while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
        start++;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        end--;
    }
    return str.substr(start, end - start);
}

std::string string_lcs(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return {};

    std::vector<std::vector<size_t>> dp(a.size() + 1, std::vector<size_t>(b.size() + 1, 0));
    size_t best_len = 0;
    size_t best_end_a = 0;

    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            if (a[i - 1] == b[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
                if (dp[i][j] > best_len) {
                    best_len = dp[i][j];
                    best_end_a = i;
                }
            }
        }
    }
    return std::string(a.substr(best_end_a - best_len, best_len));
}

void string_replace_all(std::string& s, const std::string& search, const std::string& replace) {
    if (search.empty()) {
        return;
    }
    std::string builder;
    builder.reserve(s.length());
    size_t pos = 0;
    size_t last_pos = 0;
    while ((pos = s.find(search, last_pos)) != std::string::npos) {
        builder.append(s, last_pos, pos - last_pos);
        builder.append(replace);
        last_pos = pos + search.length();
    }
    builder.append(s, last_pos, std::string::npos);
    s = std::move(builder);
}

std::string regex_escape(const std::string& s) {
    static const std::regex special_chars("[.^$|()*+?\\[\\]{}\\\\]");
    return std::regex_replace(s, special_chars, "\\$&");
}

std::string string_join(const std::vector<std::string>& values, const std::string& separator) {
    std::ostringstream result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            result << separator;
        }
        result << values[i];
    }
    return result.str();
}

std::string string_repeat(const std::string& str, size_t n) {
    if (n == 0) {
        return "";
    }

    std::string result;
    result.reserve(str.length() * n);

    for (size_t i = 0; i < n; ++i) {
        result += str;
    }

    return result;
}

std::vector<std::string> string_split(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> parts;
    size_t start = 0;
    size_t end = str.find(delimiter);

    while (end != std::string::npos) {
        parts.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
        end = str.find(delimiter, start);
    }

    parts.push_back(str.substr(start));

    return parts;
}

std::vector<llama_token> common_tokenize(const struct llama_vocab* vocab, const std::string& text,
                                         bool add_special, bool parse_special) {
    // upper limit for the number of tokens
    int n_tokens = text.length() + 2 * add_special;
    std::vector<llama_token> result(n_tokens);
    n_tokens = llama_tokenize(vocab, text.data(), text.length(), result.data(), result.size(),
                              add_special, parse_special);
    if (n_tokens == std::numeric_limits<int32_t>::min()) {
        throw std::runtime_error(
            "Tokenization failed: input text too large, tokenization result exceeds int32_t limit");
    }
    if (n_tokens < 0) {
        result.resize(-n_tokens);
        int check = llama_tokenize(vocab, text.data(), text.length(), result.data(), result.size(),
                                   add_special, parse_special);
        GGML_ASSERT(check == -n_tokens);
    } else {
        result.resize(n_tokens);
    }
    return result;
}

std::string common_token_to_piece(const struct llama_vocab* vocab, llama_token token,
                                  bool special) {
    std::string piece;
    piece.resize(piece.capacity());  // using string internal cache, 15 bytes + '\n'
    const int n_chars = llama_token_to_piece(vocab, token, &piece[0], piece.size(), 0, special);
    if (n_chars < 0) {
        piece.resize(-n_chars);
        int check = llama_token_to_piece(vocab, token, &piece[0], piece.size(), 0, special);
        GGML_ASSERT(check == -n_chars);
    } else {
        piece.resize(n_chars);
    }

    return piece;
}

void common_sampling_add_preserved_tokens(common_params_sampling& sampling,
                                          const llama_vocab* vocab,
                                          const std::vector<std::string>& tokens) {
    GGML_ASSERT(vocab != nullptr);
    for (const auto& t : tokens) {
        auto ids = common_tokenize(vocab, t, false, true);
        if (ids.size() == 1) {
            sampling.preserved_tokens.insert(ids[0]);
        }
    }
}

void common_sampling_add_grammar_triggers(common_params_sampling& sampling,
                                          const llama_vocab* vocab,
                                          std::vector<common_grammar_trigger> triggers) {
    GGML_ASSERT(vocab != nullptr);
    for (auto& trigger : triggers) {
        if (trigger.type == COMMON_GRAMMAR_TRIGGER_TYPE_WORD) {
            const auto& word = trigger.value;
            auto ids = common_tokenize(vocab, word, false, true);
            if (ids.size() == 1) {
                auto token = ids[0];
                if (std::find(sampling.preserved_tokens.begin(), sampling.preserved_tokens.end(),
                              (llama_token)token) == sampling.preserved_tokens.end()) {
                    throw std::runtime_error(
                        "Grammar trigger word should be marked as preserved token: " + word);
                }
                common_grammar_trigger token_trigger;
                token_trigger.type = COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN;
                token_trigger.value = word;
                token_trigger.token = token;
                sampling.grammar_triggers.push_back(std::move(token_trigger));
            } else {
                sampling.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, word});
            }
        } else {
            sampling.grammar_triggers.push_back(std::move(trigger));
        }
    }
    if (sampling.grammar_lazy && sampling.grammar_triggers.empty()) {
        throw std::runtime_error("Error: no triggers set for lazy grammar!");
    }
}

std::string escapeMaiLlamaGrammarWord(const std::string& value) {
    return regex_escape(value);
}

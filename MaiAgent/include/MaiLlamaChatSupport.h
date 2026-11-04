// 文件作用：只为固定版本的上游模板/语法解析提供无文件、网络或 CLI 副作用的兼容辅助。
// 内部实现接口，不能用作模型客户端或稳定的公开协议。
#pragma once

#include <string>

// 将模型模板给出的字面触发词转义成正则；不执行表达式，不解释用户文件内容。
std::string escapeMaiLlamaGrammarWord(const std::string& value);

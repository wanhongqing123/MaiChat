// 文件作用：验证真实文件查找工具的参数能穿过 Qwen3.5 原生 XML grammar 和解析器。
// 不加载 GGUF、不调用模型；模板来自仓库，上游解析器与工具 schema 均使用正式实现。
// 根级组合 schema 曾让 XML 参数枚举为空，所以必须核完整消费与映射后的参数，不能只看 fail()。
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

#include "chat.h"
#include "chat-peg-parser.h"

#include "MaiFileSystem.h"
#include "MaiFileTools.h"

#ifndef MAI_AGENT_SOURCE_DIR
#error MAI_AGENT_SOURCE_DIR must point to the MaiAgent source directory
#endif

namespace {

int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

common_chat_params makeQwenParameters(const MaiTool& tool) {
    const auto templatePath = MaiFilePath::fromUtf8(MAI_AGENT_SOURCE_DIR)
                                  .append(MaiFilePath::fromUtf8(
                                      "third_party/llama.cpp/models/templates/Qwen3.5-4B.jinja"));
    std::string source;
    const auto error = MaiFileSystem::readFile(templatePath, source);
    if (error.hasError()) throw std::runtime_error(error.message());

    const auto templates = common_chat_templates_init(nullptr, source);
    if (!templates) throw std::runtime_error("Qwen3.5 template is unavailable");

    common_chat_templates_inputs inputs;
    inputs.enable_thinking = false;
    inputs.parallel_tool_calls = false;
    common_chat_msg user;
    user.role = "user";
    user.content = "Find files in the source directory.";
    inputs.messages.push_back(user);
    inputs.tools.push_back({tool.name(), tool.description(), tool.parametersSchema()});
    return common_chat_templates_apply(templates.get(), inputs);
}

std::string xmlCall(const std::string& name, const common_json& arguments) {
    std::string result = "<tool_call>\n<function=" + name + ">\n";
    for (const auto& item : arguments.items()) {
        // XML 字符串参数是原文，其余类型保持 JSON 字面值；只用本测试生成的无标签数据。
        const std::string value =
            item.value().is_string() ? item.value().get<std::string>() : item.value().dump();
        result += "<parameter=" + item.key() + ">\n" + value + "\n</parameter>\n";
    }
    return result + "</function>\n</tool_call>";
}

void checkQwenCall(const common_chat_params& parameters, const std::string& name,
                   const common_json& expected) {
    const std::string output = xmlCall(name, expected);
    const std::string complete = parameters.generation_prompt + output;
    common_peg_parse_context context(complete, COMMON_PEG_PARSE_FLAG_NONE);
    const auto parsed = parameters.parser.parse(context);
    CHECK(!parsed.fail());
    CHECK(parsed.end == complete.size());

    // 正式完整响应入口还会加 generation prefix 并使用其完成标志，必须同样保住全部参数。
    common_chat_parser_params parserParameters(parameters);
    parserParameters.parser = parameters.parser;
    const auto message = common_chat_parse(common_chat_input(output), false, parserParameters);
    CHECK(message.tool_calls.size() == 1);
    if (message.tool_calls.size() != 1) return;
    CHECK(message.tool_calls.front().name == name);
    const auto actual = common_json::parse_no_throw(message.tool_calls.front().arguments);
    CHECK(!actual.is_discarded());
    CHECK(actual == expected);
}

void testQwenGlobArguments() {
    const auto tool = makeMaiGlobTool();
    const auto parameters = makeQwenParameters(*tool);
    CHECK(parameters.format == COMMON_CHAT_FORMAT_PEG_NATIVE);
    CHECK(!parameters.parser.empty());
    CHECK(!parameters.grammar.empty());

    // 两种互斥选择分别带齐其余参数，覆盖实际工具的十个属性及字符串/布尔/数组/整数类型。
    common_json pattern = {
        {"pattern", "**/*.{h,cpp}"},
        {"path", "src"},
        {"case_sensitive", true},
        {"include_hidden", true},
        {"include_ignored", false},
        {"exclude", common_json::array({"build/**", "vendor/**"})},
        {"limit", 17},
        {"cursor", "owned-continuation"},
        {"refresh", true},
    };
    checkQwenCall(parameters, tool->name(), pattern);

    auto query = pattern;
    query.erase("pattern");
    query["query"] = "MaiFile";
    query["case_sensitive"] = false;
    query["include_ignored"] = true;
    query["refresh"] = false;
    checkQwenCall(parameters, tool->name(), query);
}

void testNativeGlobRejectsInvalidChoice() {
    const auto tool = makeMaiGlobTool();
    const MaiToolContext context;
    // 模型侧保留平面属性供 XML grammar 枚举；最终二选一校验仍在原生执行边界。
    for (const std::string& arguments :
         {std::string("{}"), std::string(R"({"pattern":"**/*.cpp","query":"MaiFile"})")}) {
        const auto result = tool->execute(arguments, context);
        CHECK(result.hasError());
        CHECK(result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(result.error().message().find("exactly one") != std::string::npos);
    }
}

}  // namespace

int main() {
    try {
        testQwenGlobArguments();
        testNativeGlobRejectsInvalidChoice();
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        ++failures;
    }
    if (failures == 0) std::printf("MaiFileToolSchemaTests passed\n");
    return failures == 0 ? 0 : 1;
}

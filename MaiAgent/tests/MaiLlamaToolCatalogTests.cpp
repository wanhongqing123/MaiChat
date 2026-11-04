// 文件作用：验证本地主模型工具目录的普通入口、精确元数据、完整原名和严格发现参数。
// 不加载模型、不执行工具；使用调用方提供的公开快照，独立核对声明协议与拒绝边界。
#include "MaiLlamaToolCatalog.h"

#include <cstdio>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

std::vector<MaiToolSpec> publicSpecifications() {
    // 顺序、空白和转义都是宿主原始声明的一部分，发现层不能重新编码这些字段。
    return {{"file_read", "Read a file. Preserve \"quoted\" text.\nSecond line.",
             "{ \"required\": [\"path\"],\n  \"type\": \"object\", "
             "\"properties\": {\"path\": {\"type\": \"string\"}} }\n"},
            {"network_ip", "Read the device network addresses.", "{\n  \"type\":\"object\"\n}"},
            {"future_public_extension", "An unclassified public capability.", "{}"}};
}

void checkSameSpecification(const MaiToolSpec& actual, const MaiToolSpec& expected) {
    CHECK(actual.name == expected.name);
    CHECK(actual.description == expected.description);
    CHECK(actual.parametersJson == expected.parametersJson);
}

void checkRejectedRequest(const MaiLlamaToolCatalog& catalog, const std::string& arguments) {
    const auto result = catalog.resolve(arguments);
    CHECK(!result);
    CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    // 失败批次不能把已找到的声明部分交付，也不能把私有请求名带入错误文本。
    CHECK(result.value().empty());
    CHECK(result.error().message().find("private_internal_secret") == std::string::npos);
}

void testExactMetadataAndIndependentCopies() {
    auto original = publicSpecifications();
    const auto expected = original;
    const MaiLlamaToolCatalog catalog(original);
    CHECK(!catalog.error().hasError());
    original[0].description = "Changed by the caller.";
    original[0].parametersJson = "{}";

    auto direct = catalog.resolveNames({"network_ip", "file_read"});
    CHECK(direct);
    CHECK(direct.value().size() == 2);
    if (!direct || direct.value().size() != 2) return;
    checkSameSpecification(direct.value()[0], expected[1]);
    checkSameSpecification(direct.value()[1], expected[0]);

    const auto parsed = catalog.resolve(R"({"names":["network_ip","file_read"]})");
    CHECK(parsed);
    CHECK(parsed.value().size() == 2);
    if (parsed && parsed.value().size() == 2) {
        checkSameSpecification(parsed.value()[0], direct.value()[0]);
        checkSameSpecification(parsed.value()[1], direct.value()[1]);
    }
    direct.value()[1].description = "Changed in the returned copy.";
    const auto repeated = catalog.resolveNames({"file_read"});
    CHECK(repeated && repeated.value().size() == 1);
    if (repeated && repeated.value().size() == 1)
        checkSameSpecification(repeated.value()[0], expected[0]);
}

void testDiscoverySchemaIncludesEveryPublicName() {
    const auto originals = publicSpecifications();
    const MaiLlamaToolCatalog catalog(originals);
    const auto discovery = catalog.specification();
    CHECK(discovery.name == "tool_search");
    const auto schema = nlohmann::json::parse(discovery.parametersJson, nullptr, false);
    CHECK(schema.is_object());
    if (!schema.is_object()) return;
    // 这些字段由标准 JSON Schema 契约确定，不能只与本实现生成的另一个对象比较。
    CHECK(schema.value("type", "") == "object");
    CHECK(schema["required"] == nlohmann::json({"names"}));
    CHECK(schema["additionalProperties"] == false);
    CHECK(schema["properties"].size() == 1);
    const auto& names = schema["properties"]["names"];
    CHECK(names["type"] == "array");
    CHECK(names["minItems"] == 1);
    CHECK(names["maxItems"] == 8);
    CHECK(names["uniqueItems"] == true);
    CHECK(names["items"]["type"] == "string");
    CHECK(names["items"]["enum"] ==
          nlohmann::json({"file_read", "network_ip", "future_public_extension"}));
    // 新工具不需加入手写类别或关键词清单，宿主公开后立即可以按原名发现。
    const auto future = catalog.resolveNames({"future_public_extension"});
    CHECK(future && future.value().size() == 1);
    if (future && future.value().size() == 1)
        checkSameSpecification(future.value()[0], originals[2]);
}

void testStrictRequestsAndAtomicFailures() {
    const MaiLlamaToolCatalog catalog(publicSpecifications());
    const std::vector<std::string> rejected = {
        "",
        "{",
        "{}",
        "[]",
        "null",
        "true",
        R"({"names":null})",
        R"({"names":"file_read"})",
        R"({"names":[]})",
        R"({"names":[1]})",
        R"({"names":[false]})",
        R"({"names":[null]})",
        R"({"names":[{}]})",
        R"({"names":[[]]})",
        R"({"names":[""]})",
        R"({"names":["FILE_READ"]})",
        R"({"names":["tool_search"]})",
        R"({"names":["private_internal_secret"]})",
        R"({"names":["file_read","private_internal_secret"]})",
        R"({"names":["file_read","file_read"]})",
        R"({"names":["file_read"],"extra":true})",
        R"({"names":["file_read"],"names":["network_ip"]})",
        R"({"names":["file_read"],"na\u006des":["network_ip"]})",
        R"({"names":["file_read"]} trailing)",
        std::string("{\"names\":[\"") + std::string("\xc0\xaf", 2) + "\"]}"};
    for (const auto& arguments : rejected) checkRejectedRequest(catalog, arguments);

    for (const auto& names : std::vector<std::vector<std::string>>{
             {}, {"file_read", "file_read"}, {"file_read", "private_internal_secret"}}) {
        const auto result = catalog.resolveNames(names);
        CHECK(!result && result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(result.value().empty());
    }
}

void testCatalogAndRequestCountBoundaries() {
    std::vector<MaiToolSpec> originals;
    for (unsigned index = 0; index < 512; ++index)
        originals.push_back({"public_" + std::to_string(index), "Public tool.", "{}"});
    const MaiLlamaToolCatalog catalog(originals);
    CHECK(!catalog.error().hasError());
    const auto schema =
        nlohmann::json::parse(catalog.specification().parametersJson, nullptr, false);
    const auto& names = schema["properties"]["names"]["items"]["enum"];
    CHECK(names.size() == originals.size());
    for (std::size_t index = 0; index < originals.size() && index < names.size(); ++index)
        CHECK(names[index] == originals[index].name);

    std::vector<std::string> request;
    for (unsigned index = 0; index < 8; ++index) request.push_back(originals[index].name);
    const auto direct = catalog.resolveNames(request);
    CHECK(direct && direct.value().size() == 8);
    CHECK(catalog.resolve(nlohmann::json({{"names", request}}).dump()));
    request.push_back(originals[8].name);
    CHECK(!catalog.resolveNames(request));
    checkRejectedRequest(catalog, nlohmann::json({{"names", request}}).dump());

    originals.push_back({"public_512", "One too many.", "{}"});
    CHECK(MaiLlamaToolCatalog(originals).error().code() == MaiErrorCode::InvalidInput);
}

void testInvalidCatalogsFailBeforeDiscovery() {
    const std::string invalidUtf8("\xc0\xaf", 2);
    const std::string embeddedNull("prefix\0suffix", 13);
    const auto originals = publicSpecifications();
    const std::vector<std::vector<MaiToolSpec>> rejected = {
        {},
        {{"", "Empty name.", "{}"}},
        {{"tool_search", "Reserved name.", "{}"}},
        {originals[0], originals[0]},
        {{"invalid_schema", "Malformed schema.", "{"}},
        {{"array_schema", "Schema must be an object.", "[]"}},
        {{"null_schema", "Schema must be an object.", "null"}},
        {{"number_schema", "Schema must be an object.", "1"}},
        {{invalidUtf8, "Invalid name encoding.", "{}"}},
        {{"invalid_description", invalidUtf8, "{}"}},
        {{"invalid_schema_utf8", "Invalid schema encoding.",
          "{\"description\":\"" + invalidUtf8 + "\"}"}},
        {{embeddedNull, "Embedded null name.", "{}"}},
        {{"null_description", embeddedNull, "{}"}},
        {{"null_schema_bytes", "Embedded null schema.", "{}" + embeddedNull}}};
    for (const auto& specifications : rejected) {
        const MaiLlamaToolCatalog catalog(specifications);
        CHECK(catalog.error().code() == MaiErrorCode::InvalidInput);
        const auto declaration = catalog.specification();
        CHECK(declaration.name.empty() && declaration.description.empty() &&
              declaration.parametersJson.empty());
        CHECK(!catalog.resolveNames({"file_read"}));
        checkRejectedRequest(catalog, R"({"names":["file_read"]})");
    }
}

void testAggregateMetadataByteLimit() {
    // 名字、描述、原始 Schema 三者共享预算；刚好 2 MiB 必须成功，多一字节必须失败。
    const std::size_t maximumBytes = 2 * 1024 * 1024;
    const std::string name = "bounded";
    const std::string schema = "{}";
    std::vector<MaiToolSpec> specifications = {
        {name, std::string(maximumBytes - name.size() - schema.size(), 'a'), schema}};
    const MaiLlamaToolCatalog maximum(specifications);
    CHECK(!maximum.error().hasError());
    const auto result = maximum.resolveNames({name});
    CHECK(result && result.value().size() == 1);
    if (result && result.value().size() == 1)
        checkSameSpecification(result.value()[0], specifications[0]);
    specifications[0].description.push_back('a');
    CHECK(MaiLlamaToolCatalog(specifications).error().code() == MaiErrorCode::InvalidInput);

    specifications[0].description.pop_back();
    specifications.push_back({"another", "", "{}"});
    CHECK(MaiLlamaToolCatalog(specifications).error().code() == MaiErrorCode::InvalidInput);
}

}  // namespace

int main() {
    testExactMetadataAndIndependentCopies();
    testDiscoverySchemaIncludesEveryPublicName();
    testStrictRequestsAndAtomicFailures();
    testCatalogAndRequestCountBoundaries();
    testInvalidCatalogsFailBeforeDiscovery();
    testAggregateMetadataByteLimit();
    std::printf("MaiLlamaToolCatalogTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

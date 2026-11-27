// Shared native Responses fixtures for mobile HTTP contract tests.
#pragma once
#include <json.hpp>
#include <string>

namespace MaiResponsesFixture {
using Json = nlohmann::json;
inline std::string event(Json value) {
    return "data: " + value.dump() + "\n\n";
}
inline Json message(const std::string& text) {
    return {{"type", "message"}, {"role", "assistant"}, {"content", Json::array({
        {{"type", "output_text"}, {"text", text}}})}};
}
inline std::string completed(Json output) {
    return event({{"type", "response.completed"}, {"response", {
        {"status", "completed"}, {"output", std::move(output)}}}});
}
inline std::string text(const std::string& value) {
    return completed(Json::array({message(value)}));
}
// Input message content can be text or typed text/image parts; outputs have no user text.
inline std::string inputText(const Json& item) {
    if (!item.contains("content")) return {};
    const auto& content = item.at("content");
    if (content.is_string()) return content.get<std::string>();
    std::string value;
    if (content.is_array()) for (const auto& part : content)
        if (part.value("type", "") == "input_text") value += part.value("text", "");
    return value;
}
}  // namespace MaiResponsesFixture

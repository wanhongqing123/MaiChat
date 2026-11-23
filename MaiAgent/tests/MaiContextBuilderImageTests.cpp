// 文件作用：验证图片历史前缀稳定、工具配对及压缩边界；不调用真实模型或读取图片文件。
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiContextBuilder.h"
#include "MaiMessage.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

// 比较全部中立协议字段；仅比较图片数会漏掉标签、工具配对或 opaque continuation 被改写。
nlohmann::json modelMessageSnapshot(const MaiModelMessage& message) {
    using Json = nlohmann::json;
    Json images = Json::array(), calls = Json::array();
    for (const auto& image : message.images)
        images.push_back({{"path", image.path}, {"mime", image.mimeType}});
    for (const auto& call : message.invocations)
        calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    Json continuation = nullptr;
    if (message.continuation)
        continuation = {{"format", message.continuation->format},
                        {"provider", message.continuation->provider},
                        {"model", message.continuation->model},
                        {"payload", message.continuation->payload}};
    return {{"role", static_cast<int>(message.role)},
            {"content", message.content},
            {"reasoning", message.reasoning},
            {"images", images},
            {"call_id", message.toolCallId},
            {"calls", calls},
            {"continuation", continuation}};
}

MaiMessage userMessage(const std::string& text, const std::string& imagePath) {
    MaiMessage message;
    message.role = MaiRole::User;
    if (!text.empty()) {
        MaiMessagePart part;
        part.body = MaiTextPart{text};
        message.parts.push_back(std::move(part));
    }
    if (!imagePath.empty()) {
        MaiMessagePart part;
        part.body = MaiImagePart{imagePath, "image/jpeg"};
        message.parts.push_back(std::move(part));
    }
    return message;
}

MaiMessage toolImages(const std::string& prefix, int count) {
    MaiMessage message;
    message.role = MaiRole::Assistant;
    for (int index = 0; index < count; ++index) {
        MaiMessagePart part;
        part.body = MaiImagePart{prefix + std::to_string(index) + ".jpg", "image/jpeg"};
        message.parts.push_back(std::move(part));
    }
    return message;
}

void testImageRequestsOnlyAppendUntilCompaction() {
    std::vector<MaiMessage> history;
    auto first = userMessage("Original image request", "owned/first.jpg");
    first.id = "msg_first";
    first.parts.back().id = "prt_first";
    first.parts.push_back({"prt_second", MaiImagePart{"owned/second.png", "image/png"}, 1});
    history.push_back(first);
    MaiContextBuilder builder;
    auto previous = builder.build(history);
    CHECK(previous.front().content.find(
              "Image attachment 1 (id=prt_first, workspace_path=\"owned/first.jpg\").") !=
          std::string::npos);
    CHECK(previous.front().content.find("Image attachment 2 (id=prt_second,") != std::string::npos);
    const auto appendAndCheck = [&] {
        const auto current = builder.build(history);
        CHECK(current.size() > previous.size());
        for (std::size_t index = 0; index < previous.size() && index < current.size(); ++index) {
            if (modelMessageSnapshot(previous[index]) != modelMessageSnapshot(current[index])) {
                std::printf("Changed emitted prefix at model message %zu (source messages %zu)\n",
                            index, history.size());
                CHECK(false);
                break;
            }
        }
        previous = current;
    };
    history.push_back(toolImages("owned/step-", 0));
    history.back().id = "msg_steps";
    history.back().parts.push_back(
        {"prt_response",
         MaiReasoningPart{"", MaiModelContinuationState{"responses.output.v1", "owned-provider",
                                                        "owned-model", "opaque-owned-response"}},
         1});
    // 一条仍在增长的 assistant 内连续截图，必须跨越原来的8张滑动窗口。
    for (int index = 0; index < 10; ++index) {
        MaiToolPart call;
        call.tool = "screenshot";
        call.callId = "call_step_" + std::to_string(index);
        call.input = "{}";
        call.output = "Owned screenshot ready.";
        call.state = MaiToolState::Completed;
        history.back().parts.push_back({"prt_call_" + std::to_string(index), call, 1});
        history.back().parts.push_back(
            {"prt_step_" + std::to_string(index),
             MaiImagePart{"owned/step-" + std::to_string(index) + ".png", "image/png"}, 1});
        appendAndCheck();
    }
    // 新用户轮次和超过12张用户图片都不能改写早期标签、路径或像素。
    for (int index = 0; index < 14; ++index) {
        auto user = userMessage("Another image", "owned/user-" + std::to_string(index) + ".jpg");
        user.id = "msg_user_" + std::to_string(index);
        user.parts.back().id = "prt_user_" + std::to_string(index);
        history.push_back(user);
        appendAndCheck();
        history.push_back(userMessage("Text-only follow-up", {}));
        appendAndCheck();
    }
    history.push_back(toolImages("owned/later-", 9));
    appendAndCheck();
    std::size_t pixels = 0;
    for (const auto& message : previous) pixels += message.images.size();
    CHECK(pixels == 35);
    // 安全点输入组变化不应再成为选择旧图片的开关。
    const auto grouped = builder.build(history, 0, 0, {}, false, {first.id});
    CHECK(grouped.size() == previous.size());
    for (std::size_t index = 0; index < grouped.size() && index < previous.size(); ++index)
        CHECK(modelMessageSnapshot(grouped[index]) == modelMessageSnapshot(previous[index]));
    CHECK(std::get<MaiImagePart>(history.front().parts.back().body).path == "owned/second.png");
}

void testOnlyCompactionBoundaryRemovesCoveredImageParts() {
    std::vector<MaiMessage> history = {userMessage("Covered source", "owned/covered-user.jpg"),
                                       toolImages("owned/covered-tool-", 2),
                                       userMessage("Retained input", "owned/tail-user.jpg"),
                                       toolImages("owned/tail-tool-", 1)};
    for (std::size_t index = 0; index < history.size(); ++index)
        history[index].id = "msg_boundary_" + std::to_string(index);
    const auto paths = [](const std::vector<MaiModelMessage>& request) {
        std::vector<std::string> result;
        for (const auto& message : request)
            for (const auto& image : message.images) result.push_back(image.path);
        return result;
    };
    MaiContextBuilder builder;
    CHECK(paths(builder.build(history)).size() == 5);
    const auto complete = builder.build(history, 2);
    CHECK((paths(complete) ==
           std::vector<std::string>{"owned/tail-user.jpg", "owned/tail-tool-0.jpg"}));
    const auto partial = builder.build(history, 1, 1);
    CHECK((paths(partial) == std::vector<std::string>{"owned/covered-tool-1.jpg",
                                                      "owned/tail-user.jpg",
                                                      "owned/tail-tool-0.jpg"}));
    history.push_back(userMessage("Later input must not clear the retained pixels", {}));
    const auto later = builder.build(history, 2);
    CHECK(later.size() > complete.size());
    for (std::size_t index = 0; index < complete.size() && index < later.size(); ++index)
        CHECK(modelMessageSnapshot(complete[index]) == modelMessageSnapshot(later[index]));
    CHECK(paths(builder.build(history)).size() == 5);
}

void testMissingImagePathHasOnlyALocalStableNotice() {
    MaiMessage user = userMessage("Incomplete attachment", {});
    user.id = "msg_missing_path";
    user.parts.push_back({"prt_missing_user", MaiImagePart{"", "image/png"}, 1});
    MaiMessage assistant = toolImages("", 0);
    assistant.id = "msg_missing_tool";
    assistant.parts.push_back({"prt_missing_tool", MaiImagePart{"", "image/png"}, 1});
    const auto original = MaiContextBuilder().build({user, assistant});
    CHECK(original.size() == 2);
    if (original.size() != 2) return;
    for (const auto& message : original) CHECK(message.images.empty());
    CHECK(original[0].content.find("Pixels are unavailable: no workspace path was recorded.") !=
          std::string::npos);
    const auto metadata = nlohmann::json::parse(original[1].content);
    CHECK(metadata["pixel_status"] == "unavailable_no_workspace_path");
    CHECK(metadata["workspace_path"] == "");
    const auto later = MaiContextBuilder().build({user, assistant, userMessage("Continue", {})});
    CHECK(later.size() == 3);
    for (std::size_t index = 0; index < original.size() && index < later.size(); ++index)
        CHECK(modelMessageSnapshot(original[index]) == modelMessageSnapshot(later[index]));
}

void testUncompactedRequestKeepsAllUserAndToolPixels() {
    const std::vector<MaiMessage> history = {
        userMessage("Earlier request", "old-user.jpg"), toolImages("old-tool-", 3),
        userMessage("Analyze this video", "current-user.jpg"), toolImages("frame-", 39)};
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build(history);
    std::vector<std::string> images;
    for (const auto& message : request) {
        for (const auto& image : message.images) images.push_back(image.path);
    }
    CHECK(images.size() == 44);
    CHECK(images.front() == "old-user.jpg");
    if (images.size() != 44) return;
    for (int index = 0; index < 3; ++index)
        CHECK(images[index + 1] == "old-tool-" + std::to_string(index) + ".jpg");
    CHECK(images[4] == "current-user.jpg");
    for (int index = 0; index < 39; ++index)
        CHECK(images[index + 5] == "frame-" + std::to_string(index) + ".jpg");
    CHECK(request.front().role == MaiModelRole::User);
    for (const auto& item : request) CHECK(item.content.find("Omitted ") == std::string::npos);
    CHECK(request[4].content.find("Analyze this video") != std::string::npos);
    CHECK(request[4].content.find("current-user.jpg") != std::string::npos);
    CHECK(request.back().images.front().path == "frame-38.jpg");
}

void testSmallCurrentRequestKeepsEveryImage() {
    const std::vector<MaiMessage> history = {userMessage("Inspect these", {}),
                                             toolImages("frame-", 4)};
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build(history);
    int imageCount = 0;
    for (const auto& message : request) imageCount += static_cast<int>(message.images.size());
    CHECK(imageCount == 4);
    CHECK(request.back().images.front().path == "frame-3.jpg");
}

void testNewUserTurnKeepsOldImageReceiptAndPixels() {
    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    MaiToolPart viewed;
    viewed.tool = "view_image";
    viewed.callId = "call_old_photo";
    viewed.input = R"({"path":"camera/photo.jpg"})";
    viewed.output = "Loaded the image from camera/photo.jpg.";
    viewed.state = MaiToolState::Completed;
    MaiMessagePart receipt;
    receipt.body = viewed;
    assistant.parts.push_back(receipt);
    MaiMessagePart pixels;
    pixels.body = MaiImagePart{"camera/photo.jpg", "image/jpeg"};
    assistant.parts.push_back(pixels);
    const std::vector<MaiMessage> history = {userMessage("Inspect the photo", {}), assistant,
                                             userMessage("Show that photo to me", {})};
    const auto request = MaiContextBuilder().build(history);
    bool keptReceipt = false;
    bool keptCall = false;
    std::size_t pixelCount = 0;
    for (const auto& message : request) {
        for (const auto& image : message.images) {
            ++pixelCount;
            CHECK(image.path == "camera/photo.jpg");
        }
        for (const auto& call : message.invocations)
            keptCall |= call.id == viewed.callId && call.arguments == viewed.input;
        keptReceipt |= message.role == MaiModelRole::ToolResult &&
                       message.toolCallId == viewed.callId && message.content == viewed.output;
    }
    CHECK(keptCall && keptReceipt);
    CHECK(pixelCount == 1);
    CHECK(std::get<MaiToolPart>(history[1].parts[0].body).state == MaiToolState::Completed);
    CHECK(std::get<MaiImagePart>(history[1].parts[1].body).path == "camera/photo.jpg");
}

void testVideoAttachmentUsesAFileReferenceInsteadOfImagePixels() {
    MaiMessage message = userMessage("Please edit this video", {});
    MaiMessagePart video;
    video.body = MaiVideoPart{"clips/input.mp4", "video/mp4"};
    message.parts.push_back(std::move(video));
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build({message});
    CHECK(request.size() == 1);
    CHECK(request.front().images.empty());
    CHECK(request.front().content.find("clips/input.mp4") != std::string::npos);
}

void testCurrentImagesHaveExactPathAndAttachmentIdentity() {
    MaiMessage message = userMessage("Use these two grass photos", {});
    MaiMessagePart first;
    first.id = "prt_photo_one";
    first.body = MaiImagePart{"gallery/child grass 1.jpg", "image/jpeg"};
    message.parts.push_back(first);
    MaiMessagePart second;
    second.id = "prt_photo_two";
    second.body = MaiImagePart{"gallery/child grass 2.jpg", "image/jpeg"};
    message.parts.push_back(second);
    const auto request = MaiContextBuilder().build({message});
    CHECK(request.size() == 1);
    CHECK(request[0].images.size() == 2);
    CHECK(request[0].content.find("prt_photo_one") != std::string::npos);
    CHECK(request[0].content.find("gallery/child grass 1.jpg") != std::string::npos);
    CHECK(request[0].content.find("prt_photo_two") != std::string::npos);
    CHECK(request[0].content.find("gallery/child grass 2.jpg") != std::string::npos);
}

void testEveryPreviousImageKeepsItsPathAndPixels() {
    const std::vector<MaiMessage> history = {
        userMessage("first", "oldest.jpg"), userMessage("second", "recent-one.jpg"),
        userMessage("third", "recent-two.jpg"), userMessage("Use the last two", {})};
    const auto request = MaiContextBuilder().build(history);
    std::size_t pixelCount = 0;
    for (const auto& message : request) pixelCount += message.images.size();
    CHECK(pixelCount == 3);
    bool sawOldest = false;
    bool sawFirstRecent = false;
    bool sawSecondRecent = false;
    for (const auto& message : request) {
        sawOldest |= message.content.find("oldest.jpg") != std::string::npos;
        sawFirstRecent |= message.content.find("recent-one.jpg") != std::string::npos;
        sawSecondRecent |= message.content.find("recent-two.jpg") != std::string::npos;
    }
    CHECK(sawOldest);
    CHECK(sawFirstRecent);
    CHECK(sawSecondRecent);
}

void testQuotedUserMediaUsesExactSourceMessage() {
    MaiMessage source = userMessage("Earlier photo", "gallery/exact-photo.jpg");
    source.id = "msg_photo";
    source.parts.back().id = "prt_photo";
    MaiMessage quoted = userMessage("Save the photo I quoted", {});
    quoted.id = "msg_followup";
    MaiMessagePart quote;
    quote.body = MaiQuotePart{source.id, "Earlier photo"};
    quoted.parts.insert(quoted.parts.begin(), quote);
    const auto request = MaiContextBuilder().build({source, quoted});
    CHECK(request.back().role == MaiModelRole::User);
    CHECK(request.back().content.find("msg_photo") != std::string::npos);
    CHECK(request.back().content.find("prt_photo") != std::string::npos);
    CHECK(request.back().content.find("gallery/exact-photo.jpg") != std::string::npos);
    CHECK(request.back().images.empty());
}

void testExplicitToolOutputBudgetKeepsItsExistingPolicy() {
    MaiMessage probe = userMessage("Inspect video", {});
    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    MaiToolPart tool;
    tool.tool = "ffprobe";
    tool.callId = "call_probe";
    tool.input = R"({"mode":"frames"})";
    tool.output = std::string(1'600'000, 'x');
    tool.state = MaiToolState::Completed;
    MaiMessagePart part;
    part.body = std::move(tool);
    assistant.parts.push_back(std::move(part));
    const MaiMessage followup = userMessage("j", {});
    MaiContextBuilder::Options options;
    options.maxHistoryTextBytes = 256 * 1024;
    options.maxToolResultBytes = 16 * 1024;
    options.maxOlderMessageBytes = 32 * 1024;
    options.maxReasoningBytes = 8 * 1024;
    const auto request = MaiContextBuilder(options).build({probe, assistant, followup});
    std::size_t total = 0;
    bool sawCall = false;
    bool sawShortenedResult = false;
    for (const MaiModelMessage& message : request) {
        total += message.content.size() + message.reasoning.size();
        if (!message.invocations.empty() && message.invocations.front().id == "call_probe")
            sawCall = true;
        if (message.role == MaiModelRole::ToolResult && message.toolCallId == "call_probe") {
            sawShortenedResult = message.content.find("Tool output shortened") != std::string::npos;
            CHECK(message.content.size() < 17 * 1024);
        }
    }
    CHECK(sawCall);
    CHECK(sawShortenedResult);
    CHECK(total < 32 * 1024);
    CHECK(request.back().role == MaiModelRole::User);
    CHECK(request.back().content == "j");
    CHECK(std::get<MaiToolPart>(assistant.parts.front().body).output.size() == 1'600'000);
}

void testExplicitHistoryBudgetKeepsItsExistingPolicy() {
    std::vector<MaiMessage> history;
    for (int index = 0; index < 20; ++index)
        history.push_back(userMessage(
            "Earlier turn " + std::to_string(index) + " " + std::string(20'000, 'a'), {}));
    history.push_back(userMessage("Current request", {}));
    MaiContextBuilder::Options options;
    options.maxHistoryTextBytes = 256 * 1024;
    options.maxOlderMessageBytes = 32 * 1024;
    const auto request = MaiContextBuilder(options).build(history);
    std::size_t total = 0;
    for (const MaiModelMessage& message : request) total += message.content.size();
    CHECK(total < 300 * 1024);
    CHECK(request.front().role == MaiModelRole::System);
    CHECK(request.front().content.find("Earlier conversation omitted") != std::string::npos);
    CHECK(request.back().role == MaiModelRole::User);
    CHECK(request.back().content == "Current request");
}

// 使用真实原文字节比较，不能只检查存储中仍存在或模型消息条数。
// 早期链接放在长消息中部，旧的首尾摘录和总预算都无法通过这些断言。
void testDefaultHistoryKeepsLongOriginalMessagesAndToolResults() {
    const std::string firstText = std::string(600'000, 'a') +
                                  "\nhttps://example.invalid/original-video.mp4\n" +
                                  std::string(600'000, 'b');
    const std::string answerText =
        std::string(90'000, 'c') + "\nOriginal answer\n" + std::string(90'000, 'd');
    const std::string output =
        std::string(800'000, 'e') + "\nOriginal tool detail\n" + std::string(800'000, 'f');
    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    MaiMessagePart answer;
    answer.body = MaiTextPart{answerText};
    assistant.parts.push_back(answer);
    MaiToolPart tool;
    tool.tool = "read";
    tool.callId = "call_original_file";
    tool.input = R"({"path":"owned-test.txt"})";
    tool.output = output;
    tool.state = MaiToolState::Completed;
    MaiMessagePart result;
    result.body = tool;
    assistant.parts.push_back(result);
    const std::vector<MaiMessage> history = {userMessage(firstText, {}), assistant,
                                             userMessage("Use the earlier link", {})};
    const auto request = MaiContextBuilder().build(history);
    CHECK(request.size() == 4);
    if (request.size() == 4) {
        CHECK(request[0].role == MaiModelRole::User);
        CHECK(request[0].content == firstText);
        CHECK(request[1].role == MaiModelRole::Assistant);
        CHECK(request[1].content == answerText);
        CHECK(request[1].invocations.size() == 1);
        if (request[1].invocations.size() == 1) {
            CHECK(request[1].invocations[0].id == tool.callId);
            CHECK(request[1].invocations[0].name == tool.tool);
            CHECK(request[1].invocations[0].arguments == tool.input);
        }
        CHECK(request[2].role == MaiModelRole::ToolResult);
        CHECK(request[2].toolCallId == tool.callId);
        CHECK(request[2].content == output);
        CHECK(request[3].role == MaiModelRole::User);
        CHECK(request[3].content == "Use the earlier link");
    }
    CHECK(history.front().text() == firstText);
    CHECK(std::get<MaiToolPart>(history[1].parts[1].body).output == output);
}

void testDefaultHistoryKeepsEveryOldTurnInOrder() {
    std::vector<MaiMessage> history;
    for (int index = 0; index < 60; ++index)
        history.push_back(userMessage(
            "Original turn " + std::to_string(index) + " " + std::string(20'000, 'x'), {}));
    history.push_back(userMessage("Current request", {}));
    const auto request = MaiContextBuilder().build(history);
    CHECK(request.size() == history.size());
    if (request.size() != history.size()) return;
    for (std::size_t index = 0; index < history.size(); ++index) {
        CHECK(request[index].role == MaiModelRole::User);
        CHECK(request[index].content == history[index].text());
    }
}

void testDefaultHistoryKeepsLargeLatestInputAndOptedInReasoning() {
    const std::string currentText =
        std::string(600'000, 'a') + "\nFull current input\n" + std::string(600'000, 'b');
    const auto current = MaiContextBuilder().build({userMessage(currentText, {})});
    CHECK(current.size() == 1);
    if (current.size() == 1) CHECK(current[0].content == currentText);

    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    MaiMessagePart reasoning;
    const std::string reasoningText(60'000, 'r');
    reasoning.body = MaiReasoningPart{reasoningText};
    assistant.parts.push_back(reasoning);
    MaiContextBuilder::Options options;
    options.includeReasoning = true;
    const auto request = MaiContextBuilder(options).build(
        {userMessage("Earlier question", {}), assistant, userMessage("Next question", {})});
    CHECK(request.size() == 3);
    if (request.size() == 3) {
        CHECK(request[1].content == reasoningText);
        CHECK(request[1].reasoning == reasoningText);
    }
}

void testExplicitZeroTextBudgetsMeanUnlimited() {
    MaiContextBuilder::Options options;
    options.maxHistoryTextBytes = 0;
    options.maxOlderMessageBytes = 0;
    options.maxToolResultBytes = 0;
    options.maxReasoningBytes = 0;
    const std::vector<MaiMessage> history = {userMessage("Keep the first message", {}),
                                             userMessage("Keep the second message", {})};
    const auto request = MaiContextBuilder(options).build(history);
    CHECK(request.size() == 2);
    if (request.size() == 2) {
        CHECK(request[0].content == history[0].text());
        CHECK(request[1].content == history[1].text());
    }
}

void testQuotedAssistantMediaUsesDeliveredArtifact() {
    MaiMessage source;
    source.id = "msg_generated";
    source.role = MaiRole::Assistant;
    MaiMessagePart artifact;
    artifact.id = "prt_delivered_video";
    MaiToolPart tool;
    tool.tool = "agent_send_media";
    tool.state = MaiToolState::Completed;
    tool.output =
        R"({"delivery":"current_ai_session","path":"generated/final.mp4","type":"video"})";
    artifact.body = tool;
    source.parts.push_back(artifact);
    MaiMessage quoted = userMessage("Save that video", {});
    quoted.id = "msg_followup";
    MaiMessagePart quote;
    quote.body = MaiQuotePart{source.id, "Media message"};
    quoted.parts.insert(quoted.parts.begin(), quote);
    const auto request = MaiContextBuilder().build({source, quoted});
    CHECK(request.back().content.find("msg_generated") != std::string::npos);
    CHECK(request.back().content.find("prt_delivered_video") != std::string::npos);
    CHECK(request.back().content.find("generated/final.mp4") != std::string::npos);
}

void testStoredReasoningDoesNotEnterDefaultHistory() {
    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    MaiMessagePart reasoning;
    reasoning.body = MaiReasoningPart{"Private draft from the previous model"};
    assistant.parts.push_back(std::move(reasoning));
    MaiMessagePart answer;
    answer.body = MaiTextPart{"Public answer"};
    assistant.parts.push_back(std::move(answer));

    const auto request = MaiContextBuilder().build(
        {userMessage("First question", {}), assistant, userMessage("Follow-up", {})});
    CHECK(request.size() == 3);
    if (request.size() != 3) return;
    CHECK(request[1].role == MaiModelRole::Assistant);
    CHECK(request[1].content == "Public answer");
    CHECK(request[1].reasoning.empty());
}

void testResponseContinuationKeepsEveryResponseAndToolBatch() {
    const auto state = [](std::string payload) {
        return MaiModelContinuationState{"responses.output.v1", "openrouter",
                                         "anthropic/claude-fable-5.2", std::move(payload)};
    };
    const auto first =
        state("[{\"type\":\"reasoning\",\"id\":\"rs_fixture\",\"encrypted_content\":\"" +
              std::string(40000, 'x') +
              "\"},{\"type\":\"function_call\",\"id\":\"fc_a\",\"call_id\":\"call_a\","
              "\"name\":\"first\",\"arguments\":\"{}\"},{\"type\":\"function_call\","
              "\"id\":\"fc_b\",\"call_id\":\"call_b\",\"name\":\"second\",\"arguments\":\"{}\"}]");
    const auto second =
        state(R"([{"type":"reasoning","id":"rs_second","encrypted_content":"owned-second"}])");
    const auto third = state(
        R"([{"type":"message","id":"msg_final","role":"assistant","content":[{"type":"output_text","text":"Final"}]}])");
    MaiMessage assistant;
    assistant.role = MaiRole::Assistant;
    const auto append = [&](auto body) {
        MaiMessagePart part;
        part.id = "part-" + std::to_string(assistant.parts.size());
        part.body = std::move(body);
        assistant.parts.push_back(std::move(part));
    };
    append(MaiReasoningPart{"", first});
    for (const auto& id : {"call_a", "call_b"}) {
        MaiToolPart tool;
        tool.callId = id;
        tool.tool = id == std::string("call_a") ? "first" : "second";
        tool.input = "{}";
        tool.output = "owned result";
        tool.state = MaiToolState::Completed;
        append(std::move(tool));
    }
    append(MaiReasoningPart{"", second});
    append(MaiReasoningPart{"", third});
    append(MaiTextPart{"Final"});
    MaiContextBuilder::Options options;
    options.maxReasoningBytes = 8;
    const auto request = MaiContextBuilder(options).build(
        {userMessage("Question", {}), assistant, userMessage("Follow-up", {})});
    CHECK(request.size() == 7);
    if (request.size() != 7) return;
    CHECK(request[1].continuation.has_value());
    if (request[1].continuation) CHECK(request[1].continuation->payload == first.payload);
    CHECK(request[1].invocations.size() == 2);
    CHECK(request[2].role == MaiModelRole::ToolResult && request[2].toolCallId == "call_a");
    CHECK(request[3].role == MaiModelRole::ToolResult && request[3].toolCallId == "call_b");
    CHECK(request[4].continuation.has_value());
    if (request[4].continuation) CHECK(request[4].continuation->payload == second.payload);
    CHECK(request[4].content.empty() && request[4].invocations.empty());
    CHECK(request[5].continuation.has_value());
    if (request[5].continuation) CHECK(request[5].continuation->payload == third.payload);
    CHECK(request[5].content == "Final");
    CHECK(request[6].role == MaiModelRole::User && request[6].content == "Follow-up");
}

void testResponseContinuationContributesToInputBudget() {
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    MaiModelMessage message;
    message.role = MaiModelRole::Assistant;
    request.messages.push_back(message);
    const auto before = maiEstimateModelInputTokens(request);
    request.messages.front().continuation = MaiModelContinuationState{
        "responses.output.v1", "openrouter", "anthropic/claude-fable-5.2", std::string(40000, 'x')};
    const auto after = maiEstimateModelInputTokens(request);
    CHECK(after.tokens > before.tokens + 10000);
    CHECK(!after.exact);
}

}  // namespace

int main() {
    testImageRequestsOnlyAppendUntilCompaction();
    testOnlyCompactionBoundaryRemovesCoveredImageParts();
    testMissingImagePathHasOnlyALocalStableNotice();
    testUncompactedRequestKeepsAllUserAndToolPixels();
    testSmallCurrentRequestKeepsEveryImage();
    testNewUserTurnKeepsOldImageReceiptAndPixels();
    testVideoAttachmentUsesAFileReferenceInsteadOfImagePixels();
    testCurrentImagesHaveExactPathAndAttachmentIdentity();
    testEveryPreviousImageKeepsItsPathAndPixels();
    testQuotedUserMediaUsesExactSourceMessage();
    testExplicitToolOutputBudgetKeepsItsExistingPolicy();
    testExplicitHistoryBudgetKeepsItsExistingPolicy();
    testDefaultHistoryKeepsLongOriginalMessagesAndToolResults();
    testDefaultHistoryKeepsEveryOldTurnInOrder();
    testDefaultHistoryKeepsLargeLatestInputAndOptedInReasoning();
    testExplicitZeroTextBudgetsMeanUnlimited();
    testQuotedAssistantMediaUsesDeliveredArtifact();
    testStoredReasoningDoesNotEnterDefaultHistory();
    testResponseContinuationKeepsEveryResponseAndToolBatch();
    testResponseContinuationContributesToInputBudget();
    return failures == 0 ? 0 : 1;
}

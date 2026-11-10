// 文件作用：验证“MaiContextBuilderImage”的正确行为和边界条件。
// 验证入口：testCurrentRequestKeepsOnlyRecentToolPixels、testSmallCurrentRequestKeepsEveryImage、
// testVideoAttachmentUsesAFileReferenceInsteadOfImagePixels；检查可观察结果而非实现细节。
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

void testCurrentRequestKeepsOnlyRecentToolPixels() {
    const std::vector<MaiMessage> history = {
        userMessage("Earlier request", "old-user.jpg"), toolImages("old-tool-", 3),
        userMessage("Analyze this video", "current-user.jpg"), toolImages("frame-", 39)};
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build(history);
    std::vector<std::string> images;
    for (const auto& message : request) {
        for (const auto& image : message.images) images.push_back(image.path);
    }
    CHECK(images.size() == 9);
    CHECK(images.front() == "current-user.jpg");
    for (int index = 0; index < 8; ++index)
        CHECK(images[index + 1] == "frame-" + std::to_string(index + 31) + ".jpg");
    CHECK(request.front().role == MaiModelRole::System);
    CHECK(request.front().content.find("Omitted 35 older image observations") != std::string::npos);
    CHECK(request.front().content.find("view_image") == std::string::npos);
    CHECK(request.front().content.find("Do not infer their visual contents") != std::string::npos);
    CHECK(request[2].content.find("Analyze this video") != std::string::npos);
    CHECK(request[2].content.find("current-user.jpg") != std::string::npos);
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

void testNewUserTurnKeepsOldImageReceiptWithoutReinjectingPixels() {
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
    for (const auto& message : request) {
        CHECK(message.images.empty());
        for (const auto& call : message.invocations)
            keptCall |= call.id == viewed.callId && call.arguments == viewed.input;
        keptReceipt |= message.role == MaiModelRole::ToolResult &&
                       message.toolCallId == viewed.callId && message.content == viewed.output;
    }
    CHECK(keptCall && keptReceipt);
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

void testRecentPreviousImagePathsRemainAvailableWithoutOldPixels() {
    MaiContextBuilder::Options options;
    options.maxRecentUserImageReferences = 2;
    const std::vector<MaiMessage> history = {
        userMessage("first", "oldest.jpg"), userMessage("second", "recent-one.jpg"),
        userMessage("third", "recent-two.jpg"), userMessage("Use the last two", {})};
    const auto request = MaiContextBuilder(options).build(history);
    for (const auto& message : request) CHECK(message.images.empty());
    bool sawOldest = false;
    bool sawFirstRecent = false;
    bool sawSecondRecent = false;
    for (const auto& message : request) {
        sawOldest |= message.content.find("oldest.jpg") != std::string::npos;
        sawFirstRecent |= message.content.find("recent-one.jpg") != std::string::npos;
        sawSecondRecent |= message.content.find("recent-two.jpg") != std::string::npos;
    }
    CHECK(!sawOldest);
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

}  // namespace

int main() {
    testCurrentRequestKeepsOnlyRecentToolPixels();
    testSmallCurrentRequestKeepsEveryImage();
    testNewUserTurnKeepsOldImageReceiptWithoutReinjectingPixels();
    testVideoAttachmentUsesAFileReferenceInsteadOfImagePixels();
    testCurrentImagesHaveExactPathAndAttachmentIdentity();
    testRecentPreviousImagePathsRemainAvailableWithoutOldPixels();
    testQuotedUserMediaUsesExactSourceMessage();
    testExplicitToolOutputBudgetKeepsItsExistingPolicy();
    testExplicitHistoryBudgetKeepsItsExistingPolicy();
    testDefaultHistoryKeepsLongOriginalMessagesAndToolResults();
    testDefaultHistoryKeepsEveryOldTurnInOrder();
    testDefaultHistoryKeepsLargeLatestInputAndOptedInReasoning();
    testExplicitZeroTextBudgetsMeanUnlimited();
    testQuotedAssistantMediaUsesDeliveredArtifact();
    testStoredReasoningDoesNotEnterDefaultHistory();
    return failures == 0 ? 0 : 1;
}

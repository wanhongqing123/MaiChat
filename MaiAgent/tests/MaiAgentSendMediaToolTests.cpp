// 文件作用：验证“把 Agent 生成的媒体文件交付给聊天或宿主界面”的正确行为和边界条件。
// 验证入口：阅读 main 或测试注册处，核对输入、预期结果与失败输出。
#include "MaiAgentSendMediaTool.h"
#include "MaiAgentSendFileTool.h"

#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <string>

#include <json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

// Only metadata and a bounded header are needed to validate this limit. Create
// an owned sparse file instead of allocating or copying half a GiB of payload.
bool makeOwnedSparseFile(const MaiFilePath& path, std::uint64_t bytes) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    LARGE_INTEGER end;
    end.QuadPart = static_cast<LONGLONG>(bytes);
    const bool ok =
        DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
        SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    return ok;
#else
    const int file = open(path.value().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) return false;
    const bool ok = ftruncate(file, static_cast<off_t>(bytes)) == 0;
    close(file);
    return ok;
#endif
}

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-agent-send-media-tests"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath image = root.append(MaiFilePath::fromUtf8("result.png"));
    const std::string png =
        "\x89PNG\r\n\x1a\n"
        "test-image-bytes";
    CHECK(!MaiFileSystem::writeFile(image, png));
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiAgentSendMediaTool();
    CHECK(tool && tool->name() == "agent_send_media");
    const MaiToolResult delivered = tool->execute(
        R"({"file_path":"result.png","type":"image","caption":"Edited photo"})", context);
    CHECK(!delivered.hasError());
    const auto output = nlohmann::json::parse(delivered.output());
    CHECK(output["delivery"] == "current_ai_session");
    CHECK(output["path"] == "result.png");
    CHECK(output["mime_type"] == "image/png");
    CHECK(output["caption"] == "Edited photo");
    CHECK(MaiFileSystem::exists(image));
    CHECK(tool->execute(R"({"file_path":"../result.png","type":"image"})", context).hasError());
    CHECK(tool->execute(R"({"file_path":"result.png","type":"video"})", context).hasError());
    CHECK(tool->execute(R"({"file_path":"result.png","type":"image","peer_id":"someone"})", context)
              .hasError());
    const MaiFilePath video = root.append(MaiFilePath::fromUtf8("generated.mp4"));
    CHECK(!MaiFileSystem::writeFile(video, std::string("\x00\x00\x00\x18"
                                                       "ftypisom",
                                                       12)));
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "video-session";
    session.directory = root.toUtf8();
    session.model = "test-model";
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = "spt_generated_video";
    task.ownerSessionId = session.id;
    task.specialistName = "wan_video";
    task.providerTaskId = "provider-task-1";
    task.created = 1;
    CHECK(!store->insertSpecialistTask(task));
    CHECK(!store->finishSpecialistTask(task.id, session.id, MaiSpecialistTaskStatus::Succeeded,
                                       "Ready", video.toUtf8(), 2));
    context.sessionId = session.id;
    context.specialistTasks = store.get();
    const auto sourceRequest = nlohmann::json{
        {"file_path", "generated.mp4"}, {"type", "video"}, {"source_task_id", task.id}};
    const MaiToolResult sourced = tool->execute(sourceRequest.dump(), context);
    CHECK(!sourced.hasError());
    const auto sourcedOutput = nlohmann::json::parse(sourced.output());
    CHECK(sourcedOutput["source_task_id"] == task.id);
    CHECK(sourcedOutput["source_tool"] == "wan_video");
    CHECK(tool->execute(nlohmann::json{{"file_path", "generated.mp4"},
                                       {"type", "video"},
                                       {"source_task_id", "spt_other_task"}}
                            .dump(),
                        context)
              .hasError());
    context.sessionId = "other-session";
    CHECK(tool->execute(sourceRequest.dump(), context).hasError());
    context.sessionId = session.id;
    const MaiFilePath unrelated = root.append(MaiFilePath::fromUtf8("unrelated.mp4"));
    CHECK(!MaiFileSystem::writeFile(unrelated, std::string("\x00\x00\x00\x18"
                                                           "ftypisom",
                                                           12)));
    CHECK(tool->execute(nlohmann::json{{"file_path", "unrelated.mp4"},
                                       {"type", "video"},
                                       {"source_task_id", task.id}}
                            .dump(),
                        context)
              .hasError());
    CHECK(tool->execute(
                  nlohmann::json{
                      {"file_path", "result.png"}, {"type", "image"}, {"source_task_id", task.id}}
                      .dump(),
                  context)
              .hasError());
    {
        // The separate file tool shares the ordinary metadata validator. Media
        // retains its image/video/audio contract and does not gain a file alias.
        auto fileTool = makeMaiAgentSendFileTool();
        CHECK(fileTool && fileTool->name() == "agent_send_file");
        MaiToolRegistry builtin;
        registerMaiBuiltinTools(builtin);
        CHECK(builtin.findForModel("agent_send_file") != nullptr);
        const auto schema = nlohmann::json::parse(fileTool->parametersSchema());
        CHECK(schema["required"] == nlohmann::json::array({"file_path"}));
        CHECK(schema["additionalProperties"] == false);
        CHECK(!schema["properties"].contains("peer_id") && !schema["properties"].contains("type"));
        const MaiFilePath workspace = root.append(MaiFilePath::fromUtf8("owned-files"));
        CHECK(!MaiFileSystem::createDirectories(workspace));
        const MaiFilePath file = workspace.append(MaiFilePath::fromUtf8("result.bin"));
        const std::string bytes = "Owned generic attachment\n";
        CHECK(!MaiFileSystem::writeFile(file, bytes));
        MaiToolContext fileContext;
        fileContext.root = workspace.toUtf8();
        const auto prepared = maiPrepareAgentFile("result.bin", fileContext);
        CHECK(prepared);
        CHECK(prepared.value().relativePath == "result.bin");
        CHECK(prepared.value().name == "result.bin");
        CHECK(prepared.value().mimeType == "application/octet-stream");
        CHECK(prepared.value().bytes == bytes.size());
        const auto deliveredFile = fileTool->execute(
            R"({"file_path":"result.bin","caption":"Owned attachment"})", fileContext);
        CHECK(!deliveredFile.hasError());
        const auto receipt = nlohmann::json::parse(deliveredFile.output());
        CHECK(receipt["delivery"] == "current_ai_session");
        CHECK(receipt["path"] == "result.bin");
        CHECK(receipt["type"] == "file" && receipt["name"] == "result.bin");
        CHECK(receipt["mime_type"] == "application/octet-stream");
        CHECK(receipt["bytes"] == bytes.size());
        CHECK(receipt["caption"] == "Owned attachment");
        CHECK(MaiFileSystem::exists(file));
        CHECK(fileTool->execute(R"({"file_path":"result.bin","peer_id":"someone"})", fileContext)
                  .hasError());
        const MaiFilePath empty = workspace.append(MaiFilePath::fromUtf8("empty.bin"));
        CHECK(!MaiFileSystem::createEmptyFile(empty));
        const MaiFilePath outside = root.append(MaiFilePath::fromUtf8("outside.bin"));
        CHECK(!MaiFileSystem::writeFile(outside, "Owned outside-workspace file"));
        for (const auto& relative : {"missing.bin", "empty.bin", "../outside.bin"}) {
            CHECK(!maiPrepareAgentFile(relative, fileContext));
            CHECK(fileTool->execute(nlohmann::json{{"file_path", relative}}.dump(), fileContext)
                      .hasError());
        }
        CHECK(!maiPrepareAgentFile(file.toUtf8(), fileContext));
        const MaiFilePath boundary = workspace.append(MaiFilePath::fromUtf8("boundary.bin"));
        constexpr std::uint64_t limit = 512ull * 1024 * 1024;
        CHECK(makeOwnedSparseFile(boundary, limit));
        const auto atLimit = maiPrepareAgentFile("boundary.bin", fileContext);
        CHECK(atLimit && atLimit.value().bytes == limit);
        CHECK(makeOwnedSparseFile(boundary, limit + 1));
        CHECK(!maiPrepareAgentFile("boundary.bin", fileContext));
        CHECK(fileTool->execute(R"({"file_path":"boundary.bin"})", fileContext).hasError());
        CHECK(!maiPrepareAgentFile(".", fileContext));
        CHECK(fileTool->execute(R"({"file_path":"result.bin","type":"file"})", fileContext)
                  .hasError());
        std::atomic<bool> canceled{true};
        fileContext.cancel = &canceled;
        CHECK(maiPrepareAgentFile("result.bin", fileContext).error().code() ==
              MaiErrorCode::Canceled);
        CHECK(fileTool->execute(R"({"file_path":"result.bin"})", fileContext).error().code() ==
              MaiErrorCode::Canceled);
        fileContext.cancel = nullptr;
        std::string unchanged;
        CHECK(!MaiFileSystem::readFile(file, unchanged));
        CHECK(unchanged == bytes);
#if !defined(_WIN32)
        const MaiFilePath link = workspace.append(MaiFilePath::fromUtf8("escaped-link.bin"));
        CHECK(symlink(outside.value().c_str(), link.value().c_str()) == 0);
        CHECK(!maiPrepareAgentFile("escaped-link.bin", fileContext));
        // ext4 等 POSIX 文件系统允许原始非 UTF-8 名；APFS 会在 open 时以 EILSEQ 拒绝。
        // 必须经 ASCII 链接验证解析后的 referent，输入 JSON 有效不代表输出元数据有效。
        std::string rawName = "invalid-";
        rawName.push_back(char(0xff));
        rawName += ".bin";
        const auto rawPath = workspace.append(MaiFilePath(rawName));
        const int rawFile = open(rawPath.value().c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (rawFile >= 0) {
            CHECK(write(rawFile, "owned", 5) == 5);
            close(rawFile);
            const auto alias = workspace.append(MaiFilePath("ascii-reference.bin"));
            CHECK(symlink(rawPath.value().c_str(), alias.value().c_str()) == 0);
            const auto invalid = maiPrepareAgentFile("ascii-reference.bin", fileContext);
            CHECK(!invalid && invalid.error().code() == MaiErrorCode::InvalidInput);
            const auto rejected =
                fileTool->execute(R"({"file_path":"ascii-reference.bin"})", fileContext);
            CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
            char original[5] = {};
            const int verify = open(rawPath.value().c_str(), O_RDONLY);
            CHECK(verify >= 0 && read(verify, original, 5) == 5);
            close(verify);
            CHECK(std::string(original, 5) == "owned");
        } else {
            CHECK(errno == EILSEQ || errno == EINVAL);
            std::puts(
                "SKIP raw non-UTF8 referent: host filesystem refuses the filename before "
                "preparation");
        }
        CHECK(maiPrepareAgentFile(rawName, fileContext).error().code() ==
              MaiErrorCode::InvalidInput);
#endif
    }
    MaiFileSystem::removeRecursively(root);
    return 0;
}

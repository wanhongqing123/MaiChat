#include "MaiAccountTools.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiIdGenerator.h"

namespace {
using Json = nlohmann::json;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    const std::string stable = "https://ichat.life/maichat/avatars/account_a/2026-11-03/avatar.jpg";
    int reads = 0;
    Json profile{{"logged_in", true},
                 {"account_id", "account_a"},
                 {"im_user_id", "im_a"},
                 {"nickname", "Test user"},
                 {"phone_masked", "138****1234"},
                 {"avatar_url", stable},
                 {"token", "synthetic-secret"},
                 {"login_session_id", "synthetic-session"}};
    bool fail = false;
    auto read = makeMaiCurrentAccountTool([&](const std::string&, const MaiToolContext&) {
        ++reads;
        return fail ? MaiToolResult::failure(MaiErrorCode::Network, "synthetic-secret")
                    : MaiToolResult::success(profile.dump());
    });
    MaiToolContext context;
    require(read->name() == "maichat_get_current_account", "wrong read tool");
    require(!read->requiresApproval("{}") && !read->requiresPerCallApproval("{}"), "read tool asks approval");
    require(Json::parse(read->parametersSchema())["additionalProperties"] == false, "read schema accepts extra fields");
    for (const char* invalid : {"[]", "bad", R"({"token":"synthetic-secret"})", R"({"account_id":"other"})"}) {
        require(read->execute(invalid, context).hasError() && reads == 0, "untrusted account selector reached host");
    }
    auto result = read->execute("{}", context);
    require(!result.hasError(), "signed-in read failed");
    auto output = Json::parse(result.output());
    require(output.size() == 6 && output["account_id"] == "account_a" && output["im_user_id"] == "im_a",
            "public account fields differ");
    require(result.output().find("synthetic") == std::string::npos, "private auth fields leaked");
    profile["logged_in"] = false;
    result = read->execute("{}", context);
    require(!result.hasError() && Json::parse(result.output()) == Json{{"logged_in", false}},
            "logout exposed cached profile");
    profile["logged_in"] = true;
    profile["nickname"] = "";
    profile["phone_masked"] = "13812341234";
    profile["avatar_url"] = stable + "?signature=synthetic-secret";
    output = Json::parse(read->execute("{}", context).output());
    require(output["nickname"].is_null() && output["phone_masked"].is_null() && output["avatar_url"].is_null(),
            "empty nickname, raw phone or signed avatar was exposed");
    profile.erase("im_user_id");
    require(read->execute("{}", context).hasError(), "missing login identity accepted");
    fail = true;
    result = read->execute("{}", context);
    require(result.hasError() && result.error().message().find("synthetic-secret") == std::string::npos,
            "raw auth error leaked");

    namespace fs = std::filesystem;
    const auto temporary = fs::temp_directory_path() / MaiIdGenerator::generate("account-tools");
    const auto workspace = temporary / "workspace";
    fs::create_directories(workspace);
    const auto image = workspace / "image.png";
    const std::string original = "synthetic-image-content";
    std::ofstream(image, std::ios::binary) << original;
    std::ofstream(temporary / "outside.png", std::ios::binary) << original;
    context.root = workspace.u8string();
    context.sessionId = "session_a";
    int mutations = 0;
    Json received;
    Json avatarResult{{"updated", true}, {"avatar_url", stable}, {"upload_url", "synthetic-secret"}};
    auto write = makeMaiSetAvatarTool([&](const std::string& arguments, const MaiToolContext&) {
        ++mutations;
        received = Json::parse(arguments);
        return MaiToolResult::success(avatarResult.dump());
    });
    require(write->requiresApproval("{}") && write->requiresPerCallApproval("{}"), "avatar write bypassed approval");
    for (const char* invalid : {"{}", R"({"path":3})", R"({"path":"../outside.png"})",
                                R"({"path":"image.png","request_id":"forged"})", R"({"path":"missing.png"})"}) {
        require(write->execute(invalid, context).hasError() && mutations == 0,
                "invalid image/request binding reached host");
    }
    result = write->execute(R"({"path":"image.png"})", context);
    require(!result.hasError(), "valid avatar update failed");
    require(Json::parse(result.output()) == Json{{"updated", true}, {"avatar_url", stable}},
            "avatar result is not a safe confirmed receipt");
    require(received["path"] == fs::canonical(image).u8string(), "resolved image was not the canonical source");
    require(received["workspace_path"] == context.root && received["session_id"] == context.sessionId,
            "workspace/session identity was not host-bound");
    require(received["request_id"].is_string(), "internal avatar request ID was missing");
    const auto firstId = received["request_id"];
    require(!write->execute(R"({"path":"image.png"})", context).hasError() && received["request_id"] != firstId,
            "avatar request ID was reused");
    avatarResult = {{"queued", true}, {"avatar_url", stable}};
    require(write->execute(R"({"path":"image.png"})", context).hasError(), "queued update claimed SDK confirmation");
    avatarResult = {{"updated", true}, {"avatar_url", stable + "?signature=synthetic-secret"}};
    require(write->execute(R"({"path":"image.png"})", context).hasError(), "signed URL returned after update");
    std::atomic<bool> cancel{true};
    context.cancel = &cancel;
    const int beforeCancel = mutations;
    require(write->execute(R"({"path":"image.png"})", context).hasError() && mutations == beforeCancel,
            "already canceled request reached host");
    context.cancel = nullptr;
    std::ifstream source(image, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(source)), {});
    require(saved == original, "avatar tool modified the source image");
    source.close();

    // 真实在途取消通知；SDK 已确认成功时即便取消也必须保留真实成功回执。
    for (bool sdkConfirmed : {false, true}) {
        cancel = false;
        context.cancel = &cancel;
        std::atomic<bool> entered{false}, notified{false};
        std::string notification;
        auto pending = makeMaiSetAvatarTool(
            [&](const std::string&, const MaiToolContext&) {
                entered = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!notified && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return sdkConfirmed ? MaiToolResult::success(Json{{"updated", true}, {"avatar_url", stable}}.dump())
                                    : MaiToolResult::failure(MaiErrorCode::Canceled, "synthetic-secret");
            },
            [&](const std::string& id) {
                notification = id;
                notified = true;
            });
        std::thread stop([&] {
            while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cancel = true;
        });
        result = pending->execute(R"({"path":"image.png"})", context);
        stop.join();
        require(notified && !notification.empty(), "in-flight cancellation did not reach native host");
        require(sdkConfirmed ? !result.hasError() : result.hasError(), "cancellation hid a confirmed SDK outcome");
    }
    fs::remove_all(temporary);
    std::cout << "account tools contract passed: " << checks << " checks\n";
    return 0;
}

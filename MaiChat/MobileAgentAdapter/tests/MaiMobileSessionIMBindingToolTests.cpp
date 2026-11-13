#include "MaiMobileSessionIMBindingTool.h"

#include <atomic>
#include <cstdio>
#include <stdexcept>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)

int main() {
    try {
        int calls = 0;
        auto tool = makeMaiMobileSessionIMBindingTool(
            [&](const std::string& arguments, const MaiToolContext&) {
                ++calls;
                return MaiToolResult::success(arguments);
            });
        MaiToolContext context;
        std::atomic<bool> canceled{false};
        context.cancel = &canceled;
        CHECK(tool->name() == "maichat_session_im_binding");
        CHECK(!tool->requiresApproval(R"({"action":"list"})"));
        CHECK(!tool->requiresPerCallApproval(R"({"action":"list"})"));
        for (const char* arguments : {R"({"action":"bind","peer_id":"peer","session_id":"session"})",
                                      R"({"action":"unbind","peer_id":"peer"})"}) {
            CHECK(tool->requiresApproval(arguments));
            CHECK(tool->requiresPerCallApproval(arguments));
            canceled = true;
            const auto rejected = tool->execute(arguments, context);
            CHECK(rejected.error().code() == MaiErrorCode::Canceled);
            CHECK(rejected.execution() && rejected.execution()->started == false);
            CHECK(rejected.execution()->effectStatus == MaiToolEffectStatus::None);
            CHECK(calls == 0);
        }
        canceled = false;
        for (const char* invalid : {"{}", R"({"action":"activate"})",
                                     R"({"action":"list","peer_id":"peer"})",
                                     R"({"action":"bind","peer_id":"peer"})",
                                     R"({"action":"unbind","peer_id":"peer","session_id":"session"})",
                                     R"({"action":"bind","peer_id":" peer","session_id":"session"})",
                                     R"({"action":"bind","peer_id":"peer","session_id":"../session"})",
                                     R"({"action":"bind","peer_id":"peer","session_id":"session","scope":{}})"}) {
            CHECK(tool->requiresApproval(invalid));
            CHECK(tool->requiresPerCallApproval(invalid));
            const auto rejected = tool->execute(invalid, context);
            CHECK(rejected.error().code() == MaiErrorCode::InvalidInput);
            CHECK(calls == 0);
        }
        const auto listed = tool->execute(R"({"action":"list"})", context);
        CHECK(!listed.hasError());
        CHECK(calls == 1 && listed.output() == R"({"action":"list"})");
        const auto unavailable = makeMaiMobileSessionIMBindingTool({})->execute(R"({"action":"list"})", context);
        CHECK(unavailable.error().code() == MaiErrorCode::NotConfigured);
        std::puts("PASS: mobile binding tool strict actions, approval and pre-dispatch cancellation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

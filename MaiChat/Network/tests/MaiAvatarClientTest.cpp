#include "MaiAvatarClient.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
constexpr unsigned char kPng[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I',  'H',  'D',  'R', 0,
                                  0,    0,   1,   0,   0,  0,  1,  8,  6, 0, 0, 0,  0x1f, 0x15, 0xc4, 0x89};
}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected local TLS origin and CA path");
    const std::string origin = argv[1];
    MaiAvatarOptions options{};
    options.account_id_utf8 = "account_a";
    options.account_token_utf8 = "local-avatar-fixture-token";
    options.ca_path_utf8 = argv[2];
    options.proxy_url_utf8 = "";
    options.signed_upload_host_suffix_utf8 = "localhost";
    options.connect_timeout_ms = 2000;
    options.transfer_timeout_ms = 5000;
    const auto run = [&](const char* mode, MaiAvatarStatus expected, const MaiAvatarCancel* cancel = nullptr) {
        const std::string base = origin + "/" + mode;
        options.media_base_url_utf8 = base.c_str();
        MaiAvatarResult result{};
        mai_avatar_upload(&options, kPng, sizeof(kPng), "image/png", cancel, &result);
        require(result.status == expected, mode);
        if (expected == MAI_AVATAR_OK) {
            require(
                result.avatar_url_utf8 && std::string(result.avatar_url_utf8).find(base + "/avatars/account_a/") == 0,
                "success did not return the stable owned avatar URL");
            require(std::strchr(result.avatar_url_utf8, '?') == nullptr, "temporary signature leaked");
        } else {
            require(!result.avatar_url_utf8, "failed upload returned a confirmed avatar");
        }
        mai_avatar_result_free(&result);
        require(!result.avatar_url_utf8, "result URL was not released");
    };
    run("ok", MAI_AVATAR_OK);
    for (const char* invalid : {"owner", "object", "host", "headers", "signed_avatar", "non_json", "large"})
        run(invalid, MAI_AVATAR_INVALID_RESPONSE);
    run("sign_redirect", MAI_AVATAR_HTTP_ERROR);
    run("put_redirect", MAI_AVATAR_HTTP_ERROR);
    run("sign_failure", MAI_AVATAR_HTTP_ERROR);
    run("put_failure", MAI_AVATAR_HTTP_ERROR);
    const std::string base = origin + "/invalid";
    options.media_base_url_utf8 = base.c_str();
    MaiAvatarResult result{};
    mai_avatar_upload(&options, kPng, 2'000'001, "image/png", nullptr, &result);
    require(result.status == MAI_AVATAR_INVALID_ARGUMENT, "oversized input reached the signer");
    mai_avatar_upload(&options, kPng, sizeof(kPng), "image/jpeg", nullptr, &result);
    require(result.status == MAI_AVATAR_INVALID_ARGUMENT, "MIME/header mismatch reached the signer");
    const std::string insecure = "http://localhost";
    options.media_base_url_utf8 = insecure.c_str();
    mai_avatar_upload(&options, kPng, sizeof(kPng), "image/png", nullptr, &result);
    require(result.status == MAI_AVATAR_INVALID_ARGUMENT, "insecure origin accepted");
    auto* cancel = mai_avatar_cancel_create();
    require(cancel, "cannot create cancellation token");
    mai_avatar_cancel_request(cancel);
    run("pre_cancel", MAI_AVATAR_CANCELED, cancel);
    mai_avatar_cancel_destroy(cancel);
    cancel = mai_avatar_cancel_create();
    require(cancel, "cannot create cancellation token");
    std::thread stop([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        mai_avatar_cancel_request(cancel);
    });
    run("slow", MAI_AVATAR_CANCELED, cancel);
    stop.join();
    mai_avatar_cancel_destroy(cancel);
    std::cout << "avatar shared TLS transport contract passed\n";
}
